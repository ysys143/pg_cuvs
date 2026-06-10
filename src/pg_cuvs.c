/*
 * pg_cuvs.c — GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS.
 *
 * Architecture: PostgreSQL C extension registering a custom Index AM
 * (Access Method). The planner routes vector similarity queries to the
 * GPU sidecar daemon (pg_cuvs_server) via UDS+shm IPC, or avoids the
 * GPU path at plan time through local cost gates so PostgreSQL can use
 * pgvector HNSW / SeqScan. Runtime GPU failures ERROR rather than
 * pretending an index scan can switch plans mid-execution.
 *
 * C/C++ split (ADR-001): CUDA code lives in cuvs_wrapper.cu. This file
 * is pure C to avoid the PG/CUDA float4 typedef collision.
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "access/amapi.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/table.h"          /* table_open/table_close (Phase 3M) */
#include "access/relation.h"       /* index_open/index_close (Phase 3M) */
#include "access/parallel.h"       /* ParallelContext (ADR-034 §4A-2 parallel build) */
#include "access/xact.h"           /* GetTransactionSnapshot (re-include safe) */
#include "optimizer/optimizer.h"   /* plan_create_index_workers (§4A-2) */
#include "storage/shm_toc.h"       /* shm_toc for the parallel DSM (§4A-2) */
#include "utils/snapmgr.h"         /* SnapshotAny / RegisterSnapshot (§4A-2) */
#include "access/genam.h"          /* try_index_open (ADR-045 index_dir reloption) */
#include "access/reloptions.h"     /* CAGRA index_dir reloption (ADR-045) */
#include "utils/array.h"           /* deconstruct_array, ARR_ELEMTYPE (Phase 3M) */
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/itemptr.h"
#include "utils/snapmgr.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/objectaccess.h"   /* object_access_hook, OAT_DROP (Phase 3G.1) */
#include "catalog/pg_class.h"       /* RelationRelationId, RELKIND_INDEX, Form_pg_class */
#include "utils/syscache.h"         /* SearchSysCache1(RELOID) */
#include "commands/dbcommands.h"    /* get_database_name (ADR-046 orphan GC) */
#include "catalog/pg_opfamily.h"    /* OPFAMILYOID, Form_pg_opfamily */
#include "nodes/extensible.h"       /* CustomPath, CustomScan (Option A D-wedge) */
#include "optimizer/paths.h"        /* set_rel_pathlist_hook */
#include "optimizer/pathnode.h"     /* add_path */
#include "optimizer/tlist.h"        /* get_sortgroupclause_tle */
#include "executor/executor.h"      /* ExecInitQual, ExecQual */
#include "postmaster/bgworker.h"    /* BackgroundWorker, RegisterBackgroundWorker */

#include <sys/stat.h>
#include <sys/file.h>   /* flock */
#include <sys/mman.h>   /* shm_open/mmap (§4A-2 parallel partial merge) */
#include <fcntl.h>      /* O_RDWR, O_CREAT */
#include <unistd.h>     /* lseek, pread, pwrite, ftruncate, unlink */
#include <dirent.h>     /* opendir/readdir (ADR-046 orphan GC) */
#include <errno.h>
#include <math.h>       /* sqrt (cosine distance) */

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "cuvs_build_corpus.h"   /* ADR-057: leak-safe tiered build-corpus handoff */
#include "hnsw_export.h"

PG_MODULE_MAGIC;

/* ----------------------------------------------------------------
 * Minimal pgvector Vector struct (avoids pgvector header dependency)
 * ---------------------------------------------------------------- */
typedef struct {
    int32  vl_len_;
    int16  dim;
    int16  unused;
    float  x[1];   /* flexible array */
} PgVector;

#define DatumGetPgVector(d)  ((PgVector *) PG_DETOAST_DATUM(d))

/* ----------------------------------------------------------------
 * GUCs
 * ---------------------------------------------------------------- */
bool  enable_cuvs                 = true;
bool  cuvs_debug                  = false;
char *cuvs_socket_path            = NULL;
char *cuvs_index_dir              = NULL;
int   cuvs_circuit_breaker_threshold = 3;
int   cuvs_k                      = 100;   /* GPU top-k (pgvector ef_search analog) */
int   cuvs_max_build_mem_mb       = 0;     /* 0 = auto (MemAvailable * safety_ratio); >0 = hard cap MB */
double cuvs_build_mem_safety_ratio = 0.5;  /* auto-limit fraction of MemAvailable */
double cuvs_max_stale_fraction     = 0.10; /* deleted-since-build fraction that reroutes to CPU */
int   cuvs_max_delta_rows          = 10000; /* pending-insert delta cap; 0 disables delta (Phase 3A) */
int   cuvs_delta_search_mode       = 0;    /* 0=auto, 1=cpu, 2=gpu (Phase 3A-3) */
int   cuvs_shard_count             = 0;    /* 0 = auto (Phase 3G); 1 = unsharded; >=2 = N GPU shards (Phase 3F) */
int   cuvs_shard_overfetch         = 0;    /* per-shard request k = k + this; recall slop at scale (Phase 3G) */
bool  cuvs_parallel_fanout         = true; /* dispatch shards concurrently in the daemon (Phase 3G) */
bool  cuvs_cpu_hnsw_fallback       = false; /* Phase 3I-1: prefer CPU HNSW over GPU CAGRA */
char *cuvs_snapshot_uri            = NULL;  /* "gs://bucket[/prefix]" — empty = disabled (Phase 3C) */
char *cuvs_cluster_id              = NULL;  /* multi-node identifier for GCS path (Phase 3C) */
char *cuvs_gcs_key_file            = NULL;  /* service account JSON path; "" = instance metadata (Phase 3C) */
int   cuvs_warmup_threads          = 2;    /* background download/load threads in daemon (Phase 3D) */
int   cuvs_search_mode             = 0;    /* 0=cagra (default), 1=brute_force (Phase 3L) */
int   cuvs_bf_precision            = 0;    /* 0=float32 (default), 1=float16 (Phase 3L) */
int   cuvs_bf_batch_wait_us        = 0;    /* daemon BF micro-batch window μs; 0=off (Phase 3L) */
int   cuvs_max_batch_queries       = 1024; /* pg_cuvs_batch_search Q cap (Phase 3M) */
bool  cuvs_filtered_knn_hook_enabled = false; /* ADR-063 Option A custom scan hook */
/* Phase 4C: background auto-compaction */
bool   cuvs_auto_compact                = false;
int    cuvs_auto_compact_check_interval = 60;
double cuvs_auto_compact_threshold      = 0.10;
char  *cuvs_auto_compact_database       = NULL;
double cuvs_filter_auto_threshold = 0.05;    /* 3O: selectivity below which pre-filter is used */
double cuvs_stream_bf_selectivity_threshold = 0.0;  /* ADR-064: selectivity below which out-of-core stream BF is used; 0=off */
int    cuvs_stream_bf_chunk_vectors = 262144;       /* ADR-064: max vectors per GPU chunk (footprint knob) */
int   cuvs_ivfpq_n_probes          = 64;     /* 3P: IVF clusters probed per search (ivfpq AM) */
int   cuvs_extend_chunk_size       = 0;      /* 3Q: CAGRA extend max_chunk_size; 0 = auto */
double cuvs_compact_delete_ratio   = 0.10;   /* 3Q: auto-compact when dead/total >= this */
bool  cuvs_extend_sync             = false;  /* 3Q: reserved — passed to daemon in future */

/* Enum option tables for the Phase 3L GUCs (string in SQL, mapped to int in C). */
static const struct config_enum_entry cuvs_search_mode_options[] = {
    {"cagra",       0, false},
    {"brute_force", 1, false},
    {NULL, 0, false}
};
static const struct config_enum_entry cuvs_bf_precision_options[] = {
    {"float32", 0, false},
    {"float16", 1, false},
    {NULL, 0, false}
};
/* Phase 3A delta search mode (string in SQL, mapped to int in C). */
static const struct config_enum_entry cuvs_delta_search_options[] = {
    {"auto", 0, false},
    {"cpu",  1, false},
    {"gpu",  2, false},
    {NULL, 0, false}
};

/* ----------------------------------------------------------------
 * Last-search stats (process-local; one slot per backend)
 *
 * Populated at the end of cuvs_gettuple on a successful CAGRA search.
 * Exposed via pg_cuvs_last_search_* SQL functions and (when
 * cuvs.debug = on) via NOTICE messages that interleave with EXPLAIN
 * VERBOSE client output.
 * ---------------------------------------------------------------- */
static uint32_t cuvs_last_latency_us = 0;
static int32    cuvs_last_n_results = -1;        /* -1 = no scan yet */
static int32    cuvs_last_k_requested = 0;
static Oid      cuvs_last_index_oid = InvalidOid;
static uint32_t cuvs_last_metric    = 0;
static int32    cuvs_last_dim       = 0;

void _PG_init(void);

/* ----------------------------------------------------------------
 * Phase 3G.1: notify the daemon to free + unlink a dropped index.
 *
 * PostgreSQL has no per-AM drop callback, so we chain object_access_hook to
 * observe DROP of a cagra index. We must NOT notify at drop-time (a rolled-back
 * DROP would destroy a still-live index's artifacts) — instead we record the
 * dropped index OIDs and fire cuvs_ipc_drop only when the transaction COMMITS.
 * Daemon-down at commit is non-fatal: WARNING, and cleanup falls to a restart.
 *
 * ADR-060: each pending DROP remembers the subtransaction that observed it, and
 * a SubXactCallback discards drops whose subxact rolled back (ROLLBACK TO
 * savepoint) and reparents drops whose subxact committed. So
 * `SAVEPOINT s; DROP INDEX i; ROLLBACK TO s; COMMIT;` leaves i's artifacts
 * intact — matching PostgreSQL's own transactional outcome (i survives).
 * ---------------------------------------------------------------- */
static object_access_hook_type prev_object_access_hook = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL; /* ADR-063 */

/* Forward declarations for ADR-063 Option A custom scan (defined at file end). */
static void cuvs_filtered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
                                            Index rti, RangeTblEntry *rte);
void cuvs_register_custom_scan(void);

/* A pending DROP and the subtransaction that observed it (ADR-060). */
typedef struct CuvsPendingDrop
{
    Oid              oid;
    SubTransactionId subid;
} CuvsPendingDrop;

static List *cuvs_pending_drops = NIL;   /* of CuvsPendingDrop*, in TopMemoryContext */

static void
cuvs_object_access(ObjectAccessType access, Oid classId, Oid objectId,
                   int subId, void *arg)
{
    if (prev_object_access_hook)
        prev_object_access_hook(access, classId, objectId, subId, arg);

    if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
    {
        HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum(objectId));
        if (HeapTupleIsValid(tup))
        {
            Form_pg_class cf = (Form_pg_class) GETSTRUCT(tup);
            if (cf->relkind == RELKIND_INDEX &&
                cf->relam == get_am_oid("cagra", true) &&
                OidIsValid(cf->relam))
            {
                MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
                CuvsPendingDrop *pd = (CuvsPendingDrop *) palloc(sizeof(CuvsPendingDrop));
                pd->oid   = objectId;
                pd->subid = GetCurrentSubTransactionId();
                cuvs_pending_drops = lappend(cuvs_pending_drops, pd);
                MemoryContextSwitchTo(old);
            }
            ReleaseSysCache(tup);
        }
    }
}

static void
cuvs_xact_callback(XactEvent event, void *arg)
{
    if (cuvs_pending_drops == NIL)
        return;

    if (event == XACT_EVENT_COMMIT)
    {
        ListCell *lc;
        foreach(lc, cuvs_pending_drops)
        {
            CuvsPendingDrop *pd = (CuvsPendingDrop *) lfirst(lc);
            int rc = cuvs_ipc_drop(cuvs_socket_path,
                                   (uint32_t) MyDatabaseId, (uint32_t) pd->oid);
            if (rc != CUVS_STATUS_OK)
                ereport(WARNING,
                        (errmsg("pg_cuvs: daemon DROP-notify failed for index %u (status %d)",
                                pd->oid, rc),
                         errhint("GPU VRAM/artifacts for the dropped index may persist; "
                                 "a daemon restart will NOT clean them (it reloads them as "
                                 "zombies). Run SELECT pg_cuvs_gc_orphans(true); to reclaim "
                                 "them, or follow the manual cleanup in OPS_GPU_PLAYBOOK.")));
        }
    }

    /* Clear on COMMIT, ABORT, or PREPARE — whatever ends the top transaction. */
    if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
        event == XACT_EVENT_PREPARE)
    {
        list_free_deep(cuvs_pending_drops);   /* frees the CuvsPendingDrop structs too */
        cuvs_pending_drops = NIL;
    }
}

/* ADR-060: a DROP collected inside a subtransaction follows that subxact's fate.
 * On ABORT (ROLLBACK TO savepoint, error rollback of a subxact) discard its
 * drops — the index is still live. On COMMIT (RELEASE SAVEPOINT) reparent its
 * drops to the parent subxact so they survive until the parent resolves; only a
 * fully-committed top transaction fires cuvs_ipc_drop. */
static void
cuvs_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
                      SubTransactionId parentSubid, void *arg)
{
    ListCell *lc;

    if (cuvs_pending_drops == NIL)
        return;

    if (event == SUBXACT_EVENT_ABORT_SUB)
    {
        foreach(lc, cuvs_pending_drops)
        {
            CuvsPendingDrop *pd = (CuvsPendingDrop *) lfirst(lc);
            if (pd->subid == mySubid)
            {
                pfree(pd);
                cuvs_pending_drops = foreach_delete_current(cuvs_pending_drops, lc);
            }
        }
    }
    else if (event == SUBXACT_EVENT_COMMIT_SUB)
    {
        foreach(lc, cuvs_pending_drops)
        {
            CuvsPendingDrop *pd = (CuvsPendingDrop *) lfirst(lc);
            if (pd->subid == mySubid)
                pd->subid = parentSubid;   /* drop now owned by the parent subxact */
        }
    }
}

/* ----------------------------------------------------------------
 * ADR-045: per-index "index_dir" reloption.
 *
 * CAGRA artifacts (.tids/.cagra/.delta/.tombstone/...) are keyed only by
 * (db_oid, index_oid); the sole variable in their path is the directory, which
 * was historically resolved from the per-session cuvs.index_dir GUC. A backend
 * that did not SET the same value resolved a different path, so the planner
 * gates failed to find the artifact and silently routed to seqscan. Recording
 * the directory in the index's reloptions makes it catalog-durable and
 * cross-backend stable: every backend resolves the build-time directory
 * regardless of its session GUC. Mirrors the pg_cuvs_hnsw reloptions machinery
 * (hnsw_export.c). The option is build-time-immutable -> AccessExclusiveLock.
 * ---------------------------------------------------------------- */
typedef struct CuvsCagraOptions
{
    int32 vl_len_;          /* varlena header — do not access directly */
    int   index_dir_offset; /* relopt string offset; 0 = option absent */
    /* 3R: CAGRA build params. ints carry their default when the option is
     * absent; build_algo is a string offset (0 = absent = auto). */
    int   graph_degree;
    int   intermediate_graph_degree;
    int   build_algo_offset;
} CuvsCagraOptions;

static relopt_kind cuvs_cagra_relopt_kind;

/* 3R: validate the build_algo string reloption at DDL time (fail-closed on typo). */
static void
cuvs_validate_build_algo(const char *value)
{
    if (value == NULL || value[0] == '\0')
        return;   /* absent -> auto */
    if (strcmp(value, "auto") != 0 &&
        strcmp(value, "ivf_pq") != 0 &&
        strcmp(value, "nn_descent") != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_cuvs: invalid build_algo \"%s\"", value),
                 errhint("Valid values: auto, ivf_pq, nn_descent.")));
}

/* One-time relopt registration; called from _PG_init. */
static void
cuvs_cagra_init_reloptions(void)
{
    cuvs_cagra_relopt_kind = add_reloption_kind();
    add_string_reloption(cuvs_cagra_relopt_kind, "index_dir",
                         "Directory holding this CAGRA index's artifacts "
                         "(overrides cuvs.index_dir for this index).",
                         "", NULL, AccessExclusiveLock);
    /* 3R: CAGRA graph-construction tuning. Build-time-immutable -> AccessExclusiveLock. */
    add_int_reloption(cuvs_cagra_relopt_kind, "graph_degree",
                      "CAGRA graph degree (neighbors per node).",
                      64, 8, 512, AccessExclusiveLock);
    add_int_reloption(cuvs_cagra_relopt_kind, "intermediate_graph_degree",
                      "CAGRA intermediate graph degree (must be >= graph_degree).",
                      128, 8, 1024, AccessExclusiveLock);
    add_string_reloption(cuvs_cagra_relopt_kind, "build_algo",
                         "CAGRA graph build algorithm: auto | ivf_pq | nn_descent.",
                         "auto", cuvs_validate_build_algo, AccessExclusiveLock);
}

static bytea *
cuvs_cagra_amoptions(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"index_dir", RELOPT_TYPE_STRING,
         offsetof(CuvsCagraOptions, index_dir_offset)},
        {"graph_degree", RELOPT_TYPE_INT,
         offsetof(CuvsCagraOptions, graph_degree)},
        {"intermediate_graph_degree", RELOPT_TYPE_INT,
         offsetof(CuvsCagraOptions, intermediate_graph_degree)},
        {"build_algo", RELOPT_TYPE_STRING,
         offsetof(CuvsCagraOptions, build_algo_offset)},
    };
    return (bytea *) build_reloptions(reloptions, validate,
                                      cuvs_cagra_relopt_kind,
                                      sizeof(CuvsCagraOptions),
                                      tab, lengthof(tab));
}

/* ----------------------------------------------------------------
 * 3P: IVF-PQ index reloptions — WITH (n_lists, pq_bits, pq_dim)
 * ---------------------------------------------------------------- */
typedef struct {
    int32  vl_len_;     /* varlena header, DO NOT TOUCH */
    int    n_lists;     /* IVF cluster count; 0 = auto → 1024 */
    int    pq_bits;     /* PQ bits per code; 0 = auto → 8 */
    int    pq_dim;      /* PQ subspace count; 0 = auto → ceil(dim/2) */
} CuvsIvfPqOptions;

static relopt_kind cuvs_ivfpq_relopt_kind;

static void
cuvs_ivfpq_init_reloptions(void)
{
    cuvs_ivfpq_relopt_kind = add_reloption_kind();
    add_int_reloption(cuvs_ivfpq_relopt_kind, "n_lists",
                      "IVF cluster count (0 = auto → 1024).",
                      1024, 1, 65536, AccessExclusiveLock);
    add_int_reloption(cuvs_ivfpq_relopt_kind, "pq_bits",
                      "PQ bits per code (4–8; 0 = auto → 8).",
                      8, 4, 8, AccessExclusiveLock);
    add_int_reloption(cuvs_ivfpq_relopt_kind, "pq_dim",
                      "PQ subspace count (0 = auto → ceil(dim/2)).",
                      0, 0, 65536, AccessExclusiveLock);
}

static bytea *
cuvs_ivfpq_amoptions(Datum reloptions, bool validate)
{
    static const relopt_parse_elt tab[] = {
        {"n_lists", RELOPT_TYPE_INT, offsetof(CuvsIvfPqOptions, n_lists)},
        {"pq_bits", RELOPT_TYPE_INT, offsetof(CuvsIvfPqOptions, pq_bits)},
        {"pq_dim",  RELOPT_TYPE_INT, offsetof(CuvsIvfPqOptions, pq_dim)},
    };
    return (bytea *) build_reloptions(reloptions, validate,
                                      cuvs_ivfpq_relopt_kind,
                                      sizeof(CuvsIvfPqOptions),
                                      tab, lengthof(tab));
}

/* 3S: polled by cuvs_ipc_search's reply wait. Reports a pending query cancel /
 * statement_timeout / backend termination WITHOUT servicing it (no longjmp — the
 * IPC layer must still close its socket and free shm). The caller raises the
 * interrupt via CHECK_FOR_INTERRUPTS once cuvs_ipc_search returns CANCELED. */
static int
cuvs_search_wait_should_abort(void)
{
    return (QueryCancelPending || ProcDiePending) ? 1 : 0;
}

