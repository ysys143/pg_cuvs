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
#define CUVS_OP_SHARD_STATS 6 /* Phase 3F: per-shard stats for sharded indexes */
#define CUVS_OP_DROP_INDEX  7 /* Phase 3G.1: free a dropped index + unlink its artifacts */
#define CUVS_OP_EXPORT_ADJACENCY 8 /* Phase 3J: export CAGRA adjacency+vecs via shm */
#define CUVS_OP_EXPORT_HNSW_SHM  9 /* Phase 3J: run from_cagra() → /dev/shm, return path */
#define CUVS_OP_SEARCH_BATCH    10 /* Phase 3M: Q queries in one request, Q×K reply via shm */
#define CUVS_OP_BUILD_IVFPQ    11 /* 3P: build an IVF-PQ index */
#define CUVS_OP_SEARCH_IVFPQ   12 /* 3P: search an IVF-PQ index */
#define CUVS_OP_EXTEND         13 /* 3Q: cagra::extend CAGRA index in-place */
#define CUVS_OP_COMPACT        14 /* 3Q: cagra::merge with tombstone filter */
#define CUVS_OP_SET_VRAM_BUDGET 15 /* admin/test: set per-GPU VRAM budget (bytes); 0=unlimited */
#define CUVS_OP_EAT_VRAM        16 /* test: cudaMalloc to leave only n_vecs bytes free on GPU dim */
#define CUVS_OP_FREE_VRAM       17 /* test: release the g_vram_eaten allocation on GPU dim */

/* ----------------------------------------------------------------
 * Distance metrics (mirror pgvector operator names)
 * ---------------------------------------------------------------- */
#define CUVS_METRIC_L2      0
#define CUVS_METRIC_COSINE  1
#define CUVS_METRIC_IP      2

/* 3R: CAGRA graph-construction algorithm (cuVS 26.04 graph_build_params variant).
 * AUTO leaves cuVS's heuristic (ivf_pq for large datasets, nn_descent for small). */
#define CUVS_CAGRA_BUILD_AUTO        0
#define CUVS_CAGRA_BUILD_IVF_PQ      1
#define CUVS_CAGRA_BUILD_NN_DESCENT  2

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
#define CUVS_STATUS_NO_VECTORS    10   /* Phase 3L: brute_force requested but .vectors sidecar missing → REINDEX */
#define CUVS_STATUS_CANCELED      11   /* 3S: backend aborted the wait (statement_timeout / query cancel) */

/* ----------------------------------------------------------------
 * Command frame (sent over UDS, fixed size)
 * ---------------------------------------------------------------- */
