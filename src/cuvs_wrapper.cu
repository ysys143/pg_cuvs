/*
 * cuvs_wrapper.cu — C++ / CUDA bridge to NVIDIA cuVS.
 *
 * Compiled as CUDA C++ (nvcc). Exposes a pure-C API declared in
 * cuvs_wrapper.h so that pg_cuvs.c (plain C) can call it without
 * triggering the float4 typedef collision between PostgreSQL headers
 * and CUDA headers.
 *
 * Dependency: libcuvs (RAPIDS cuVS). Install via conda:
 *   mamba install -c rapidsai -c nvidia libcuvs cuda-version=12.4
 */

#include "cuvs_wrapper.h"

#include <cuvs/neighbors/brute_force.hpp>
#include <cuvs/neighbors/cagra.hpp>
#include <raft/core/device_resources.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/host_mdarray.hpp>

#include <cuda_runtime.h>
#include <cstring>
#include <memory>
#include <vector>

/* ----------------------------------------------------------------
 * GPU availability check
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_gpu_available(void)
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    return (err == cudaSuccess && device_count > 0) ? 1 : 0;
}

/* ----------------------------------------------------------------
 * Brute-force search (exact, no index)
 *
 * Used during Phase 1 before CAGRA index build is implemented.
 * Transfers corpus to GPU on every call — not production-ready,
 * but sufficient to prove the GPU computation path works.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_brute_force_search(
    const float      *corpus_vecs,
    const float      *query_vec,
    int64_t           n_corpus,
    int               dim,
    int               top_k,
    CuvsSearchResult *results)
{
    try {
        raft::device_resources res;

        /* Upload corpus to device */
        auto d_corpus = raft::make_device_matrix<float, int64_t>(
            res, n_corpus, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), corpus_vecs,
                   n_corpus * dim, res.get_stream());

        /* Upload query to device */
        auto d_queries = raft::make_device_matrix<float, int64_t>(
            res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec,
                   dim, res.get_stream());

        /* Allocate result buffers on device */
        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, top_k);

        /* Build brute-force index and search */
        auto index = cuvs::neighbors::brute_force::build(
            res,
            raft::make_const_mdspan(d_corpus.view()),
            cuvs::distance::DistanceType::L2Expanded);

        cuvs::neighbors::brute_force::search(
            res, index,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        /* Copy results back to host */
        std::vector<int64_t> h_indices(top_k);
        std::vector<float>   h_distances(top_k);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   top_k, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), top_k, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < top_k; i++) {
            results[i].item_id  = h_indices[i];
            results[i].distance = h_distances[i];
        }

        return 0;
    } catch (...) {
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index — Phase 1 stubs
 *
 * These will be filled in once the sidecar daemon and shared-memory
 * IPC layer are implemented. For now they return null/error so the
 * planner falls back to CPU via cuvsamcostestimate.
 * ---------------------------------------------------------------- */
extern "C" CuvsCagraIndex
cuvs_cagra_build(const float *vecs, int64_t n_vecs, int dim)
{
    /* TODO Phase 1: implement CAGRA build */
    (void)vecs; (void)n_vecs; (void)dim;
    return nullptr;
}

extern "C" int
cuvs_cagra_search(CuvsCagraIndex index, const float *query_vec,
                  int dim, int top_k, CuvsSearchResult *results)
{
    /* TODO Phase 1: implement CAGRA search */
    (void)index; (void)query_vec; (void)dim; (void)top_k; (void)results;
    return 1;
}

extern "C" void
cuvs_cagra_free(CuvsCagraIndex index)
{
    /* TODO Phase 1: free CAGRA index resources */
    (void)index;
}