void
_PG_init(void)
{
    /* Phase 3K: register pg_cuvs_hnsw WITH(source, mode, ...) reloptions. */
    cuvs_hnsw_init_reloptions();

    /* ADR-045: register CAGRA WITH(index_dir) reloption. */
    cuvs_cagra_init_reloptions();

    /* 3P: register ivfpq WITH(n_lists, pq_bits, pq_dim) reloptions. */
    cuvs_ivfpq_init_reloptions();

    DefineCustomBoolVariable(
        "enable_cuvs",
        "Enable GPU-accelerated vector search via pg_cuvs.",
        "When off, pg_cuvs AM routes all queries to pgvector CPU path.",
        &enable_cuvs,
        true,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "cuvs.debug",
        "Emit per-search NOTICE messages with daemon latency and metric.",
        "Pairs with EXPLAIN VERBOSE so the client sees GPU-side stats inline. "
        "Off by default; rely on pg_cuvs_last_search_* functions for "
        "scriptable access without log spam.",
        &cuvs_debug,
        false,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.socket_path",
        "UDS socket path for pg_cuvs_server.",
        "Default: /tmp/.s.pg_cuvs",
        &cuvs_socket_path,
        "/tmp/.s.pg_cuvs",
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.index_dir",
        "Directory for CAGRA index files.",
        "Default: $PGDATA/cuvs_indexes",
        &cuvs_index_dir,
        "",        /* empty = will be resolved to DataDir/cuvs_indexes at runtime */
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.circuit_breaker_threshold",
        "Consecutive GPU errors before circuit breaker trips.",
        "Use pg_cuvs_reset_circuit() to re-enable GPU routing.",
        &cuvs_circuit_breaker_threshold,
        3, 1, 100,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.k",
        "GPU top-k candidates fetched per cagra index scan.",
        "Analogous to hnsw.ef_search: the AM cannot read SQL LIMIT directly, so "
        "this bounds how many neighbors the GPU returns. The executor then "
        "applies the query's LIMIT. Larger values raise recall and VRAM/latency.",
        &cuvs_k,
        100, 1, 2000,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.max_build_mem_mb",
        "Backend memory limit (MB) for accumulating a CAGRA build corpus.",
        "0 = auto (MemAvailable * cuvs.build_mem_safety_ratio). >0 = hard cap in MB. "
        "CREATE INDEX fails fast with a clear error instead of OOM-killing the backend.",
        &cuvs_max_build_mem_mb,
        0, 0, INT_MAX,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.build_mem_safety_ratio",
        "Fraction of MemAvailable usable for a build corpus when max_build_mem_mb=0 (auto).",
        "Only used in auto mode. Lower is safer against OOM; higher allows larger builds.",
        &cuvs_build_mem_safety_ratio,
        0.5, 0.01, 0.95,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.max_stale_fraction",
        "Deleted-since-build row fraction above which a cagra index reroutes to CPU.",
        "Backstop for delete drift that the .stale marker misses (e.g. when "
        "vacuum_index_cleanup is off or VACUUM's failsafe bypasses ambulkdelete). "
        "Compared at plan time against the .tids build count. 1.0 disables the gate.",
        &cuvs_max_stale_fraction,
        0.10, 0.0, 1.0,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.filter_auto_threshold",
        "Selectivity below which filtered BF uses GPU BITSET prefilter (3O) instead of D-wedge.",
        "Selectivity = |filter| / N. Below this value, the daemon searches only within the "
        "filter set via a GPU BITSET mask (3O), giving recall=1.00 at any selectivity. "
        "Above this value, the D-wedge post-filter (k*4 overfetch) is used. "
        "0.0 = always D-wedge, 1.0 = always 3O. Default 0.05 (confirmed by benchmark).",
        &cuvs_filter_auto_threshold,
        0.05, 0.0, 1.0,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.stream_bf_selectivity_threshold",
        "Selectivity below which filtered BF streams from the .vectors sidecar (out-of-core, ADR-064).",
        "Selectivity = |filter| / N. Below this value the daemon gathers ONLY the "
        "filter-passing vectors from the on-disk .vectors sidecar (never resident "
        "whole in VRAM) and runs chunked GPU brute force — exact, for datasets that "
        "exceed VRAM. Takes precedence over cuvs.filter_auto_threshold (3O) when both "
        "would fire and the sidecar is present. 0.0 = off (default).",
        &cuvs_stream_bf_selectivity_threshold,
        0.0, 0.0, 1.0,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.stream_bf_chunk_vectors",
        "Max vectors gathered to the GPU per chunk in streaming BF (ADR-064).",
        "Footprint knob only: chunk size bounds the per-dispatch GPU buffer but does "
        "NOT affect the result — the running top-k merge is exact for any chunking. "
        "Lower it to cap VRAM use on huge filter sets; raise it to reduce dispatch "
        "count. (RMM/mempool-aware auto-sizing is a tracked ADR-065 follow-up.)",
        &cuvs_stream_bf_chunk_vectors,
        262144, 1, INT_MAX,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.max_delta_rows",
        "Max pending-insert rows merged from the .delta sidecar before CPU reroute.",
        "Phase 3A: INSERT/UPDATE append the new vector to a per-index .delta file "
        "that a query merges (CPU-exact) with the base CAGRA results — no rebuild. "
        "Above this many pending rows the planner reroutes to CPU (REINDEX to "
        "absorb the delta). 0 disables the delta and falls back to mark-stale.",
        &cuvs_max_delta_rows,
        10000, 0, INT_MAX,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomEnumVariable(
        "cuvs.delta_search",
        "Delta search mode for a cagra scan: auto (default), cpu, or gpu.",
        "Controls how pending-delta rows are searched during a cagra scan. "
        "auto: GPU-merge when the daemon can, CPU-exact fallback otherwise. "
        "cpu: always CPU-exact merge, ignore the daemon delta cache. "
        "gpu: GPU-only, no CPU fallback (may miss delta rows).",
        &cuvs_delta_search_mode,
        0,
        cuvs_delta_search_options,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomEnumVariable(
        "cuvs.search_mode",
        "Search algorithm for cagra indexes: cagra (ANN, default) or brute_force (GPU exact).",
        "Phase 3L. brute_force runs an exact GPU k-NN over the index's .vectors "
        "sidecar (recall=1.0) instead of the CAGRA graph; useful for small indexes, "
        "ground-truth generation, or when CAGRA recall is insufficient. Requires the "
        ".vectors sidecar, which CREATE INDEX USING cagra writes automatically.",
        &cuvs_search_mode,
        0,
        cuvs_search_mode_options,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomEnumVariable(
        "cuvs.bf_precision",
        "Numeric precision of the resident GPU brute-force index: float32 (default) or float16.",
        "Phase 3L. float16 halves the brute-force VRAM footprint and raises throughput "
        "at a small recall cost. Only affects cuvs.search_mode='brute_force'. Changing it "
        "rebuilds the resident BF index on the next brute_force search.",
        &cuvs_bf_precision,
        0,
        cuvs_bf_precision_options,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.bf_batch_wait_us",
        "Daemon brute-force micro-batch window in microseconds (0 = disabled).",
        "Phase 3L. When > 0, the daemon coalesces concurrent brute_force search "
        "requests arriving within this window into a single GPU dispatch, raising "
        "throughput for bandwidth-bound BF search. 0 dispatches each request immediately.",
        &cuvs_bf_batch_wait_us,
        0, 0, 10000,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.max_batch_queries",
        "Maximum number of query vectors accepted by pg_cuvs_batch_search in one call.",
        "Phase 3M. Bounds the request/reply shared-memory size (Q*dim*4 in, Q*k*12 out). "
        "A call with more than this many queries raises an error.",
        &cuvs_max_batch_queries,
        1024, 1, 4096,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.shard_count",
        "Number of GPU shards to split a CAGRA index into at build time (Phase 3F).",
        "0 or 1 keeps the legacy single-GPU unsharded index. A value >= 2 splits "
        "the index into N standalone CAGRA shard artifacts over contiguous "
        "build-order ranges; the daemon places shards across GPUs and merges a "
        "global top-k at query time. Read at CREATE INDEX/REINDEX time only.",
        &cuvs_shard_count,
        0, 0, CUVS_SHARDS_MAX,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.shard_overfetch",
        "Extra candidates fetched per shard before the global top-k merge (Phase 3G).",
        "Each shard of a sharded index is searched for k + this many candidates; the "
        "daemon then merges a global top-k. 0 (default) keeps the legacy per-shard k "
        "and is byte-identical to Phase 3F. Raise it to defend recall as shard count "
        "grows. Ignored for unsharded indexes. Read per SEARCH.",
        &cuvs_shard_overfetch,
        0, 0, 4096,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "cuvs.parallel_fanout",
        "Dispatch a sharded index's per-shard searches concurrently in the daemon (Phase 3G).",
        "on (default) fans out to all shards in parallel and merges a global top-k, "
        "cutting per-query latency from the sum of per-shard times toward the max. "
        "off forces the legacy sequential fanout (useful for A/B latency comparison "
        "and as a kill switch). No effect on unsharded indexes. Read per SEARCH.",
        &cuvs_parallel_fanout,
        true,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomBoolVariable(
        "cuvs.cpu_hnsw_fallback",
        "Serve queries from the CPU HNSW sidecar instead of GPU CAGRA (Phase 3I-1).",
        "When on, the daemon loads the .hnsw sidecar built alongside the CAGRA index "
        "and searches it on CPU.  Useful for GPU-less testing or when latency "
        "requirements allow CPU serving.  Default off.",
        &cuvs_cpu_hnsw_fallback,
        false,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.snapshot_uri",
        "GCS root URI for artifact snapshots (Phase 3C).",
        "Format: gs://bucket[/prefix]. Empty string disables GCS upload/download.",
        &cuvs_snapshot_uri,
        "",
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.cluster_id",
        "Cluster identifier used in the GCS artifact path (Phase 3C).",
        "Distinguishes artifacts from multiple pg_cuvs clusters sharing a bucket.",
        &cuvs_cluster_id,
        "",
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.gcs_key_file",
        "Path to GCP service account JSON key file (Phase 3C).",
        "Empty string uses the GCP instance metadata server for authentication.",
        &cuvs_gcs_key_file,
        "",
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.warmup_threads",
        "Number of background warmup threads in the daemon (Phase 3D).",
        "Controls how many GCS downloads can run concurrently during startup "
        "or on-demand cache miss warmup. Passed to daemon via --warmup-threads.",
        &cuvs_warmup_threads,
        2, 1, 8,
        PGC_SUSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.ivfpq_n_probes",
        "IVF clusters probed per query for ivfpq indexes (3P).",
        "Higher values improve recall at the cost of latency. "
        "Must be <= n_lists used at build time.",
        &cuvs_ivfpq_n_probes,
        64, 1, 4096,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.extend_chunk_size",
        "CAGRA extend max_chunk_size (3Q); 0 = auto.",
        NULL,
        &cuvs_extend_chunk_size,
        0, 0, 65536,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.compact_delete_ratio",
        "Fraction of deleted vectors that triggers auto-compact after VACUUM (3Q).",
        "Set to 0 to disable auto-compact.",
        &cuvs_compact_delete_ratio,
        0.10, 0.0, 1.0,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    /* D-wedge spike (ADR-063): Option A custom scan. */
    cuvs_register_custom_scan();
    prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
    set_rel_pathlist_hook = cuvs_filtered_set_rel_pathlist;

    DefineCustomBoolVariable(
        "cuvs.filtered_knn_hook",
        "Enable D-wedge Custom Scan hook (ADR-063 spike, default off).",
        "Intercepts planner path generation for CAGRA-indexed relations with a "
        "WHERE filter, injecting a GPU BF post-filter path for comparison with "
        "the cuvs_filtered_knn() Function API (Option B).",
        &cuvs_filtered_knn_hook_enabled,
        false,
        PGC_USERSET,
        0, NULL, NULL, NULL);

    /* Phase 3G.1: observe DROP INDEX of cagra indexes and notify the daemon to
     * free + unlink artifacts at transaction commit. */
    prev_object_access_hook = object_access_hook;
    object_access_hook = cuvs_object_access;
    RegisterXactCallback(cuvs_xact_callback, NULL);
    RegisterSubXactCallback(cuvs_subxact_callback, NULL);  /* ADR-060 */
    cuvs_ipc_set_wait_callback(cuvs_search_wait_should_abort);  /* 3S: cancelable search */

    /* ADR-057: test seam — CUVS_FORCE_CORPUS=memfd|shm|heap pins the build tier
     * for every backend (inherited from the postmaster env). Unset => auto. Used
     * by the leak-verification harness to exercise the T2 reaper / heap path on
     * a memfd-capable host. */
    cuvs_corpus_force_kind(getenv("CUVS_FORCE_CORPUS"));

    /* ADR-057: at server (re)start, reclaim any T2 build-corpus shm orphans a
     * crashed/killed build left in a previous lifetime. The default memfd tier
     * leaves no /dev/shm name (nothing to reap); this only matters where memfd
     * is unavailable (old kernel / seccomp) and named shm is used. flock-based:
     * only dead-owner segments are unlinked, never a live build's. */
    cuvs_corpus_reap_orphans(1);

    /* Phase 4C: background auto-compaction GUCs */
    DefineCustomBoolVariable(
        "cuvs.auto_compact",
        "Automatically REINDEX CAGRA indexes when extend_count exceeds threshold.",
        NULL,
        &cuvs_auto_compact,
        false,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "cuvs.auto_compact_check_interval",
        "Seconds between auto-compaction checks.",
        NULL,
        &cuvs_auto_compact_check_interval,
        60, 10, 3600,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomRealVariable(
        "cuvs.auto_compact_threshold",
        "Trigger REINDEX when extend_count / n_vecs exceeds this ratio.",
        "Default 0.10 triggers at 10% extended vectors.",
        &cuvs_auto_compact_threshold,
        0.10, 0.01, 1.0,
        PGC_SIGHUP, 0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "cuvs.auto_compact_database",
        "Database to monitor for auto-compaction (empty = disabled).",
        NULL,
        &cuvs_auto_compact_database,
        "",
        PGC_POSTMASTER, 0, NULL, NULL, NULL);

    /* Phase 4C: register compaction bgworker */
    {
        extern PGDLLEXPORT void cuvs_compaction_worker_main(Datum);
        BackgroundWorker worker;
        memset(&worker, 0, sizeof(worker));
        snprintf(worker.bgw_name, BGW_MAXLEN, "pg_cuvs compaction worker");
        snprintf(worker.bgw_type, BGW_MAXLEN, "pg_cuvs compaction");
        worker.bgw_flags      = BGWORKER_SHMEM_ACCESS |
                                BGWORKER_BACKEND_DATABASE_CONNECTION;
        worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
        worker.bgw_restart_time = 10;
        snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_cuvs");
        snprintf(worker.bgw_function_name, BGW_MAXLEN,
                 "cuvs_compaction_worker_main");
        RegisterBackgroundWorker(&worker);
    }
}

/* Resolve cuvs.index_dir: if empty, default to DataDir/cuvs_indexes */
static const char *
get_index_dir(void)
{
    static char buf[MAXPGPATH];

    if (cuvs_index_dir && cuvs_index_dir[0] != '\0')
        return cuvs_index_dir;

    snprintf(buf, sizeof(buf), "%s/cuvs_indexes", DataDir);
    return buf;
}

/* ADR-045 directory resolution. Precedence: (1) the index's index_dir
 * reloption, (2) the cuvs.index_dir session GUC, (3) $PGDATA/cuvs_indexes.
 * (2)+(3) are get_index_dir(); the reloption is the catalog-durable override
 * that survives a backend not having SET the GUC. */

/* Read the index_dir reloption from an open index relation's parsed rd_options.
 * Returns the stored directory, or NULL when the option is absent. The pointer
 * is valid only while indexRel is open. */
static const char *
cuvs_reloption_index_dir(Relation indexRel)
{
    CuvsCagraOptions *opts = (CuvsCagraOptions *) indexRel->rd_options;

    if (opts != NULL && opts->index_dir_offset != 0)
    {
        const char *dir = (const char *) opts + opts->index_dir_offset;

        if (dir[0] != '\0')
            return dir;
    }
    return NULL;
}

/* Resolver for callers holding an open index Relation (build/insert/bulkdelete):
 * reads rd_options directly, so no relcache open. */
static const char *
cuvs_resolve_index_dir_rel(Relation indexRel)
{
    const char *reldir = cuvs_reloption_index_dir(indexRel);

    return reldir != NULL ? reldir : get_index_dir();
}

/* 3R: resolve CAGRA build params from a cagra index's reloptions. ints carry
 * their reloption default (64/128) when rd_options is set; defaults are applied
 * when the index has no reloptions at all. build_algo string -> CUVS_CAGRA_BUILD_*.
 * Validates intermediate_graph_degree >= graph_degree (a cuVS requirement).
 * Only called from the CAGRA ambuild path (indexRel is a cagra index). */
static void
cuvs_resolve_build_params_rel(Relation indexRel, int *graph_degree,
                              int *intermediate_graph_degree, uint32_t *build_algo)
{
    CuvsCagraOptions *opts = (CuvsCagraOptions *) indexRel->rd_options;

    *graph_degree              = 64;
    *intermediate_graph_degree = 128;
    *build_algo                = CUVS_CAGRA_BUILD_AUTO;

    if (opts != NULL)
    {
        *graph_degree              = opts->graph_degree;
        *intermediate_graph_degree = opts->intermediate_graph_degree;
        if (opts->build_algo_offset != 0)
        {
            const char *s = (const char *) opts + opts->build_algo_offset;
            if (strcmp(s, "ivf_pq") == 0)
                *build_algo = CUVS_CAGRA_BUILD_IVF_PQ;
            else if (strcmp(s, "nn_descent") == 0)
                *build_algo = CUVS_CAGRA_BUILD_NN_DESCENT;
            /* else "auto"/"" -> AUTO */
        }
    }

    if (*intermediate_graph_degree < *graph_degree)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("pg_cuvs: intermediate_graph_degree (%d) must be >= "
                        "graph_degree (%d)",
                        *intermediate_graph_degree, *graph_degree)));
}

/* Resolver for the plan-time gates and shared path builders, which hold only an
 * Oid. Opens the index relcache entry WITHOUT a lock: by cost-estimation time
 * the planner already holds the lock transitively (get_relation_info opened the
 * same entry), so this is a refcount-only relcache lookup — no IPC, no CUDA, no
 * lock-manager traffic — far below the stat()/fread() the gates already perform,
 * and negligible against the per-row file I/O on the aminsert/ambulkdelete delta
 * paths. try_index_open returns NULL if the index was concurrently dropped, in
 * which case we fall back to the GUC/DataDir path. The resolved string is copied
 * into a static buffer before close so no rd_options pointer escapes. */
static const char *
cuvs_resolve_index_dir(Oid index_oid)
{
    static char buf[MAXPGPATH];
    Relation    rel = try_index_open(index_oid, NoLock);
    bool        have_reldir = false;

    if (rel != NULL)
    {
        const char *reldir = cuvs_reloption_index_dir(rel);

        if (reldir != NULL)
        {
            strlcpy(buf, reldir, sizeof(buf));
            have_reldir = true;
        }
        index_close(rel, NoLock);
    }
    return have_reldir ? buf : get_index_dir();
}

/* Plan-time stale check. The daemon writes a "<db>_<idx>.stale" sidecar when a
 * heap write marks the index stale (pg_cuvs_server.c stale_file_path); we read
 * it here so the planner can route around a stale CAGRA index. Pure stat() — no
 * IPC or CUDA, so it is safe in the cost path. Naming MUST match the daemon. */
static bool
cuvs_index_is_stale(Oid index_oid)
{
    char        path[MAXPGPATH];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%u_%u.stale",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
    return stat(path, &st) == 0;
}

/* Plan-time delete-drift gate. The `.stale` marker is set by ambulkdelete, but
 * VACUUM can skip ambulkdelete entirely (vacuum_index_cleanup=off, the wraparound
 * failsafe, or the <2%-dead-pages bypass), leaving a stale graph unmarked and
 * silently eroding recall. As a backstop we read the build-time corpus size from
 * the `.tids` header (32-byte fixed header, no IPC/CUDA) and compare it to the
 * planner's current live-row estimate; if the deleted fraction exceeds
 * cuvs.max_stale_fraction, treat the index as stale. Inserts (live > build) are
 * already caught per-row by aminsert, so only deletions matter here. */
static bool
cuvs_index_delete_drift_stale(Oid index_oid, double live_rows)
{
    char            path[MAXPGPATH];
    FILE           *f;
    CuvsTidsHeader  hdr;
    size_t          got;

    if (cuvs_max_stale_fraction >= 1.0 || live_rows < 0.0)
        return false;   /* gate disabled, or live count unknown */

    snprintf(path, sizeof(path), "%s/%u_%u.tids",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
    f = AllocateFile(path, PG_BINARY_R);
    if (f == NULL)
        return false;   /* no persisted artifact -> other gates/paths handle it */
    got = fread(&hdr, 1, sizeof(hdr), f);
    FreeFile(f);

    if (got != sizeof(hdr) || hdr.magic != CUVS_TIDS_MAGIC
        || hdr.version != CUVS_TIDS_VERSION || hdr.n_vecs <= 0)
        return false;
    if (live_rows >= (double) hdr.n_vecs)
        return false;   /* no net deletions vs build */

    return ((double) hdr.n_vecs - live_rows) / (double) hdr.n_vecs
           > cuvs_max_stale_fraction;
}

/* Plan-time daemon availability gate. The daemon creates its UDS socket only
 * after full startup (warmup + index loading). A missing socket means the
 * daemon is not yet ready or was cleanly shut down; silently raise cost so the
 * planner routes to seqscan/pgvector instead of a GPU index that cannot serve.
 *
 * A crash leaves the socket file in place (stale socket). That case is handled
 * at runtime: UNAVAILABLE → ERROR + circuit-breaker record. After N consecutive
 * errors the breaker opens and this plan-time gate fires on the next query too. */
static bool
cuvs_daemon_socket_ready(void)
{
    struct stat st;

    if (cuvs_socket_path == NULL || cuvs_socket_path[0] == '\0')
        return false;
    return stat(cuvs_socket_path, &st) == 0;
}

/* Plan-time artifact-existence gate. The daemon writes a .tids file when it
 * completes a build. An index created on an empty table (ambuildempty path)
 * skips the daemon entirely, so no .tids exists. Without this gate, a query on
 * such an index would reach amgettuple, get CUVS_STATUS_NOT_FOUND from the
 * daemon, and ERROR. Instead, silence it at plan time: no .tids → no GPU
 * artifact → route to seqscan/CPU. */
static bool
cuvs_index_has_artifact(Oid index_oid)
{
    char        path[MAXPGPATH];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%u_%u.tids",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
    return stat(path, &st) == 0;
}

/* ----------------------------------------------------------------
 * Phase 3A pending-delta sidecar (CPU MVP)
 *
 * INSERT/UPDATE append the new vector to "<db>_<idx>.delta" instead of marking
 * the index stale, so the daemon keeps serving the base CAGRA and the backend
 * merges base + CPU-exact delta candidates at scan time. The delta is tied to
 * its base build via base_tids_crc32 (the .tids body CRC); a REINDEX rewrites
 * the base and invalidates a leftover delta. All reads here are local file I/O
 * (no IPC/CUDA), safe in the planner.
 * ---------------------------------------------------------------- */

/* Build "<index_dir>/<db>_<idx>.delta". */
static void
cuvs_delta_path(Oid index_oid, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s/%u_%u.delta",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
}

/* Read the base .tids body_crc32 (generation token). Returns true + *crc_out on
 * success; false if the .tids artifact is missing or its header unreadable. */
static bool
cuvs_read_tids_crc(Oid index_oid, uint32_t *crc_out)
{
    char            path[MAXPGPATH];
    FILE           *f;
    CuvsTidsHeader  hdr;
    size_t          got;

    snprintf(path, sizeof(path), "%s/%u_%u.tids",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
    f = AllocateFile(path, PG_BINARY_R);
    if (f == NULL)
        return false;
    got = fread(&hdr, 1, sizeof(hdr), f);
    FreeFile(f);
    if (got != sizeof(hdr) || hdr.magic != CUVS_TIDS_MAGIC
        || hdr.version != CUVS_TIDS_VERSION)
        return false;
    *crc_out = hdr.body_crc32;
    return true;
}

/* Plan-time gate: a .delta that exists but cannot be merged forces CPU reroute.
 * Unusable = corrupt header, body-size mismatch (truncation), n_rows over
 * cuvs.max_delta_rows, or generation mismatch vs the current base .tids. A
 * missing .delta is usable (base-only path). Pure file reads. */
static bool
cuvs_index_delta_unusable(Oid index_oid)
{
    char            path[MAXPGPATH];
    FILE           *f;
    CuvsDeltaHeader hdr;
    long            fsize;
    uint32_t        base_crc;

    cuvs_delta_path(index_oid, path, sizeof(path));
    f = AllocateFile(path, PG_BINARY_R);
    if (f == NULL)
        return false;   /* no delta -> base-only path is fine */

    if (cuvs_delta_read_header(f, &hdr) != 0
        || fseek(f, 0, SEEK_END) != 0
        || (fsize = ftell(f)) < (long) sizeof(hdr))
    {
        FreeFile(f);
        return true;    /* corrupt / unreadable */
    }
    FreeFile(f);

    if (cuvs_delta_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0)
        return true;    /* truncated / oversized body */
    if (hdr.n_rows > (int64_t) cuvs_max_delta_rows)
        return true;    /* too big to merge -> CPU reroute (REINDEX) */
    if (!cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32)
        return true;    /* delta belongs to a previous base build */
    return false;
}

/* Remove the .delta sidecar (best-effort; called after a successful build). */
static void
cuvs_delta_unlink(Oid index_oid)
{
    char path[MAXPGPATH];

    cuvs_delta_path(index_oid, path, sizeof(path));
    (void) unlink(path);
}

/* ----------------------------------------------------------------
 * Phase 3A-4 tombstone sidecar
 *
 * Records dead TIDs from ambulkdelete so base CAGRA results can be
 * filtered before merge, avoiding wasted k-slots on dead tuples.
 * ---------------------------------------------------------------- */
static void
cuvs_tombstone_path(Oid index_oid, char *buf, size_t buflen)
{
    snprintf(buf, buflen, "%s/%u_%u.tombstone",
             cuvs_resolve_index_dir(index_oid), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
}

static void
cuvs_tombstone_unlink(Oid index_oid)
{
    char path[MAXPGPATH];
    cuvs_tombstone_path(index_oid, path, sizeof(path));
    (void) unlink(path);
}

static bool
cuvs_index_tombstone_unusable(Oid index_oid)
{
    char                 path[MAXPGPATH];
    FILE                *f;
    CuvsTombstoneHeader  hdr;
    long                 fsize;
    uint32_t             base_crc;

    cuvs_tombstone_path(index_oid, path, sizeof(path));
    f = AllocateFile(path, PG_BINARY_R);
    if (f == NULL)
        return false;   /* no tombstones -> fine */

    if (cuvs_tombstone_read_header(f, &hdr) != 0
        || fseek(f, 0, SEEK_END) != 0
        || (fsize = ftell(f)) < (long) sizeof(hdr))
    {
        FreeFile(f);
        return true;
    }
    FreeFile(f);

    if (cuvs_tombstone_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0)
        return true;
    if (hdr.n_entries > (int64_t) cuvs_max_delta_rows)
        return true;
    if (!cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32)
        return true;
    return false;
}

/* Phase 3A recall: the number of pending dead TIDs the .tombstone records for
 * the current base build, or 0 if there is no usable tombstone (missing,
 * corrupt, or belonging to a previous build). A query over-fetches the base by
 * this count so dead-TID filtering cannot starve the live top-k. Pure file
 * reads; mirrors cuvs_index_tombstone_unusable but returns the entry count. */
static int64_t
cuvs_tombstone_count(Oid index_oid)
{
    char                 path[MAXPGPATH];
    FILE                *f;
    CuvsTombstoneHeader  hdr;
    long                 fsize;
    uint32_t             base_crc;

    cuvs_tombstone_path(index_oid, path, sizeof(path));
    f = AllocateFile(path, PG_BINARY_R);
    if (f == NULL)
        return 0;

    if (cuvs_tombstone_read_header(f, &hdr) != 0
        || fseek(f, 0, SEEK_END) != 0
        || (fsize = ftell(f)) < (long) sizeof(hdr))
    {
        FreeFile(f);
        return 0;
    }
    FreeFile(f);

    if (cuvs_tombstone_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0)
        return 0;
    if (!cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32)
        return 0;       /* belongs to a previous base build */
    return hdr.n_entries;
}

/* pread/pwrite full-length wrappers (retry partials and EINTR). 0 on success. */
static int
cuvs_pwrite_all(int fd, off_t off, const void *buf, size_t len)
{
    const char *p = (const char *) buf;

    while (len > 0)
    {
        ssize_t w = pwrite(fd, p, len, off);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            return -1;
        p += w; off += w; len -= (size_t) w;
    }
    return 0;
}

static int
cuvs_pread_all(int fd, off_t off, void *buf, size_t len)
{
    char *p = (char *) buf;

    while (len > 0)
    {
        ssize_t r = pread(fd, p, len, off);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            return -1;   /* short file */
        p += r; off += r; len -= (size_t) r;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * Cost model (ADR-028)
 *
 * Calibrated against pilot benchmarks (bench/results/pilot.csv), dim=384, k=10:
 *
 *   N=1K:    CAGRA p50= 871us,  seqscan ~  100us  -> seqscan wins (correct)
 *   N=10K:   CAGRA p50=1146us,  seqscan ~  800us  -> GPU ~tied  (acceptable)
 *   N=100K:  CAGRA p50=1228us,  seqscan ~ 7700us  -> GPU 6.3x  [OK]
 *   N=1M:    CAGRA p50=1196us,  seqscan ~77000us  -> GPU 64x   [OK]
 *
 * Planner crossover (dim=384, seq_page_cost=1.0):
 *   seqscan_cost ~ N * (16 + dim*4) / 8192  ->  N * 0.189 at dim=384
 *   CAGRA_cost   = STARTUP + K_COST*k + ROWS_COST*N
 *   Crossover N  ~ STARTUP / 0.189  ~  5300 rows
 *
 * STARTUP_COST=1000 models the cold-path CUDA context initialisation
 * (~100ms on first query after a backend start).  Warm-path IPC round-trip
 * is ~0.5ms but the planner cannot distinguish warm vs cold.  The
 * conservative 1000 also ensures seqscan wins below ~5K rows, consistent
 * with the pilot: CAGRA p50=871us at N=1K where seqscan is ~100us.
 *
 * ROWS_COST=0.00001 reflects CAGRA's near-N-independence: p50 grows from
 * 871us (N=1K) to 1196us (N=1M) -- only 1.37x for a 1000x row increase.
 * The non-zero term lets seqscan compete at N->0 without a hard cutoff.
 *
 * dim scaling is implicit: wider rows produce more pages, so seqscan cost
 * grows automatically; CAGRA p50 rises only mildly (1228us at dim=384 ->
 * 1605us at dim=1536 for N=100K, 1.31x), meaning GPU advantage widens
 * at higher dimensions without any explicit dim term.
 * ---------------------------------------------------------------- */
#define CUVS_STARTUP_COST      1000.0
/* CAGRA returns k rows regardless of N; the k-term keeps this cost above
 * a pure-selectivity estimate and below seqscan for any practical k. */
#define CUVS_K_COST            0.5
/* Tiny N-term: CAGRA is ~N-independent but must not beat seqscan at N->0. */
#define CUVS_ROWS_COST         0.00001

/* PG16 amcostestimate is a direct C function pointer, not a SQL function.
 *
 * IMPORTANT: This runs in the planner on every query — once per candidate
 * index path. It must NOT touch the CUDA runtime: cudaGetDeviceCount() etc.
 * lazily initialize the CUDA context per backend, which costs ~100ms the
 * first time and inflates Planning Time. Six plan-time gates short-circuit the
 * GPU route without IPC or CUDA: enable_cuvs (GUC), circuit breaker (repeated
 * runtime failures), .stale sidecar (heap writes since build), delete-drift
 * (.tids header vs live-row estimate), socket existence (daemon ready), and
 * artifact existence (no .tids = empty-table build, no GPU artifact). */
static void
cuvsamcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                   Cost *indexStartupCost, Cost *indexTotalCost,
                   Selectivity *indexSelectivity, double *indexCorrelation,
                   double *indexPages)
{
    double rows = path->path.rows;
    Oid    index_oid = path->indexinfo->indexoid;
    bool   gpu_off;

    (void) root;
    (void) loop_count;

    /* Eight gates turn the GPU path off — all local file/state reads, no IPC.
     * Cost is set to 1e15 (> PG disable_cost 1e10) so the gated index loses
     * even when enable_seqscan = off, preventing empty-result or ERROR paths.
     *  1. enable_cuvs GUC
     *  2. circuit breaker (repeated runtime failures → breaker open)
     *  3. .stale sidecar (heap write marked index stale since build)
     *  4. delete-drift (.tids build count vs planner live-row estimate)
     *  5. socket existence (missing → daemon cleanly stopped or not yet started)
     *  6. artifact existence (no .tids → index was built on empty table)
     *  7. delta unusable (.delta corrupt / generation mismatch / over cap)
     *  8. tombstone unusable (.tombstone corrupt / over cap / gen mismatch) */
    gpu_off = !enable_cuvs
           || cuvs_circuit_is_open((uint32_t) index_oid)
           || cuvs_index_is_stale(index_oid)
           || cuvs_index_delete_drift_stale(index_oid, path->indexinfo->rel->tuples)
           || !cuvs_daemon_socket_ready()
           || !cuvs_index_has_artifact(index_oid)
           || cuvs_index_delta_unusable(index_oid)
           || cuvs_index_tombstone_unusable(index_oid);

    if (gpu_off)
    {
        *indexStartupCost = 1e15;
        *indexTotalCost   = 1e15;
    }
    else if (cuvs_search_mode == 1)
    {
        /* Phase 3L (ADR-039): brute_force is exact GPU k-NN — latency is
         * bandwidth-bound in the corpus size N, not in cuvs.k. Model it as a
         * small N-scaled cost with no per-k term; still far below a disabled
         * seqscan so the GPU path is chosen whenever the user opts into
         * brute_force. A missing .vectors sidecar is NOT gated here: it surfaces
         * as a clear ERROR at execution, not a silent CPU fallback. */
        double n = path->indexinfo->rel->tuples;
        if (n < 1) n = 1;
        *indexStartupCost = CUVS_STARTUP_COST;
        *indexTotalCost   = CUVS_STARTUP_COST + CUVS_ROWS_COST * n;
    }
    else
    {
        *indexStartupCost = CUVS_STARTUP_COST;
        *indexTotalCost   = CUVS_STARTUP_COST
                          + CUVS_K_COST    * (double) cuvs_k
                          + CUVS_ROWS_COST * rows;
    }

    if (cuvs_debug)
        ereport(DEBUG1,
                (errmsg("pg_cuvs: costest oid=%u rows=%.0f k=%d gpu_off=%d total=%.1f",
                        (uint32_t) index_oid, rows, cuvs_k, (int) gpu_off,
                        *indexTotalCost)));

    *indexSelectivity = 1.0;
    *indexCorrelation = 0.0;
    *indexPages       = 0.0;
}

/* ----------------------------------------------------------------
 * Determine a cagra index's metric from its operator class.
 *
 * The three opclasses (vector_l2_ops / vector_cosine_ops / vector_ip_ops)
 * each create their own operator family. We resolve those three opfamily
 * OIDs once (by name, under the cagra AM) and compare the index's opfamily.
 * Used identically at build and scan so the two always agree. An unknown
 * family falls back to L2 with a one-time WARNING.
 * ---------------------------------------------------------------- */
static uint32_t
cuvs_index_metric(Relation indexRel)
{
    /* Resolve by opclass-family NAME (vector_l2_ops / vector_cosine_ops /
     * vector_ip_ops). pgvector names the family after the opclass, and both
     * the cagra and pg_cuvs_hnsw AMs reuse those same names, so this is AM-
     * agnostic and build/scan always agree. (The previous opfamily-OID cache
     * was AM-specific and mis-mapped when both AMs were used in one session.) */
    Oid       idxopf = indexRel->rd_opfamily[0];
    int       m = -1;
    HeapTuple oftup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(idxopf));
    if (HeapTupleIsValid(oftup))
    {
        const char *opfname =
            NameStr(((Form_pg_opfamily) GETSTRUCT(oftup))->opfname);
        m = cuvs_metric_from_opclass_name(opfname);
        ReleaseSysCache(oftup);
    }

    if (m >= 0)
        return (uint32_t) m;

    ereport(WARNING,
            (errmsg("pg_cuvs: unrecognized opclass family for index %u; assuming L2",
                    RelationGetRelid(indexRel))));
    return CUVS_METRIC_L2;
}

/* ----------------------------------------------------------------
 * Build memory cap
 *
 * cuvs_ambuild accumulates the whole corpus in backend memory before handing
 * it to the daemon. On a large table that can OOM-kill the backend. The cap
 * lets a build fail fast with a clear error instead. See plan Step 5.
 * ---------------------------------------------------------------- */
static size_t
read_mem_available_bytes(void)
{
    FILE  *f = fopen("/proc/meminfo", "r");
    char   line[256];
    size_t kb = 0;

    if (!f)
        return 0;   /* non-Linux or unreadable: caller treats as "no limit" */
    while (fgets(line, sizeof(line), f))
    {
        if (sscanf(line, "MemAvailable: %zu kB", &kb) == 1)
            break;
    }
    fclose(f);
    return kb * (size_t) 1024;
}

/* Effective build-memory cap in bytes. 0 means "no limit" (hard cap unset and
 * MemAvailable unknown). */
static size_t
cuvs_effective_build_cap_bytes(void)
{
    if (cuvs_max_build_mem_mb > 0)
        return (size_t) cuvs_max_build_mem_mb * 1024 * 1024;   /* operator hard cap */

    /* auto: a fraction of currently-available host memory */
    size_t avail = read_mem_available_bytes();
    if (avail == 0)
        return 0;
    return (size_t) ((double) avail * cuvs_build_mem_safety_ratio);
}

/* ----------------------------------------------------------------
 * Build state for CREATE INDEX scan
 * ---------------------------------------------------------------- */
typedef struct CuvsBuildState {
    int      dim;
    int64_t  n_vecs;
    int64_t  n_allocated;
    float   *vectors;     /* [n_allocated][dim]: corpus.base for memfd/shm, else malloc'd */
    uint64_t *tids;       /* malloc'd: [n_allocated] — always heap (8B/row, tiny) */
    uint32_t metric;      /* CUVS_METRIC_* */
    double   reltuples;
    size_t   cap_bytes;   /* build memory limit (0 = none); see Step 5 */
    /* ADR-057: vectors accumulate in a leak-safe corpus (memfd -> shm -> heap).
     * For memfd/shm tiers, `vectors` aliases corpus.base; for the heap tier the
     * corpus is CORPUS_HEAP and `vectors` is a plain malloc'd buffer. */
    CuvsBuildCorpus corpus;
} CuvsBuildState;

static void
grow_build_buffers(CuvsBuildState *bs)
{
    int64_t new_size = bs->n_allocated * 2;
    if (new_size < 64)
        new_size = 64;

    /* Runtime guard: catch tables whose preflight estimate was unavailable
     * (never ANALYZEd) or too low. On ERROR the caller's PG_FINALLY reclaims the
     * corpus + heap buffers, so we must not free here (would double-free). */
    if (bs->cap_bytes > 0)
    {
        size_t projected = (size_t) new_size * bs->dim * sizeof(float)
                         + (size_t) new_size * sizeof(uint64_t);
        if (projected > bs->cap_bytes)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_cuvs: build corpus exceeds the build memory limit (%zu MB)",
                            bs->cap_bytes / (1024 * 1024)),
                     errhint("Raise cuvs.max_build_mem_mb (hard cap) or "
                             "cuvs.build_mem_safety_ratio, shard the table, or see "
                             "docs/playbooks/large-dataset-benchmark.md.")));
    }

    /* Vectors: grow the corpus in place (memfd/shm: ftruncate + remap; heap:
     * realloc). TIDs are always a small heap buffer either way. */
    if (bs->corpus.kind == CORPUS_HEAP)
    {
        bs->vectors = realloc(bs->vectors, (size_t)new_size * bs->dim * sizeof(float));
        if (!bs->vectors)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("pg_cuvs: out of memory accumulating index vectors")));
    }
    else
    {
        if (cuvs_corpus_resize(&bs->corpus, (size_t)new_size * bs->dim * sizeof(float)) != 0)
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("pg_cuvs: failed to grow build corpus: %m")));
        bs->vectors = (float *) bs->corpus.base;
    }

    bs->tids = realloc(bs->tids, (size_t)new_size * sizeof(uint64_t));
    if (!bs->tids)
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("pg_cuvs: out of memory accumulating index TIDs")));

    bs->n_allocated = new_size;
}