typedef struct CuvsCmdFrame {
    uint32_t op;            /* CUVS_OP_* */
    uint32_t db_oid;        /* PostgreSQL database OID */
    uint32_t index_oid;     /* PostgreSQL index OID */
    uint32_t k;             /* top-k for SEARCH / SEARCH_BATCH */
    uint32_t metric;        /* CUVS_METRIC_* */
    uint32_t dim;           /* vector dimension */
    int64_t  n_vecs;        /* BUILD: corpus size; SEARCH_BATCH: Q query count; SEARCH: unused */
    char     shm_key[64];   /* shm_open name for payload data */
    uint32_t table_oid;     /* BUILD: heap relation OID (for manifest) */
    uint32_t relfilenode;   /* BUILD: heap relfilenode (heap compat identity, ADR-013) */
    uint32_t shard_count;   /* BUILD: Phase 3F; 0=auto (3G), 1=unsharded, >=2 = N shards */
    uint32_t shard_overfetch; /* SEARCH: Phase 3G; per-shard request k = k + this (recall slop) */
    uint32_t parallel_fanout; /* SEARCH: Phase 3G; 1 = dispatch shards concurrently, 0 = sequential */
    uint32_t use_cpu_hnsw;  /* SEARCH: Phase 3I-1; 1 = use CPU HNSW instead of GPU CAGRA */
    uint32_t search_mode;   /* SEARCH/SEARCH_BATCH: Phase 3L; 0=cagra (default), 1=brute_force */
    uint32_t bf_precision;  /* SEARCH/SEARCH_BATCH: Phase 3L; 0=float32, 1=float16 (BF only) */
    uint32_t bf_batch_wait_us; /* SEARCH: Phase 3L; daemon BF micro-batch window (0=off) */
    uint32_t n_partials;    /* BUILD: ADR-059; 0 = single corpus (shm_key/passed fd),
                             * >0 = N worker partials follow the index_dir frame as a
                             * CuvsPartialDesc list (daemon mmaps each → direct H2D). */
    uint32_t graph_degree;             /* BUILD: 3R; 0 = cuVS default (64) */
    uint32_t intermediate_graph_degree;/* BUILD: 3R; 0 = cuVS default (128) */
    uint32_t build_algo;               /* BUILD: 3R; CUVS_CAGRA_BUILD_* (0=AUTO) */
    /* D-wedge filter (Option A Custom Scan / Option B Function API).
     * 0 = no filter; >0 = sorted uint64_t TID array in filter_shm_key shm.
     * use_prefilter=0: daemon post-filters BF results to only those TIDs (D-wedge).
     * use_prefilter=1: daemon converts TIDs to GPU BITSET and calls cuVS prefilter (3O). */
    uint32_t n_filter_tids;            /* SEARCH: filter set size; 0=no filter */
    char     filter_shm_key[64];       /* SEARCH: shm_open name for sorted uint64_t TIDs */
    uint32_t use_prefilter;            /* SEARCH: 3O: 1=GPU BITSET prefilter, 0=D-wedge */
    /* 3P: IVF-PQ params (appended at end to preserve ABI for existing ops) */
    uint32_t n_lists;   /* BUILD_IVFPQ: IVF cluster count (0 = auto -> 1024) */
    uint32_t pq_bits;   /* BUILD_IVFPQ: bits per PQ code (0 = auto -> 8) */
    uint32_t pq_dim;    /* BUILD_IVFPQ: PQ subspace count (0 = auto -> ceil(dim/2)) */
    uint32_t n_probes;  /* SEARCH_IVFPQ: IVF clusters to probe at query time */
    /* 3Q: EXTEND params */
    uint32_t max_chunk_size; /* EXTEND: 0 = auto (cagra::extend_params.max_chunk_size) */
    /* shm_key reused: EXTEND payload = [float32 vecs: n_vecs×dim][uint64_t tids: n_vecs]
     * COMPACT has no extra payload — daemon reads .tombstone directly. */
} CuvsCmdFrame;

/*
 * shm layout for BUILD (single corpus, n_partials == 0):
 *   [float32 vectors: n_vecs × dim]  (row-major)
 *   [uint64_t tids:   n_vecs]        (block << 16 | offset)
 *
 * shm layout for SEARCH:
 *   [float32 query:   dim]
 *
 * ADR-059 multi-partial BUILD (n_partials > 0): after the cmd frame and the
 * index_dir frame, the leader sends n_partials × CuvsPartialDesc. Each names a
 * worker's named-shm partial with the same [vectors][tids] layout above (sized
 * to that partial's n_vecs). Σ desc.n_vecs == cmd.n_vecs.
 */
