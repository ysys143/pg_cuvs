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
#define CUVS_OP_STATUS    3   /* per-index search stats; index_oid==0 = all in db */
#define CUVS_OP_MARK_STALE 4  /* flag an index stale after a heap write */
#define CUVS_OP_CACHE_STATS 5 /* daemon-global VRAM cache counters */

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
#define CUVS_STATUS_BUILD_FAILED   5   /* GPU index build failed */
#define CUVS_STATUS_PERSIST_FAILED 6   /* build OK, disk persist failed */
#define CUVS_STATUS_DIM_MISMATCH   7   /* query dim != index dim → user error */
#define CUVS_STATUS_METRIC_MISMATCH 8  /* index built with a different metric → REINDEX */
#define CUVS_STATUS_STALE          9   /* index stale (writes since build) → CPU fallback */

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
    uint32_t table_oid;     /* BUILD: heap relation OID (for manifest) */
    uint32_t relfilenode;   /* BUILD: heap relfilenode (heap compat identity, ADR-013) */
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
    uint32_t delta_merged;  /* 1 if the daemon already merged the .delta into results
                             * (Phase 3B); backend then skips its CPU merge */
    char     error[128];
} CuvsReplyHeader;

typedef struct CuvsResult {
    uint64_t tid;           /* heap TID encoded as block<<16|offset */
    float    distance;
} CuvsResult;

/* ----------------------------------------------------------------
 * STATS reply payload (CUVS_OP_STATUS).
 *
 * Reply is the standard CuvsReplyHeader (status=OK, n_results = number of
 * index entries) followed by n_results × CuvsIndexStats. The daemon owns
 * the per-index latency histogram and sends pre-computed percentiles, so
 * the bucket count is not part of the wire ABI.
 * ---------------------------------------------------------------- */
typedef struct CuvsIndexStats {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t dim;
    uint32_t metric;            /* CUVS_METRIC_* */
    int64_t  n_vecs;
    uint64_t vram_bytes;
    uint32_t resident;          /* 1 if currently VRAM-resident (IndexEntry.valid) */
    uint32_t last_status;       /* CUVS_STATUS_* of the most recent search */
    uint64_t search_count;      /* successful (CUVS_STATUS_OK) searches */
    uint64_t error_count;       /* searches that ended non-OK and were attributable */
    uint64_t total_latency_us;  /* avg = total_latency_us / search_count */
    uint32_t last_requested_k;  /* top-k of the most recent OK search (reflects cuvs.k) */
    uint32_t last_returned_k;   /* rows the most recent OK search actually returned */
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t p99_us;
    int64_t  last_search_at;    /* epoch seconds; 0 if never searched */
    uint32_t stale;             /* 1 if writes happened since build (REINDEX needed) */
    int64_t  stale_since;       /* epoch seconds the index was first marked stale; 0 if fresh */
    char     last_error[128];
} CuvsIndexStats;

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
    uint32_t     *latency_us_out,  /* daemon-reported wall-clock; 0 if unknown */
    int          *delta_merged_out /* OUT: 1 if daemon merged .delta (may be NULL) */
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
    const char    *index_dir,   /* daemon saves index here */
    uint32_t       table_oid,   /* heap relation OID */
    uint32_t       relfilenode  /* heap relfilenode (heap compat identity) */
);

/*
 * cuvs_ipc_stats — fetch per-index search statistics (CUVS_OP_STATUS).
 *
 * index_oid == 0 requests every resident index in db_oid; otherwise just
 * that one. Fills up to `max` CuvsIndexStats into `out` and sets *n_out.
 *
 * Returns CUVS_STATUS_OK on success (including zero rows). Returns
 * CUVS_STATUS_UNAVAILABLE if the daemon is unreachable — callers (e.g. the
 * pg_stat_gpu_search view) should treat that as an empty result, never an
 * error, so monitoring keeps working while the daemon restarts.
 */
int cuvs_ipc_stats(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    CuvsIndexStats *out,
    int             max,
    int            *n_out
);

/* ----------------------------------------------------------------
 * CACHE_STATS reply payload (CUVS_OP_CACHE_STATS): daemon-global VRAM cache
 * counters. Reply is CuvsReplyHeader (status=OK, n_results=1) + one struct.
 * ---------------------------------------------------------------- */
typedef struct CuvsCacheStats {
    uint64_t hits;              /* searches served from a resident index */
    uint64_t misses;            /* searches whose index was not resident */
    uint64_t evictions;         /* LRU evictions performed */
    uint64_t reloads;           /* indexes reloaded from disk after a miss */
    uint64_t persist_failures;  /* eviction aborted because save_index failed */
    uint32_t resident_count;    /* indexes currently VRAM-resident */
    uint64_t vram_used_bytes;   /* sum of resident vram_bytes */
    uint64_t vram_budget_bytes; /* g_max_vram_bytes; 0 = unlimited */
} CuvsCacheStats;

int cuvs_ipc_cache_stats(const char *socket_path, CuvsCacheStats *out);

/*
 * cuvs_ipc_mark_stale — flag an index stale after a heap write (CUVS_OP_MARK_STALE).
 * Best-effort: returns CUVS_STATUS_UNAVAILABLE if the daemon is down (the write
 * itself must not fail because the GPU sidecar is unreachable).
 */
int cuvs_ipc_mark_stale(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid
);

/* Circuit breaker state machine moved to cuvs_util.h (structural commit). */