/* Callback invoked for each heap tuple during CREATE INDEX scan */
static void
cuvs_build_callback(Relation index,
                    ItemPointer tid,
                    Datum *values,
                    bool  *isnull,
                    bool   tupleIsAlive,
                    void  *state)
{
    CuvsBuildState *bs = (CuvsBuildState *)state;

    if (!tupleIsAlive || isnull[0])
        return;

    PgVector *vec = DatumGetPgVector(values[0]);

    if (bs->dim == 0)
        bs->dim = (int)vec->dim;
    else if ((int)vec->dim != bs->dim)
        return;  /* dimension mismatch — skip */

    if (bs->n_vecs >= bs->n_allocated)
        grow_build_buffers(bs);

    /* Copy vector floats into flat buffer */
    float *dst = bs->vectors + (bs->n_vecs * bs->dim);
    memcpy(dst, vec->x, (size_t)bs->dim * sizeof(float));

    /* Encode TID as block<<16|offset */
    bs->tids[bs->n_vecs] =
        cuvs_tid_encode(ItemPointerGetBlockNumber(tid),
                        ItemPointerGetOffsetNumber(tid));

    bs->n_vecs++;
}

/* ================================================================
 * ADR-034 §4A-2: parallel CAGRA build.
 *
 * Each participant (leader + workers) scans a DISJOINT share of the heap via a
 * shared ParallelTableScanDesc into its own named-shm (T2) partial corpus
 * ([vectors][tids]). After the workers finish, the leader concatenates all
 * partials into a single memfd corpus (ADR-057) and hands it to the daemon.
 *
 * Correctness: CAGRA is order-independent and the daemon shards the corpus by
 * POSITION with positional (vectors[i], tids[i]) pairing — so a plain concat
 * (no sort) of per-worker partials, which preserves each pair, is equivalent to
 * the single-process corpus. max_parallel_maintenance_workers = 0 keeps the
 * single-process path (byte-identical).
 * ================================================================ */
#define PCUVS_KEY_SHARED   UINT64CONST(0xC5005001)
#define PCUVS_KEY_SCAN     UINT64CONST(0xC5005002)

typedef struct CuvsPartialSlot {
    int64   n_vecs;          /* vectors this participant produced (0 = empty share) */
    double  reltuples;       /* heap tuples it scanned (for pg_class stats) */
    char    shm_name[64];    /* named-shm holding its [vecs][tids]; "" if empty */
} CuvsPartialSlot;

typedef struct ParallelCuvsShared {
    Oid     heap_oid;
    Oid     index_oid;
    int     dim;
    uint32  metric;
    Size    cap_bytes;
    int     nparticipants;   /* nworkers + 1 (leader slot is slots[nparticipants-1]) */
    CuvsPartialSlot slots[FLEXIBLE_ARRAY_MEMBER];
} ParallelCuvsShared;

/* Scan a (parallel) share into a named-shm partial; record (n_vecs, reltuples,
 * shm_name) into *slot. The segment is detached by name (the leader opens +
 * unlinks it at merge). On error or empty share, no live segment is left. */
static void
cuvs_scan_shm_partial(Relation heapRel, Relation indexRel, IndexInfo *indexInfo,
                      ParallelTableScanDesc pscan, int dim, uint32 metric,
                      Size cap_bytes, CuvsPartialSlot *slot)
{
    CuvsBuildState bs;

    slot->n_vecs = 0;
    slot->reltuples = 0;
    slot->shm_name[0] = '\0';

    memset(&bs, 0, sizeof(bs));
    bs.metric = metric;
    bs.cap_bytes = cap_bytes;
    bs.dim = dim;
    bs.corpus.kind = CORPUS_HEAP;
    bs.corpus.fd = -1;

    /* The partial must be openable by name from the leader -> force the shm tier
     * for this open only (restored immediately; corpus_open never longjmps). */
    cuvs_corpus_force_kind("shm");
    if (cuvs_corpus_open(&bs.corpus, (Size) 64 * dim * sizeof(float)) != 0
        || bs.corpus.kind != CORPUS_SHM)
    {
        cuvs_corpus_force_kind(NULL);
        if (bs.corpus.kind != CORPUS_NONE)
            cuvs_corpus_close(&bs.corpus);
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("pg_cuvs: parallel build: shm partial unavailable: %m")));
    }
    cuvs_corpus_force_kind(NULL);
    bs.vectors = (float *) bs.corpus.base;
    bs.tids = (uint64_t *) malloc((Size) 64 * sizeof(uint64_t));
    if (bs.tids == NULL)
    {
        cuvs_corpus_close(&bs.corpus);
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                        errmsg("pg_cuvs: out of memory (parallel TID buffer)")));
    }
    bs.n_allocated = 64;

    PG_TRY();
    {
        /* table_index_build_scan needs a TableScanDesc; derive one from the
         * shared parallel scan (it ends the scan internally). */
        TableScanDesc scan = table_beginscan_parallel(heapRel, pscan);
        bs.reltuples = table_index_build_scan(heapRel, indexRel, indexInfo,
                                              true, true, cuvs_build_callback, &bs, scan);
        slot->reltuples = bs.reltuples;
        if (bs.n_vecs > 0)
        {
            Size vb = (Size) bs.n_vecs * dim * sizeof(float);
            Size tb = (Size) bs.n_vecs * sizeof(uint64_t);
            if (cuvs_corpus_resize(&bs.corpus, vb + tb) != 0)
                ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY),
                                errmsg("pg_cuvs: parallel partial finalize: %m")));
            memcpy((char *) bs.corpus.base + vb, bs.tids, tb);
            slot->n_vecs = bs.n_vecs;
            cuvs_corpus_detach(&bs.corpus, slot->shm_name, sizeof(slot->shm_name));
        }
        else
        {
            cuvs_corpus_close(&bs.corpus);   /* empty share: nothing to hand off */
        }
    }
    PG_FINALLY();
    {
        if (bs.tids)
            free(bs.tids);
        /* On error before detach, the segment is still ours -> unlink it. */
        if (bs.corpus.kind == CORPUS_SHM)
            cuvs_corpus_close(&bs.corpus);
    }
    PG_END_TRY();
}

/* Parallel worker entry point (resolved by ParallelContext via library+name). */
PGDLLEXPORT void cuvs_build_worker_main(dsm_segment *seg, shm_toc *toc);
void
cuvs_build_worker_main(dsm_segment *seg, shm_toc *toc)
{
    ParallelCuvsShared    *shared = shm_toc_lookup(toc, PCUVS_KEY_SHARED, false);
    ParallelTableScanDesc  pscan  = shm_toc_lookup(toc, PCUVS_KEY_SCAN, false);
    Relation   heapRel  = table_open(shared->heap_oid, ShareLock);
    Relation   indexRel = index_open(shared->index_oid, AccessExclusiveLock);
    IndexInfo *indexInfo = BuildIndexInfo(indexRel);

    (void) seg;
    cuvs_scan_shm_partial(heapRel, indexRel, indexInfo, pscan,
                          shared->dim, shared->metric, shared->cap_bytes,
                          &shared->slots[ParallelWorkerNumber]);

    index_close(indexRel, AccessExclusiveLock);
    table_close(heapRel, ShareLock);
}

/* Leader: run the build with `request_workers` parallel workers. Returns true if
 * it handled the build (caller returns), false to fall back to single-process
 * (e.g. DSM unavailable). Sets *out_n_vecs / *out_reltuples. */
static bool
cuvs_build_parallel(Relation heapRel, Relation indexRel, IndexInfo *indexInfo,
                    uint32_t build_index_oid, uint32_t shard_count, bool use_cpu_hnsw,
                    int dim, uint32 metric, Size cap_bytes, int request_workers,
                    int graph_degree, int intermediate_graph_degree, uint32_t build_algo,
                    int64_t *out_n_vecs, double *out_reltuples)
{
    ParallelContext       *pcxt;
    ParallelCuvsShared    *shared;
    ParallelTableScanDesc  pscan;
    Size       scan_sz, shared_sz;
    int        nparticipants = request_workers + 1;   /* leader participates */
    int        i;
    CuvsPartialSlot *slots;
    int64      total = 0;
    double     reltuples = 0;
    int        rc = CUVS_STATUS_OK;

    EnterParallelMode();
    pcxt = CreateParallelContext("pg_cuvs", "cuvs_build_worker_main", request_workers);

    scan_sz   = table_parallelscan_estimate(heapRel, SnapshotAny);
    shared_sz = offsetof(ParallelCuvsShared, slots)
              + (Size) nparticipants * sizeof(CuvsPartialSlot);
    shm_toc_estimate_chunk(&pcxt->estimator, shared_sz);
    shm_toc_estimate_chunk(&pcxt->estimator, scan_sz);
    shm_toc_estimate_keys(&pcxt->estimator, 2);

    InitializeParallelDSM(pcxt);
    if (pcxt->seg == NULL)
    {
        /* no DSM available -> caller falls back to single-process */
        DestroyParallelContext(pcxt);
        ExitParallelMode();
        return false;
    }

    shared = shm_toc_allocate(pcxt->toc, shared_sz);
    shared->heap_oid  = RelationGetRelid(heapRel);
    shared->index_oid = RelationGetRelid(indexRel);
    shared->dim       = dim;
    shared->metric    = metric;
    shared->cap_bytes = cap_bytes;
    shared->nparticipants = nparticipants;
    for (i = 0; i < nparticipants; i++)
    {
        shared->slots[i].n_vecs = 0;
        shared->slots[i].reltuples = 0;
        shared->slots[i].shm_name[0] = '\0';
    }
    shm_toc_insert(pcxt->toc, PCUVS_KEY_SHARED, shared);

    pscan = shm_toc_allocate(pcxt->toc, scan_sz);
    table_parallelscan_initialize(heapRel, pscan, SnapshotAny);
    shm_toc_insert(pcxt->toc, PCUVS_KEY_SCAN, pscan);

    LaunchParallelWorkers(pcxt);

    /* Leader scans its own share into the last slot. */
    cuvs_scan_shm_partial(heapRel, indexRel, indexInfo, pscan, dim, metric, cap_bytes,
                          &shared->slots[nparticipants - 1]);

    WaitForParallelWorkersToFinish(pcxt);

    /* Copy results out of DSM (freed by DestroyParallelContext), then leave
     * parallel mode before the daemon handoff. The partials persist by name. */
    slots = (CuvsPartialSlot *) palloc(nparticipants * sizeof(CuvsPartialSlot));
    memcpy(slots, shared->slots, nparticipants * sizeof(CuvsPartialSlot));
    DestroyParallelContext(pcxt);
    ExitParallelMode();

    for (i = 0; i < nparticipants; i++)
    {
        total     += slots[i].n_vecs;
        reltuples += slots[i].reltuples;
    }
    *out_n_vecs = total;
    if (out_reltuples) *out_reltuples = reltuples;

    if (total == 0)
    {
        pfree(slots);
        return true;   /* empty table — nothing to build */
    }

    /* ADR-059: hand the worker partials to the daemon directly — no leader
     * merge copy. The daemon shm_open's each named-shm partial and streams it to
     * the GPU (cuvs_cagra_build_multi). We own the partials and unlink them after
     * the daemon replies; on any error the flock reaper (ADR-057) is the backstop. */
    {
        CuvsPartialDesc *descs =
            (CuvsPartialDesc *) palloc(nparticipants * sizeof(CuvsPartialDesc));
        uint32  ndesc = 0;

        for (i = 0; i < nparticipants; i++)
        {
            if (slots[i].n_vecs == 0 || slots[i].shm_name[0] == '\0')
                continue;   /* empty worker share: nothing to hand off */
            strlcpy(descs[ndesc].shm_name, slots[i].shm_name,
                    sizeof(descs[ndesc].shm_name));
            descs[ndesc].n_vecs = slots[i].n_vecs;
            ndesc++;
        }

        PG_TRY();
        {
            rc = cuvs_ipc_build_multi(
                cuvs_socket_path, (uint32_t) MyDatabaseId, build_index_oid,
                descs, ndesc, total, dim, metric,
                cuvs_resolve_index_dir_rel(indexRel),
                (uint32_t) RelationGetRelid(heapRel),
                (uint32_t) heapRel->rd_rel->relfilenode,
                shard_count, use_cpu_hnsw ? 1 : 0,
                (uint32_t) graph_degree, (uint32_t) intermediate_graph_degree, build_algo);

            if (rc != CUVS_STATUS_OK)
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("pg_cuvs: BUILD failed (status %d); CREATE INDEX "
                                "aborted to preserve catalog durability", rc),
                         errhint("Check pg_cuvs_server journal (journalctl -u "
                                 "pg-cuvs-server).")));
        }
        PG_FINALLY();
        {
            for (i = 0; i < nparticipants; i++)
                if (slots[i].shm_name[0])
                    shm_unlink(slots[i].shm_name);
            pfree(descs);
        }
        PG_END_TRY();
    }

    pfree(slots);
    return true;
}

