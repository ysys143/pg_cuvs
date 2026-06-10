#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Phase 3E: maximum GPUs the daemon can manage. */
#define CUVS_MAX_GPUS 16

typedef struct CuvsGpuDeviceInfo {
    int     device_id;
    size_t  total_vram_bytes;
    char    name[64];
} CuvsGpuDeviceInfo;

/* Detect available CUDA GPUs. Fills up to max_devices entries in out[].
 * Returns the number of GPUs found (0 if none). */
int cuvs_detect_gpus(CuvsGpuDeviceInfo *out, int max_devices);

/* Result from GPU search: top-K (item_id, distance) pairs. */
typedef struct CuvsSearchResult {
    int64_t  item_id;
    float    distance;
} CuvsSearchResult;

/*
 * cuvs_brute_force_search
 *
 * Runs exact cosine/L2 search on the GPU via cuVS BruteForce API.
 * corpus_vecs  -- row-major float32 matrix, shape [n_corpus, dim]
 * query_vec    -- float32 vector, length dim
 * n_corpus     -- number of vectors in corpus
 * dim          -- vector dimension
 * top_k        -- number of results to return
 * results      -- caller-allocated array of CuvsSearchResult[top_k]
 * metric       -- CUVS_METRIC_* (see cuvs_ipc.h)
 * device_id    -- CUDA device to run on
 *
 * Returns 0 on success, non-zero on failure.
 */
int cuvs_brute_force_search(
    const float    *corpus_vecs,
    const float    *query_vec,
    int64_t         n_corpus,
    int             dim,
    int             top_k,
    uint32_t        metric,
    CuvsSearchResult *results,
    int             device_id
);

/*
 * Resident brute-force index (Phase 3B delta cache).
 *
 * cuvs_bf_build uploads the corpus once and keeps it device-resident inside an
 * opaque handle, so repeated searches reuse it (build-once / search-many). Used
 * by the daemon to hold a per-index GPU brute-force index over the pending
 * `.delta` vectors. metric is a CUVS_METRIC_* value (same scale as the CAGRA
 * base index). cuvs_bf_search returns 0 on success, 2 on dim mismatch, 1 on
 * other failure; top_k must be <= the corpus size the index was built with.
 *
 * Phase 3L: `precision` selects the resident numeric type — 0 = float32,
 * 1 = float16. float16 halves the dataset VRAM and raises throughput; the
 * search path converts the query to the matching type. The `.vectors` sidecar
 * on disk is always float32; precision only affects the device-resident copy.
 * The delta cache always uses float32 (precision=0) for CPU-exact equivalence.
 */
typedef void *CuvsBfIndex;

CuvsBfIndex cuvs_bf_build(
    const float *vecs,
    int64_t      n,
    int          dim,
    uint32_t     metric,
    uint32_t     precision,   /* 0 = float32, 1 = float16 */
    int          device_id
);

int cuvs_bf_search(
    CuvsBfIndex      index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id
);

void cuvs_bf_free(CuvsBfIndex index, int device_id);

/*
 * cuvs_bf_search_filtered — GPU BF search with host-side BITSET prefilter (3O).
 *
 * bitset_words : host uint32_t[] following pg_cuvs convention: bit[item_id]=1 means
 *               EXCLUDE that item from search; bit=0 means include.
 * bitset_bits  : total bit count = n_vecs.
 *
 * Returns 0 on success, 2 on dim mismatch, 1 on failure.
 */
int cuvs_bf_search_filtered(
    CuvsBfIndex       index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    const uint32_t   *bitset_words,
    int64_t           bitset_bits,
    CuvsSearchResult *results,
    int               device_id
);

/*
 * cuvs_cagra_build / cuvs_cagra_search
 *
 * CAGRA index operations managed by the pg_cuvs_server sidecar daemon.
 * All operations are pinned to a specific CUDA device.
 */
typedef void *CuvsCagraIndex;

/* metric is a CUVS_METRIC_* value (see cuvs_ipc.h). It is baked into the
 * CAGRA graph at build time; search inherits it. 3R: graph_degree /
 * intermediate_graph_degree (<=0 keep cuVS defaults 64/128) and build_algo
 * (CUVS_CAGRA_BUILD_*; AUTO keeps cuVS heuristic) come from index reloptions. */
