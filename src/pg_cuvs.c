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

#include <sys/stat.h>

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"
#include "cuvs_util.h"

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

void
_PG_init(void)
{
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
             get_index_dir(), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
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
             get_index_dir(), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
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
             get_index_dir(), (uint32_t) MyDatabaseId, (uint32_t) index_oid);
    return stat(path, &st) == 0;
}

/* ----------------------------------------------------------------
 * Cost model
 *
 * startup_cost=1000 models UDS round-trip + CUDA context overhead.
 * Total cost is dominated by the requested top-k, not table size (ADR-003).
 * ---------------------------------------------------------------- */
#define CUVS_STARTUP_COST      1000.0
/* KNN returns ~k rows regardless of table size, so cost is dominated by the
 * requested top-k (cuvs.k), with only a weak table-size term. This keeps the
 * GPU path preferred over seqscan+sort on large tables. */
#define CUVS_K_COST            0.5
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

    /* Six gates turn the GPU path off — all local file/state reads, no IPC.
     * Cost is set to 1e15 (> PG disable_cost 1e10) so the gated index loses
     * even when enable_seqscan = off, preventing empty-result or ERROR paths.
     *  1. enable_cuvs GUC
     *  2. circuit breaker (repeated runtime failures → breaker open)
     *  3. .stale sidecar (heap write marked index stale since build)
     *  4. delete-drift (.tids build count vs planner live-row estimate)
     *  5. socket existence (missing → daemon cleanly stopped or not yet started)
     *  6. artifact existence (no .tids → index was built on empty table) */
    gpu_off = !enable_cuvs
           || cuvs_circuit_is_open((uint32_t) index_oid)
           || cuvs_index_is_stale(index_oid)
           || cuvs_index_delete_drift_stale(index_oid, path->indexinfo->rel->tuples)
           || !cuvs_daemon_socket_ready()
           || !cuvs_index_has_artifact(index_oid);

    if (gpu_off)
    {
        *indexStartupCost = 1e15;
        *indexTotalCost   = 1e15;
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
    static bool resolved = false;
    static Oid  opf_l2 = InvalidOid, opf_cos = InvalidOid, opf_ip = InvalidOid;
    Oid relam = indexRel->rd_rel->relam;
    Oid idxopf;

    if (!resolved)
    {
        Oid c;
        c = get_opclass_oid(relam, list_make1(makeString("vector_l2_ops")), true);
        opf_l2  = OidIsValid(c) ? get_opclass_family(c) : InvalidOid;
        c = get_opclass_oid(relam, list_make1(makeString("vector_cosine_ops")), true);
        opf_cos = OidIsValid(c) ? get_opclass_family(c) : InvalidOid;
        c = get_opclass_oid(relam, list_make1(makeString("vector_ip_ops")), true);
        opf_ip  = OidIsValid(c) ? get_opclass_family(c) : InvalidOid;
        resolved = true;
    }

    idxopf = indexRel->rd_opfamily[0];
    if (OidIsValid(opf_cos) && idxopf == opf_cos) return CUVS_METRIC_COSINE;
    if (OidIsValid(opf_ip)  && idxopf == opf_ip)  return CUVS_METRIC_IP;
    if (OidIsValid(opf_l2)  && idxopf == opf_l2)  return CUVS_METRIC_L2;

    ereport(WARNING,
            (errmsg("pg_cuvs: unrecognized opclass family for cagra index %u; assuming L2",
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
    float   *vectors;     /* malloc'd: [n_allocated][dim] */
    uint64_t *tids;       /* malloc'd: [n_allocated] */
    uint32_t metric;      /* CUVS_METRIC_* */
    double   reltuples;
    size_t   cap_bytes;   /* build memory limit (0 = none); see Step 5 */
} CuvsBuildState;

static void
grow_build_buffers(CuvsBuildState *bs)
{
    int64_t new_size = bs->n_allocated * 2;
    if (new_size < 64)
        new_size = 64;

    /* Runtime guard: catch tables whose preflight estimate was unavailable
     * (never ANALYZEd) or too low. Free what we hold before erroring so the
     * ereport longjmp does not leak the malloc'd buffers. */
    if (bs->cap_bytes > 0)
    {
        size_t projected = (size_t) new_size * bs->dim * sizeof(float)
                         + (size_t) new_size * sizeof(uint64_t);
        if (projected > bs->cap_bytes)
        {
            free(bs->vectors); bs->vectors = NULL;
            free(bs->tids);    bs->tids = NULL;
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("pg_cuvs: build corpus exceeds the build memory limit (%zu MB)",
                            bs->cap_bytes / (1024 * 1024)),
                     errhint("Raise cuvs.max_build_mem_mb (hard cap) or "
                             "cuvs.build_mem_safety_ratio, shard the table, or see "
                             "docs/playbooks/large-dataset-benchmark.md.")));
        }
    }

    bs->vectors = realloc(bs->vectors,
                          (size_t)new_size * bs->dim * sizeof(float));
    bs->tids    = realloc(bs->tids,
                          (size_t)new_size * sizeof(uint64_t));

    if (!bs->vectors || !bs->tids)
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("pg_cuvs: out of memory accumulating index vectors")));

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