/* ----------------------------------------------------------------
 * Phase 3K: shared heap-scan + CAGRA build.
 *
 * Used by both cuvs_ambuild (USING cagra) and the source-less pg_cuvs_hnsw
 * path (ephemeral CAGRA — see src/hnsw_export.c). Scans the heap into a corpus
 * and asks the daemon to build a CAGRA index under (MyDatabaseId,
 * build_index_oid). On an empty heap, sets *out_n_vecs = 0 and builds nothing.
 * The corpus is freed internally; a build failure raises ERROR (DDL durability
 * contract).
 * ---------------------------------------------------------------- */
void
cuvs_build_cagra_from_heap(Relation heapRel, Relation indexRel, IndexInfo *indexInfo,
                           uint32_t build_index_oid, uint32_t shard_count,
                           bool use_cpu_hnsw, int graph_degree,
                           int intermediate_graph_degree, uint32_t build_algo,
                           int64_t *out_n_vecs, double *out_reltuples)
{
    CuvsBuildState bs;
    AttrNumber heap_attno;
    int32  typmod;
    double reltuples_est;
    int    rc = CUVS_STATUS_OK;

    memset(&bs, 0, sizeof(bs));
    bs.metric = cuvs_index_metric(indexRel);  /* baked into the CAGRA graph */
    bs.cap_bytes = cuvs_effective_build_cap_bytes();
    bs.corpus.kind = CORPUS_HEAP;             /* until cuvs_corpus_open succeeds */
    bs.corpus.fd = -1;

    /* Preflight: estimate the corpus size from the planner's row estimate and
     * the indexed column's declared dimension. Used to (a) fail fast if it would
     * blow the build memory limit, and (b) size the corpus. */
    heap_attno = indexInfo->ii_IndexAttrNumbers[0];
    typmod = (heap_attno >= 1)
        ? TupleDescAttr(RelationGetDescr(heapRel), heap_attno - 1)->atttypmod
        : -1;
    reltuples_est = heapRel->rd_rel->reltuples;

    if (bs.cap_bytes > 0 && typmod > 0 && reltuples_est > 0)
    {
        size_t est = (size_t) reltuples_est * (size_t) typmod * sizeof(float)
                   + (size_t) reltuples_est * sizeof(uint64_t);
        if (est > bs.cap_bytes)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_cuvs: estimated build corpus %zu MB exceeds the "
                            "build memory limit (%zu MB)",
                            est / (1024 * 1024), bs.cap_bytes / (1024 * 1024)),
                     errdetail("%s",
                               cuvs_max_build_mem_mb > 0
                               ? "Limit is the cuvs.max_build_mem_mb hard cap."
                               : "Limit is auto (MemAvailable * cuvs.build_mem_safety_ratio)."),
                     errhint("Raise cuvs.max_build_mem_mb (hard cap) or "
                             "cuvs.build_mem_safety_ratio, shard the table, or see "
                             "docs/playbooks/large-dataset-benchmark.md.")));
    }

    /* ADR-034 §4A-2: parallelize the heap scan + detoast across maintenance
     * workers when the planner grants any (respects max_parallel_maintenance_
     * workers + table size). Skipped for CONCURRENTLY (snapshot handling) and
     * undeclared-dim columns. When the parallel path runs it does the whole
     * build (scan -> merge -> daemon) and we return; it falls back to the
     * single-process path below only if DSM is unavailable. */
    if (typmod > 0 && !indexInfo->ii_Concurrent)
    {
        int nworkers = plan_create_index_workers(RelationGetRelid(heapRel),
                                                 RelationGetRelid(indexRel));
        if (nworkers > 0
            && cuvs_build_parallel(heapRel, indexRel, indexInfo, build_index_oid,
                                   shard_count, use_cpu_hnsw, typmod, bs.metric,
                                   bs.cap_bytes, nworkers,
                                   graph_degree, intermediate_graph_degree, build_algo,
                                   out_n_vecs, out_reltuples))
            return;
    }

    /* ADR-057: when the column has a declared dimension (vector(N), the common
     * case), accumulate vectors directly in a leak-safe corpus (memfd -> shm)
     * sized to the row estimate — no heap->shm copy, and a crash cannot orphan
     * the segment. A column with no declared dim falls back to the heap tier
     * (corpus_open returns CORPUS_HEAP), which the legacy grow/IPC path handles.
     * Pre-sizing n_allocated skips grow_build_buffers for the first init_rows
     * tuples, so the heap TID buffer (grown there) must be allocated up front. */
    if (typmod > 0)
    {
        int64_t init_rows = (reltuples_est > 0) ? (int64_t) reltuples_est : 64;
        if (init_rows < 64)
            init_rows = 64;
        if (cuvs_corpus_open(&bs.corpus,
                             (size_t) init_rows * (size_t) typmod * sizeof(float)) == 0
            && bs.corpus.kind != CORPUS_HEAP)
        {
            bs.tids = (uint64_t *) malloc((size_t) init_rows * sizeof(uint64_t));
            if (bs.tids == NULL)
            {
                cuvs_corpus_close(&bs.corpus);
                ereport(ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("pg_cuvs: out of memory allocating build TID buffer")));
            }
            bs.dim         = typmod;            /* declared dim; callback rechecks per row */
            bs.vectors     = (float *) bs.corpus.base;
            bs.n_allocated = init_rows;
        }
    }

    /* The scan can longjmp (query cancel, statement_timeout, detoast error, the
     * build-memory guard); PG_FINALLY reclaims the corpus + heap buffers on every
     * path. The memfd tier is additionally crash-safe by construction. */
    PG_TRY();
    {
        bs.reltuples = table_index_build_scan(
            heapRel, indexRel, indexInfo,
            true, true,
            cuvs_build_callback, &bs, NULL);

        if (out_reltuples) *out_reltuples = bs.reltuples;
        *out_n_vecs = bs.n_vecs;

        if (bs.n_vecs > 0)
        {
            uint32_t heap_table_oid   = (uint32_t) RelationGetRelid(heapRel);
            uint32_t heap_relfilenode = (uint32_t) heapRel->rd_rel->relfilenode;

            /* Finalize the corpus to the daemon's exact [vectors][tids] layout:
             * shrink to the exact total and append the (small) TID region. The
             * heap tier hands its buffers to cuvs_ipc_build, which copies them. */
            if (bs.corpus.kind != CORPUS_HEAP)
            {
                size_t vec_bytes = (size_t) bs.n_vecs * bs.dim * sizeof(float);
                size_t tid_bytes = (size_t) bs.n_vecs * sizeof(uint64_t);
                if (cuvs_corpus_resize(&bs.corpus, vec_bytes + tid_bytes) != 0)
                    ereport(ERROR,
                            (errcode(ERRCODE_OUT_OF_MEMORY),
                             errmsg("pg_cuvs: failed to finalize build corpus: %m")));
                memcpy((char *) bs.corpus.base + vec_bytes, bs.tids, tid_bytes);
            }

            rc = cuvs_ipc_build(
                cuvs_socket_path,
                (uint32_t) MyDatabaseId,
                build_index_oid,
                &bs.corpus,
                bs.corpus.kind == CORPUS_HEAP ? bs.vectors : NULL,
                bs.corpus.kind == CORPUS_HEAP ? (const uint64_t *) bs.tids : NULL,
                bs.n_vecs,
                bs.dim,
                bs.metric,
                cuvs_resolve_index_dir_rel(indexRel),
                heap_table_oid,
                heap_relfilenode,
                shard_count,
                use_cpu_hnsw ? 1 : 0,  /* Phase 3I-1: serialize .hnsw sidecar */
                (uint32_t) graph_degree,
                (uint32_t) intermediate_graph_degree,
                build_algo);  /* 3R: CAGRA build params from reloptions */

            if (rc != CUVS_STATUS_OK)
            {
                /* DDL durability contract: fail the transaction so the catalog
                 * entry rolls back and the user knows to retry. */
                const char *hint;
                switch (rc)
                {
                    case CUVS_STATUS_UNAVAILABLE:
                        hint = "pg_cuvs_server is not reachable. Start it and retry "
                               "CREATE INDEX, or use SET enable_cuvs = off + pgvector "
                               "HNSW if GPU acceleration is not required.";
                        break;
                    case CUVS_STATUS_OOM_FALLBACK:
                        hint = "GPU VRAM exhausted. Free VRAM (drop other cagra "
                               "indexes or restart pg_cuvs_server) and retry, or use "
                               "pgvector HNSW instead.";
                        break;
                    default:
                        hint = "Check pg_cuvs_server journal (journalctl -u "
                               "pg-cuvs-server) for the underlying error. "
                               "Common causes: disk full or permission denied on "
                               "cuvs.index_dir.";
                        break;
                }
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("pg_cuvs: BUILD failed (status %d); CREATE INDEX "
                                "aborted to preserve catalog durability", rc),
                         errhint("%s", hint)));
            }
        }
    }
    PG_FINALLY();
    {
        /* For memfd/shm, bs.vectors aliases corpus.base (freed by corpus_close);
         * only the heap tier owns bs.vectors. TIDs are always heap. */
        if (bs.corpus.kind == CORPUS_HEAP && bs.vectors)
            free(bs.vectors);
        cuvs_corpus_close(&bs.corpus);   /* munmap/close (+shm_unlink); no-op for heap */
        if (bs.tids)
            free(bs.tids);
    }
    PG_END_TRY();
}

/* ----------------------------------------------------------------
 * Index AM: ambuild — handles CREATE INDEX USING cagra
 * ---------------------------------------------------------------- */
static IndexBuildResult *
cuvs_ambuild(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
    IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));
    int64_t n_vecs    = 0;
    double  reltuples = 0.0;

    /* Wave 1 release-hardening: build-time advisories (ADR-043 / OBJSTORE-03).
     * Emitted once per CREATE INDEX / REINDEX; never force a change. */
    {
        /* (1) TOAST: a toastable vector column pays detoast overhead when rows
         * are large enough to be stored out-of-line. Measured (ADR-044, 1M×1024):
         * detoast ~6.8s of the ~15.5s build backend, i.e. ~8% of total build time
         * (PLAIN also wins ~10% on INSERT + total disk). An earlier estimate of
         * "~25-35%" was an over-claim corrected by VM profiling.
         * pgvector's `vector` default storage is EXTERNAL ('e'); EXTENDED ('x')
         * also toasts. Only warn when the declared dimension is large enough that
         * rows actually TOAST (~2KB) — small vectors stay inline regardless of
         * storage class, so there is no detoast cost to avoid. Never force a change. */
        AttrNumber heap_attno = indexInfo->ii_IndexAttrNumbers[0];
        if (heap_attno >= 1)
        {
            Form_pg_attribute att =
                TupleDescAttr(RelationGetDescr(heapRel), heap_attno - 1);
            if (att->attstorage != 'p'   /* toastable (not PLAIN); vector default is 'e' */
                && att->atttypmod > 0
                && (int64) att->atttypmod * (int64) sizeof(float) >= 2000)
                ereport(NOTICE,
                        (errmsg("pg_cuvs: indexed column \"%s\" uses TOAST-able storage and "
                                "toasts at this dimension; PLAIN storage avoids detoast "
                                "overhead during build", NameStr(att->attname)),
                         errhint("For vector-only tables: ALTER TABLE %s ALTER COLUMN %s "
                                 "SET STORAGE PLAIN; VACUUM FULL; then rebuild. "
                                 "See docs/best-practices.md.",
                                 RelationGetRelationName(heapRel), NameStr(att->attname))));
        }

        /* (2) index_dir inside $PGDATA is copied wholesale by pg_basebackup
         * (backup bloat + standby-provisioning cost). Locality is satisfied by
         * any local volume, so a sibling dir OUTSIDE the PGDATA tree is preferred. */
        {
            const char *idir = cuvs_resolve_index_dir_rel(indexRel);
            size_t dlen = (DataDir != NULL) ? strlen(DataDir) : 0;
            if (idir != NULL && dlen > 0
                && strncmp(idir, DataDir, dlen) == 0
                && (idir[dlen] == '/' || idir[dlen] == '\0'))
                ereport(WARNING,
                        (errmsg("pg_cuvs: index_dir \"%s\" is inside the data directory; "
                                "its artifacts will be included in pg_basebackup", idir),
                         errhint("CAGRA artifacts are rebuildable and multi-GB. Place index_dir "
                                 "in a sibling directory on the same local volume but OUTSIDE "
                                 "$PGDATA (preserves locality, excludes from base backups). "
                                 "See docs/best-practices.md and OPS_GPU_PLAYBOOK section 6.")));
        }
    }

    /* Scan the heap and build the CAGRA index on the daemon under this index's
     * own OID (shard_count / cpu_hnsw from GUCs; 3R build params from reloptions). */
    {
        int      gd, igd;
        uint32_t algo;
        cuvs_resolve_build_params_rel(indexRel, &gd, &igd, &algo);  /* validates igd >= gd */
        cuvs_build_cagra_from_heap(heapRel, indexRel, indexInfo,
                                   (uint32_t) RelationGetRelid(indexRel),
                                   (uint32_t) cuvs_shard_count,
                                   cuvs_cpu_hnsw_fallback,
                                   gd, igd, algo,
                                   &n_vecs, &reltuples);
    }

    result->heap_tuples  = reltuples;
    result->index_tuples = (double) n_vecs;

    if (n_vecs == 0)
        return result;  /* empty table — nothing built */

    /* Phase 3C: write .relfilenode sidecar so the daemon can verify heap
     * compatibility before loading a GCS artifact on a new node. */
    {
        uint32_t heap_table_oid   = (uint32_t) RelationGetRelid(heapRel);
        uint32_t heap_relfilenode = (uint32_t) heapRel->rd_rel->relfilenode;
        char sidecar[MAXPGPATH];
        snprintf(sidecar, sizeof(sidecar), "%s/%u_%u.relfilenode",
                 cuvs_resolve_index_dir_rel(indexRel),
                 (uint32_t) MyDatabaseId,
                 (uint32_t) RelationGetRelid(indexRel));
        FILE *f = fopen(sidecar, "w");
        if (f)
        {
            fprintf(f, "%u %u\n", heap_relfilenode, heap_table_oid);
            fclose(f);
        }
        /* Non-fatal: GCS download will skip compat check if sidecar is absent */
    }

    /* Phase 3A: the fresh base absorbs all current rows, so any pending-insert
     * delta and tombstones from before this (RE)build are obsolete. Drop them;
     * the new base .tids CRC also invalidates leftovers via generation guard. */
    cuvs_delta_unlink(RelationGetRelid(indexRel));
    cuvs_tombstone_unlink(RelationGetRelid(indexRel));

    return result;
}

static void
cuvs_ambuildempty(Relation indexRel)
{
    (void)indexRel;
}

/* ----------------------------------------------------------------
 * Scan state (stored in IndexScanDesc->opaque)
 * ---------------------------------------------------------------- */
typedef struct CuvsScanState {
    uint64_t *tids;       /* palloc'd result TIDs */
    float    *distances;  /* palloc'd result distances */
    int       n_results;
    int       cur;        /* next result to return */
    bool      searched;   /* cuvs_ipc_search already called */
} CuvsScanState;

/* ----------------------------------------------------------------
 * Phase 3A delta merge (CPU MVP)
 *
 * After the daemon returns base CAGRA candidates, merge in the pending-insert
 * delta vectors computed exactly on the CPU. Distances MUST be on the same
 * scale as the daemon's cuVS output so the merged order is correct (the scan
 * sets xs_recheckorderby=false, i.e. the executor trusts this order):
 *   L2     -> squared L2  (cuVS L2Expanded), nearer = smaller
 *   cosine -> 1 - cossim  (cuVS CosineExpanded), nearer = smaller
 *   ip     -> raw dot     (cuVS InnerProduct), nearer = LARGER
 * ---------------------------------------------------------------- */
typedef struct CuvsCand {
    uint64_t tid;
    float    dist;
} CuvsCand;

/* Distance on the cuVS scale for the index metric (see table above). */
static float
cuvs_cpu_distance(uint32_t metric, const float *q, const float *v, int dim)
{
    double dot = 0.0, qn = 0.0, vn = 0.0, l2 = 0.0;

    for (int i = 0; i < dim; i++)
    {
        double qi = (double) q[i];
        double vi = (double) v[i];
        double d  = qi - vi;
        l2  += d * d;
        dot += qi * vi;
        qn  += qi * qi;
        vn  += vi * vi;
    }

    switch (metric)
    {
        case CUVS_METRIC_COSINE:
        {
            double denom = sqrt(qn) * sqrt(vn);
            return (denom > 0.0) ? (float) (1.0 - dot / denom) : 1.0f;
        }
        case CUVS_METRIC_IP:
            return (float) dot;        /* raw inner product */
        case CUVS_METRIC_L2:
        default:
            return (float) l2;         /* squared L2 */
    }
}

/* qsort_arg comparator: order candidates nearest-first. arg = &metric. */
static int
cuvs_cand_cmp(const void *a, const void *b, void *arg)
{
    const CuvsCand *x = (const CuvsCand *) a;
    const CuvsCand *y = (const CuvsCand *) b;
    uint32_t metric = *(const uint32_t *) arg;

    if (metric == CUVS_METRIC_IP)   /* larger dot = nearer */
    {
        if (x->dist > y->dist) return -1;
        if (x->dist < y->dist) return 1;
        return 0;
    }
    if (x->dist < y->dist) return -1;   /* smaller distance = nearer */
    if (x->dist > y->dist) return 1;
    return 0;
}

/* Phase 3A-4: filter base CAGRA + delta results against tombstoned TIDs.
 * Snapshot-aware per WRITE-04B: only filter if the delete xact committed and
 * is visible to the current snapshot. No tombstones = no-op. */
static void
cuvs_apply_tombstones(CuvsScanState *ss, Oid index_oid)
{
    char                 path[MAXPGPATH];
    int                  fd;
    CuvsTombstoneHeader  hdr;
    off_t                fsize;
    uint32_t             base_crc;
    CuvsTombstoneRecord *recs;
    int                  out = 0;

    cuvs_tombstone_path(index_oid, path, sizeof(path));
    fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    if (fd < 0)
        return;

    fsize = lseek(fd, 0, SEEK_END);
    if (fsize < (off_t) sizeof(hdr)
        || cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
        || hdr.magic != CUVS_TOMBSTONE_MAGIC
        || hdr.version != CUVS_TOMBSTONE_VERSION
        || cuvs_tombstone_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0
        || !cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32
        || hdr.n_entries == 0)
    {
        CloseTransientFile(fd);
        return;
    }

    {
        size_t recs_bytes = (size_t) hdr.n_entries * sizeof(CuvsTombstoneRecord);
        Snapshot snap = GetActiveSnapshot();

        recs = (CuvsTombstoneRecord *) palloc(recs_bytes);
        if (cuvs_pread_all(fd, (off_t) sizeof(hdr), recs, recs_bytes) != 0)
        {
            CloseTransientFile(fd);
            pfree(recs);
            return;
        }
        CloseTransientFile(fd);

        for (int i = 0; i < ss->n_results; i++)
        {
            bool dead = false;
            for (int64_t t = 0; t < hdr.n_entries; t++)
            {
                if (recs[t].tid == ss->tids[i])
                {
                    TransactionId xid = (TransactionId) recs[t].delete_xid;
                    if (TransactionIdDidCommit(xid)
                        && !XidInMVCCSnapshot(xid, snap))
                    {
                        dead = true;
                        break;
                    }
                }
            }
            if (!dead)
            {
                ss->tids[out]      = ss->tids[i];
                ss->distances[out] = ss->distances[i];
                out++;
            }
        }
        ss->n_results = out;
        pfree(recs);
    }
}

/* Merge the .delta pending-insert vectors into the base result set already in
 * *ss (tids/distances/n_results), keeping the nearest k overall. No delta ->
 * leaves the base set untouched. A delta that is corrupt / generation-mismatched
 * / oversized at scan time (a plan->execute race vs the cost gate) ERRORs so the
 * next query replans to CPU — never silently drop the pending rows. */
static void
cuvs_merge_delta(CuvsScanState *ss, Oid index_oid, const float *query,
                 int dim, int k, uint32_t metric)
{
    char            path[MAXPGPATH];
    int             fd;
    CuvsDeltaHeader hdr;
    off_t           fsize;
    size_t          rec_bytes = cuvs_delta_record_bytes((uint32_t) dim);
    uint32_t        base_crc;
    CuvsCand       *cands;
    char           *recbuf;
    int             n_base, n_total, out;

    cuvs_delta_path(index_oid, path, sizeof(path));
    fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    if (fd < 0)
        return;     /* no delta -> base-only (matches the plan gate) */

    fsize = lseek(fd, 0, SEEK_END);
    if (cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
        || hdr.magic != CUVS_DELTA_MAGIC || hdr.version != CUVS_DELTA_VERSION
        || hdr.dim != (uint32_t) dim
        || fsize < 0
        || cuvs_delta_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0
        || hdr.n_rows > (int64_t) cuvs_max_delta_rows
        || !cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32)
    {
        CloseTransientFile(fd);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_cuvs: delta sidecar unusable mid-scan; "
                        "retry will replan to CPU")));
    }
    if (hdr.n_rows == 0)
    {
        CloseTransientFile(fd);
        return;     /* empty delta -> base only */
    }

    n_base  = ss->n_results;
    n_total = n_base + (int) hdr.n_rows;
    cands   = (CuvsCand *) palloc((Size) n_total * sizeof(CuvsCand));

    for (int i = 0; i < n_base; i++)
    {
        cands[i].tid  = ss->tids[i];
        cands[i].dist = ss->distances[i];
    }

    recbuf = (char *) palloc(rec_bytes);
    for (int64_t i = 0; i < hdr.n_rows; i++)
    {
        off_t    off = (off_t) sizeof(hdr) + (off_t) i * (off_t) rec_bytes;
        uint64_t tid;

        if (cuvs_pread_all(fd, off, recbuf, rec_bytes) != 0)
        {
            CloseTransientFile(fd);
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pg_cuvs: delta record read failed mid-scan; "
                            "retry will replan to CPU")));
        }
        memcpy(&tid, recbuf, sizeof(tid));
        cands[n_base + i].tid  = tid;
        cands[n_base + i].dist = cuvs_cpu_distance(metric, query,
                                                   (const float *) (recbuf + sizeof(tid)),
                                                   dim);
    }
    pfree(recbuf);
    CloseTransientFile(fd);

    qsort_arg(cands, (size_t) n_total, sizeof(CuvsCand), cuvs_cand_cmp, &metric);

    out = (n_total < k) ? n_total : k;
    for (int i = 0; i < out; i++)
    {
        ss->tids[i]      = cands[i].tid;
        ss->distances[i] = cands[i].dist;
    }
    ss->n_results = out;
    pfree(cands);
}

/* Binary search: return true if enc_tid is in sorted filter_arr[0..n_filter). */
static bool
cuvs_tid_in_filter(uint64_t enc_tid, const uint64_t *filter_arr, uint32_t n_filter)
{
    uint32_t lo = 0, hi = n_filter;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if (filter_arr[mid] == enc_tid) return true;
        if (filter_arr[mid] <  enc_tid) lo = mid + 1;
        else                            hi = mid;
    }
    return false;
}

/* Filtered variant of cuvs_merge_delta: only delta rows whose TID is in
 * filter_arr (sorted, length n_filter) are merged. Operates directly on
 * caller-supplied tids_out/dists_out/*n_results arrays (no CuvsScanState). */
