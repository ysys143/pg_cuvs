#pragma once

/*
 * pg_cuvs IPC layer — Shared Memory RPC between PostgreSQL backends
 * and the pg_cuvs_server GPU sidecar daemon.
 *
 * Design: "vector pointer in, TID+distance array out."
 * Actual vectors live in shared memory; only handles are exchanged.
 *
 * Status: stub — pg_cuvs_server not yet implemented (Phase 1).
 * Currently pg_cuvs calls cuvs_wrapper directly (in-process).
 */

#include <stdint.h>
#include <stddef.h>
#include "cuvs_wrapper.h"

#define CUVS_IPC_SHM_NAME  "/pg_cuvs_server"
#define CUVS_IPC_MAX_DIM   4096
#define CUVS_IPC_MAX_TOPK  1000

typedef enum CuvsIpcStatus {
    CUVS_IPC_OK       = 0,
    CUVS_IPC_ERROR    = 1,
    CUVS_IPC_TIMEOUT  = 2,
    CUVS_IPC_UNAVAIL  = 3,   /* GPU daemon not running — caller should fall back */
} CuvsIpcStatus;

typedef struct CuvsIpcRequest {
    int64_t  collection_id;
    int      dim;
    int      top_k;
    float    query_vec[CUVS_IPC_MAX_DIM];
} CuvsIpcRequest;

typedef struct CuvsIpcResponse {
    CuvsIpcStatus    status;
    int              n_results;
    CuvsSearchResult results[CUVS_IPC_MAX_TOPK];
} CuvsIpcResponse;

/* Attempt to contact the GPU daemon and run a search.
 * Returns CUVS_IPC_UNAVAIL if daemon is not running — caller must
 * fall back to CPU path (pgvector HNSW/SeqScan). */
CuvsIpcStatus cuvs_ipc_search(
    const CuvsIpcRequest  *req,
    CuvsIpcResponse       *resp,
    int                    timeout_ms
);
