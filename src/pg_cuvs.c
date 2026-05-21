/*
 * pg_cuvs.c — GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS.
 *
 * Architecture: PostgreSQL C extension that registers a custom Index AM
 * (Access Method). The planner's cost model decides whether to route a
 * vector similarity query to the GPU (via cuvs_wrapper / IPC daemon) or
 * fall back to pgvector's CPU path (HNSW / SeqScan).
 *
 * C/C++ split: all CUDA code lives in cuvs_wrapper.cu (.cpp compiled as CUDA).
 * This file is pure C so it can include PostgreSQL headers without the
 * float4 typedef collision that occurs when PG and CUDA headers coexist.
 *
 * Phase 1 status: brute-force in-process GPU search. Sidecar daemon (IPC)
 * is stubbed in cuvs_ipc.h and will be wired up in a later phase.
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "access/amapi.h"
#include "nodes/pathnodes.h"
#include "utils/builtins.h"
#include "catalog/pg_type.h"

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"

PG_MODULE_MAGIC;

/* ----------------------------------------------------------------
 * GUC: enable_cuvs
 * SET enable_cuvs = off; to force CPU path (for testing / debugging).
 * ---------------------------------------------------------------- */
bool enable_cuvs = true;

void _PG_init(void);

void
_PG_init(void)
{
    DefineCustomBoolVariable(
        "enable_cuvs",
        "Enable GPU-accelerated vector search via pg_cuvs.",
        "When off, pg_cuvs AM falls back to pgvector CPU path.",
        &enable_cuvs,
        true,
        PGC_USERSET,
        0,
        NULL, NULL, NULL
    );
}

/* ----------------------------------------------------------------
 * Cost model helpers
 *
 * The planner calls cuvsamcostestimate to decide whether the CAGRA
 * index is cheaper than HNSW or SeqScan for a given query.
 *
 * Cost_total = Cost_IPC + Cost_GPU_kernel + Cost_CPU_recheck
 *
 * startup_cost=1000 models CUDA context + PCI-e transfer overhead.
 * For small tables the IPC cost dominates → planner prefers CPU.
 * For large tables the per-tuple advantage of GPU dominates.
 * ---------------------------------------------------------------- */
#define CUVS_STARTUP_COST   1000.0
#define CUVS_PER_TUPLE_COST 0.0001

PG_FUNCTION_INFO_V1(cuvsamcostestimate);
Datum
cuvsamcostestimate(PG_FUNCTION_ARGS)
{
    /* PlannerInfo  *root       = (PlannerInfo *)  PG_GETARG_POINTER(0); */
    /* IndexPath    *path       = (IndexPath *)     PG_GETARG_POINTER(1); */
    double       *indexStartupCost = (double *) PG_GETARG_POINTER(4);
    double       *indexTotalCost   = (double *) PG_GETARG_POINTER(5);
    /* Selectivity *indexSelectivity = ... */
    double       *indexCorrelation  = (double *) PG_GETARG_POINTER(7);
    double       *indexPages        = (double *) PG_GETARG_POINTER(8);

    if (!enable_cuvs || !cuvs_gpu_available())
    {
        /* Signal planner that this AM is unavailable — force CPU path. */
        *indexStartupCost = 1e9;
        *indexTotalCost   = 1e9;
    }
    else
    {
        *indexStartupCost = CUVS_STARTUP_COST;
        *indexTotalCost   = CUVS_STARTUP_COST + CUVS_PER_TUPLE_COST * 1000;
    }

    *indexCorrelation = 0.0;
    *indexPages       = 0.0;

    PG_RETURN_VOID();
}

/* ----------------------------------------------------------------
 * Index AM handler — registered as USING cagra in pg_cuvs--0.1.0.sql
 *
 * Most AM callbacks are stubs: Phase 1 only implements scan path.
 * Build, insert, and vacuum are deferred to Phase 2.
 * ---------------------------------------------------------------- */

static IndexScanDesc cuvs_beginscan(Relation rel, int nkeys, int norderbys);
static void          cuvs_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
                                  ScanKey orderbys, int norderbys);
static bool          cuvs_gettuple(IndexScanDesc scan, ScanDirection dir);
static void          cuvs_endscan(IndexScanDesc scan);

PG_FUNCTION_INFO_V1(cuvsamhandler);
Datum
cuvsamhandler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

    amroutine->amstrategies      = 0;
    amroutine->amsupport         = 1;
    amroutine->amcanmulticol     = false;
    amroutine->amsearcharray      = false;
    amroutine->amhasgettuple     = true;
    amroutine->amhasgetbitmap    = false;
    amroutine->amcanorderbyop    = true;
    amroutine->amoptionalkey     = true;

    /* Phase 1: scan only. Build/insert/vacuum stubs return immediately. */
    amroutine->ambuild           = NULL;
    amroutine->ambuildempty      = NULL;
    amroutine->aminsert          = NULL;
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
 * Scan implementation — placeholder
 *
 * Full implementation: embed query vector → send to GPU daemon via
 * cuvs_ipc_search → iterate over TID results → heap_fetch each TID.
 * Falls back to pgvector SeqScan if IPC returns CUVS_IPC_UNAVAIL.
 * ---------------------------------------------------------------- */

static IndexScanDesc
cuvs_beginscan(Relation rel, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);
    scan->opaque = NULL;
    return scan;
}

static void
cuvs_rescan(IndexScanDesc scan, ScanKey keys, int nkeys,
            ScanKey orderbys, int norderbys)
{
    if (keys && scan->numberOfKeys > 0)
        memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
    if (orderbys && scan->numberOfOrderBys > 0)
        memmove(scan->orderByData, orderbys,
                scan->numberOfOrderBys * sizeof(ScanKeyData));
}

static bool
cuvs_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    /*
     * TODO Phase 1: call cuvs_ipc_search() with query vector extracted
     * from scan->orderByData, iterate over results, set scan->xs_heaptid.
     * If CUVS_IPC_UNAVAIL, elog(WARNING) and return false (let planner
     * retry with sequential scan).
     */
    (void) dir;
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("pg_cuvs: GPU scan not yet implemented (Phase 1 in progress)"),
             errhint("Use SET enable_cuvs = off to fall back to pgvector HNSW.")));
    return false;
}

static void
cuvs_endscan(IndexScanDesc scan)
{
    (void) scan;
}