static void
cuvs_merge_delta_filtered(Oid index_oid, const float *query,
                          int dim, int k, uint32_t metric,
                          const uint64_t *filter_arr, uint32_t n_filter,
                          uint64_t *tids_out, float *dists_out, int *n_results)
{
    char            path[MAXPGPATH];
    int             fd;
    CuvsDeltaHeader hdr;
    off_t           fsize;
    size_t          rec_bytes = cuvs_delta_record_bytes((uint32_t) dim);
    uint32_t        base_crc;
    CuvsCand       *cands;
    char           *recbuf;
    int             n_base, n_cands, out;

    cuvs_delta_path(index_oid, path, sizeof(path));
    fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    if (fd < 0)
        return;

    fsize = lseek(fd, 0, SEEK_END);
    if (cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
        || hdr.magic != CUVS_DELTA_MAGIC || hdr.version != CUVS_DELTA_VERSION
        || hdr.dim != (uint32_t) dim
        || fsize < 0
        || cuvs_delta_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0
        || hdr.n_rows > (int64_t) cuvs_max_delta_rows
        || !cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32)
    {
        CloseTransientFile(fd);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_cuvs: delta sidecar unusable mid-scan; "
                        "retry will replan to CPU")));
    }
    if (hdr.n_rows == 0)
    {
        CloseTransientFile(fd);
        return;
    }

    n_base  = *n_results;
    n_cands = n_base;
    cands   = (CuvsCand *) palloc((Size)(n_base + (int) hdr.n_rows) * sizeof(CuvsCand));

    for (int i = 0; i < n_base; i++)
    {
        cands[i].tid  = tids_out[i];
        cands[i].dist = dists_out[i];
    }

    recbuf = (char *) palloc(rec_bytes);
    for (int64_t i = 0; i < hdr.n_rows; i++)
    {
        off_t    off = (off_t) sizeof(hdr) + (off_t) i * (off_t) rec_bytes;
        uint64_t tid;

        if (cuvs_pread_all(fd, off, recbuf, rec_bytes) != 0)
        {
            CloseTransientFile(fd);
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pg_cuvs: delta record read failed mid-scan; "
                            "retry will replan to CPU")));
        }
        memcpy(&tid, recbuf, sizeof(tid));
        /* Skip delta rows not in the caller's filter set. */
        if (n_filter > 0 && !cuvs_tid_in_filter(tid, filter_arr, n_filter))
            continue;
        cands[n_cands].tid  = tid;
        cands[n_cands].dist = cuvs_cpu_distance(metric, query,
                                                (const float *) (recbuf + sizeof(tid)),
                                                dim);
        n_cands++;
    }
    pfree(recbuf);
    CloseTransientFile(fd);

    qsort_arg(cands, (size_t) n_cands, sizeof(CuvsCand), cuvs_cand_cmp, &metric);

    out = (n_cands < k) ? n_cands : k;
    for (int i = 0; i < out; i++)
    {
        tids_out[i]  = cands[i].tid;
        dists_out[i] = cands[i].dist;
    }
    *n_results = out;
    pfree(cands);
}

/* Filtered variant of cuvs_apply_tombstones: operates on caller arrays. */
static void
cuvs_apply_tombstones_filtered(Oid index_oid,
                               uint64_t *tids_out, float *dists_out,
                               int *n_results)
{
    char                 path[MAXPGPATH];
    int                  fd;
    CuvsTombstoneHeader  hdr;
    off_t                fsize;
    uint32_t             base_crc;
    CuvsTombstoneRecord *recs;
    int                  out = 0;

    cuvs_tombstone_path(index_oid, path, sizeof(path));
    fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    if (fd < 0)
        return;

    fsize = lseek(fd, 0, SEEK_END);
    if (fsize < (off_t) sizeof(hdr)
        || cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
        || hdr.magic != CUVS_TOMBSTONE_MAGIC
        || hdr.version != CUVS_TOMBSTONE_VERSION
        || cuvs_tombstone_validate(&hdr, (int64_t) fsize - (int64_t) sizeof(hdr)) != 0
        || !cuvs_read_tids_crc(index_oid, &base_crc)
        || base_crc != hdr.base_tids_crc32
        || hdr.n_entries == 0)
    {
        CloseTransientFile(fd);
        return;
    }

    {
        size_t recs_bytes = (size_t) hdr.n_entries * sizeof(CuvsTombstoneRecord);
        Snapshot snap = GetActiveSnapshot();

        recs = (CuvsTombstoneRecord *) palloc(recs_bytes);
        if (cuvs_pread_all(fd, (off_t) sizeof(hdr), recs, recs_bytes) != 0)
        {
            CloseTransientFile(fd);
            pfree(recs);
            return;
        }
        CloseTransientFile(fd);

        for (int i = 0; i < *n_results; i++)
        {
            bool dead = false;
            for (int64_t t = 0; t < hdr.n_entries; t++)
            {
                if (recs[t].tid == tids_out[i])
                {
                    TransactionId xid = (TransactionId) recs[t].delete_xid;
                    if (TransactionIdDidCommit(xid)
                        && !XidInMVCCSnapshot(xid, snap))
                    {
                        dead = true;
                        break;
                    }
                }
            }
            if (!dead)
            {
                tids_out[out]  = tids_out[i];
                dists_out[out] = dists_out[i];
                out++;
            }
        }
        *n_results = out;
        pfree(recs);
    }
}

static IndexScanDesc
cuvs_beginscan(Relation rel, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);
    CuvsScanState *ss  = palloc0(sizeof(CuvsScanState));
    scan->opaque = ss;

    /* RelationGetIndexScan does NOT allocate xs_orderbyvals/xs_orderbynulls;
     * an amcanorderbyop AM must allocate them itself (as pgvector does). The
     * executor's ORDER BY reorder path reads these arrays, so leaving them
     * uninitialized makes it dereference garbage -- a non-deterministic
     * segfault (it may survive one scan and crash the next). */
    if (scan->numberOfOrderBys > 0)
    {
        scan->xs_orderbyvals  = palloc0(sizeof(Datum) * scan->numberOfOrderBys);
        scan->xs_orderbynulls = palloc0(sizeof(bool) * scan->numberOfOrderBys);
    }
    return scan;
}

static void
cuvs_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
            ScanKey orderbys, int norderbys)
{
    CuvsScanState *ss = (CuvsScanState *)scan->opaque;
    ss->searched  = false;
    ss->cur       = 0;
    ss->n_results = 0;

    if (ss->tids)      { pfree(ss->tids);      ss->tids      = NULL; }
    if (ss->distances) { pfree(ss->distances);  ss->distances = NULL; }

    /* Clear the ORDER BY output arrays so a rescan (cursor MOVE/re-FETCH)
     * never reads distances left over from the previous scan. gettuple
     * re-seeds [0] before any read, but resetting here keeps the invariant
     * explicit. */
    if (scan->numberOfOrderBys > 0 && scan->xs_orderbyvals != NULL)
    {
        memset(scan->xs_orderbyvals, 0, sizeof(Datum) * scan->numberOfOrderBys);
        memset(scan->xs_orderbynulls, 0, sizeof(bool) * scan->numberOfOrderBys);
    }

    if (keys && scan->numberOfKeys > 0)
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    if (orderbys && scan->numberOfOrderBys > 0)
        memmove(scan->orderByData, orderbys,
                scan->numberOfOrderBys * sizeof(ScanKeyData));
}

/* ----------------------------------------------------------------
 * gettuple — called repeatedly to iterate search results
 *
 * On the first call: extract query vector from orderByData, run IPC
 * search, store result set. Subsequent calls iterate stored results.
 * ---------------------------------------------------------------- */
static bool
cuvs_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    CuvsScanState *ss = (CuvsScanState *)scan->opaque;
    (void)dir;

    if (!ss->searched)
    {
        ss->searched = true;

        /* These should normally be caught by cuvsamcostestimate before this
         * index path is chosen. If they change between planning and execution,
         * returning false ends this index scan; the next statement replans
         * through the cost gates. */
        Oid index_oid = RelationGetRelid(scan->indexRelation);
        if (!enable_cuvs || cuvs_circuit_is_open((uint32_t)index_oid))
            return false;

        /* Extract query vector from ORDER BY operator argument */
        if (scan->numberOfOrderBys < 1 || !scan->orderByData)
            return false;

        /* A NULL query vector (e.g. `embedding <-> NULL`) has no neighbors.
         * Return an empty scan instead of dereferencing a NULL Datum in
         * DatumGetPgVector below (crash). */
        if (scan->orderByData[0].sk_flags & SK_ISNULL)
            return false;

        Datum query_datum = scan->orderByData[0].sk_argument;
        PgVector *qvec    = DatumGetPgVector(query_datum);
        int       dim     = (int)qvec->dim;
        int       k       = cuvs_k;  /* GPU top-k; SQL LIMIT applied by executor */

        /* Phase 3A recall: over-fetch the base by the pending dead-TID count so
         * tombstone filtering (cuvs_apply_tombstones, below) cannot drop the
         * live result below cuvs_k when the base's nearest candidates are all
         * tombstoned. Bounded to one extra cuvs_k so a large tombstone set
         * cannot blow up GPU work — heavier delete drift is caught first by the
         * delete-drift gate. No-op when there are no tombstones, so the common
         * path is unchanged. */
        if (cuvs_max_delta_rows > 0)
        {
            int64_t n_tomb = cuvs_tombstone_count(index_oid);
            if (n_tomb > 0)
                k += (int) Min(n_tomb, (int64_t) cuvs_k);
        }

        ss->tids      = palloc(k * sizeof(uint64_t));
        ss->distances = palloc(k * sizeof(float));

        /* Metric is determined by the index's operator class (same source as
         * build), not the ORDER BY strategy number (all three opclasses use
         * strategy 1, so the old heuristic never fired and forced L2). */
        uint32_t metric = cuvs_index_metric(scan->indexRelation);

        uint32_t latency_us = 0;
        int      delta_merged = 0;
        int rc = cuvs_ipc_search(
            cuvs_socket_path,
            (uint32_t)MyDatabaseId,
            (uint32_t)index_oid,
            qvec->x,
            dim, k, metric,
            (uint32_t)cuvs_shard_overfetch,
            cuvs_parallel_fanout ? 1 : 0,
            cuvs_cpu_hnsw_fallback ? 1 : 0,  /* Phase 3I-1 */
            (uint32_t)cuvs_search_mode,      /* Phase 3L: 0=cagra, 1=brute_force */
            (uint32_t)cuvs_bf_precision,     /* Phase 3L: 0=float32, 1=float16 */
            (uint32_t)cuvs_bf_batch_wait_us, /* Phase 3L: BF micro-batch window μs */
            ss->tids,
            ss->distances,
            &ss->n_results,
            &latency_us,
            &delta_merged);

        /* 3S: the reply wait was aborted by a pending cancel / statement_timeout.
         * Not a GPU failure — don't touch the circuit breaker; raise the actual
         * interrupt now that the IPC socket/shm are cleaned up. */
        if (rc == CUVS_STATUS_CANCELED)
        {
            CHECK_FOR_INTERRUPTS();   /* normally longjmps out here */
            return false;             /* fallback if the interrupt was cleared */
        }

        if (rc != CUVS_STATUS_OK)
        {
            /* Record error for circuit breaker. DIM_MISMATCH and METRIC_MISMATCH
             * are user/config errors, not repeatable GPU failures. STALE is
             * already gated at plan time. Everything else — including UNAVAILABLE
             * (daemon crash after planning) — counts so the breaker opens and
             * subsequent queries replan to CPU via the socket-existence gate. */
            if (rc != CUVS_STATUS_DIM_MISMATCH
                && rc != CUVS_STATUS_METRIC_MISMATCH && rc != CUVS_STATUS_STALE
                && rc != CUVS_STATUS_NO_VECTORS)
                cuvs_circuit_record_error((uint32_t)index_oid,
                                          cuvs_circuit_breaker_threshold);

            switch (rc)
            {
                case CUVS_STATUS_STALE:
                    /* The cost path (cuvs_index_is_stale) normally steers the
                     * planner away from a stale index, so we only reach here
                     * when a write marked the index stale between planning and
                     * this scan. Return no rows for this scan; the next query
                     * replans onto the CPU path. */
                    ereport(WARNING,
                            (errmsg("pg_cuvs: cagra index went stale mid-scan; "
                                    "returning no rows (retry replans to CPU)"),
                             errhint("REINDEX the index to re-enable GPU search.")));
                    /* Phase 3: delta correction plugs in here
                     * (stale && delta-available -> GPU+delta, else fallback). */
                    break;
                case CUVS_STATUS_METRIC_MISMATCH:
                    /* The index was built with a different metric (e.g. built
                     * before opclass-metric support, always L2). cuVS bakes the
                     * metric into the graph, so the only fix is a rebuild. Fail
                     * loudly rather than silently return wrong-metric results. */
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("pg_cuvs: cagra index was built with a different "
                                    "distance metric than this query's operator class"),
                             errhint("REINDEX the index to rebuild it with the current metric.")));
                    break;
                case CUVS_STATUS_DIM_MISMATCH:
                    /* User error: query/index dimension differ. Fail loudly
                     * like pgvector does, rather than silently returning no
                     * rows from this index scan. */
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_EXCEPTION),
                             errmsg("pg_cuvs: query vector dimension %d does not "
                                    "match the cagra index dimension", dim)));
                    break;
                case CUVS_STATUS_UNAVAILABLE:
                    /* Daemon was reachable at plan time (socket existed) but
                     * unreachable now — crash or restart between plan and execute.
                     * ERROR aborts this query; the breaker records the failure so
                     * the next query replans to CPU via the socket-existence gate. */
                    ereport(ERROR,
                            (errcode(ERRCODE_CONNECTION_FAILURE),
                             errmsg("pg_cuvs: GPU daemon unavailable after planning; "
                                    "retry will use CPU while breaker is open"),
                             errhint("The daemon crashed or restarted between plan and "
                                     "execute time. Subsequent queries replan to CPU "
                                     "automatically once the breaker opens.")));
                    break;
                case CUVS_STATUS_OOM_FALLBACK:
                    ereport(ERROR,
                            (errcode(ERRCODE_OUT_OF_MEMORY),
                             errmsg("pg_cuvs: GPU VRAM exhausted; "
                                    "retry will use CPU while breaker is open"),
                             errhint("REINDEX or reduce cuvs.k to lower GPU memory use.")));
                    break;
                case CUVS_STATUS_NOT_FOUND:
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("pg_cuvs: cagra index not loaded on GPU daemon; "
                                    "retry will use CPU while breaker is open"),
                             errhint("REINDEX the index to reload it on the daemon.")));
                    break;
                case CUVS_STATUS_NO_VECTORS:
                    /* Phase 3L: brute_force requested but the .vectors sidecar is
                     * missing or stale. User/config error — fail loudly with
                     * guidance rather than silently returning ANN/no rows. */
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("pg_cuvs: brute_force search requires a .vectors "
                                    "sidecar that is missing or stale for this index"),
                             errhint("REINDEX the index to (re)build the brute_force "
                                     "vector sidecar, or SET cuvs.search_mode='cagra'.")));
                    break;
                default:
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("pg_cuvs: GPU search failed (status %d); "
                                    "retry will use CPU while breaker is open", rc)));
                    break;
            }
            return false;
        }

        /* Successful search — reset consecutive error count */
        cuvs_circuit_record_success((uint32_t)index_oid);
        ss->cur = 0;

        /* Phase 3A-3 tri-mode delta search: auto=GPU-then-CPU, cpu=always CPU,
         * gpu=GPU-only (no CPU fallback). */
        if (cuvs_max_delta_rows > 0)
        {
            int do_cpu = (cuvs_delta_search_mode == 1)
                      || (cuvs_delta_search_mode == 0 && !delta_merged);
            if (do_cpu)
                cuvs_merge_delta(ss, index_oid, qvec->x, dim, k, metric);
        }

        /* Phase 3A-4: filter dead TIDs via tombstones (snapshot-aware). */
        if (cuvs_max_delta_rows > 0)
            cuvs_apply_tombstones(ss, index_oid);

        /* Record per-search stats for pg_cuvs_last_search_* and EXPLAIN. */
        cuvs_last_latency_us   = latency_us;
        cuvs_last_n_results    = ss->n_results;
        cuvs_last_k_requested  = k;
        cuvs_last_index_oid    = index_oid;
        cuvs_last_metric       = metric;
        cuvs_last_dim          = dim;

        if (cuvs_debug)
        {
            static const char * const metric_names[] = {"l2", "cosine", "ip"};
            const char *mname = (metric < 3) ? metric_names[metric] : "?";
            ereport(NOTICE,
                    (errmsg("pg_cuvs: cagra scan oid=%u dim=%d metric=%s "
                            "k=%d n=%d latency_us=%u",
                            (uint32_t)index_oid, dim, mname,
                            k, ss->n_results, latency_us)));
        }
    }

    /* Iterate stored results */
    if (ss->cur >= ss->n_results)
        return false;

    uint64_t tid     = ss->tids[ss->cur];
    uint32_t blk;
    uint16_t offset;
    cuvs_tid_decode(tid, &blk, &offset);

    ItemPointerSet(&scan->xs_heaptid, blk, offset);
    scan->xs_recheck = true;   /* recheck predicate on heap tuple */

    /* ORDER BY support. Because amcanorderbyop is true, the executor may use
     * IndexNextWithReorder, whose reorder queue reads xs_orderbyvals /
     * xs_orderbynulls for every returned tuple. If we leave them unset they
     * hold uninitialized/stale Datums and cmp_orderbyvals dereferences garbage
     * (segfault, typically on the 2nd scan in a backend once the array holds
     * stale values). Seed with the daemon's distance and set recheckorderby so
     * the executor recomputes the exact <-> value on the heap tuple, giving the
     * correct order regardless of metric or squared-vs-sqrt distance. */
    if (scan->numberOfOrderBys > 0 && scan->xs_orderbyvals != NULL)
    {
        scan->xs_orderbyvals[0]  = Float8GetDatum((double) ss->distances[ss->cur]);
        scan->xs_orderbynulls[0] = false;
        /* false: trust the daemon's distance + CAGRA's sorted order (like
         * pgvector). The executor then does NOT recompute via
         * EvalOrderByExpressions; results stay in distance order. The values
         * are monotonic with the true <-> distance, so ordering (and recall)
         * is correct without paying a per-tuple recompute. */
        scan->xs_recheckorderby  = false;

        /* We only produce a distance for the first ORDER BY key. If a query
         * supplies more than one (e.g. two <-> operators on this column), mark
         * the rest NULL rather than leaving them holding stale/zero Datums that
         * the executor's reorder comparison would read. */
        for (int i = 1; i < scan->numberOfOrderBys; i++)
            scan->xs_orderbynulls[i] = true;
    }
    ss->cur++;

    return true;
}

static void
cuvs_endscan(IndexScanDesc scan)
{
    CuvsScanState *ss = (CuvsScanState *)scan->opaque;
    if (ss->tids)      pfree(ss->tids);
    if (ss->distances) pfree(ss->distances);
    pfree(ss);
    scan->opaque = NULL;
}

/* Flag this index stale on the daemon after a heap write. Best-effort: a
 * daemon that is down must not fail the user's write (the next reachable
 * write or a REINDEX re-establishes the marker). */
static void
cuvs_mark_index_stale(Relation indexRel)
{
    (void) cuvs_ipc_mark_stale(cuvs_socket_path,
                               (uint32_t) MyDatabaseId,
                               (uint32_t) RelationGetRelid(indexRel));
}

/* Append one {TID, vector} record to the index's .delta sidecar. flock-
 * serialized across backends; positions the record at the canonical offset
 * (sizeof(header) + n_rows*record_bytes) and ftruncates, so a torn tail from a
 * crashed prior append self-heals. Returns true on durable append; false on any
 * error or when the delta has reached cuvs.max_delta_rows (caller then fails
 * closed by marking the index stale). */
static bool
cuvs_delta_append(Relation indexRel, uint64_t tid, const float *vec, int dim,
                  uint32_t metric)
{
    char            path[MAXPGPATH];
    int             fd;
    CuvsDeltaHeader hdr;
    off_t           fsize, rec_off;
    size_t          rec_bytes = cuvs_delta_record_bytes((uint32_t) dim);
    bool            ok = false;

    cuvs_delta_path(RelationGetRelid(indexRel), path, sizeof(path));

    fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
    if (fd < 0)
        return false;
    /* Serialize concurrent appends from other backends (O_CREAT does not
     * truncate, so racing creators share one file; flock orders the writes). */
    if (flock(fd, LOCK_EX) != 0)
    {
        CloseTransientFile(fd);
        return false;
    }
    /* Sibling artifacts (.cagra/.tids) are 0644; the daemon reads .delta too, so
     * match them rather than the 0600 OpenTransientFile default. Access is gated
     * by the index directory's own permissions (0700 in production), and the
     * vectors are already readable via .cagra. */
    (void) fchmod(fd, 0644);

    fsize = lseek(fd, 0, SEEK_END);
    if (fsize < 0)
        goto done;

    if (fsize == 0)
    {
        /* New delta: tie it to the current base build via the .tids body CRC. */
        uint32_t base_crc;
        if (!cuvs_read_tids_crc(RelationGetRelid(indexRel), &base_crc))
            goto done;   /* no base artifact -> can't establish generation */
        cuvs_delta_header_init(&hdr, (uint32_t) dim, metric, base_crc);
    }
    else if (fsize < (off_t) sizeof(hdr)
             || cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
             || hdr.magic != CUVS_DELTA_MAGIC
             || hdr.version != CUVS_DELTA_VERSION
             || hdr.dim != (uint32_t) dim)
    {
        goto done;       /* corrupt / dim drift -> fail closed */
    }

    if (hdr.n_rows >= (int64_t) cuvs_max_delta_rows)
        goto done;       /* cap reached -> caller marks stale, bounds growth */

    /* Write the record at the canonical slot, bump the count, trim torn tail. */
    rec_off = (off_t) sizeof(hdr) + (off_t) hdr.n_rows * (off_t) rec_bytes;
    if (cuvs_pwrite_all(fd, rec_off, &tid, sizeof(tid)) != 0)
        goto done;
    if (cuvs_pwrite_all(fd, rec_off + (off_t) sizeof(tid), vec,
                        (size_t) dim * sizeof(float)) != 0)
        goto done;
    hdr.n_rows++;
    if (cuvs_pwrite_all(fd, 0, &hdr, sizeof(hdr)) != 0)
        goto done;
    if (ftruncate(fd, (off_t) sizeof(hdr) + (off_t) hdr.n_rows * (off_t) rec_bytes) != 0)
        goto done;
    if (pg_fsync(fd) != 0)
        goto done;
    ok = true;

done:
    flock(fd, LOCK_UN);
    CloseTransientFile(fd);
    return ok;
}

/* aminsert fires per inserted/updated row. Phase 3A: record the new vector in
 * the durable .delta sidecar so a query merges it with the base CAGRA results
 * — no rebuild, no stale reroute. The daemon is NOT told stale, so it keeps
 * serving the base search. Fail closed: on any delta error (or the delta cap),
 * mark the index stale so the planner routes to CPU and the row is never lost.
 * DELETE/VACUUM use the tombstone path via ambulkdelete, with stale as the
 * fail-closed fallback when tombstones cannot be recorded safely. */
static bool
cuvs_aminsert(Relation indexRel, Datum *values, bool *isnull,
              ItemPointer heap_tid, Relation heapRel,
              IndexUniqueCheck checkUnique, bool indexUnchanged,
              IndexInfo *indexInfo)
{
    PgVector *vec;
    uint64_t  tid;
    uint32_t  metric;

    (void) heapRel; (void) checkUnique; (void) indexUnchanged; (void) indexInfo;

    /* NULL vector: nothing to index (pgvector does not search NULLs either). */
    if (isnull[0])
        return false;

    /* Delta disabled (GUC 0): fall back to Phase 2 behavior — mark stale once
     * per relcache life and route to CPU. */
    if (cuvs_max_delta_rows <= 0)
    {
        if (indexRel->rd_amcache == NULL)
        {
            cuvs_mark_index_stale(indexRel);
            indexRel->rd_amcache = MemoryContextAllocZero(indexRel->rd_indexcxt,
                                                          sizeof(bool));
        }
        return false;
    }

    /* Already stale (delete-driven via ambulkdelete): the planner reroutes to
     * CPU regardless, so don't grow the delta. Leave it until REINDEX. */
    if (cuvs_index_is_stale(RelationGetRelid(indexRel)))
        return false;

    vec    = DatumGetPgVector(values[0]);
    tid    = cuvs_tid_encode(ItemPointerGetBlockNumber(heap_tid),
                             ItemPointerGetOffsetNumber(heap_tid));
    metric = cuvs_index_metric(indexRel);

    /* 3Q: EXTEND path — update VRAM CAGRA graph in-place via IPC. */
    if (cuvs_socket_path && cuvs_socket_path[0] != '\0')
    {
        int rc = cuvs_ipc_extend(cuvs_socket_path,
                                 (uint32_t) MyDatabaseId,
                                 (uint32_t) RelationGetRelid(indexRel),
                                 vec->x, &tid, 1, (int) vec->dim,
                                 (uint32_t) cuvs_extend_chunk_size,
                                 cuvs_resolve_index_dir_rel(indexRel));
        if (rc == CUVS_STATUS_OK)
            return false;
        /* NOT_FOUND / BUILD_FAILED / daemon down → fall through to delta. */
    }

    if (!cuvs_delta_append(indexRel, tid, vec->x, (int) vec->dim, metric))
        cuvs_mark_index_stale(indexRel);

    return false;   /* no in-index entry stored */
}

/* Phase 3A-4: append a single dead TID to the .tombstone sidecar.
 * Returns true on success, false on any failure (caller marks stale). */