CuvsCagraIndex cuvs_cagra_build(
    const float *vecs,
    int64_t      n_vecs,
    int          dim,
    uint32_t     metric,
    int          graph_degree,
    int          intermediate_graph_degree,
    uint32_t     build_algo,
    int          device_id
);

/*
 * ADR-059: build a CAGRA index from N host partitions without host-side
 * concatenation. d_corpus[total][dim] is allocated once and each partition i
 * ([n_each[i]][dim] at vecs[i]) is copied to its row offset. Σ n_each == total.
 * Used by the daemon to stream parallel-build worker partials straight to the
 * GPU (no leader merge copy). cuvs_cagra_build is the n_parts==1 special case.
 * 3R build params as in cuvs_cagra_build.
 */
CuvsCagraIndex cuvs_cagra_build_multi(
    const float **vecs,
    const int64_t *n_each,
    int           n_parts,
    int64_t       total,
    int           dim,
    uint32_t      metric,
    int           graph_degree,
    int           intermediate_graph_degree,
    uint32_t      build_algo,
    int           device_id
);

int cuvs_cagra_search(
    CuvsCagraIndex   index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id
);

/*
 * cuvs_cagra_search_filtered — GPU CAGRA search with BITSET prefilter (3O).
 *
 * bitset_words : host uint32_t[] following pg_cuvs convention: bit[item_id]=1 means
 *               EXCLUDE that item from search; bit=0 means include.
 * bitset_bits  : total bit count = n_vecs.
 *
 * Returns 0 on success, 2 on dim mismatch, 1 on failure.
 * Approximate (graph-based); faster than BF prefilter at large N.
 */
int cuvs_cagra_search_filtered(
    CuvsCagraIndex    index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    const uint32_t   *bitset_words,
    int64_t           bitset_bits,
    CuvsSearchResult *results,
    int               device_id
);

void cuvs_cagra_free(CuvsCagraIndex index, int device_id);

/* 3Q: Extend a CAGRA index in-place with n_new additional vectors.
 * new_vecs: row-major float32 [n_new × dim] on host.
 * max_chunk_size: 0 = auto (cagra::extend_params.max_chunk_size).
 * Returns 0 on success, 2 on dim mismatch, 1 on other failure. */
int cuvs_cagra_extend(CuvsCagraIndex index,
                      const float   *new_vecs,
                      int64_t        n_new,
                      int            dim,
                      uint32_t       max_chunk_size,
                      int            device_id);

/* 3Q: Compact a CAGRA index by removing tombstoned vectors.
 * keep_bits_words: host uint32_t bitset (bit[i]=1 → keep vector i).
 * n_total: total vector count (= bitset width = current index size).
 * metric: CUVS_METRIC_* — preserved in the rebuilt graph.
 * Returns a NEW CuvsCagraIndex; caller must cuvs_cagra_free the old one.
 * Returns NULL on failure. */
CuvsCagraIndex cuvs_cagra_compact(CuvsCagraIndex  index,
                                   const uint32_t *keep_bits_words,
                                   int64_t         n_total,
                                   uint32_t        metric,
                                   int             device_id);

/*
 * 3P: GPU IVF-PQ index — product quantization, 10-100× less VRAM than CAGRA.
 * IdxT = int64_t; results map directly to CuvsSearchResult.item_id.
 * n_lists/pq_bits/pq_dim: 0 = auto (1024 / 8 / ceil(dim/2)).
 */
typedef void *CuvsIvfPqIndex;

int            cuvs_ivfpq_build(const float *vecs, int64_t n_vecs, int dim,
                                 uint32_t metric, uint32_t n_lists,
                                 uint32_t pq_bits, uint32_t pq_dim,
                                 int device_id, CuvsIvfPqIndex *out);
int            cuvs_ivfpq_search(CuvsIvfPqIndex index, const float *query_vec,
                                  int dim, int top_k, uint32_t n_probes,
                                  CuvsSearchResult *results, int device_id);
int            cuvs_ivfpq_serialize(CuvsIvfPqIndex index, const char *path,
                                     int device_id);