typedef struct CuvsPartialDesc {
    char     shm_name[64];  /* named-shm segment (shm_open by the daemon) */
    int64_t  n_vecs;        /* vectors in this partial; 0 = skip */
} CuvsPartialDesc;

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
    /* Phase 3A-3 delta stats (wire ABI extension — co-deploy daemon+extension) */
    int64_t  delta_rows;         /* pending-delta rows in daemon GPU cache */
    uint32_t delta_generation;   /* base_tids_crc32 the delta cache was built on */
    uint64_t delta_vram_bytes;   /* VRAM held by the delta brute-force cache */
    uint64_t delta_merged_count; /* searches where daemon merged delta on GPU */
    uint32_t delta_search_mode;  /* 0=none, 1=cpu, 2=gpu (what last search used) */
    /* Phase 3D: warmup stats (wire ABI extension — co-deploy daemon+extension) */
    uint32_t warmup_state;       /* WarmupState enum value (0=hot..5=failed) */
    int64_t  last_warmup_at;     /* epoch seconds of last completed warmup; 0 if never */
    uint32_t warmup_duration_ms; /* wall-clock ms of last download+load cycle */
    uint32_t download_count;     /* GCS downloads for this index */
    uint64_t cache_miss_count;   /* searches that found this index not resident */
    /* Phase 3E: multi-GPU */
    uint32_t gpu_device_id;      /* CUDA device this index lives on; 0xFFFFFFFF if cold or sharded */
    /* Phase 3F: 0/1 = unsharded; >=2 = sharded logical index spanning N GPUs */
    uint32_t shard_count;
    /* Phase 3I-1/3L: 0=gpu_cagra, 1=cpu_hnsw, 2=cpu_fallback, 3=gpu_bf */
    uint32_t search_mode;
    /* Phase 3L-9: coalesced brute_force micro-batch dispatches for this index */
    uint64_t bf_batch_count;
    /* Phase 4C: streaming update and compaction counters */
    int64_t  n_extended;        /* vectors added via EXTEND since last build/compact */
    uint64_t compact_count;     /* cuvsCagraMerge compact ops since last build */
    int64_t  last_compact_at;   /* epoch seconds of last compact; 0 if never */
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
 * Returns CUVS_STATUS_CANCELED if the wait callback (3S) aborted the reply wait.
 */

/* 3S: register a callback the search reply-wait polls (~250ms) to detect a
 * pending query cancel / statement_timeout. Return nonzero to abort the wait
 * (cuvs_ipc_search then returns CUVS_STATUS_CANCELED after cleanup). The callback
 * must NOT longjmp (no CHECK_FOR_INTERRUPTS) — it only inspects flags; the PG
 * caller raises the interrupt after cuvs_ipc_search returns. NULL = no cancel
 * checking (blocking wait, legacy behavior). */
void cuvs_ipc_set_wait_callback(int (*cb)(void));

/*
 * cuvs_ipc_search_filtered — D-wedge spike: filtered BF search.
 *
 * filter_tids: sorted uint64_t array of heap TIDs to include; NULL = no filter.
 * n_filter:    length of filter_tids (0 = no filter, ignored if filter_tids NULL).
 *
 * The daemon post-filters BF results to only the provided TID set.
 * Works for both unsharded and sharded (per-shard post-filter).
 * Returns same status codes as cuvs_ipc_search.
 */
int cuvs_ipc_search_filtered(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      search_mode,
    uint32_t      bf_precision,
    const uint64_t *filter_tids,   /* sorted; NULL = no filter */
    uint32_t      n_filter,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out,
    int          *delta_merged_out, /* OUT: 1 if daemon merged .delta; may be NULL */
    int           use_prefilter     /* 3O: 1=GPU BITSET prefilter, 0=D-wedge post-filter */
);

int cuvs_ipc_search(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch, /* Phase 3G: per-shard k+slop; ignored if unsharded */
    int           parallel_fanout, /* Phase 3G: 1=concurrent shard dispatch, 0=sequential */
    uint32_t      use_cpu_hnsw,    /* Phase 3I-1: 1=prefer CPU HNSW over GPU CAGRA */
    uint32_t      search_mode,     /* Phase 3L: 0=cagra (default), 1=brute_force */
    uint32_t      bf_precision,    /* Phase 3L: 0=float32, 1=float16 (BF only) */
    uint32_t      bf_batch_wait_us,/* Phase 3L: daemon BF micro-batch window μs (0=off) */
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out,  /* daemon-reported wall-clock; 0 if unknown */
    int          *delta_merged_out /* OUT: 1 if daemon merged .delta (may be NULL) */
);