static bool
cuvs_tombstone_append(Relation indexRel, uint64_t tid, uint64_t delete_xid)
{
    char                 path[MAXPGPATH];
    int                  fd;
    CuvsTombstoneHeader  hdr;
    off_t                fsize, rec_off;
    CuvsTombstoneRecord  rec;
    bool                 ok = false;

    cuvs_tombstone_path(RelationGetRelid(indexRel), path, sizeof(path));
    fd = OpenTransientFile(path, O_RDWR | O_CREAT | PG_BINARY);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX) != 0) { CloseTransientFile(fd); return false; }
    (void) fchmod(fd, 0644);

    fsize = lseek(fd, 0, SEEK_END);
    if (fsize < 0) goto done;

    if (fsize == 0)
    {
        uint32_t base_crc;
        if (!cuvs_read_tids_crc(RelationGetRelid(indexRel), &base_crc))
            goto done;
        cuvs_tombstone_header_init(&hdr, base_crc);
    }
    else if (fsize < (off_t) sizeof(hdr)
             || cuvs_pread_all(fd, 0, &hdr, sizeof(hdr)) != 0
             || hdr.magic != CUVS_TOMBSTONE_MAGIC
             || hdr.version != CUVS_TOMBSTONE_VERSION)
    {
        goto done;
    }

    if (hdr.n_entries >= (int64_t) cuvs_max_delta_rows)
        goto done;

    rec.tid        = tid;
    rec.delete_xid = delete_xid;
    rec_off = (off_t) sizeof(hdr) + (off_t) hdr.n_entries * (off_t) sizeof(rec);

    if (cuvs_pwrite_all(fd, rec_off, &rec, sizeof(rec)) != 0)
        goto done;
    hdr.n_entries++;
    if (cuvs_pwrite_all(fd, 0, &hdr, sizeof(hdr)) != 0)
        goto done;
    if (ftruncate(fd, (off_t) sizeof(hdr) + (off_t) hdr.n_entries * (off_t) sizeof(rec)) != 0)
        goto done;
    if (pg_fsync(fd) != 0)
        goto done;
    ok = true;

done:
    flock(fd, LOCK_UN);
    CloseTransientFile(fd);
    return ok;
}

/* ambulkdelete: Phase 3A-4 records dead TIDs as tombstones instead of
 * unconditionally marking stale. Falls back to mark-stale if tombstone
 * append fails or exceeds the cap. */
static IndexBulkDeleteResult *
cuvs_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                  IndexBulkDeleteCallback callback, void *callback_state)
{
    Relation    indexRel = info->index;
    Oid         index_oid = RelationGetRelid(indexRel);
    bool        all_ok = true;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    if (cuvs_max_delta_rows <= 0)
    {
        cuvs_mark_index_stale(indexRel);
        return stats;
    }

    if (cuvs_index_is_stale(index_oid))
        return stats;

    {
        char            tids_path[MAXPGPATH];
        FILE           *f;
        CuvsTidsHeader  thdr;
        uint64_t       *base_tids = NULL;
        int64_t         n_total_vecs = 0;

        snprintf(tids_path, sizeof(tids_path), "%s/%u_%u.tids",
                 cuvs_resolve_index_dir_rel(indexRel),
                 (uint32_t) MyDatabaseId,
                 (uint32_t) index_oid);
        f = AllocateFile(tids_path, PG_BINARY_R);
        if (f && cuvs_tids_read(f, &thdr, &base_tids) == 0)
        {
            TransactionId delete_xid = GetCurrentTransactionId();
            n_total_vecs = thdr.n_vecs;
            for (int64_t i = 0; i < thdr.n_vecs; i++)
            {
                uint32_t blk;
                uint16_t off;
                ItemPointerData iptr;

                cuvs_tid_decode(base_tids[i], &blk, &off);
                ItemPointerSet(&iptr, blk, off);

                if (callback(&iptr, callback_state))
                {
                    stats->tuples_removed += 1;
                    if (!cuvs_tombstone_append(indexRel, base_tids[i],
                                              (uint64_t) delete_xid))
                    {
                        all_ok = false;
                        break;
                    }
                }
            }
            free(base_tids);

            /* 3Q: advisory auto-compact when dead ratio >= threshold. */
            if (all_ok && cuvs_compact_delete_ratio > 0 &&
                n_total_vecs > 0 && cuvs_socket_path && cuvs_socket_path[0] != '\0' &&
                (double) stats->tuples_removed / (double) n_total_vecs
                    >= cuvs_compact_delete_ratio)
            {
                (void) cuvs_ipc_compact(cuvs_socket_path,
                                        (uint32_t) MyDatabaseId,
                                        (uint32_t) index_oid,
                                        cuvs_resolve_index_dir_rel(indexRel));
            }
        }
        else
        {
            all_ok = false;
        }
        if (f) FreeFile(f);
    }

    if (!all_ok)
        cuvs_mark_index_stale(indexRel);

    return stats;
}

/* VACUUM calls amvacuumcleanup once per index. A CAGRA index is an immutable
 * GPU snapshot (rebuilt only by REINDEX), so there is nothing to clean up —
 * but the handler must exist, otherwise VACUUM (and autovacuum) ERROR with
 * "function amvacuumcleanup is not defined" on any table that has a cagra
 * index. Return stats unchanged (NULL when no ambulkdelete ran). */
static IndexBulkDeleteResult *
cuvs_amvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    Relation indexRel  = info->index;
    Oid      index_oid = RelationGetRelid(indexRel);
    char     tpath[MAXPGPATH];
    struct   stat st;

    if (cuvs_socket_path && cuvs_socket_path[0] != '\0' &&
        !cuvs_index_is_stale(index_oid))
    {
        cuvs_tombstone_path(index_oid, tpath, sizeof(tpath));
        if (stat(tpath, &st) == 0)
            (void) cuvs_ipc_compact(cuvs_socket_path,
                                    (uint32_t) MyDatabaseId,
                                    (uint32_t) index_oid,
                                    cuvs_resolve_index_dir_rel(indexRel));
    }
    return stats;
}

/* ----------------------------------------------------------------
 * 3P: IVF-PQ Access Method
 *
 * ivfpq_ambuild  — scans the heap and builds an IVF-PQ index via the daemon.
 * ivfpq_gettuple — runs one IVF-PQ search on first call, iterates results.
 * ivfpq_aminsert — no-op; IVF-PQ is rebuild-only (REINDEX after bulk loads).
 *
 * Scan state is the same CuvsScanState used by CAGRA (compatible allocation).
 * cuvs_beginscan and cuvs_endscan are reused directly.
 * ---------------------------------------------------------------- */
static IndexBuildResult *
ivfpq_ambuild(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
    IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));
    CuvsBuildState    bs;
    double            reltuples = 0.0;

    memset(&bs, 0, sizeof(bs));
    bs.metric     = cuvs_index_metric(indexRel);
    bs.cap_bytes  = cuvs_effective_build_cap_bytes();
    bs.corpus.kind = CORPUS_HEAP;
    bs.corpus.fd   = -1;

    /* Read IVF-PQ build params from index reloptions (or defaults). */
    CuvsIvfPqOptions *opts = (CuvsIvfPqOptions *) indexRel->rd_options;
    uint32_t n_lists = opts ? (uint32_t)opts->n_lists : 1024;
    uint32_t pq_bits = opts ? (uint32_t)opts->pq_bits : 8;
    uint32_t pq_dim  = opts ? (uint32_t)opts->pq_dim  : 0;

    PG_TRY();
    {
        reltuples = table_index_build_scan(
            heapRel, indexRel, indexInfo,
            true, true,
            cuvs_build_callback, &bs, NULL);

        if (bs.n_vecs > 0)
        {
            uint32_t table_oid   = (uint32_t) RelationGetRelid(heapRel);
            uint32_t relfilenode = (uint32_t) heapRel->rd_rel->relfilenode;

            int rc = cuvs_ipc_build_ivfpq(
                cuvs_socket_path,
                (uint32_t) MyDatabaseId,
                (uint32_t) RelationGetRelid(indexRel),
                bs.vectors,
                (const uint64_t *) bs.tids,
                bs.n_vecs,
                bs.dim,
                bs.metric,
                get_index_dir(),
                table_oid,
                relfilenode,
                n_lists, pq_bits, pq_dim);

            if (rc != CUVS_STATUS_OK)
            {
                const char *hint;
                switch (rc)
                {
                    case CUVS_STATUS_UNAVAILABLE:
                        hint = "pg_cuvs_server is not reachable. Start it and retry "
                               "CREATE INDEX, or use pgvector HNSW.";
                        break;
                    case CUVS_STATUS_OOM_FALLBACK:
                        hint = "GPU VRAM exhausted. Free VRAM and retry CREATE INDEX.";
                        break;
                    default:
                        hint = "Check pg_cuvs_server logs for details.";
                        break;
                }
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("pg_cuvs: IVF-PQ BUILD failed (status %d); "
                                "CREATE INDEX aborted", rc),
                         errhint("%s", hint)));
            }
        }
    }
    PG_FINALLY();
    {
        if (bs.vectors)
            free(bs.vectors);
        if (bs.tids)
            free(bs.tids);
    }
    PG_END_TRY();

    result->heap_tuples  = reltuples;
    result->index_tuples = (double) bs.n_vecs;
    return result;
}

static void
ivfpq_ambuildempty(Relation indexRel)
{
    (void)indexRel;
}

static bool
ivfpq_aminsert(Relation indexRel, Datum *values, bool *isnull,
               ItemPointer ht_ctid, Relation heapRel,
               IndexUniqueCheck checkUnique,
               bool indexUnchanged, IndexInfo *indexInfo)
{
    /* IVF-PQ does not support incremental inserts. REINDEX after bulk loads. */
    (void)indexRel; (void)values; (void)isnull; (void)ht_ctid;
    (void)heapRel; (void)checkUnique; (void)indexUnchanged; (void)indexInfo;
    return false;
}

static void
ivfpq_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
             ScanKey orderbys, int norderbys)
{
    /* Identical to cuvs_rescan — resets scan state for a new query vector. */
    CuvsScanState *ss = (CuvsScanState *)scan->opaque;
    ss->searched  = false;
    ss->cur       = 0;
    ss->n_results = 0;

    if (ss->tids)      { pfree(ss->tids);      ss->tids      = NULL; }
    if (ss->distances) { pfree(ss->distances);  ss->distances = NULL; }

    if (scan->numberOfOrderBys > 0 && scan->xs_orderbyvals != NULL)
    {
        memset(scan->xs_orderbyvals, 0, sizeof(Datum) * scan->numberOfOrderBys);
        memset(scan->xs_orderbynulls, 0, sizeof(bool) * scan->numberOfOrderBys);
    }

    if (keys && scan->numberOfKeys > 0)
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    if (orderbys && scan->numberOfOrderBys > 0)
        memmove(scan->orderByData, orderbys,
                scan->numberOfOrderBys * sizeof(ScanKeyData));
}

static bool
ivfpq_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    CuvsScanState *ss = (CuvsScanState *)scan->opaque;
    (void)dir;

    if (!ss->searched)
    {
        ss->searched = true;

        Oid index_oid = RelationGetRelid(scan->indexRelation);
        if (!enable_cuvs)
            return false;

        if (scan->numberOfOrderBys < 1 || !scan->orderByData)
            return false;
        if (scan->orderByData[0].sk_flags & SK_ISNULL)
            return false;

        Datum    query_datum = scan->orderByData[0].sk_argument;
        PgVector *qvec       = DatumGetPgVector(query_datum);
        int       dim        = (int)qvec->dim;
        int       k          = cuvs_k;
        uint32_t  metric     = cuvs_index_metric(scan->indexRelation);
        uint32_t  n_probes   = (uint32_t) cuvs_ivfpq_n_probes;
        uint32_t  latency_us = 0;

        ss->tids      = palloc(k * sizeof(uint64_t));
        ss->distances = palloc(k * sizeof(float));

        int rc = cuvs_ipc_search_ivfpq(
            cuvs_socket_path,
            (uint32_t) MyDatabaseId,
            (uint32_t) index_oid,
            qvec->x,
            dim, k, metric, n_probes,
            ss->tids, ss->distances,
            &ss->n_results,
            &latency_us);

        if (rc != CUVS_STATUS_OK)
        {
            switch (rc)
            {
                case CUVS_STATUS_METRIC_MISMATCH:
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("pg_cuvs: ivfpq index was built with a different metric"),
                             errhint("REINDEX to rebuild with the current metric.")));
                    break;
                case CUVS_STATUS_DIM_MISMATCH:
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_EXCEPTION),
                             errmsg("pg_cuvs: query vector dimension %d does not "
                                    "match the ivfpq index dimension", dim)));
                    break;
                default:
                    break;
            }
            return false;
        }

        cuvs_last_latency_us  = latency_us;
        cuvs_last_n_results   = ss->n_results;
        cuvs_last_k_requested = k;
        cuvs_last_index_oid   = index_oid;
        cuvs_last_metric      = metric;
        cuvs_last_dim         = dim;
        ss->cur = 0;

        if (cuvs_debug)
            ereport(NOTICE,
                    (errmsg("pg_cuvs: ivfpq scan oid=%u dim=%d n_probes=%u "
                            "k=%d n=%d latency_us=%u",
                            (uint32_t)index_oid, dim, n_probes,
                            k, ss->n_results, latency_us)));
    }

    if (ss->cur >= ss->n_results)
        return false;

    uint64_t tid = ss->tids[ss->cur];
    uint32_t blk;
    uint16_t offset;
    cuvs_tid_decode(tid, &blk, &offset);

    ItemPointerSet(&scan->xs_heaptid, blk, offset);
    scan->xs_recheck = true;

    if (scan->numberOfOrderBys > 0 && scan->xs_orderbyvals != NULL)
    {
        scan->xs_orderbyvals[0]  = Float8GetDatum((double) ss->distances[ss->cur]);
        scan->xs_orderbynulls[0] = false;
        scan->xs_recheckorderby  = false;
        for (int i = 1; i < scan->numberOfOrderBys; i++)
            scan->xs_orderbynulls[i] = true;
    }
    ss->cur++;
    return true;
}

/* ----------------------------------------------------------------
 * Index AM handler
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(cuvsamhandler);
Datum
cuvsamhandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies      = 0;
    amroutine->amsupport         = 1;
    amroutine->amcanmulticol     = false;
    amroutine->amsearcharray     = false;
    amroutine->amcanorderbyop    = true;
    amroutine->amoptionalkey     = true;

    amroutine->ambuild           = cuvs_ambuild;
    amroutine->ambuildempty      = cuvs_ambuildempty;
    amroutine->aminsert          = cuvs_aminsert;     /* appends pending delta */
    amroutine->ambulkdelete      = cuvs_ambulkdelete; /* tombstone or stale fallback */
    amroutine->amvacuumcleanup   = cuvs_amvacuumcleanup;

    amroutine->ambeginscan       = cuvs_beginscan;
    amroutine->amrescan          = cuvs_rescan;
    amroutine->amgettuple        = cuvs_gettuple;
    amroutine->amendscan         = cuvs_endscan;
    amroutine->amcostestimate    = cuvsamcostestimate;
    amroutine->amoptions         = cuvs_cagra_amoptions; /* WITH (index_dir) */

    PG_RETURN_POINTER(amroutine);
}

/* ----------------------------------------------------------------
 * 3P: IVF-PQ AM handler
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(ivfpqamhandler);
Datum
ivfpqamhandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies      = 0;
    amroutine->amsupport         = 1;
    amroutine->amcanmulticol     = false;
    amroutine->amsearcharray     = false;
    amroutine->amcanorderbyop    = true;
    amroutine->amoptionalkey     = true;

    amroutine->ambuild           = ivfpq_ambuild;
    amroutine->ambuildempty      = ivfpq_ambuildempty;
    amroutine->aminsert          = ivfpq_aminsert;
    amroutine->ambulkdelete      = cuvs_ambulkdelete;
    amroutine->amvacuumcleanup   = cuvs_amvacuumcleanup;

    amroutine->ambeginscan       = cuvs_beginscan;
    amroutine->amrescan          = ivfpq_rescan;
    amroutine->amgettuple        = ivfpq_gettuple;
    amroutine->amendscan         = cuvs_endscan;
    amroutine->amcostestimate    = cuvsamcostestimate;
    amroutine->amoptions         = cuvs_ivfpq_amoptions;

    PG_RETURN_POINTER(amroutine);
}

/* ----------------------------------------------------------------
 * pg_cuvs_reset_circuit(index_name text) — re-enable GPU routing
 * after circuit breaker tripped (FALLBACK-04 in SPEC.md).
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_cuvs_reset_circuit);
Datum
pg_cuvs_reset_circuit(PG_FUNCTION_ARGS)
{
    Oid index_oid = PG_GETARG_OID(0);

    cuvs_circuit_reset((uint32_t)index_oid);

    ereport(NOTICE,
            (errmsg("pg_cuvs: circuit breaker reset for index oid %u",
                    index_oid)));

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * pg_cuvs_last_search_* — process-local last-scan stats.
 * Returns the stats from the most recent successful cagra index scan
 * in this backend. NULL if no scan has happened yet.
 * Use these for scriptable inspection; set cuvs.debug=on to also see
 * them inline with EXPLAIN VERBOSE output via NOTICE.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_cuvs_last_search_latency_us);
Datum
pg_cuvs_last_search_latency_us(PG_FUNCTION_ARGS)
{
    if (cuvs_last_n_results < 0)
        PG_RETURN_NULL();
    PG_RETURN_INT32((int32)cuvs_last_latency_us);
}

PG_FUNCTION_INFO_V1(pg_cuvs_last_search_n_results);
Datum
pg_cuvs_last_search_n_results(PG_FUNCTION_ARGS)
{
    if (cuvs_last_n_results < 0)
        PG_RETURN_NULL();
    PG_RETURN_INT32(cuvs_last_n_results);
}

PG_FUNCTION_INFO_V1(pg_cuvs_last_search_k);
Datum
pg_cuvs_last_search_k(PG_FUNCTION_ARGS)
{
    if (cuvs_last_n_results < 0)
        PG_RETURN_NULL();
    PG_RETURN_INT32(cuvs_last_k_requested);
}

PG_FUNCTION_INFO_V1(pg_cuvs_last_search_index);
Datum
pg_cuvs_last_search_index(PG_FUNCTION_ARGS)
{
    if (cuvs_last_n_results < 0)
        PG_RETURN_NULL();
    PG_RETURN_OID(cuvs_last_index_oid);
}

PG_FUNCTION_INFO_V1(pg_cuvs_last_search_metric);
Datum
pg_cuvs_last_search_metric(PG_FUNCTION_ARGS)
{
    if (cuvs_last_n_results < 0)
        PG_RETURN_NULL();
    const char *name;
    switch (cuvs_last_metric)
    {
        case CUVS_METRIC_L2:     name = "l2";     break;
        case CUVS_METRIC_COSINE: name = "cosine"; break;
        case CUVS_METRIC_IP:     name = "ip";     break;
        default:                 name = "unknown"; break;
    }
    PG_RETURN_TEXT_P(cstring_to_text(name));
}

/* ----------------------------------------------------------------
 * pg_cuvs_gpu_search_stats() — set-returning backing function for the
 * pg_stat_gpu_search view. Queries the sidecar daemon (the cross-backend
 * source of truth) for per-index search stats in the current database.
 *
 * Daemon-down policy: if the daemon is unreachable, return ZERO rows (not
 * an error). pg_stat_* views are polled by monitoring on tight loops, so
 * the view must stay queryable while the daemon restarts. (See plan: a
 * future liveness column can distinguish "down" from "idle".)
 * ---------------------------------------------------------------- */
#define GPU_STATS_NCOLS 38

static const char *
cuvs_metric_name(uint32_t metric)
{
    switch (metric)
    {
        case CUVS_METRIC_L2:     return "l2";
        case CUVS_METRIC_COSINE: return "cosine";
        case CUVS_METRIC_IP:     return "ip";
        default:                 return "unknown";
    }
}

/* ----------------------------------------------------------------
 * Phase 3M: pg_cuvs_batch_search(rel regclass, queries vector[], k int)
 *   RETURNS TABLE(query_idx int, ctid tid, distance float4)
 *
 * Sends Q queries to the daemon in one IPC round-trip (one batched GPU
 * dispatch) and returns up to K = min(k, n_vecs) neighbors per query. Mirrors
 * the single-query semantics: raw ctids + daemon distance, no internal heap
 * visibility filtering (the caller JOINs on ctid for MVCC, exactly like the AM
 * scan sets xs_recheck). Honors cuvs.search_mode / cuvs.bf_precision, so it
 * works in brute_force mode too once a .vectors sidecar exists.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(pg_cuvs_batch_search);
Datum
pg_cuvs_batch_search(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext   per_query_ctx, oldcontext;

    Oid        table_oid = PG_GETARG_OID(0);
    ArrayType *qarr      = PG_GETARG_ARRAYTYPE_P(1);
    int        k         = PG_GETARG_INT32(2);

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    if (k < 1 || k > 2000)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("pg_cuvs_batch_search: k must be between 1 and 2000")));

    /* --- Resolve the USING cagra index on the table. --- */
    Relation  heapRel = table_open(table_oid, AccessShareLock);
    List     *indexes = RelationGetIndexList(heapRel);
    Oid       cagra_am = get_am_oid("cagra", true);
    Oid       index_oid = InvalidOid;
    ListCell *lc;

    foreach(lc, indexes)
    {
        Oid       io = lfirst_oid(lc);
        Relation  ir = index_open(io, AccessShareLock);
        bool      is_cagra = (OidIsValid(cagra_am) && ir->rd_rel->relam == cagra_am);
        index_close(ir, AccessShareLock);
        if (is_cagra) { index_oid = io; break; }
    }
    list_free(indexes);
    if (!OidIsValid(index_oid))
    {
        table_close(heapRel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pg_cuvs_batch_search: no \"USING cagra\" index on relation \"%s\"",
                        get_rel_name(table_oid))));
    }

    {
        Relation indexRel = index_open(index_oid, AccessShareLock);
        uint32_t metric   = cuvs_index_metric(indexRel);
        index_close(indexRel, AccessShareLock);
        table_close(heapRel, AccessShareLock);

        /* --- Deconstruct queries vector[] into a row-major float buffer. --- */
        Oid    elemtype = ARR_ELEMTYPE(qarr);
        int16  elmlen;
        bool   elmbyval;
        char   elmalign;
        Datum *elems;
        bool  *elnulls;
        int    Q;
        int    dim = 0;
        float *qbuf = NULL;

        get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
        deconstruct_array(qarr, elemtype, elmlen, elmbyval, elmalign,
                          &elems, &elnulls, &Q);

        if (Q < 1)
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_EXCEPTION),
                     errmsg("pg_cuvs_batch_search: queries array is empty")));
        if (Q > cuvs_max_batch_queries)
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_cuvs_batch_search: %d queries exceeds cuvs.max_batch_queries (%d)",
                            Q, cuvs_max_batch_queries)));

        for (int q = 0; q < Q; q++)
        {
            PgVector *v;
            if (elnulls[q])
                ereport(ERROR,
                        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                         errmsg("pg_cuvs_batch_search: NULL query vector at index %d", q)));
            v = DatumGetPgVector(elems[q]);
            if (q == 0)
            {
                dim  = (int) v->dim;
                qbuf = palloc((size_t) Q * (size_t) dim * sizeof(float));
            }
            else if ((int) v->dim != dim)
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_EXCEPTION),
                         errmsg("pg_cuvs_batch_search: query %d has dim %d, expected %d",
                                q, (int) v->dim, dim)));
            memcpy(qbuf + (size_t) q * dim, v->x, (size_t) dim * sizeof(float));
        }

        /* --- One IPC round-trip; honors search_mode / bf_precision. --- */
        {
            uint64_t *tids = palloc((size_t) Q * (size_t) k * sizeof(uint64_t));
            float    *dists = palloc((size_t) Q * (size_t) k * sizeof(float));
            uint32_t  Kout = 0, latency_us = 0;
            int rc = cuvs_ipc_search_batch(
                cuvs_socket_path, (uint32_t) MyDatabaseId, (uint32_t) index_oid,
                qbuf, (uint32_t) Q, dim, k, metric,
                (uint32_t) cuvs_shard_overfetch, cuvs_parallel_fanout ? 1 : 0,
                (uint32_t) cuvs_search_mode, (uint32_t) cuvs_bf_precision,
                tids, dists, &Kout, &latency_us);

            if (rc != CUVS_STATUS_OK)
            {
                switch (rc)
                {
                    case CUVS_STATUS_NO_VECTORS:
                        ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_cuvs_batch_search: brute_force requires a .vectors "
                                        "sidecar that is missing or stale for this index"),
                                 errhint("REINDEX the index, or SET cuvs.search_mode='cagra'.")));
                        break;
                    case CUVS_STATUS_DIM_MISMATCH:
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_EXCEPTION),
                                 errmsg("pg_cuvs_batch_search: query dim %d does not match the "
                                        "cagra index dimension", dim)));
                        break;
                    case CUVS_STATUS_METRIC_MISMATCH:
                        ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_cuvs_batch_search: index built with a different metric "
                                        "than this query's operator class"),
                                 errhint("REINDEX the index.")));
                        break;
                    case CUVS_STATUS_STALE:
                        ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_cuvs_batch_search: cagra index is stale (writes since build)"),
                                 errhint("REINDEX the index.")));
                        break;
                    case CUVS_STATUS_UNAVAILABLE:
                        ereport(ERROR,
                                (errcode(ERRCODE_CONNECTION_FAILURE),
                                 errmsg("pg_cuvs_batch_search: GPU daemon unavailable")));
                        break;
                    case CUVS_STATUS_NOT_FOUND:
                        ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_cuvs_batch_search: cagra index not loaded on the GPU daemon"),
                                 errhint("REINDEX the index to reload it on the daemon.")));
                        break;
                    default:
                        ereport(ERROR,
                                (errcode(ERRCODE_INTERNAL_ERROR),
                                 errmsg("pg_cuvs_batch_search: batch search failed (status %d)", rc)));
                        break;
                }
            }

            /* --- Materialize (query_idx, ctid, distance) rows. --- */
            per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
            oldcontext = MemoryContextSwitchTo(per_query_ctx);
            tupstore = tuplestore_begin_heap(true, false, work_mem);
            rsinfo->returnMode = SFRM_Materialize;
            rsinfo->setResult  = tupstore;
            rsinfo->setDesc    = tupdesc;
            MemoryContextSwitchTo(oldcontext);

            for (int q = 0; q < Q; q++)
            {
                for (uint32_t j = 0; j < Kout; j++)
                {
                    uint64_t        tid = tids[(size_t) q * Kout + j];
                    uint32_t        blk;
                    uint16_t        off;
                    ItemPointerData iptr;
                    Datum           values[3];
                    bool            isnull[3] = {false, false, false};

                    if (tid == 0)
                        continue;   /* sentinel: this query had fewer than K neighbors */

                    cuvs_tid_decode(tid, &blk, &off);
                    ItemPointerSet(&iptr, blk, off);
                    values[0] = Int32GetDatum(q);
                    values[1] = PointerGetDatum(&iptr);
                    values[2] = Float4GetDatum(dists[(size_t) q * Kout + j]);
                    tuplestore_putvalues(tupstore, tupdesc, values, isnull);
                }
            }
        }
    }

    return (Datum) 0;
}

