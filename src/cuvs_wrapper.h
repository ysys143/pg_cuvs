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
 */
typedef void *CuvsBfIndex;

CuvsBfIndex cuvs_bf_build(
    const float *vecs,
    int64_t      n,
    int          dim,
    uint32_t     metric,
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
 * cuvs_cagra_build / cuvs_cagra_search
 *
 * CAGRA index operations managed by the pg_cuvs_server sidecar daemon.
 * All operations are pinned to a specific CUDA device.
 */
typedef void *CuvsCagraIndex;

/* metric is a CUVS_METRIC_* value (see cuvs_ipc.h). It is baked into the
 * CAGRA graph at build time; search inherits it. */
CuvsCagraIndex cuvs_cagra_build(
    const float *vecs,
    int64_t      n_vecs,
    int          dim,
    uint32_t     metric,
    int          device_id
);

int cuvs_cagra_search(
    CuvsCagraIndex   index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id
);

void cuvs_cagra_free(CuvsCagraIndex index, int device_id);

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

#ifdef __cplusplus
}
#endif