/*
 * cuvs_ipc_search_batch — Phase 3M batch search (Q queries, one round-trip).
 *
 * queries is [n_queries][dim] row-major. The daemon runs one batched GPU
 * dispatch and returns up to K = min(k, corpus) neighbors per query into
 * tids_out / dist_out (each must hold n_queries*k; only the first
 * n_queries*(*k_out) entries are written, row-major [q*(*k_out) + j]).
 * Returns CUVS_STATUS_OK on success, or the same status codes as cuvs_ipc_search.
 */
int cuvs_ipc_search_batch(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *queries,         /* n_queries * dim, row-major */
    uint32_t      n_queries,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch, /* Phase 3G: per-shard k+slop; ignored if unsharded */
    int           parallel_fanout, /* Phase 3G: 1=concurrent shard dispatch */
    uint32_t      search_mode,     /* Phase 3L: 0=cagra, 1=brute_force */
    uint32_t      bf_precision,    /* Phase 3L: 0=float32, 1=float16 (BF only) */
    uint64_t     *tids_out,        /* n_queries * k */
    float        *dist_out,        /* n_queries * k */
    uint32_t     *k_out,           /* OUT: per-query result stride K (<= k) */
    uint32_t     *latency_us_out   /* daemon wall-clock; 0 if unknown (may be NULL) */
);

/*
 * cuvs_ipc_build — send a BUILD command to the daemon (ADR-057).
 *
 * The corpus ([vectors][tids] contiguous) is handed off by tier:
 *   CORPUS_MEMFD — corpus->fd is passed to the daemon via SCM_RIGHTS (no name,
 *                  no orphan possible); heap_vecs/heap_tids ignored.
 *   CORPUS_SHM   — corpus->shm_name carries the segment; caller owns its
 *                  lifetime (this fn does not unlink it).
 *   CORPUS_HEAP  — heap_vecs/heap_tids are copied into a fresh named shm here
 *                  and unlinked here before returning (legacy path).
 *
 * Returns CUVS_STATUS_OK on success.
 */
struct CuvsBuildCorpus;   /* full definition in cuvs_build_corpus.h */
int cuvs_ipc_build(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const struct CuvsBuildCorpus *corpus, /* tier + fd + shm_name */
    const float   *heap_vecs,   /* CORPUS_HEAP only; NULL otherwise */
    const uint64_t *heap_tids,  /* CORPUS_HEAP only; NULL otherwise */
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,   /* daemon saves index here */
    uint32_t       table_oid,   /* heap relation OID */
    uint32_t       relfilenode, /* heap relfilenode (heap compat identity) */
    uint32_t       shard_count, /* Phase 3F: 0/1 = unsharded, >=2 = N shards */
    uint32_t       use_cpu_hnsw,/* Phase 3I-1: 1 = serialize .hnsw sidecar */
    uint32_t       graph_degree,             /* 3R; 0 = cuVS default (64) */
    uint32_t       intermediate_graph_degree,/* 3R; 0 = cuVS default (128) */
    uint32_t       build_algo                /* 3R; CUVS_CAGRA_BUILD_* (0=AUTO) */
);

/*
 * cuvs_ipc_build_multi — ADR-059: send a BUILD command referencing N worker
 * named-shm partials instead of one merged corpus. The daemon shm_open's each
 * partial and streams it straight to the GPU (cuvs_cagra_build_multi), removing
 * the leader's merge copy. The caller (parallel-build leader) owns the partials'
 * lifetime — it unlinks them after this returns.
 *
 * No SCM_RIGHTS fd is passed (pass_fd = -1); partials are named. Σ partials[i].
 * n_vecs must equal total. Empty partials (n_vecs == 0) may be included and are
 * skipped by the daemon. Returns CUVS_STATUS_OK on success.
 */
