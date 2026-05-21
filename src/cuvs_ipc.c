/*
 * cuvs_ipc.c — Shared Memory IPC stub.
 *
 * Phase 1: always returns CUVS_IPC_UNAVAIL so pg_cuvs falls back to
 * the in-process GPU call (cuvs_brute_force_search) or the CPU path.
 *
 * Phase 2: implement pg_cuvs_server daemon connection via shm_open,
 * semaphores, and a request/response ring buffer.
 */

#include "cuvs_ipc.h"

CuvsIpcStatus
cuvs_ipc_search(const CuvsIpcRequest *req,
                CuvsIpcResponse      *resp,
                int                   timeout_ms)
{
    /* TODO Phase 2: connect to pg_cuvs_server via shared memory */
    (void) req;
    (void) timeout_ms;

    resp->status    = CUVS_IPC_UNAVAIL;
    resp->n_results = 0;

    return CUVS_IPC_UNAVAIL;
}