CuvsIvfPqIndex cuvs_ivfpq_deserialize(const char *path, int device_id);
void           cuvs_ivfpq_free(CuvsIvfPqIndex index, int device_id);

/*
 * Phase 3M: batched search — n_queries queries in one GPU dispatch.
 * queries is [n_queries][dim] row-major; results is [n_queries][top_k] row-major
 * (results[q*top_k + j] = j-th neighbor of query q). cuVS cagra/brute_force
 * search accept a Q×dim query matrix, so the whole batch is one kernel launch.
 * Caller must pass top_k <= corpus size. cuvs_bf_search_batch pads any tail
 * slots [n, top_k) with item_id = -1 when the BF corpus has fewer than top_k
 * vectors. Both return 0 on success, 2 on dim mismatch, 1 on other failure.
 */
int cuvs_cagra_search_batch(
    CuvsCagraIndex   index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id
);

int cuvs_bf_search_batch(
    CuvsBfIndex      index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id
);

/*
 * cuVS CPU HNSW index (Phase 3I-1)
 *
 * After CAGRA build, cuvs_hnsw_serialize() converts the GPU graph to a
 * CPU hnswlib index and writes it to `path` (.hnsw sidecar).  The daemon
 * can then load and search it without a GPU when cpu_hnsw_fallback is on.
 */
typedef void *CuvsHnswIndex;

int           cuvs_hnsw_serialize(CuvsCagraIndex cagra_idx,
                                   const char *path, int device_id);
CuvsHnswIndex cuvs_hnsw_deserialize(const char *path, int dim,
                                     uint32_t metric, int device_id);
/* ef <= 0 => use max(200, k) */
int           cuvs_hnsw_search(CuvsHnswIndex hidx, const float *query,
                                int dim, int k, int ef,
                                CuvsSearchResult *out);
void          cuvs_hnsw_free(CuvsHnswIndex hidx);

/*
 * cuvs_cagra_extract_adjacency — Phase 3J: direct CAGRA→pgvector path.
 *
 * Copies the CAGRA graph adjacency list and corpus vectors from GPU VRAM to
 * caller-allocated CPU buffers (freed by the caller with free()).
 *
 * adj_out       : [n_vecs * graph_degree] uint32_t row-major adjacency matrix
 * vecs_out      : [n_vecs * dim] float32 corpus vectors
 * n_vecs_out    : number of vectors
 * graph_degree_out : adjacency list width (CAGRA build parameter)
 *
 * Returns 0 on success, -1 on failure (logs to stderr).
 */
int cuvs_cagra_extract_adjacency(
    CuvsCagraIndex  handle,
    uint32_t      **adj_out,
    float         **vecs_out,
    size_t         *n_vecs_out,
    int            *graph_degree_out,
    int             device_id
);

/*
 * cuvs_cagra_serialize / cuvs_cagra_deserialize
 * Persist/restore a CAGRA index to/from a file path using cuVS native format.
 */
int            cuvs_cagra_serialize(CuvsCagraIndex index, const char *path,
                                    int device_id);
CuvsCagraIndex cuvs_cagra_deserialize(const char *path, int dim,
                                       int device_id);

/* VRAM query -- returns free VRAM bytes on the specified CUDA device. */
size_t cuvs_vram_free_bytes_on(int device_id);

/* Legacy: free VRAM on device 0 (used by unit test binary). */
size_t cuvs_vram_free_bytes(void);

/* GPU availability check -- returns 1 if any CUDA device is accessible. */
int cuvs_gpu_available(void);

/* Warm-up a specific GPU device: trigger one-time context/RMM/cuBLAS init. */
void cuvs_warmup_device(int device_id);

/* Legacy: warm-up device 0 (used by unit test binary). */
void cuvs_warmup(void);

/* test/admin: eat/free raw VRAM to trigger physical CUDA OOM */
int cuvs_eat_vram(int64_t leave_bytes, int device_id);
int cuvs_free_vram(int device_id);

/* test: arm (enable=1) or disarm (enable=0) synthetic OOM injection in cuvs_cagra_extend.
 * When armed, the next extend call throws bad_alloc and self-clears the flag. */
void cuvs_set_inject_extend_oom(int enable);

#ifdef __cplusplus
}
#endif