int cuvs_ipc_build_multi(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const CuvsPartialDesc *partials,
    uint32_t       n_partials,
    int64_t        total,       /* Σ partials[i].n_vecs */
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count,
    uint32_t       use_cpu_hnsw,
    uint32_t       graph_degree,             /* 3R; 0 = cuVS default (64) */
    uint32_t       intermediate_graph_degree,/* 3R; 0 = cuVS default (128) */
    uint32_t       build_algo                /* 3R; CUVS_CAGRA_BUILD_* (0=AUTO) */
);

/*
 * cuvs_ipc_export_adjacency — Phase 3J: request CAGRA adjacency list + corpus
 * vectors from the daemon for a loaded index (CUVS_OP_EXPORT_ADJACENCY).
 *
 * Daemon copies the graph and vectors from VRAM to a shared memory segment,
 * and writes the shm_key into the reply header. The caller receives caller-
 * owned malloc'd buffers (free() each when done).
 *
 * Returns CUVS_STATUS_OK on success, CUVS_STATUS_NOT_FOUND if the index is
 * not loaded, or CUVS_STATUS_ERROR on other failures.
 *
 * shm layout (daemon-written):
 *   [uint32_t n_vecs]  [uint32_t graph_degree]  [uint32_t dim]  [uint32_t _pad]
 *   [uint32_t adj[n_vecs * graph_degree]]   row-major CAGRA adjacency
 *   [float    vecs[n_vecs * dim]]           corpus vectors (row-major float32)
 *   [uint64_t tids[n_vecs]]                 heap TIDs (block<<16|offset)
 */
/*
 * cuvs_ipc_export_hnsw_shm — Phase 3J: run from_cagra() on the loaded CAGRA
 * index and serialize the resulting multi-level HNSW to /dev/shm (no disk I/O).
 * The path is returned in shm_path_out (caller must unlink after reading).
 * Returns CUVS_STATUS_OK on success, NOT_FOUND if index not loaded.
 */
int cuvs_ipc_export_hnsw_shm(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid,
    char       *shm_path_out,  /* caller-allocated, at least 128 chars */
    size_t      shm_path_len
);

