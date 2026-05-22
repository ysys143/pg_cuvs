#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Result from GPU search: top-K (item_id, distance) pairs. */
typedef struct CuvsSearchResult {
    int64_t  item_id;
    float    distance;
} CuvsSearchResult;

/*
 * cuvs_brute_force_search
 *
 * Runs exact cosine/L2 search on the GPU via cuVS BruteForce API.
 * corpus_vecs  — row-major float32 matrix, shape [n_corpus, dim]
 * query_vec    — float32 vector, length dim
 * n_corpus     — number of vectors in corpus
 * dim          — vector dimension
 * top_k        — number of results to return
 * results      — caller-allocated array of CuvsSearchResult[top_k]
 *
 * Returns 0 on success, non-zero on failure.
 */
int cuvs_brute_force_search(
    const float    *corpus_vecs,
    const float    *query_vec,
    int64_t         n_corpus,
    int             dim,
    int             top_k,
    CuvsSearchResult *results
);

/*
 * cuvs_cagra_build / cuvs_cagra_search
 *
 * Placeholder for CAGRA index operations (Phase 1 in-progress).
 * These functions will operate on an opaque index handle managed
 * by the pg_cuvs_server sidecar daemon.
 */
typedef void *CuvsCagraIndex;

CuvsCagraIndex cuvs_cagra_build(
    const float *vecs,
    int64_t      n_vecs,
    int          dim
);

int cuvs_cagra_search(
    CuvsCagraIndex   index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results
);

void cuvs_cagra_free(CuvsCagraIndex index);

/*
 * cuvs_cagra_serialize / cuvs_cagra_deserialize
 * Persist/restore a CAGRA index to/from a file path using cuVS native format.
 */
int            cuvs_cagra_serialize(CuvsCagraIndex index, const char *path);
CuvsCagraIndex cuvs_cagra_deserialize(const char *path, int dim);

/* VRAM query — returns free VRAM bytes on the current CUDA device. */
size_t cuvs_vram_free_bytes(void);

/* GPU availability check — returns 1 if CUDA device is accessible. */
int cuvs_gpu_available(void);

#ifdef __cplusplus
}
#endif
