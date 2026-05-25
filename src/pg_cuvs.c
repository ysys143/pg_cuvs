/*
 * pg_cuvs.c — GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS.
 *
 * Architecture: PostgreSQL C extension registering a custom Index AM
 * (Access Method). The planner routes vector similarity queries to the
 * GPU sidecar daemon (pg_cuvs_server) via UDS+shm IPC, or falls back
 * to pgvector HNSW / SeqScan via four automatic fallback conditions.
 *
 * C/C++ split (ADR-001): CUDA code lives in cuvs_wrapper.cu. This file
 * is pure C to avoid the PG/CUDA float4 typedef collision.
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "access/amapi.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"

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

/* ----------------------------------------------------------------
 * Cost model
 *
 * startup_cost=1000 models UDS round-trip + CUDA context overhead.
 * Per-tuple cost reflects GPU batch parallelism advantage (ADR-003).
 * ---------------------------------------------------------------- */
#define CUVS_STARTUP_COST      1000.0
#define CUVS_PER_TUPLE_COST    0.0001

/* PG16 amcostestimate is a direct C function pointer, not a SQL function.
 *
 * IMPORTANT: This runs in the planner on every query — once per candidate
 * index path. It must NOT touch the CUDA runtime: cudaGetDeviceCount() etc.
 * lazily initialize the CUDA context per backend, which costs ~100ms the
 * first time and inflates Planning Time. Daemon availability is checked at
 * runtime by cuvs_ipc_search; if the daemon is down, gettuple returns
 * CUVS_STATUS_UNAVAILABLE and the executor falls back to CPU with a
 * WARNING. The cost path only needs to short-circuit when the GPU route is
 * explicitly off (enable_cuvs / circuit breaker). */
static void
cuvsamcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                   Cost *indexStartupCost, Cost *indexTotalCost,
                   Selectivity *indexSelectivity, double *indexCorrelation,
                   double *indexPages)
{
    double rows = path->path.rows;
    Oid    index_oid = path->indexinfo->indexoid;

    (void) root;
    (void) loop_count;

    if (!enable_cuvs
        || cuvs_circuit_is_open((uint32_t)index_oid))
    {
        *indexStartupCost = 1e9;
        *indexTotalCost   = 1e9;
    }
    else
    {
        *indexStartupCost = CUVS_STARTUP_COST;
        *indexTotalCost   = CUVS_STARTUP_COST + CUVS_PER_TUPLE_COST * rows;
    }

    *indexSelectivity = 1.0;
    *indexCorrelation = 0.0;
    *indexPages       = 0.0;
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
} CuvsBuildState;

static void
grow_build_buffers(CuvsBuildState *bs)
{
    int64_t new_size = bs->n_allocated * 2;
    if (new_size < 64)
        new_size = 64;

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
    bs.metric = CUVS_METRIC_L2;  /* default; TODO: derive from opclass */

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

        /* Fallback triggers 1 and 2: GUC or circuit breaker */
        Oid index_oid = RelationGetRelid(scan->indexRelation);
        if (!enable_cuvs || cuvs_circuit_is_open((uint32_t)index_oid))
            return false;   /* executor retries with HNSW/seqscan */

        /* Extract query vector from ORDER BY operator argument */
        if (scan->numberOfOrderBys < 1 || !scan->orderByData)
            return false;

        Datum query_datum = scan->orderByData[0].sk_argument;
        PgVector *qvec    = DatumGetPgVector(query_datum);
        int       dim     = (int)qvec->dim;
        int       k       = 100;  /* default top-k; TODO: planner hint */

        ss->tids      = palloc(k * sizeof(uint64_t));
        ss->distances = palloc(k * sizeof(float));

        /* Determine metric from operator class */
        uint32_t metric = CUVS_METRIC_L2;
        if (scan->numberOfOrderBys > 0)
        {
            Oid opno = scan->orderByData[0].sk_strategy;
            /* Heuristic: cosine op strategy = 2, ip = 3 */
            if (opno == 2)
                metric = CUVS_METRIC_COSINE;
            else if (opno == 3)
                metric = CUVS_METRIC_IP;
        }

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
            /* Record error for circuit breaker. UNAVAILABLE is not an
             * index-specific failure, so don't count it toward the breaker. */
            if (rc != CUVS_STATUS_UNAVAILABLE)
                cuvs_circuit_record_error((uint32_t)index_oid,
                                          cuvs_circuit_breaker_threshold);

            switch (rc)
            {
                case CUVS_STATUS_UNAVAILABLE:
                    ereport(WARNING,
                            (errmsg("pg_cuvs: pg_cuvs_server unreachable, "
                                    "falling back to CPU")));
                    break;
                case CUVS_STATUS_OOM_FALLBACK:
                    ereport(WARNING,
                            (errmsg("pg_cuvs: VRAM exhausted, falling back to CPU")));
                    break;
                case CUVS_STATUS_NOT_FOUND:
                    ereport(WARNING,
                            (errmsg("pg_cuvs: index not loaded on daemon, "
                                    "falling back to CPU")));
                    break;
                default:
                    ereport(WARNING,
                            (errmsg("pg_cuvs: GPU search failed (status %d), "
                                    "falling back to CPU", rc)));
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
        scan->xs_recheckorderby  = true;
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
    amroutine->aminsert          = NULL;    /* lazy rebuild via AUTOVACUUM */
    amroutine->ambulkdelete      = NULL;
    amroutine->amvacuumcleanup   = NULL;

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
