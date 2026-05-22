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
#include "catalog/pg_type.h"
#include "nodes/pathnodes.h"
#include "storage/bufmgr.h"
#include "storage/itemptr.h"

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"

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
char *cuvs_socket_path            = NULL;
char *cuvs_index_dir              = NULL;
int   cuvs_circuit_breaker_threshold = 3;

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

PG_FUNCTION_INFO_V1(cuvsamcostestimate);
Datum
cuvsamcostestimate(PG_FUNCTION_ARGS)
{
    IndexPath  *path              = (IndexPath *)     PG_GETARG_POINTER(1);
    double     *indexStartupCost  = (double *)        PG_GETARG_POINTER(4);
    double     *indexTotalCost    = (double *)        PG_GETARG_POINTER(5);
    double     *indexCorrelation  = (double *)        PG_GETARG_POINTER(7);
    double     *indexPages        = (double *)        PG_GETARG_POINTER(8);

    double      rows              = path->path.rows;

    /* Force CPU path if disabled, circuit-tripped, or GPU unavailable */
    Oid index_oid = path->indexinfo->indexoid;

    if (!enable_cuvs
        || !cuvs_gpu_available()
        || cuvs_circuit_is_open((uint32_t)index_oid))
    {
        *indexStartupCost = 1e9;
        *indexTotalCost   = 1e9;
    }
    else
    {
        /* Scale startup_cost by relative vector size vs 1536-dim baseline */
        *indexStartupCost = CUVS_STARTUP_COST;
        *indexTotalCost   = CUVS_STARTUP_COST + CUVS_PER_TUPLE_COST * rows;
    }

    *indexCorrelation = 0.0;
    *indexPages       = 0.0;

    PG_RETURN_VOID();
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
        ((uint64_t)ItemPointerGetBlockNumber(tid) << 16) |
        (uint64_t)ItemPointerGetOffsetNumber(tid);

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
        ereport(WARNING,
                (errmsg("pg_cuvs: daemon returned status %d during BUILD; "
                        "index may not be GPU-accelerated", rc),
                 errhint("Check pg_cuvs_server is running. "
                         "Queries will fall back to pgvector HNSW.")));

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

        int rc = cuvs_ipc_search(
            cuvs_socket_path,
            (uint32_t)MyDatabaseId,
            (uint32_t)index_oid,
            qvec->x,
            dim, k, metric,
            ss->tids,
            ss->distances,
            &ss->n_results);

        if (rc != CUVS_STATUS_OK)
        {
            /* Record error for circuit breaker */
            cuvs_circuit_record_error((uint32_t)index_oid,
                                      cuvs_circuit_breaker_threshold);

            if (rc == CUVS_STATUS_OOM_FALLBACK)
                ereport(WARNING,
                        (errmsg("pg_cuvs: VRAM exhausted, falling back to CPU")));
            else
                ereport(WARNING,
                        (errmsg("pg_cuvs: daemon unreachable (status %d), "
                                "falling back to CPU", rc)));
            return false;
        }

        /* Successful search — reset consecutive error count */
        cuvs_circuit_record_success((uint32_t)index_oid);
        ss->cur = 0;
    }

    /* Iterate stored results */
    if (ss->cur >= ss->n_results)
        return false;

    uint64_t tid     = ss->tids[ss->cur];
    uint32_t blk     = (uint32_t)(tid >> 16);
    uint16_t offset  = (uint16_t)(tid & 0xFFFF);
    ss->cur++;

    ItemPointerSet(&scan->xs_heaptid, blk, offset);
    scan->xs_recheck = true;   /* recheck predicate on heap tuple */

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
    amroutine->amhasgettuple     = true;
    amroutine->amhasgetbitmap    = false;
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
    text *index_name_text = PG_GETARG_TEXT_PP(0);
    char *index_name      = text_to_cstring(index_name_text);

    /* Resolve index name to OID */
    Oid index_oid = RelnameGetRelid(index_name);
    if (!OidIsValid(index_oid))
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_TABLE),
                 errmsg("index \"%s\" does not exist", index_name)));

    cuvs_circuit_reset((uint32_t)index_oid);

    ereport(NOTICE,
            (errmsg("pg_cuvs: circuit breaker reset for index \"%s\" (oid %u)",
                    index_name, index_oid)));

    PG_RETURN_VOID();
}