PG_FUNCTION_INFO_V1(pg_cuvs_gpu_search_stats);
Datum
pg_cuvs_gpu_search_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc       tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext   per_query_ctx;
    MemoryContext   oldcontext;
    CuvsIndexStats  stats[CUVS_MAX_TRACKED_INDEXES];
    int             n = 0;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    /* index_oid == 0 -> all resident indexes for this database. A daemon-down
     * result (UNAVAILABLE) leaves n == 0, producing an empty (not failed) set. */
    cuvs_ipc_stats(cuvs_socket_path, (uint32_t) MyDatabaseId, 0,
                   stats, CUVS_MAX_TRACKED_INDEXES, &n);

    for (int i = 0; i < n; i++)
    {
        CuvsIndexStats *s = &stats[i];
        Datum   values[GPU_STATS_NCOLS];
        bool    nulls[GPU_STATS_NCOLS];
        char   *relname;
        double  avg_us;

        memset(nulls, 0, sizeof(nulls));

        values[0] = ObjectIdGetDatum((Oid) s->db_oid);
        values[1] = ObjectIdGetDatum((Oid) s->index_oid);

        relname = get_rel_name((Oid) s->index_oid);
        if (relname)
            values[2] = CStringGetTextDatum(relname);
        else
            nulls[2] = true;   /* index dropped since the daemon loaded it */

        values[3] = Int32GetDatum((int32) s->dim);
        values[4] = CStringGetTextDatum(cuvs_metric_name(s->metric));
        values[5] = Int64GetDatum(s->n_vecs);
        values[6] = Int64GetDatum((int64) s->vram_bytes);
        values[7] = BoolGetDatum(s->resident != 0);
        values[8] = Int64GetDatum((int64) s->search_count);
        values[9] = Int64GetDatum((int64) s->error_count);

        avg_us = (s->search_count > 0)
            ? (double) s->total_latency_us / (double) s->search_count
            : 0.0;
        values[10] = Float8GetDatum(avg_us);
        values[11] = Int32GetDatum((int32) s->p50_us);
        values[12] = Int32GetDatum((int32) s->p95_us);
        values[13] = Int32GetDatum((int32) s->p99_us);
        values[14] = CStringGetTextDatum(cuvs_status_str((int) s->last_status));

        if (s->last_error[0] != '\0')
            values[15] = CStringGetTextDatum(s->last_error);
        else
            nulls[15] = true;

        if (s->last_search_at != 0)
            values[16] = TimestampTzGetDatum(time_t_to_timestamptz((pg_time_t) s->last_search_at));
        else
            nulls[16] = true;

        values[17] = Int32GetDatum((int32) s->last_requested_k);
        values[18] = Int32GetDatum((int32) s->last_returned_k);
        values[19] = BoolGetDatum(s->stale != 0);
        if (s->stale_since != 0)
            values[20] = TimestampTzGetDatum(time_t_to_timestamptz((pg_time_t) s->stale_since));
        else
            nulls[20] = true;

        values[21] = Int64GetDatum(s->delta_rows);
        values[22] = Int64GetDatum((int64) s->delta_generation);
        values[23] = Int64GetDatum((int64) s->delta_vram_bytes);
        values[24] = Int64GetDatum((int64) s->delta_merged_count);
        values[25] = CStringGetTextDatum(
            s->delta_search_mode == 2 ? "gpu" :
            s->delta_search_mode == 1 ? "cpu" : "none");

        /* Phase 3D: warmup stats */
        {
            static const char *warmup_names[] = {
                "hot", "cold", "queued", "downloading", "loading", "failed"
            };
            values[26] = CStringGetTextDatum(
                s->warmup_state < 6 ? warmup_names[s->warmup_state] : "unknown");
        }
        if (s->last_warmup_at != 0)
            values[27] = TimestampTzGetDatum(
                time_t_to_timestamptz((pg_time_t) s->last_warmup_at));
        else
            nulls[27] = true;
        values[28] = Int32GetDatum((int32) s->warmup_duration_ms);
        values[29] = Int64GetDatum((int64) s->download_count);
        values[30] = Int64GetDatum((int64) s->cache_miss_count);
        /* Phase 3E: GPU device. For a sharded index the logical row has no
         * single device (gpu_device_id == 0xFFFFFFFF) -> NULL; per-shard GPUs
         * are in pg_stat_gpu_shards. */
        if (s->gpu_device_id != 0xFFFFFFFF)
            values[31] = Int32GetDatum((int32) s->gpu_device_id);
        else
            nulls[31] = true;

        /* Phase 3F: shard count (0/1 = unsharded). */
        values[32] = Int32GetDatum((int32) s->shard_count);

        /* Phase 3I-1 / 3L / 3P / 3O / ADR-064: last search mode for this index.
         * Values: 0=gpu_cagra, 1=cpu_hnsw, 2=cpu_fallback, 3=brute_force,
         *         4=cagra_prefilter, 5=ivfpq, 6=stream_bf */
        values[33] = CStringGetTextDatum(
            s->search_mode == 1 ? "cpu_hnsw" :
            s->search_mode == 2 ? "cpu_fallback" :
            s->search_mode == 3 ? "brute_force" :
            s->search_mode == 4 ? "cagra_prefilter" :
            s->search_mode == 5 ? "ivfpq" :
            s->search_mode == 6 ? "stream_bf" : "gpu_cagra");

        /* Phase 3L-9: coalesced brute_force micro-batch dispatch count. */
        values[34] = Int64GetDatum((int64) s->bf_batch_count);

        /* Phase 4C: streaming update and compaction observability. */
        values[35] = Int64GetDatum(s->n_extended);
        values[36] = Int64GetDatum((int64) s->compact_count);
        if (s->last_compact_at != 0)
            values[37] = TimestampTzGetDatum(
                time_t_to_timestamptz((pg_time_t) s->last_compact_at));
        else
            nulls[37] = true;

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/* ----------------------------------------------------------------
 * pg_cuvs_gpu_cache_stats() — daemon-global VRAM tiered-cache counters,
 * backing the pg_stat_gpu_cache view. One row normally; zero rows when the
 * daemon is unreachable (same convention as pg_stat_gpu_search).
 * ---------------------------------------------------------------- */
#define GPU_CACHE_NCOLS 11

PG_FUNCTION_INFO_V1(pg_cuvs_gpu_cache_stats);
Datum
pg_cuvs_gpu_cache_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext    per_query_ctx;
    MemoryContext    oldcontext;
    CuvsCacheStats   cs;
    int              rc;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    {
        CuvsCacheStats rows[CUVS_MAX_GPUS];
        int n_rows = 0;
        rc = cuvs_ipc_cache_stats(cuvs_socket_path, rows, CUVS_MAX_GPUS, &n_rows);
        if (rc == CUVS_STATUS_OK)
        {
            for (int i = 0; i < n_rows; i++)
            {
                CuvsCacheStats *cs = &rows[i];
                Datum values[GPU_CACHE_NCOLS];
                bool  nulls[GPU_CACHE_NCOLS];

                memset(nulls, 0, sizeof(nulls));
                values[0] = Int32GetDatum((int32) cs->gpu_device_id);
                values[1] = Int64GetDatum((int64) cs->hits);
                values[2] = Int64GetDatum((int64) cs->misses);
                values[3] = Int64GetDatum((int64) cs->evictions);
                values[4] = Int64GetDatum((int64) cs->reloads);
                values[5] = Int64GetDatum((int64) cs->persist_failures);
                values[6] = Int32GetDatum((int32) cs->resident_count);
                values[7] = Int64GetDatum((int64) (cs->vram_used_bytes / (1024 * 1024)));
                values[8] = Int64GetDatum((int64) (cs->vram_budget_bytes / (1024 * 1024)));
                /* Phase 3L: resident brute-force index VRAM + precision. */
                values[9] = Int64GetDatum((int64) (cs->bf_vram_bytes / (1024 * 1024)));
                values[10] = CStringGetTextDatum(cs->bf_precision == 1 ? "float16" : "float32");
                tuplestore_putvalues(tupstore, tupdesc, values, nulls);
            }
        }
    }
    /* daemon down (UNAVAILABLE) -> empty result, not an error */

    return (Datum) 0;
}

/* ----------------------------------------------------------------
 * pg_cuvs_gpu_shard_stats() — per-shard rows for sharded CAGRA indexes in this
 * database (Phase 3F), backing the pg_stat_gpu_shards view. Zero rows when the
 * daemon is unreachable or no sharded index exists (same convention as the
 * other pg_stat_gpu_* views).
 * ---------------------------------------------------------------- */
#define GPU_SHARD_NCOLS 12
#define GPU_SHARD_MAXROWS 1024   /* sum of shards across all sharded indexes */

PG_FUNCTION_INFO_V1(pg_cuvs_gpu_shard_stats);
Datum
pg_cuvs_gpu_shard_stats(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext    per_query_ctx;
    MemoryContext    oldcontext;
    CuvsShardStats  *rows;
    int              n = 0;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    rows = (CuvsShardStats *) palloc(GPU_SHARD_MAXROWS * sizeof(CuvsShardStats));
    cuvs_ipc_shard_stats(cuvs_socket_path, (uint32_t) MyDatabaseId, 0,
                         rows, GPU_SHARD_MAXROWS, &n);

    for (int i = 0; i < n; i++)
    {
        CuvsShardStats *r = &rows[i];
        Datum   values[GPU_SHARD_NCOLS];
        bool    nulls[GPU_SHARD_NCOLS];
        char   *relname;

        memset(nulls, 0, sizeof(nulls));
        values[0] = ObjectIdGetDatum((Oid) r->db_oid);
        values[1] = ObjectIdGetDatum((Oid) r->index_oid);

        relname = get_rel_name((Oid) r->index_oid);
        if (relname)
            values[2] = CStringGetTextDatum(relname);
        else
            nulls[2] = true;

        values[3] = Int32GetDatum((int32) r->shard_id);
        values[4] = Int32GetDatum((int32) r->gpu_device_id);
        values[5] = Int64GetDatum(r->n_vecs);
        values[6] = Int64GetDatum(r->tid_offset);
        values[7] = Int64GetDatum((int64) (r->vram_bytes / (1024 * 1024)));
        values[8] = Int64GetDatum((int64) r->search_count);
        values[9] = Int64GetDatum((int64) r->error_count);
        values[10] = BoolGetDatum(r->resident != 0);
        values[11] = CStringGetTextDatum(cuvs_status_str((int) r->last_status));

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/* ----------------------------------------------------------------
 * pg_cuvs_gc_orphans(do_delete boolean DEFAULT false) — ADR-046
 *
 * Backend-driven reconciliation of the daemon's index_dir against the
 * PostgreSQL catalog. The daemon is a standalone sidecar (ADR-002) with no
 * catalog access, so it cannot tell a live artifact from one left behind by a
 * daemon-down DROP or a DROP DATABASE; on restart startup_load_indexes() reloads
 * them as zombies (VRAM + disk leak). A backend HAS the catalog, so it does the
 * reconciliation here.
 *
 * Each artifact identity ("<db>_<idx>.cagra" unsharded, or "<db>_<idx>.shards"
 * sharded) is classified:
 *   - db_oid == MyDatabaseId, index OID absent from pg_class -> missing_in_catalog
 *   - db_oid not in pg_database                              -> dead_database
 *   - db_oid is some OTHER live database                     -> unverifiable_other_db
 *     (this backend cannot read that DB's catalog -> conservatively skipped;
 *      run the function in that database to reclaim it).
 * Live indexes in the current DB are not reported.
 *
 * do_delete=false (default): dry-run, reports candidates, removes nothing.
 * do_delete=true: for each orphan, cuvs_ipc_drop() (daemon frees VRAM + unlinks
 *   the whole artifact family); if the daemon is unreachable, the backend
 *   directly unlinks every "<db>_<idx>.*" file in index_dir.
 * ---------------------------------------------------------------- */
#define GC_ORPHANS_NCOLS 4

/* Direct-unlink fallback (daemon down): remove every "<db>_<idx>." prefixed file
 * in idir. The trailing dot makes "12_3." not match "12_34.cagra". Returns the
 * number unlinked, or -1 if the directory cannot be opened. */
static int
gc_unlink_family(const char *idir, uint32_t db_oid, uint32_t index_oid)
{
    char            prefix[64];
    char            path[MAXPGPATH];
    DIR            *dir;
    struct dirent  *ent;
    size_t          plen;
    int             removed = 0;

    snprintf(prefix, sizeof(prefix), "%u_%u.", db_oid, index_oid);
    plen = strlen(prefix);

    dir = opendir(idir);
    if (dir == NULL)
        return -1;
    while ((ent = readdir(dir)) != NULL)
    {
        if (strncmp(ent->d_name, prefix, plen) == 0)
        {
            snprintf(path, sizeof(path), "%s/%s", idir, ent->d_name);
            if (unlink(path) == 0)
                removed++;
        }
    }
    closedir(dir);
    return removed;
}

/* 3Q: manual COMPACT trigger — pg_cuvs_compact(index_rel regclass) */
PG_FUNCTION_INFO_V1(pg_cuvs_compact);
Datum
pg_cuvs_compact(PG_FUNCTION_ARGS)
{
    Oid      index_oid = PG_GETARG_OID(0);
    Relation indexRel;
    int      rc;

    indexRel = try_index_open(index_oid, AccessShareLock);
    if (!indexRel)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("index %u does not exist", index_oid)));

    rc = cuvs_ipc_compact(cuvs_socket_path,
                          (uint32_t) MyDatabaseId,
                          (uint32_t) index_oid,
                          cuvs_resolve_index_dir_rel(indexRel));

    index_close(indexRel, AccessShareLock);

    if (rc != CUVS_STATUS_OK && rc != CUVS_STATUS_NOT_FOUND)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_cuvs_compact: daemon returned status %d", rc)));

    PG_RETURN_VOID();
}

/* pg_cuvs_set_vram_budget(budget_bytes bigint) — runtime VRAM cap override.
 * 0 = unlimited. Intended for testing and capacity management. */
PG_FUNCTION_INFO_V1(pg_cuvs_set_vram_budget);
Datum
pg_cuvs_set_vram_budget(PG_FUNCTION_ARGS)
{
    int64 budget_bytes = PG_GETARG_INT64(0);
    int   rc;

    rc = cuvs_ipc_set_vram_budget(cuvs_socket_path, (int64_t)budget_bytes);
    if (rc != CUVS_STATUS_OK && rc != CUVS_STATUS_UNAVAILABLE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_cuvs_set_vram_budget: daemon returned status %d", rc)));
    PG_RETURN_VOID();
}

/* pg_cuvs_eat_vram(leave_bytes bigint) — pre-allocate VRAM leaving only
 * leave_bytes free.  Forces physical CUDA OOM on the next large GPU op,
 * bypassing the budget-check path.  Test-only.  Device 0. */
PG_FUNCTION_INFO_V1(pg_cuvs_eat_vram);
Datum
pg_cuvs_eat_vram(PG_FUNCTION_ARGS)
{
    int64 leave_bytes = PG_GETARG_INT64(0);
    int   rc;

    rc = cuvs_ipc_eat_vram(cuvs_socket_path, (int64_t)leave_bytes, 0);
    if (rc != CUVS_STATUS_OK && rc != CUVS_STATUS_UNAVAILABLE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_cuvs_eat_vram: daemon returned status %d", rc)));
    PG_RETURN_VOID();
}

/* pg_cuvs_free_vram() — release the allocation from pg_cuvs_eat_vram.
 * Best-effort cleanup: never throws — failure to free is not a fatal error. */
PG_FUNCTION_INFO_V1(pg_cuvs_free_vram);
Datum
pg_cuvs_free_vram(PG_FUNCTION_ARGS)
{
    (void) cuvs_ipc_free_vram(cuvs_socket_path, 0);
    PG_RETURN_VOID();
}

/* pg_cuvs_inject_extend_oom(enable int) — arm (1) or disarm (0) synthetic OOM
 * injection in cuvs_cagra_extend.  When armed, the next extend throws bad_alloc,
 * exercises _pr.poison() → BUILD_FAILED → delta fallback.  Self-clears on fire.
 * Test-only; no-op if daemon is unavailable. */
PG_FUNCTION_INFO_V1(pg_cuvs_inject_extend_oom);
Datum
pg_cuvs_inject_extend_oom(PG_FUNCTION_ARGS)
{
    int32 enable = PG_GETARG_INT32(0);
    int   rc;

    rc = cuvs_ipc_inject_extend_oom(cuvs_socket_path, (int)enable);
    if (rc != CUVS_STATUS_OK && rc != CUVS_STATUS_UNAVAILABLE)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("pg_cuvs_inject_extend_oom: daemon returned status %d", rc)));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pg_cuvs_gc_orphans);