int cuvs_ipc_export_adjacency(
    const char  *socket_path,
    uint32_t     db_oid,
    uint32_t     index_oid,
    uint32_t   **adj_out,        /* [n_vecs * graph_degree], caller-freed */
    float      **vecs_out,       /* [n_vecs * dim],          caller-freed */
    uint64_t   **tids_out,       /* [n_vecs],                caller-freed */
    size_t      *n_vecs_out,
    int         *graph_degree_out,
    int         *dim_out,
    uint32_t    *metric_out      /* CUVS_METRIC_* of the source CAGRA index */
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
 * CACHE_STATS reply payload (CUVS_OP_CACHE_STATS): per-GPU VRAM cache
 * counters. Reply is CuvsReplyHeader (status=OK, n_results=N_GPUs) + N structs.
 * ---------------------------------------------------------------- */
typedef struct CuvsCacheStats {
    uint32_t gpu_device_id;     /* Phase 3E: which GPU this row describes */
    uint32_t resident_count;    /* indexes currently VRAM-resident on this GPU */
    uint64_t hits;              /* searches served from a resident index */
    uint64_t misses;            /* searches whose index was not resident */
    uint64_t evictions;         /* LRU evictions performed */
    uint64_t reloads;           /* indexes reloaded from disk after a miss */
    uint64_t persist_failures;  /* eviction aborted because save_index failed */
    uint64_t vram_used_bytes;   /* sum of resident vram_bytes */
    uint64_t vram_budget_bytes; /* g_max_vram_bytes; 0 = unlimited */
    uint64_t bf_vram_bytes;     /* Phase 3L: VRAM held by resident brute-force indexes */
    uint32_t bf_precision;      /* Phase 3L: precision of resident BF indexes (0=f32, 1=f16) */
} CuvsCacheStats;

int cuvs_ipc_cache_stats(const char *socket_path, CuvsCacheStats *out,
                         int max, int *n_out);

/* ----------------------------------------------------------------
 * SHARD_STATS reply payload (CUVS_OP_SHARD_STATS): one row per shard of every
 * resident sharded index in the requesting database (Phase 3F). Reply is
 * CuvsReplyHeader (status=OK, n_results=total shard rows) + N structs.
 * cmd.index_oid == 0 => all sharded indexes in db; otherwise just that one.
 * ---------------------------------------------------------------- */
typedef struct CuvsShardStats {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t shard_id;
    uint32_t gpu_device_id;     /* CUDA device this shard is resident on */
    int64_t  n_vecs;            /* vectors in this shard */
    int64_t  tid_offset;        /* global TID start offset of the shard range */
    uint64_t vram_bytes;        /* estimated VRAM held by this shard */
    uint64_t search_count;      /* OK searches dispatched to this shard */
    uint64_t error_count;       /* failed shard searches */
    uint32_t resident;          /* 1 if the shard handle is loaded */
    uint32_t last_status;       /* CUVS_STATUS_* of this shard's last search */
} CuvsShardStats;

int cuvs_ipc_shard_stats(const char *socket_path, uint32_t db_oid,
                         uint32_t index_oid, CuvsShardStats *out,
                         int max, int *n_out);

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

/*
 * cuvs_ipc_drop — tell the daemon a DROP INDEX committed (CUVS_OP_DROP_INDEX).
 * The daemon frees the index from VRAM and unlinks its on-disk artifacts.
 * Best-effort, like cuvs_ipc_mark_stale: returns CUVS_STATUS_UNAVAILABLE if the
 * daemon is down — the caller must NOT fail the (already committed) DROP.
 */
int cuvs_ipc_drop(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid
);

/* 3P: IVF-PQ build — heap-tier corpus (vecs+tids direct pointers). */
int cuvs_ipc_build_ivfpq(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    const float    *vecs,
    const uint64_t *tids,
    int64_t         n_vecs,
    int             dim,
    uint32_t        metric,
    const char     *index_dir,
    uint32_t        table_oid,
    uint32_t        relfilenode,
    uint32_t        n_lists,
    uint32_t        pq_bits,
    uint32_t        pq_dim
);

/* 3P: IVF-PQ search. */
int cuvs_ipc_search_ivfpq(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      n_probes,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out
);

/* 3Q: EXTEND — write n_new vectors (+ their TIDs) to shm and ask the daemon
 * to call cagra::extend in-place on the loaded CAGRA index.
 * max_chunk_size: 0 = auto. index_dir: where the daemon finds the index on disk.
 * Returns CUVS_STATUS_OK on success, CUVS_STATUS_NOT_FOUND if the index is not
 * loaded, CUVS_STATUS_UNAVAILABLE if the daemon is unreachable. */
int cuvs_ipc_extend(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    const float    *new_vecs,     /* host float32 [n_new × dim] */
    const uint64_t *new_tids,     /* host uint64_t [n_new] */
    int64_t         n_new,
    int             dim,
    uint32_t        max_chunk_size,
    const char     *index_dir
);

/* 3Q: COMPACT — ask the daemon to compact a loaded CAGRA index by removing
 * tombstoned vectors via cagra::merge. No payload: the daemon reads the
 * .tombstone sidecar directly from index_dir. Returns CUVS_STATUS_OK on
 * success or if there is nothing to compact (no .tombstone file). */
int cuvs_ipc_compact(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid,
    const char *index_dir
);

int cuvs_ipc_set_vram_budget(
    const char *socket_path,
    int64_t     budget_bytes    /* bytes; 0 = unlimited */
);

int cuvs_ipc_eat_vram(
    const char *socket_path,
    int64_t     leave_bytes,    /* bytes to leave free; 0 = eat everything */
    int         device_id
);

int cuvs_ipc_free_vram(
    const char *socket_path,
    int         device_id
);

/* Circuit breaker state machine moved to cuvs_util.h (structural commit). */
