#pragma once

/*
 * cuvs_ipc.h — UDS + shm_open IPC protocol between PostgreSQL backends
 * and the pg_cuvs_server GPU sidecar daemon (ADR-008).
 *
 * Transport split:
 *   UDS socket  — small command/reply frames (metadata only)
 *   shm_open    — large vector payloads (zero-copy)
 *
 * Default socket path: /tmp/.s.pg_cuvs.<postmaster_pid>
 * Configured via cuvs.socket_path GUC.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* ----------------------------------------------------------------
 * Op codes
 * ---------------------------------------------------------------- */
#define CUVS_OP_SEARCH    1
#define CUVS_OP_BUILD     2
#define CUVS_OP_STATUS    3

/* ----------------------------------------------------------------
 * Distance metrics (mirror pgvector operator names)
 * ---------------------------------------------------------------- */
#define CUVS_METRIC_L2      0
#define CUVS_METRIC_COSINE  1
#define CUVS_METRIC_IP      2

/* ----------------------------------------------------------------
 * Reply status codes
 * ---------------------------------------------------------------- */
#define CUVS_STATUS_OK           0
#define CUVS_STATUS_ERROR        1   /* daemon up but operation failed */
#define CUVS_STATUS_OOM_FALLBACK 2   /* VRAM exhausted → caller must use CPU */
#define CUVS_STATUS_NOT_FOUND    3   /* index OID not loaded */
#define CUVS_STATUS_UNAVAILABLE  4   /* daemon unreachable (connect failed) */

/* ----------------------------------------------------------------
 * Command frame (sent over UDS, fixed size)
 * ---------------------------------------------------------------- */
typedef struct CuvsCmdFrame {
    uint32_t op;            /* CUVS_OP_* */
    uint32_t db_oid;        /* PostgreSQL database OID */
    uint32_t index_oid;     /* PostgreSQL index OID */
    uint32_t k;             /* top-k for SEARCH */
    uint32_t metric;        /* CUVS_METRIC_* */
    uint32_t dim;           /* vector dimension */
    int64_t  n_vecs;        /* BUILD: corpus size; SEARCH: unused */
    char     shm_key[64];   /* shm_open name for payload data */
} CuvsCmdFrame;

/*
 * shm layout for BUILD:
 *   [float32 vectors: n_vecs × dim]  (row-major)
 *   [uint64_t tids:   n_vecs]        (block << 16 | offset)
 *
 * shm layout for SEARCH:
 *   [float32 query:   dim]
 */

/* ----------------------------------------------------------------
 * Reply frame (sent over UDS, variable size)
 *
 * Fixed header is followed by n_results × CuvsResult entries.
 * ---------------------------------------------------------------- */
typedef struct CuvsReplyHeader {
    uint32_t status;        /* CUVS_STATUS_* */
    uint32_t n_results;
    uint32_t latency_us;
    char     error[128];
} CuvsReplyHeader;

typedef struct CuvsResult {
    uint64_t tid;           /* heap TID encoded as block<<16|offset */
    float    distance;
} CuvsResult;

/* ----------------------------------------------------------------
 * Client API (used by pg_cuvs.c)
 * ---------------------------------------------------------------- */

/*
 * cuvs_ipc_search — send a SEARCH command to the daemon.
 *
 * query_vec: caller's float32 query vector of length dim
 * tids_out:  caller-allocated array of n_results uint64_t TIDs
 * dist_out:  caller-allocated array of n_results float distances
 * n_out:     actual number of results returned
 *
 * Returns CUVS_STATUS_OK on success.
 * Returns CUVS_STATUS_OOM_FALLBACK if daemon has no VRAM → use CPU.
 * Returns CUVS_STATUS_ERROR if daemon is unreachable → use CPU.
 * Returns CUVS_STATUS_NOT_FOUND if index not loaded → use CPU.
 */
int cuvs_ipc_search(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out   /* daemon-reported wall-clock; 0 if unknown */
);

/*
 * cuvs_ipc_build — send a BUILD command to the daemon.
 *
 * vecs:  corpus vectors, shape [n_vecs][dim], row-major float32
 * tids:  corresponding heap TIDs, length n_vecs
 *
 * Returns CUVS_STATUS_OK on success.
 */
int cuvs_ipc_build(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const float   *vecs,
    const uint64_t *tids,
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir   /* daemon saves index here */
);

/* Circuit breaker state machine moved to cuvs_util.h (structural commit). */