Datum
pg_cuvs_gc_orphans(PG_FUNCTION_ARGS)
{
    /* (db, idx) identity collected from a ".cagra"/".shards" anchor file. */
    typedef struct { uint32_t db; uint32_t idx; } GcId;
    bool             do_delete = PG_GETARG_BOOL(0);
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext    per_query_ctx;
    MemoryContext    oldcontext;
    const char      *idir;
    DIR             *dir;
    struct dirent   *ent;
    GcId            *ids = NULL;
    int              n_ids = 0, cap = 0;

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext = MemoryContextSwitchTo(per_query_ctx);
    tupstore = tuplestore_begin_heap(true, false, work_mem);
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;
    MemoryContextSwitchTo(oldcontext);

    idir = get_index_dir();

    /* Pass 1: collect identity anchors, then close the dir BEFORE any unlink.
     * Mutating the directory while readdir() is mid-stream is unspecified by
     * POSIX, so deletion must happen after the scan completes. */
    dir = opendir(idir);
    if (dir == NULL)
    {
        ereport(NOTICE,
                (errmsg("pg_cuvs_gc_orphans: index_dir \"%s\" not accessible (%m); nothing to do",
                        idir)));
        return (Datum) 0;
    }
    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        char     tail[16] = {0};

        /* Identity anchors only: unsharded ".cagra" or sharded ".shards". */
        if (cuvs_parse_index_filename(ent->d_name, &db_oid, &index_oid) != 0)
        {
            if (!(sscanf(ent->d_name, "%u_%u.%15s", &db_oid, &index_oid, tail) == 3
                  && strcmp(tail, "shards") == 0))
                continue;
        }
        if (n_ids == cap)
        {
            cap = cap ? cap * 2 : 16;
            ids = ids ? repalloc(ids, cap * sizeof(GcId))
                      : palloc(cap * sizeof(GcId));
        }
        ids[n_ids].db = db_oid;
        ids[n_ids].idx = index_oid;
        n_ids++;
    }
    closedir(dir);

    /* Pass 2: classify against catalog authority, act, emit. */
    for (int i = 0; i < n_ids; i++)
    {
        uint32_t    db_oid = ids[i].db;
        uint32_t    index_oid = ids[i].idx;
        const char *reason;
        const char *action;
        bool        is_orphan;
        Datum       values[GC_ORPHANS_NCOLS];
        bool        nulls[GC_ORPHANS_NCOLS];

        if (db_oid == (uint32_t) MyDatabaseId)
        {
            HeapTuple tup = SearchSysCache1(RELOID, ObjectIdGetDatum((Oid) index_oid));
            if (HeapTupleIsValid(tup))
            {
                ReleaseSysCache(tup);
                continue;   /* live index in this DB — not an orphan */
            }
            reason = "missing_in_catalog";
            is_orphan = true;
        }
        else
        {
            char *dbname = get_database_name((Oid) db_oid);
            if (dbname == NULL)
            {
                reason = "dead_database";
                is_orphan = true;
            }
            else
            {
                pfree(dbname);
                reason = "unverifiable_other_db";   /* conservatively keep */
                is_orphan = false;
            }
        }

        if (!is_orphan)
            action = "skipped";
        else if (!do_delete)
            action = "would_delete";
        else
        {
            int rc = cuvs_ipc_drop(cuvs_socket_path, db_oid, index_oid);
            if (rc == CUVS_STATUS_OK)
                action = "deleted";                 /* daemon freed VRAM + unlinked */
            else
            {
                /* daemon down/unreachable — reclaim disk directly */
                int n = gc_unlink_family(idir, db_oid, index_oid);
                action = (n >= 0) ? "deleted" : "delete_failed";
            }
        }

        memset(nulls, 0, sizeof(nulls));
        values[0] = ObjectIdGetDatum((Oid) db_oid);
        values[1] = ObjectIdGetDatum((Oid) index_oid);
        values[2] = CStringGetTextDatum(reason);
        values[3] = CStringGetTextDatum(action);
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/* ----------------------------------------------------------------
 * cuvs_filtered_knn — D-wedge spike: Option B Function API
 *
 * cuvs_filtered_knn(index_rel regclass,
 *                   query     vector,
 *                   filter_tids bigint[],   -- sorted encoded TIDs; NULL = no filter
 *                   k         int)
 * RETURNS TABLE (ctid tid, distance float4)
 *
 * Bypasses the AM scan path so non-index quals (e.g. tenant_id = $1) can
 * be passed as an explicit TID set.  The daemon post-filters BF results to
 * only the provided TIDs.  Degrades to unfiltered BF when filter_tids is
 * NULL or empty.
 * ---------------------------------------------------------------- */
PG_FUNCTION_INFO_V1(cuvs_filtered_knn);
Datum
cuvs_filtered_knn(PG_FUNCTION_ARGS)
{
    ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    TupleDesc        tupdesc;
    Tuplestorestate *tupstore;
    MemoryContext    per_query_ctx, oldcontext;

    Oid        index_oid   = PG_GETARG_OID(0);
    Datum      query_datum = PG_GETARG_DATUM(1);
    /* arg 2: filter_tids bigint[] — may be NULL */
    int        k = PG_GETARG_INT32(3);

    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("set-valued function called in context that cannot accept a set")));
    if (!(rsinfo->allowedModes & SFRM_Materialize))
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("materialize mode required, but it is not allowed in this context")));
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("return type must be a row type")));

    if (k < 1 || k > 2000)
        ereport(ERROR,
                (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                 errmsg("cuvs_filtered_knn: k must be between 1 and 2000")));

    /* --- Open index to validate and get metric. --- */
    Relation  indexRel = index_open(index_oid, AccessShareLock);
    Oid       cagra_am = get_am_oid("cagra", true);
    if (!OidIsValid(cagra_am) || indexRel->rd_rel->relam != cagra_am)
    {
        index_close(indexRel, AccessShareLock);
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("cuvs_filtered_knn: relation is not a \"USING cagra\" index")));
    }
    uint32_t metric = cuvs_index_metric(indexRel);
    index_close(indexRel, AccessShareLock);

    /* --- Query vector. --- */
    PgVector *qvec = DatumGetPgVector(query_datum);
    int       dim  = qvec->dim;

    /* --- Deconstruct filter_tids bigint[] if provided. --- */
    uint64_t *filter_arr = NULL;
    uint32_t  n_filter   = 0;
    if (!PG_ARGISNULL(2))
    {
        ArrayType *farr = PG_GETARG_ARRAYTYPE_P(2);
        int16  elmlen;
        bool   elmbyval;
        char   elmalign;
        Datum *elems;
        bool  *elnulls;
        int    nelems;

        get_typlenbyvalalign(INT8OID, &elmlen, &elmbyval, &elmalign);
        deconstruct_array(farr, INT8OID, elmlen, elmbyval, elmalign,
                          &elems, &elnulls, &nelems);
        if (nelems > 0)
        {
            filter_arr = palloc((size_t)nelems * sizeof(uint64_t));
            for (int i = 0; i < nelems; i++)
                filter_arr[i] = elnulls[i] ? 0 : (uint64_t) DatumGetInt64(elems[i]);
            n_filter = (uint32_t) nelems;
        }
    }

    /* --- IPC call. --- */
    /*
     * use_filter:    caller provided a non-empty TID whitelist.
     * use_prefilter: 3O GPU BITSET path when selectivity < filter_auto_threshold.
     * k_fetch:       D-wedge overfetches 4x; 3O pre-filter fetches exactly k.
     */
    {
        bool      use_filter    = (filter_arr != NULL && n_filter > 0);
        bool      use_prefilter = false;
        bool      use_stream_bf = false;

        if (use_filter &&
            (cuvs_filter_auto_threshold > 0.0 ||
             cuvs_stream_bf_selectivity_threshold > 0.0))
        {
            Oid heap_oid = IndexGetRelation((Oid) index_oid, true);
            if (OidIsValid(heap_oid))
            {
                Relation hr = try_relation_open(heap_oid, AccessShareLock);
                if (hr)
                {
                    double N = (double) hr->rd_rel->reltuples;
                    if (N > 0.0)
                    {
                        double sel = (double) n_filter / N;
                        /* ADR-064 streaming BF (out-of-core) takes precedence over
                         * the in-VRAM 3O prefilter when both thresholds fire. */
                        if (cuvs_stream_bf_selectivity_threshold > 0.0 &&
                            sel < cuvs_stream_bf_selectivity_threshold)
                            use_stream_bf = true;
                        else if (cuvs_filter_auto_threshold > 0.0 &&
                                 sel < cuvs_filter_auto_threshold)
                            use_prefilter = true;
                    }
                    relation_close(hr, AccessShareLock);
                }
            }
        }

        int       k_fetch    = (use_filter && !use_prefilter && !use_stream_bf)
                               ? Min(k * 4, 4000) : k;
        int       emit_limit;
        int       rc;
        int       n_results  = 0;
        uint32_t  latency_us = 0;
        uint64_t *tids_out   = palloc((size_t)k_fetch * sizeof(uint64_t));
        float    *dists_out  = palloc((size_t)k_fetch * sizeof(float));

        if (use_filter)
        {
            int delta_merged = 0;

            if (use_stream_bf)
            {
                rc = cuvs_ipc_search_stream_bf(
                    cuvs_socket_path,
                    (uint32_t) MyDatabaseId,
                    (uint32_t) index_oid,
                    qvec->x,
                    dim, k_fetch, metric,
                    filter_arr, n_filter,
                    cuvs_stream_bf_chunk_vectors,
                    tids_out, dists_out, &n_results, &latency_us);
                /* Sidecar / reverse map absent -> fall back to exact 3O prefilter
                 * (same k_fetch=k, no overfetch). */
                if (rc == CUVS_STATUS_NO_VECTORS)
                {
                    use_stream_bf = false;
                    use_prefilter = true;
                }
            }

            if (!use_stream_bf)
                rc = cuvs_ipc_search_filtered(
                    cuvs_socket_path,
                    (uint32_t) MyDatabaseId,
                    (uint32_t) index_oid,
                    qvec->x,
                    dim, k_fetch, metric,
                    1,   /* search_mode = brute_force */
                    (uint32_t) cuvs_bf_precision,
                    filter_arr, n_filter,
                    tids_out, dists_out, &n_results, &latency_us, &delta_merged,
                    (int) use_prefilter);

            /* Merge .delta pending-insert rows (filtered) and apply tombstones. */
            if (rc == CUVS_STATUS_OK && cuvs_max_delta_rows > 0)
            {
                int do_cpu = (cuvs_delta_search_mode == 1)
                          || (cuvs_delta_search_mode == 0 && !delta_merged);
                if (do_cpu)
                    cuvs_merge_delta_filtered(
                        (Oid) index_oid, qvec->x, dim, k, metric,
                        filter_arr, n_filter,
                        tids_out, dists_out, &n_results);
                cuvs_apply_tombstones_filtered(
                    (Oid) index_oid, tids_out, dists_out, &n_results);
            }
        }
        else
        {
            /* NULL or empty filter_tids → unfiltered BF */
            int delta_merged = 0;
            rc = cuvs_ipc_search(
                cuvs_socket_path,
                (uint32_t) MyDatabaseId,
                (uint32_t) index_oid,
                qvec->x,
                dim, k, metric,
                1,   /* shard_overfetch */
                0,   /* parallel_fanout */
                0,   /* use_cpu_hnsw */
                1,   /* search_mode = brute_force */
                (uint32_t) cuvs_bf_precision,
                0,   /* bf_batch_wait_us */
                tids_out, dists_out, &n_results,
                &latency_us, &delta_merged);
        }

        if (rc == CUVS_STATUS_CANCELED)
        {
            CHECK_FOR_INTERRUPTS();
            return (Datum) 0;
        }
        if (rc != CUVS_STATUS_OK)
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("cuvs_filtered_knn: search failed (status %d)", rc)));

        /* --- Materialize (ctid, distance) rows. --- */
        per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
        oldcontext = MemoryContextSwitchTo(per_query_ctx);
        tupstore = tuplestore_begin_heap(true, false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult  = tupstore;
        rsinfo->setDesc    = tupdesc;
        MemoryContextSwitchTo(oldcontext);

        emit_limit = Min(n_results, k);
        for (int i = 0; i < emit_limit; i++)
        {
            uint32_t        blk;
            uint16_t        off;
            ItemPointerData iptr;
            Datum           values[2];
            bool            isnull[2] = {false, false};

            if (tids_out[i] == 0)
                continue;

            cuvs_tid_decode(tids_out[i], &blk, &off);
            ItemPointerSet(&iptr, blk, off);
            values[0] = PointerGetDatum(&iptr);
            values[1] = Float4GetDatum(dists_out[i]);
            tuplestore_putvalues(tupstore, tupdesc, values, isnull);
        }
    }

    return (Datum) 0;
}

/* ================================================================
 * D-wedge spike: Option A — Custom Scan Node (ADR-063)
 *
 * GUC: cuvs.filtered_knn_hook = on  (default off)
 *
 * When on, set_rel_pathlist_hook fires for base relations that have:
 *   (1) a CAGRA index, (2) non-index quals, (3) an ORDER BY with a
 *   Const query vector.
 *
 * Execution:
 *   a. SeqScan heap with filter quals → sorted TID set
 *   b. cuvs_ipc_search_filtered (GPU BF post-filter)
 *   c. Return full heap tuples matched by result TIDs
 *
 * pgvector compatibility: transparent to standard SQL —
 *   SELECT ... WHERE tenant_id=5 ORDER BY emb <-> $q LIMIT k
 * ================================================================ */

/* ---- per-scan state ---- */
typedef struct CuvsFilteredScanState {
    CustomScanState css;
    Oid         index_oid;
    uint32_t    metric;
    Datum       query_datum;
    List       *filter_rinfos;  /* RestrictInfo* list from plan time */
    Oid         heap_oid;
    /* populated on first ExecCustomScan call */
    bool        built;
    uint64_t   *result_tids;
    float      *result_dists;
    int         n_results;
    int         cursor;
    Relation    heap_rel;
    /* heap-AM compatible slot for table_tuple_fetch_row_version */
    TupleTableSlot *heap_fetch_slot;
    /* timing: daemon-reported wall-clock latency of last GPU search (us) */
    uint32_t        last_latency_us;
} CuvsFilteredScanState;

/* ---- forward declarations ---- */
static Plan *cuvs_cs_create_plan(PlannerInfo *root, RelOptInfo *rel,
                                  CustomPath *best_path, List *tlist,
                                  List *clauses, List *custom_plans);
static Node *cuvs_cs_create_state(CustomScan *cscan);
static void  cuvs_cs_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *cuvs_cs_exec(CustomScanState *node);
static void  cuvs_cs_end(CustomScanState *node);
static void  cuvs_cs_rescan(CustomScanState *node);
static void  cuvs_cs_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static CustomPathMethods cuvs_filtered_path_methods = {
    .CustomName    = "CuvsFilteredScan",
    .PlanCustomPath = cuvs_cs_create_plan,
};
static CustomScanMethods cuvs_filtered_scan_methods = {
    .CustomName          = "CuvsFilteredScan",
    .CreateCustomScanState = cuvs_cs_create_state,
};
static CustomExecMethods cuvs_filtered_exec_methods = {
    .CustomName        = "CuvsFilteredScan",
    .BeginCustomScan   = cuvs_cs_begin,
    .ExecCustomScan    = cuvs_cs_exec,
    .EndCustomScan     = cuvs_cs_end,
    .ReScanCustomScan  = cuvs_cs_rescan,
    .ExplainCustomScan = cuvs_cs_explain,
};

/* ---- planner path → plan ---- */
static Plan *
cuvs_cs_create_plan(PlannerInfo *root, RelOptInfo *rel,
                    CustomPath *best_path, List *tlist,
                    List *clauses, List *custom_plans)
{
    CustomScan *cscan = makeNode(CustomScan);
    cscan->scan.plan.targetlist = tlist;
    cscan->scan.scanrelid       = best_path->path.parent->relid;
    cscan->custom_scan_tlist    = NIL;
    cscan->methods              = &cuvs_filtered_scan_methods;
    cscan->custom_private       = best_path->custom_private;
    return (Plan *) cscan;
}

/* ---- plan → exec state ---- */
static Node *
cuvs_cs_create_state(CustomScan *cscan)
{
    CuvsFilteredScanState *state = palloc0(sizeof(CuvsFilteredScanState));
    NodeSetTag(state, T_CustomScanState);
    state->css.methods = &cuvs_filtered_exec_methods;
    return (Node *) state;
}

/* ---- BeginCustomScan ---- */
static void
cuvs_cs_begin(CustomScanState *node, EState *estate, int eflags)
{
    CuvsFilteredScanState *state = (CuvsFilteredScanState *) node;
    CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

    List *priv = cscan->custom_private;

    /* custom_private: {index_oid_int, query_const, rinfos_list, heap_oid_int} */
    state->index_oid    = (Oid) intVal(linitial(priv));
    state->query_datum  = ((Const *) lsecond(priv))->constvalue;
    state->filter_rinfos = (List *) lthird(priv);
    state->heap_oid     = (Oid) intVal(lfourth(priv));
    state->built        = false;
    state->result_tids  = NULL;
    state->result_dists = NULL;
    state->n_results    = 0;
    state->cursor       = 0;

    state->heap_rel = table_open(state->heap_oid, AccessShareLock);

    /* Dedicated heap-AM slot for table_tuple_fetch_row_version.
     * We keep it separate from ss_ScanTupleSlot (virtual) to avoid
     * the PG type-check that fires when ExecInitScanTupleSlot replaces
     * the planner-derived virtual slot with the physical heap descriptor. */
    state->heap_fetch_slot = table_slot_create(state->heap_rel, NULL);

    /* Get metric from the CAGRA index. */
    {
        Relation indexRel = index_open(state->index_oid, AccessShareLock);
        state->metric = cuvs_index_metric(indexRel);
        index_close(indexRel, AccessShareLock);
    }
}

/* TID comparator for qsort */
static int
compare_tid_enc(const void *a, const void *b)
{
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

/* ---- Build TID set + GPU search (called on first ExecCustomScan) ---- */
static void
cuvs_cs_build(CuvsFilteredScanState *state, EState *estate)
{
    /* --- Step 1: seqscan heap, evaluate filter quals, collect TIDs. --- */
    List           *qual_exprs = NIL;
    ListCell       *lc;
    ExprState      *qual_state;
    ExprContext    *filter_ctx;
    TupleTableSlot *slot;
    int             cap   = 1024;
    int             ntids = 0;
    uint64_t       *tids;
    TableScanDesc   scan;

    foreach(lc, state->filter_rinfos)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        qual_exprs = lappend(qual_exprs, rinfo->clause);
    }

    /*
     * Use NULL parent so ExecInitQual does NOT embed the parent's virtual
     * ss_ScanTupleSlot pointer into EEOP_SCAN_FETCHSOME.  With a parent, the
     * slot-type optimization bakes the empty virtual slot into the ExprState;
     * every SCAN_VAR read then gets 0 from that slot (wrong).  With NULL,
     * the ExprState reads ecxt_scantuple dynamically at eval time, which is
     * exactly the heap-AM scan slot we supply below.
     */
    qual_state = ExecInitQual(qual_exprs, NULL);

    /* Standalone context to avoid polluting ps_ExprContext. */
    filter_ctx = CreateStandaloneExprContext();

    slot  = table_slot_create(state->heap_rel, NULL);
    tids  = palloc((size_t)cap * sizeof(uint64_t));

    scan = table_beginscan(state->heap_rel, GetTransactionSnapshot(), 0, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        ItemPointer iptr;
        uint64_t    enc;

        filter_ctx->ecxt_scantuple = slot;
        if (!ExecQual(qual_state, filter_ctx))
            continue;

        iptr = &slot->tts_tid;
        enc  = ((uint64_t) ItemPointerGetBlockNumber(iptr) << 16)
             | (uint64_t) ItemPointerGetOffsetNumber(iptr);
        if (ntids >= cap)
        {
            cap *= 2;
            tids = repalloc(tids, (size_t)cap * sizeof(uint64_t));
        }
        tids[ntids++] = enc;
    }
    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    FreeExprContext(filter_ctx, true);

    if (ntids > 1)
        qsort(tids, (size_t)ntids, sizeof(uint64_t), compare_tid_enc);

    /* --- Step 2: GPU BF search — 3O pre-filter or D-wedge based on selectivity. --- */
    PgVector *qvec = DatumGetPgVector(state->query_datum);
    int       dim  = qvec->dim;
    int       k    = (int) cuvs_k;

    bool use_prefilter = false;
    bool use_stream_bf = false;
    if (ntids > 0 && state->heap_rel->rd_rel->reltuples > 0.0
        && (cuvs_filter_auto_threshold > 0.0 ||
            cuvs_stream_bf_selectivity_threshold > 0.0))
    {
        double N   = (double) state->heap_rel->rd_rel->reltuples;
        double sel = (double) ntids / N;
        /* ADR-064 streaming BF takes precedence over 3O prefilter. */
        if (cuvs_stream_bf_selectivity_threshold > 0.0 &&
            sel < cuvs_stream_bf_selectivity_threshold)
            use_stream_bf = true;
        else if (cuvs_filter_auto_threshold > 0.0 &&
                 sel < cuvs_filter_auto_threshold)
            use_prefilter = true;
    }
    /* Always send k to daemon: daemon handles 4x D-wedge overfetch internally.
     * Result buffers are sized k; sending k*4 would overflow them. */

    state->result_tids  = palloc((size_t)k * sizeof(uint64_t));
    state->result_dists = palloc((size_t)k * sizeof(float));
    uint32_t latency_us = 0;

    int delta_merged = 0;
    int rc;
    if (use_stream_bf)
    {
        rc = cuvs_ipc_search_stream_bf(
            cuvs_socket_path,
            (uint32_t) MyDatabaseId,
            (uint32_t) state->index_oid,
            qvec->x, dim, k, state->metric,
            tids, (uint32_t) ntids,
            cuvs_stream_bf_chunk_vectors,
            state->result_tids, state->result_dists, &state->n_results,
            &latency_us);
        if (rc == CUVS_STATUS_NO_VECTORS)   /* sidecar absent -> exact 3O prefilter */
        {
            use_stream_bf = false;
            use_prefilter = true;
        }
    }
    if (!use_stream_bf)
        rc = cuvs_ipc_search_filtered(
            cuvs_socket_path,
            (uint32_t) MyDatabaseId,
            (uint32_t) state->index_oid,
            qvec->x, dim, k, state->metric,
            1,   /* brute_force */
            (uint32_t) cuvs_bf_precision,
            tids, (uint32_t) ntids,
            state->result_tids, state->result_dists, &state->n_results,
            &latency_us, &delta_merged, (int) use_prefilter);

    state->last_latency_us = latency_us;

    if (rc == CUVS_STATUS_OK && cuvs_max_delta_rows > 0)
    {
        int do_cpu = (cuvs_delta_search_mode == 1)
                  || (cuvs_delta_search_mode == 0 && !delta_merged);
        if (do_cpu)
            cuvs_merge_delta_filtered(
                state->index_oid, qvec->x, dim, k, state->metric,
                tids, (uint32_t) ntids,
                state->result_tids, state->result_dists, &state->n_results);
        cuvs_apply_tombstones_filtered(
            state->index_oid,
            state->result_tids, state->result_dists, &state->n_results);
    }

    pfree(tids);

    if (rc == CUVS_STATUS_CANCELED)
        CHECK_FOR_INTERRUPTS();
    else if (rc != CUVS_STATUS_OK)
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("cuvs filtered scan: search failed (status %d)", rc)));
    state->built  = true;
    state->cursor = 0;
}

/* ---- ExecCustomScan ---- */
static TupleTableSlot *
cuvs_cs_exec(CustomScanState *node)
{
    CuvsFilteredScanState *state = (CuvsFilteredScanState *) node;
    TupleTableSlot        *fetch = state->heap_fetch_slot;
    /* ps_ProjInfo was compiled against the virtual scan slot; use it as
     * the ecxt_scantuple so expression evaluation reads the right ops. */
    TupleTableSlot        *scan  = node->ss.ss_ScanTupleSlot;

    if (!state->built)
        cuvs_cs_build(state, node->ss.ps.state);

    while (state->cursor < state->n_results)
    {
        uint64_t        tid_enc = state->result_tids[state->cursor++];
        uint32_t        blk;
        uint16_t        off;
        ItemPointerData iptr;
        int             i;
        int             natts;

        if (tid_enc == 0)
            continue;

        cuvs_tid_decode(tid_enc, &blk, &off);
        ItemPointerSet(&iptr, blk, off);

        ExecClearTuple(fetch);
        if (!table_tuple_fetch_row_version(state->heap_rel, &iptr,
                                           GetTransactionSnapshot(), fetch))
            continue;   /* dead / invisible tuple */

        /*
         * Deform the heap tuple and copy attribute values into the virtual
         * scan slot that ps_ProjInfo was compiled against.  ExecProject
         * crashes if given a heap-AM slot when the ExprState expects a virtual
         * slot (different tts_ops → wrong slot-type fast path).
         */
        slot_getallattrs(fetch);
        natts = scan->tts_tupleDescriptor->natts;
        ExecClearTuple(scan);
        for (i = 0; i < natts; i++)
        {
            scan->tts_values[i] = fetch->tts_values[i];
            scan->tts_isnull[i] = fetch->tts_isnull[i];
        }
        ExecStoreVirtualTuple(scan);

        node->ss.ps.ps_ExprContext->ecxt_scantuple = scan;
        return ExecProject(node->ss.ps.ps_ProjInfo);
    }

    return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
}

/* ---- EndCustomScan ---- */
static void
cuvs_cs_end(CustomScanState *node)
{
    CuvsFilteredScanState *state = (CuvsFilteredScanState *) node;
    if (state->heap_fetch_slot)
    {
        ExecDropSingleTupleTableSlot(state->heap_fetch_slot);
        state->heap_fetch_slot = NULL;
    }
    if (state->heap_rel)
    {
        table_close(state->heap_rel, AccessShareLock);
        state->heap_rel = NULL;
    }
    if (state->result_tids)  { pfree(state->result_tids);  state->result_tids  = NULL; }
    if (state->result_dists) { pfree(state->result_dists); state->result_dists = NULL; }
}

/* ---- ReScanCustomScan ---- */
static void
cuvs_cs_rescan(CustomScanState *node)
{
    CuvsFilteredScanState *state = (CuvsFilteredScanState *) node;
    /* Re-run the build on next ExecCustomScan call. */
    state->built    = false;
    state->cursor   = 0;
    state->n_results = 0;
    if (state->result_tids)  { pfree(state->result_tids);  state->result_tids  = NULL; }
    if (state->result_dists) { pfree(state->result_dists); state->result_dists = NULL; }
}

/* ---- ExplainCustomScan ---- */
static void
cuvs_cs_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
    CuvsFilteredScanState *state = (CuvsFilteredScanState *) node;
    if (es->analyze && state->last_latency_us > 0)
        ExplainPropertyInteger("GPU IPC latency", "us",
                               (int64) state->last_latency_us, es);
}

/* ---- set_rel_pathlist_hook ---- */
static void
cuvs_filtered_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
                                Index rti, RangeTblEntry *rte)
{
    Oid             cagra_am;
    Oid             index_oid = InvalidOid;
    ListCell       *lc;
    SortGroupClause *sgc;
    TargetEntry    *tle;
    OpExpr         *op;
    Expr           *arg1;
    Expr           *arg2;
    Const          *query_const = NULL;
    CustomPath     *cpath;

    if (prev_set_rel_pathlist_hook)
        prev_set_rel_pathlist_hook(root, rel, rti, rte);

    if (!cuvs_filtered_knn_hook_enabled)
        return;
    if (rte->rtekind != RTE_RELATION || rel->reloptkind != RELOPT_BASEREL)
        return;
    if (rel->baserestrictinfo == NIL)
        return;

    /* Require a CAGRA index on the relation. */
    cagra_am = get_am_oid("cagra", true);
    if (!OidIsValid(cagra_am))
        return;
    foreach(lc, rel->indexlist)
    {
        IndexOptInfo *ioi = lfirst_node(IndexOptInfo, lc);
        if (ioi->relam == cagra_am) { index_oid = ioi->indexoid; break; }
    }
    if (!OidIsValid(index_oid))
        return;

    /* Extract query vector from first ORDER BY (must be a Const). */
    if (root->parse->sortClause == NIL)
        return;
    sgc = linitial_node(SortGroupClause, root->parse->sortClause);
    tle = get_sortgroupclause_tle(sgc, root->parse->targetList);
    if (tle == NULL || !IsA(tle->expr, OpExpr))
        return;
    op = (OpExpr *) tle->expr;
    if (list_length(op->args) != 2)
        return;
    arg1 = linitial(op->args);
    arg2 = lsecond(op->args);
    if (IsA(arg1, Const))       query_const = (Const *) arg1;
    else if (IsA(arg2, Const))  query_const = (Const *) arg2;
    if (query_const == NULL)
        return;   /* parametrized query — skip for spike */

    /* Build the CustomPath. */
    cpath = makeNode(CustomPath);
    cpath->path.pathtype      = T_CustomScan;
    cpath->path.parent        = rel;
    cpath->path.pathtarget    = rel->reltarget;
    cpath->path.param_info    = NULL;
    cpath->path.parallel_aware  = false;
    cpath->path.parallel_safe   = rel->consider_parallel;
    cpath->path.parallel_workers = 0;
    cpath->path.rows          = rel->rows;
    /* Cost: competitive with index scan for high-selectivity filters. */
    cpath->path.startup_cost  = 0;
    cpath->path.total_cost    = rel->rows * cpu_tuple_cost + 200.0;
    cpath->flags              = 0;
    cpath->methods            = &cuvs_filtered_path_methods;
    /* custom_private: {index_oid, query_const, rinfos, heap_oid} */
    cpath->custom_private = list_make4(
        makeInteger((long) index_oid),
        query_const,
        rel->baserestrictinfo,
        makeInteger((long) rte->relid));

    add_path(rel, (Path *) cpath);
}

/* Register the Custom Scan methods so the executor can deserialize the node. */
void
cuvs_register_custom_scan(void)
{
    RegisterCustomScanMethods(&cuvs_filtered_scan_methods);
}