/* ----------------------------------------------------------------
 * Index AM: ambuild — handles CREATE INDEX USING cagra
 * ---------------------------------------------------------------- */
static IndexBuildResult *
cuvs_ambuild(Relation heapRel, Relation indexRel, IndexInfo *indexInfo)
{
    IndexBuildResult *result = palloc0(sizeof(IndexBuildResult));

    CuvsBuildState bs;
    memset(&bs, 0, sizeof(bs));
    bs.metric = cuvs_index_metric(indexRel);  /* baked into the CAGRA graph */
    bs.cap_bytes = cuvs_effective_build_cap_bytes();

    /* Preflight: estimate the corpus size from the planner's row estimate and
     * the indexed column's declared dimension, and fail before scanning if it
     * would blow the build memory limit. (Unknown dim/reltuples -> skip; the
     * runtime guard in grow_build_buffers still protects accumulation.) */
    if (bs.cap_bytes > 0)
    {
        AttrNumber heap_attno = indexInfo->ii_IndexAttrNumbers[0];
        int32 typmod = (heap_attno >= 1)
            ? TupleDescAttr(RelationGetDescr(heapRel), heap_attno - 1)->atttypmod
            : -1;
        double reltuples = heapRel->rd_rel->reltuples;
        if (typmod > 0 && reltuples > 0)
        {
            size_t est = (size_t) reltuples * (size_t) typmod * sizeof(float)
                       + (size_t) reltuples * sizeof(uint64_t);
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
    }

    /* Scan all live heap tuples, collect vectors + TIDs */
    bs.reltuples = table_index_build_scan(
        heapRel, indexRel, indexInfo,
        true, true,
        cuvs_build_callback, &bs, NULL);

    result->heap_tuples  = bs.reltuples;
    result->index_tuples = (double)bs.n_vecs;

    if (bs.n_vecs == 0)
    {
        /* Empty table — nothing to build */
        if (bs.vectors) free(bs.vectors);
        if (bs.tids)    free(bs.tids);
        return result;
    }

    /* Send corpus to daemon for CAGRA build */
    int rc = cuvs_ipc_build(
        cuvs_socket_path,
        (uint32_t)MyDatabaseId,
        (uint32_t)RelationGetRelid(indexRel),
        bs.vectors,
        (const uint64_t *)bs.tids,
        bs.n_vecs,
        bs.dim,
        bs.metric,
        get_index_dir());

    free(bs.vectors);
    free(bs.tids);

    if (rc != CUVS_STATUS_OK)
    {
        /* DDL durability contract: CREATE INDEX must produce a durable
         * index on success. We fail the transaction so the catalog entry
         * is rolled back and the user knows to retry. */
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

        ss->tids      = palloc(k * sizeof(uint64_t));
        ss->distances = palloc(k * sizeof(float));

        /* Metric is determined by the index's operator class (same source as
         * build), not the ORDER BY strategy number (all three opclasses use
         * strategy 1, so the old heuristic never fired and forced L2). */
        uint32_t metric = cuvs_index_metric(scan->indexRelation);

        uint32_t latency_us = 0;
        int rc = cuvs_ipc_search(
            cuvs_socket_path,
            (uint32_t)MyDatabaseId,
            (uint32_t)index_oid,
            qvec->x,
            dim, k, metric,
            ss->tids,
            ss->distances,
            &ss->n_results,
            &latency_us);

        if (rc != CUVS_STATUS_OK)
        {
            /* Record error for circuit breaker. DIM_MISMATCH and METRIC_MISMATCH
             * are user/config errors, not repeatable GPU failures. STALE is
             * already gated at plan time. Everything else — including UNAVAILABLE
             * (daemon crash after planning) — counts so the breaker opens and
             * subsequent queries replan to CPU via the socket-existence gate. */
            if (rc != CUVS_STATUS_DIM_MISMATCH
                && rc != CUVS_STATUS_METRIC_MISMATCH && rc != CUVS_STATUS_STALE)
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

/* aminsert fires per inserted/updated row. CAGRA keeps no incremental per-row
 * structure, so we don't store a tuple — we only need to mark the index stale
 * once. Dedup via rd_amcache (relcache-lifetime; cleared on REINDEX's relcache
 * invalidation, so post-REINDEX writes re-mark correctly). */
static bool
cuvs_aminsert(Relation indexRel, Datum *values, bool *isnull,
              ItemPointer heap_tid, Relation heapRel,
              IndexUniqueCheck checkUnique, bool indexUnchanged,
              IndexInfo *indexInfo)
{
    (void) values; (void) isnull; (void) heap_tid; (void) heapRel;
    (void) checkUnique; (void) indexUnchanged; (void) indexInfo;

    if (indexRel->rd_amcache == NULL)
    {
        cuvs_mark_index_stale(indexRel);
        /* sentinel: presence means "already marked stale this relcache life" */
        indexRel->rd_amcache = MemoryContextAllocZero(indexRel->rd_indexcxt,
                                                      sizeof(bool));
    }
    return false;   /* no index entry stored */
}

/* ambulkdelete runs once per VACUUM when dead tuples (DELETE/UPDATE) are
 * removed. Mark the index stale so the next query falls back to CPU. */
static IndexBulkDeleteResult *
cuvs_ambulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                  IndexBulkDeleteCallback callback, void *callback_state)
{
    (void) callback; (void) callback_state;
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    cuvs_mark_index_stale(info->index);
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
    (void) info;
    return stats;
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
    amroutine->aminsert          = cuvs_aminsert;     /* marks index stale */
    amroutine->ambulkdelete      = cuvs_ambulkdelete; /* marks index stale */
    amroutine->amvacuumcleanup   = cuvs_amvacuumcleanup;

    amroutine->ambeginscan       = cuvs_beginscan;
    amroutine->amrescan          = cuvs_rescan;
    amroutine->amgettuple        = cuvs_gettuple;
    amroutine->amendscan         = cuvs_endscan;
    amroutine->amcostestimate    = cuvsamcostestimate;

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
#define GPU_STATS_NCOLS 21

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

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }

    return (Datum) 0;
}

/* ----------------------------------------------------------------
 * pg_cuvs_gpu_cache_stats() — daemon-global VRAM tiered-cache counters,
 * backing the pg_stat_gpu_cache view. One row normally; zero rows when the
 * daemon is unreachable (same convention as pg_stat_gpu_search).
 * ---------------------------------------------------------------- */
#define GPU_CACHE_NCOLS 8

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

    memset(&cs, 0, sizeof(cs));
    rc = cuvs_ipc_cache_stats(cuvs_socket_path, &cs);
    if (rc == CUVS_STATUS_OK)
    {
        Datum values[GPU_CACHE_NCOLS];
        bool  nulls[GPU_CACHE_NCOLS];

        memset(nulls, 0, sizeof(nulls));
        values[0] = Int64GetDatum((int64) cs.hits);
        values[1] = Int64GetDatum((int64) cs.misses);
        values[2] = Int64GetDatum((int64) cs.evictions);
        values[3] = Int64GetDatum((int64) cs.reloads);
        values[4] = Int64GetDatum((int64) cs.persist_failures);
        values[5] = Int32GetDatum((int32) cs.resident_count);
        values[6] = Int64GetDatum((int64) (cs.vram_used_bytes / (1024 * 1024)));
        values[7] = Int64GetDatum((int64) (cs.vram_budget_bytes / (1024 * 1024)));
        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
    }
    /* daemon down (UNAVAILABLE) -> empty result, not an error */

    return (Datum) 0;
}
