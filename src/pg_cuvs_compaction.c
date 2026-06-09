/*
 * pg_cuvs_compaction.c — Phase 4C: background auto-compaction bgworker.
 *
 * Polls pg_stat_gpu_search every cuvs.auto_compact_check_interval seconds.
 * When extend_count / n_vecs > cuvs.auto_compact_threshold, issues
 * REINDEX INDEX CONCURRENTLY to rebuild the CAGRA graph from scratch.
 *
 * Transaction design:
 *   Phase 1 (SELECT candidates): regular SPI inside StartTransactionCommand.
 *   Phase 2 (REINDEX): SPI_connect_ext(SPI_OPT_NONATOMIC) so that
 *     REINDEX CONCURRENTLY can manage its own transactions internally
 *     (requires PROCESS_UTILITY_TOPLEVEL, which SPI NONATOMIC mode provides).
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "executor/spi.h"
#include "utils/snapmgr.h"
#include "utils/builtins.h"
#include "utils/memutils.h"         /* TopMemoryContext */
#include "storage/latch.h"
#include "storage/ipc.h"
#include "access/xact.h"

#include <signal.h>

/* GUC variables — defined in pg_cuvs.c; exported for use in _PG_init. */
extern bool   cuvs_auto_compact;
extern int    cuvs_auto_compact_check_interval;
extern double cuvs_auto_compact_threshold;
extern char  *cuvs_auto_compact_database;

/* Signal flag */
static volatile sig_atomic_t compaction_got_sigterm = false;

static void
compaction_sigterm(SIGNAL_ARGS)
{
    int save_errno = errno;
    compaction_got_sigterm = true;
    SetLatch(MyLatch);
    errno = save_errno;
}

/*
 * cuvs_compaction_worker_main
 *
 * Entry point registered with RegisterBackgroundWorker in _PG_init.
 * Must be a global symbol so the dynamic loader can find it after restart.
 */
PGDLLEXPORT void cuvs_compaction_worker_main(Datum main_arg);

void
cuvs_compaction_worker_main(Datum main_arg)
{
    pqsignal(SIGTERM, compaction_sigterm);
    BackgroundWorkerUnblockSignals();

    /* No database configured → exit immediately; harmless on non-GPU hosts. */
    if (!cuvs_auto_compact_database || cuvs_auto_compact_database[0] == '\0')
        proc_exit(0);

    BackgroundWorkerInitializeConnection(cuvs_auto_compact_database, NULL, 0);

    elog(LOG, "pg_cuvs compaction worker started for database \"%s\"",
         cuvs_auto_compact_database);

    while (!compaction_got_sigterm)
    {
        /* Declare all locals at top of block (C90). */
        int             rc;
        char          **cands;
        int             ncands;
        char            select_sql[512];
        int             i;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       (long) cuvs_auto_compact_check_interval * 1000L,
                       0);
        ResetLatch(MyLatch);

        if (rc & WL_EXIT_ON_PM_DEATH)
            break;

        if (!cuvs_auto_compact)
            continue;

        cands  = NULL;
        ncands = 0;

        /* ---- Phase 1: collect candidates via regular SPI ---- */
        snprintf(select_sql, sizeof(select_sql),
                 "SELECT quote_ident(nspname) || '.' || quote_ident(relname)"
                 " FROM pg_stat_gpu_search gs"
                 " JOIN pg_class c ON c.oid = gs.index_oid"
                 " JOIN pg_namespace n ON n.oid = c.relnamespace"
                 " WHERE gs.n_vecs > 0"
                 "   AND gs.extend_count::float8 / gs.n_vecs > %.9g",
                 cuvs_auto_compact_threshold);

        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        SPI_connect();
        PushActiveSnapshot(GetTransactionSnapshot());

        rc = SPI_execute(select_sql, true, 0);
        if (rc == SPI_OK_SELECT && SPI_processed > 0)
        {
            MemoryContext old;
            uint64        j;

            old    = MemoryContextSwitchTo(TopMemoryContext);
            ncands = (int) SPI_processed;
            cands  = palloc(ncands * sizeof(char *));
            for (j = 0; j < (uint64) ncands; j++)
                cands[j] = pstrdup(
                    SPI_getvalue(SPI_tuptable->vals[j],
                                 SPI_tuptable->tupdesc, 1));
            MemoryContextSwitchTo(old);
        }

        PopActiveSnapshot();
        SPI_finish();
        CommitTransactionCommand();

        /* ---- Phase 2: REINDEX each candidate ---- */
        for (i = 0; i < ncands && !compaction_got_sigterm; i++)
        {
            char reindex_sql[512];
            int  ret;

            snprintf(reindex_sql, sizeof(reindex_sql),
                     "REINDEX INDEX CONCURRENTLY %s", cands[i]);

            elog(LOG, "pg_cuvs compaction: reindexing %s", cands[i]);

            /*
             * SPI_OPT_NONATOMIC lets SPI execute top-level-only statements
             * (REINDEX CONCURRENTLY) by passing PROCESS_UTILITY_TOPLEVEL to
             * ProcessUtility, bypassing PreventInTransactionBlock.
             */
            SetCurrentStatementStartTimestamp();
            StartTransactionCommand();
            SPI_connect_ext(SPI_OPT_NONATOMIC);

            ret = SPI_execute(reindex_sql, false, 0);

            SPI_finish();
            CommitTransactionCommand();

            if (ret == SPI_OK_UTILITY)
                elog(LOG, "pg_cuvs compaction: reindex of %s complete",
                     cands[i]);
            else
                elog(WARNING,
                     "pg_cuvs compaction: REINDEX failed for %s (SPI ret %d)",
                     cands[i], ret);

            pfree(cands[i]);
        }
        if (cands)
            pfree(cands);
    }

    proc_exit(0);
}
