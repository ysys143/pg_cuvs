/*
 * cuvs_wrapper.cu — C++ / CUDA bridge to NVIDIA cuVS.
 *
 * Compiled as CUDA C++ (nvcc). Exposes a pure-C API declared in
 * cuvs_wrapper.h so that pg_cuvs.c (plain C) can call it without
 * triggering the float4 typedef collision between PostgreSQL headers
 * and CUDA headers.
 *
 * Dependency: libcuvs (RAPIDS cuVS). Install via conda:
 *   mamba install -c rapidsai -c nvidia libcuvs=24.12 cuda-toolkit=12.4
 */

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"   /* CUVS_METRIC_* (PG-free, CUDA-free defines) */

#include <cuvs/neighbors/brute_force.hpp>
#include <cuvs/neighbors/cagra.hpp>  /* serialize/deserialize merged here in cuVS 25.x+ */
#include <cuvs/neighbors/hnsw.hpp>   /* Phase 3I-1: CPU HNSW fallback */
#include <raft/core/device_resources.hpp>
#include <raft/core/device_mdarray.hpp>
#include <raft/core/host_mdarray.hpp>

#include <cuda_runtime.h>
#include <cuda_fp16.h>   /* Phase 3L: half precision for brute-force (cuvs.bf_precision) */
#include <cstring>
#include <memory>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <mutex>

/* ----------------------------------------------------------------
 * Per-device device_resources pool (Phase 3E)
 *
 * Each cuVS call needs a raft::device_resources (a CUDA stream + lazily
 * created cuBLAS/cuSOLVER handles). Creating one per call repeats that setup,
 * and the daemon spawns a NEW thread per request (so thread_local would never
 * be reused). Instead we keep a per-device free-list and hand resources
 * out via RAII: a request borrows one (creating lazily if none free) and
 * returns it on scope exit. Sequential reuse across threads is safe because
 * the streams/handles are bound to a specific CUDA device context, and the
 * free-list guarantees only one thread uses a given object at a time.
 *
 * raft::device_resources objects are permanently bound to the CUDA device
 * they were created on. A resource created on GPU-0 cannot be used for
 * GPU-2 operations. Each device gets its own pool.
 *
 * device_resources has a deleted move ctor in cuVS 25.x+, so the pool holds
 * unique_ptr (movable) rather than the objects by value.
 * ---------------------------------------------------------------- */
namespace {

struct DevicePool {
    std::mutex mutex;
    std::vector<std::unique_ptr<raft::device_resources>> pool;
};

static DevicePool g_device_pools[CUVS_MAX_GPUS];

std::unique_ptr<raft::device_resources> acquire_res(int device_id)
{
    DevicePool &dp = g_device_pools[device_id];
    {
        std::lock_guard<std::mutex> lk(dp.mutex);
        if (!dp.pool.empty()) {
            auto r = std::move(dp.pool.back());
            dp.pool.pop_back();
            return r;
        }
    }
    cudaError_t err = cudaSetDevice(device_id);
    if (err != cudaSuccess) {
        fprintf(stderr, "[acquire_res] cudaSetDevice(%d) failed: %s\n",
                device_id, cudaGetErrorString(err));
        throw std::runtime_error("cudaSetDevice failed in acquire_res");
    }
    return std::make_unique<raft::device_resources>();
}

void release_res(int device_id, std::unique_ptr<raft::device_resources> r)
{
    DevicePool &dp = g_device_pools[device_id];
    std::lock_guard<std::mutex> lk(dp.mutex);
    dp.pool.push_back(std::move(r));
}

/* RAII: borrow on construction, return to the pool on destruction. If a cuVS
 * call threw, the stream/handles may carry a sticky error; reusing such a
 * resource from the pool can abort a later, unrelated request (the whole
 * daemon, since one process serves every backend). poison() makes the
 * destructor DESTROY the resource instead of pooling it, so a failed request
 * cannot corrupt healthy ones. Declare PooledRes OUTSIDE the try block and
 * call poison() in the catch so it is still alive when the catch runs.
 *
 * cudaSetDevice is per-thread state. Since the daemon spawns one pthread per
 * connection and each handles exactly one request, setting the device in the
 * constructor is safe -- no other thread can race the device selection within
 * the same PooledRes lifetime. */
struct PooledRes {
    std::unique_ptr<raft::device_resources> r;
    int dev;
    bool poisoned = false;
    explicit PooledRes(int device_id) : r(acquire_res(device_id)), dev(device_id) {
        cudaError_t err = cudaSetDevice(device_id);
        if (err != cudaSuccess) {
            fprintf(stderr, "[PooledRes] cudaSetDevice(%d) failed: %s\n",
                    device_id, cudaGetErrorString(err));
            poisoned = true;
            throw std::runtime_error("cudaSetDevice failed");
        }
    }
    ~PooledRes() { if (r && !poisoned) release_res(dev, std::move(r)); }
    raft::device_resources &get() { return *r; }
    void poison() { poisoned = true; }
};

} // namespace

/* Opaque CAGRA index wrapper.
 *
 * The dataset (d_corpus) MUST outlive the index because cagra::index holds an
 * mdspan view into the dataset's device memory rather than owning it. If the
 * dataset is freed (e.g., function-scope local), the index has dangling
 * pointers. Symptom: SIGSEGV on serialize(include_dataset=true) and on search.
 *
 * raft::device_resources has a deleted move constructor in cuVS 25.x+; the
 * pool above holds them via unique_ptr and hands them out with PooledRes.
 */
struct CuvsCagraIndexImpl {
    /* dataset MUST be declared before idx so destruction is reverse order:
     * idx destroyed first, then dataset. */
    raft::device_matrix<float, int64_t> dataset;
    cuvs::neighbors::cagra::index<float, uint32_t> idx;

    CuvsCagraIndexImpl(raft::device_matrix<float, int64_t> &&d,
                       cuvs::neighbors::cagra::index<float, uint32_t> &&i)
        : dataset(std::move(d)), idx(std::move(i))
    {}
};

/* Forward decl: defined below, but cuvs_brute_force_search / cuvs_bf_build (above
 * the definition) need the metric -> cuVS DistanceType mapping. */
static cuvs::distance::DistanceType cuvs_distance_type(uint32_t metric);

/* Phase 3I-1: CPU HNSW index (hnswlib-backed via cuVS).
 * from_cagra() returns unique_ptr; deserialize() returns raw ptr via out-param.
 * We store a raw ptr and delete in cuvs_hnsw_free. */
struct CuvsHnswIndexImpl {
    cuvs::neighbors::hnsw::index<float> *idx; /* owned; delete in free */
    int     dim;
    uint32_t metric;
};

/* Opaque resident brute-force index (Phase 3B delta cache + Phase 3L main BF).
 * Like CAGRA, the dataset must outlive the index, so we retain it alongside;
 * n bounds a defensive top_k clamp (brute_force cannot return more than the
 * corpus). Phase 3L: the resident copy is either float32 or float16 — exactly
 * one variant pair is active (the other is null). Heap-owned so the index's
 * dataset view stays valid for the handle's lifetime. */
struct CuvsBfIndexImpl {
    int      dim;
    int64_t  n;
    uint32_t precision;   /* 0 = float32, 1 = float16 */

    /* cuVS brute_force::index is index<DataT, DistanceT>; distances always
     * accumulate in float, so the half variant is index<half, float> (NOT
     * index<half, half>) to match build()'s return type and search()'s
     * overloads. float's DistanceT already defaults to float. */
    raft::device_matrix<float, int64_t>              *ds_f32  = nullptr;
    cuvs::neighbors::brute_force::index<float>        *idx_f32 = nullptr;
    raft::device_matrix<half, int64_t>               *ds_f16  = nullptr;
    cuvs::neighbors::brute_force::index<half, float>  *idx_f16 = nullptr;

    CuvsBfIndexImpl(int dm, int64_t nn, uint32_t prec)
        : dim(dm), n(nn), precision(prec) {}
    ~CuvsBfIndexImpl() {
        delete idx_f32; delete ds_f32;
        delete idx_f16; delete ds_f16;
    }
};

/* ----------------------------------------------------------------
 * GPU detection (Phase 3E)
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_detect_gpus(CuvsGpuDeviceInfo *out, int max_devices)
{
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err != cudaSuccess || device_count <= 0)
        return 0;

    int n = (device_count < max_devices) ? device_count : max_devices;
    for (int i = 0; i < n; i++) {
        out[i].device_id = i;
        out[i].total_vram_bytes = 0;
        out[i].name[0] = '\0';

        cudaDeviceProp prop;
        if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: cudaGetDeviceProperties failed, treating as unavailable\n", i);
            continue;
        }
        if (cudaSetDevice(i) != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: cudaSetDevice failed, treating as unavailable\n", i);
            continue;
        }
        void *probe = NULL;
        cudaError_t probe_err = cudaMalloc(&probe, 1);
        if (probe_err != cudaSuccess) {
            fprintf(stderr, "[cuvs_detect_gpus] GPU %d: health probe cudaMalloc failed (%s), treating as unavailable\n",
                    i, cudaGetErrorString(probe_err));
            continue;
        }
        cudaFree(probe);

        out[i].total_vram_bytes = prop.totalGlobalMem;
        strncpy(out[i].name, prop.name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }
    return n;
}

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
 * VRAM query
 * ---------------------------------------------------------------- */
extern "C" size_t
cuvs_vram_free_bytes_on(int device_id)
{
    size_t free_bytes = 0, total_bytes = 0;
    if (cudaSetDevice(device_id) != cudaSuccess)
        return 0;
    cudaMemGetInfo(&free_bytes, &total_bytes);
    return free_bytes;
}

extern "C" size_t
cuvs_vram_free_bytes(void)
{
    return cuvs_vram_free_bytes_on(0);
}

/* ----------------------------------------------------------------
 * Brute-force search (exact, no index)
 * Used when no CAGRA index is available.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_brute_force_search(
    const float      *corpus_vecs,
    const float      *query_vec,
    int64_t           n_corpus,
    int               dim,
    int               top_k,
    uint32_t          metric,
    CuvsSearchResult *results,
    int               device_id)
{
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, n_corpus, (int64_t)dim);
        raft::copy(d_corpus.data_handle(), corpus_vecs, n_corpus * dim, res.get_stream());

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, top_k);

        auto index = cuvs::neighbors::brute_force::build(
            res,
            raft::make_const_mdspan(d_corpus.view()),
            cuvs_distance_type(metric));

        cuvs::neighbors::brute_force::search(
            res, index,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

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
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * Resident brute-force index (Phase 3B delta cache): build once, search many.
 * ---------------------------------------------------------------- */
extern "C" CuvsBfIndex
cuvs_bf_build(const float *vecs, int64_t n, int dim, uint32_t metric,
              uint32_t precision, int device_id)
{
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        const size_t total = (size_t)n * (size_t)dim;

        /* Upload corpus once; retained in the impl so the index's view stays
         * valid for the handle's lifetime (mirror of the CAGRA dataset rule).
         * unique_ptr makes a mid-build throw clean up the partial impl. */
        std::unique_ptr<CuvsBfIndexImpl> impl(new CuvsBfIndexImpl(dim, n, precision));

        if (precision == 1 /* float16 */) {
            /* Convert host float32 -> host half once, then upload. half search
             * accumulates in float32 inside cuVS, so recall is preserved while
             * the resident dataset uses half the VRAM bandwidth. */
            impl->ds_f16 = new raft::device_matrix<half, int64_t>(
                raft::make_device_matrix<half, int64_t>(res, n, (int64_t)dim));
            {
                std::vector<half> h_half(total);
                for (size_t i = 0; i < total; i++)
                    h_half[i] = __float2half(vecs[i]);
                raft::copy(impl->ds_f16->data_handle(), h_half.data(), total, res.get_stream());
                res.sync_stream();
            }
            auto idx = cuvs::neighbors::brute_force::build(
                res,
                raft::make_const_mdspan(impl->ds_f16->view()),
                cuvs_distance_type(metric));
            res.sync_stream();
            impl->idx_f16 = new cuvs::neighbors::brute_force::index<half, float>(std::move(idx));
        } else {
            impl->ds_f32 = new raft::device_matrix<float, int64_t>(
                raft::make_device_matrix<float, int64_t>(res, n, (int64_t)dim));
            raft::copy(impl->ds_f32->data_handle(), vecs, total, res.get_stream());
            res.sync_stream();
            auto idx = cuvs::neighbors::brute_force::build(
                res,
                raft::make_const_mdspan(impl->ds_f32->view()),
                cuvs_distance_type(metric));
            res.sync_stream();
            impl->idx_f32 = new cuvs::neighbors::brute_force::index<float>(std::move(idx));
        }
        return impl.release();
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_build] exception: %s\n", e.what());
        _pr.poison();
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_build] unknown exception\n");
        _pr.poison();
        return nullptr;
    }
}

extern "C" int
cuvs_bf_search(
    CuvsBfIndex       index,
    const float      *query_vec,
    int               dim,
    int               top_k,
    CuvsSearchResult *results,
    int               device_id)
{
    if (!index)
        return 1;

    CuvsBfIndexImpl *impl = static_cast<CuvsBfIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    /* brute_force cannot return more neighbors than the corpus holds. */
    if ((int64_t)top_k > impl->n)
        top_k = (int)impl->n;
    if (top_k <= 0)
        return 0;

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, 1, top_k);

        if (impl->precision == 1 /* float16 */) {
            auto d_queries = raft::make_device_matrix<half, int64_t>(res, (int64_t)1, (int64_t)dim);
            {
                std::vector<half> h_q(dim);
                for (int i = 0; i < dim; i++)
                    h_q[i] = __float2half(query_vec[i]);
                raft::copy(d_queries.data_handle(), h_q.data(), dim, res.get_stream());
            }
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f16,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view());
        } else {
            auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
            raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f32,
                raft::make_const_mdspan(d_queries.view()),
                d_indices.view(),
                d_distances.view());
        }

        res.sync_stream();

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
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_search] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_search] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" void
cuvs_bf_free(CuvsBfIndex index, int device_id)
{
    if (index) {
        if (cudaSetDevice(device_id) != cudaSuccess)
            fprintf(stderr, "[cuvs_bf_free] cudaSetDevice(%d) failed\n", device_id);
        delete static_cast<CuvsBfIndexImpl *>(index);
    }
}

/* ----------------------------------------------------------------
 * CAGRA index build
 * ---------------------------------------------------------------- */
/* Map a CUVS_METRIC_* code to the cuVS DistanceType baked into the graph. */
static cuvs::distance::DistanceType
cuvs_distance_type(uint32_t metric)
{
    switch (metric) {
        case CUVS_METRIC_COSINE: return cuvs::distance::DistanceType::CosineExpanded;
        case CUVS_METRIC_IP:     return cuvs::distance::DistanceType::InnerProduct;
        case CUVS_METRIC_L2:
        default:                 return cuvs::distance::DistanceType::L2Expanded;
    }
}

/* ADR-059: build a CAGRA index from N host partitions WITHOUT host-side
 * concatenation. One device matrix [total][dim] is allocated and each partition
 * is copied to its row offset (N H2D copies vs one). This lets the daemon stream
 * parallel-build worker partials (separate named-shm segments) straight to the
 * GPU, eliminating the leader's merge copy (ADR-058 bottleneck).
 *
 * Correctness: CAGRA is order-independent and the daemon pairs (vector[i],
 * tid[i]) positionally, so concatenating partitions (each partition's pairs kept
 * in order) is equivalent to a single contiguous corpus. n_parts==1 reduces to a
 * single offset-0 copy — byte-identical to the legacy single-corpus build. */
extern "C" CuvsCagraIndex
cuvs_cagra_build_multi(const float **vecs, const int64_t *n_each, int n_parts,
                       int64_t total, int dim, uint32_t metric,
                       int graph_degree, int intermediate_graph_degree,
                       uint32_t build_algo, int device_id)
{
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        /* One device matrix for the whole corpus; moved into the impl below so
         * its memory stays alive for the index's lifetime. */
        auto d_corpus = raft::make_device_matrix<float, int64_t>(res, total, (int64_t)dim);
        int64_t off = 0;   /* running row offset into d_corpus */
        for (int i = 0; i < n_parts; i++) {
            if (n_each[i] <= 0)
                continue;   /* empty worker share: nothing to copy */
            raft::copy(d_corpus.data_handle() + off * (int64_t)dim,
                       vecs[i], n_each[i] * (int64_t)dim, res.get_stream());
            off += n_each[i];
        }
        res.sync_stream();

        /* CAGRA build parameters. metric is baked into the graph; search inherits
         * it. graph_degree / intermediate_graph_degree / build_algo come from the
         * index's reloptions (3R) — <=0 / AUTO keep cuVS defaults. */
        cuvs::neighbors::cagra::index_params params;
        params.metric                = cuvs_distance_type(metric);
        params.graph_degree              = (graph_degree > 0)
                                         ? (size_t)graph_degree : 64;
        params.intermediate_graph_degree = (intermediate_graph_degree > 0)
                                         ? (size_t)intermediate_graph_degree : 128;

        /* build_algo: graph-construction algorithm (cuVS 26.04 graph_build_params
         * std::variant). AUTO leaves std::monostate so cuVS picks heuristically
         * (ivf_pq for large, nn_descent for small). */
        namespace gbp = cuvs::neighbors::cagra::graph_build_params;
        if (build_algo == CUVS_CAGRA_BUILD_IVF_PQ)
            params.graph_build_params = gbp::ivf_pq_params(d_corpus.extents());
        else if (build_algo == CUVS_CAGRA_BUILD_NN_DESCENT)
            params.graph_build_params =
                gbp::nn_descent_params(params.intermediate_graph_degree);
        /* else CUVS_CAGRA_BUILD_AUTO: leave monostate (heuristic). */

        auto idx = cuvs::neighbors::cagra::build(
            res,
            params,
            raft::make_const_mdspan(d_corpus.view()));

        /* Re-attach dataset explicitly so the index owns a valid view that
         * matches our retained d_corpus memory. */
        idx.update_dataset(res, raft::make_const_mdspan(d_corpus.view()));
        res.sync_stream();

        return new CuvsCagraIndexImpl(std::move(d_corpus), std::move(idx));
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_build_multi] exception: %s\n", e.what());
        _pr.poison();
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_build_multi] unknown exception\n");
        _pr.poison();
        return nullptr;
    }
}

extern "C" CuvsCagraIndex
cuvs_cagra_build(const float *vecs, int64_t n_vecs, int dim, uint32_t metric,
                 int graph_degree, int intermediate_graph_degree,
                 uint32_t build_algo, int device_id)
{
    /* Single-partition special case of the multi-partition build. */
    const float  *parts[1]  = { vecs };
    const int64_t n_each[1] = { n_vecs };
    return cuvs_cagra_build_multi(parts, n_each, 1, n_vecs, dim, metric,
                                  graph_degree, intermediate_graph_degree,
                                  build_algo, device_id);
}

/* ----------------------------------------------------------------
 * CAGRA index search
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_search(
    CuvsCagraIndex   index,
    const float     *query_vec,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id)
{
    if (!index)
        return 1;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);

    /* Guard: a query whose dimension differs from the index dimension makes
     * cuVS throw a RAFT failure mid-search, and the resulting sticky CUDA
     * error has aborted the entire daemon (SIGABRT). Refuse it here, before
     * touching the GPU. Return 2 = dimension mismatch (distinct from 1). */
    if ((int64_t)dim != impl->idx.dim()) {
        fprintf(stderr,
                "[cuvs_cagra_search] dim mismatch: query=%d index=%lld; refusing\n",
                dim, (long long)impl->idx.dim());
        return 2;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, (int64_t)1, (int64_t)dim);
        raft::copy(d_queries.data_handle(), query_vec, dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, 1, top_k);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, 1, top_k);

        cuvs::neighbors::cagra::search_params sparams;
        /* CAGRA multi-cta search requires:
         *   num_cta_per_query * 32 >= top_k
         * where num_cta_per_query = max(search_width, ceil(itopk_size / 32))
         * Default itopk_size=64, search_width=1 -> num_cta=2 -> max top_k=64.
         * Round itopk_size up to a multiple of 32 that satisfies top_k. */
        int itopk = ((top_k + 31) / 32) * 32;
        if (itopk < 64) itopk = 64;
        sparams.itopk_size = itopk;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());

        res.sync_stream();

        std::vector<uint32_t> h_indices(top_k);
        std::vector<float>    h_distances(top_k);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   top_k, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), top_k, res.get_stream());
        res.sync_stream();

        for (int i = 0; i < top_k; i++) {
            results[i].item_id  = (int64_t)h_indices[i];
            results[i].distance = h_distances[i];
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * Phase 3M: batched search — Q queries in one GPU dispatch.
 * queries is Q*dim row-major; results is Q*top_k row-major
 * (results[q*top_k + j] = j-th neighbor of query q). One kernel launch for the
 * whole batch (cuVS cagra/brute_force search take a Q×dim query matrix), so the
 * fixed per-call overhead is amortized across Q. The caller must pass
 * top_k <= corpus size.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_search_batch(
    CuvsCagraIndex   index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id)
{
    if (!index)
        return 1;
    if (n_queries <= 0 || top_k <= 0)
        return 0;

    CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
    if ((int64_t)dim != impl->idx.dim()) {
        fprintf(stderr,
                "[cuvs_cagra_search_batch] dim mismatch: query=%d index=%lld; refusing\n",
                dim, (long long)impl->idx.dim());
        return 2;
    }

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        int64_t Q = n_queries;
        size_t  n = (size_t)Q * (size_t)top_k;

        auto d_queries = raft::make_device_matrix<float, int64_t>(res, Q, (int64_t)dim);
        raft::copy(d_queries.data_handle(), queries, (size_t)Q * dim, res.get_stream());

        auto d_indices   = raft::make_device_matrix<uint32_t, int64_t>(res, Q, top_k);
        auto d_distances = raft::make_device_matrix<float,    int64_t>(res, Q, top_k);

        cuvs::neighbors::cagra::search_params sparams;
        int itopk = ((top_k + 31) / 32) * 32;
        if (itopk < 64) itopk = 64;
        sparams.itopk_size = itopk;

        cuvs::neighbors::cagra::search(
            res, sparams, impl->idx,
            raft::make_const_mdspan(d_queries.view()),
            d_indices.view(),
            d_distances.view());
        res.sync_stream();

        std::vector<uint32_t> h_indices(n);
        std::vector<float>    h_distances(n);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   n, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), n, res.get_stream());
        res.sync_stream();

        for (size_t i = 0; i < n; i++) {
            results[i].item_id  = (int64_t)h_indices[i];
            results[i].distance = h_distances[i];
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_search_batch] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_search_batch] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

extern "C" int
cuvs_bf_search_batch(
    CuvsBfIndex      index,
    const float     *queries,
    int              n_queries,
    int              dim,
    int              top_k,
    CuvsSearchResult *results,
    int              device_id)
{
    if (!index)
        return 1;
    if (n_queries <= 0 || top_k <= 0)
        return 0;

    CuvsBfIndexImpl *impl = static_cast<CuvsBfIndexImpl *>(index);
    if (dim != impl->dim)
        return 2;
    /* brute_force cannot return more neighbors than the corpus holds; the tail
     * slots [bk, top_k) are padded with a sentinel so the Q*top_k layout holds. */
    int bk = top_k;
    if ((int64_t)bk > impl->n)
        bk = (int)impl->n;

    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        int64_t Q = n_queries;
        size_t  nb = (size_t)Q * (size_t)bk;

        auto d_indices   = raft::make_device_matrix<int64_t, int64_t>(res, Q, bk);
        auto d_distances = raft::make_device_matrix<float,   int64_t>(res, Q, bk);

        if (impl->precision == 1 /* float16 */) {
            auto d_q = raft::make_device_matrix<half, int64_t>(res, Q, (int64_t)dim);
            {
                std::vector<half> h_q((size_t)Q * dim);
                for (size_t i = 0; i < (size_t)Q * dim; i++)
                    h_q[i] = __float2half(queries[i]);
                raft::copy(d_q.data_handle(), h_q.data(), (size_t)Q * dim, res.get_stream());
            }
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f16,
                raft::make_const_mdspan(d_q.view()),
                d_indices.view(),
                d_distances.view());
        } else {
            auto d_q = raft::make_device_matrix<float, int64_t>(res, Q, (int64_t)dim);
            raft::copy(d_q.data_handle(), queries, (size_t)Q * dim, res.get_stream());
            cuvs::neighbors::brute_force::search(
                res, *impl->idx_f32,
                raft::make_const_mdspan(d_q.view()),
                d_indices.view(),
                d_distances.view());
        }
        res.sync_stream();

        std::vector<int64_t> h_indices(nb);
        std::vector<float>   h_distances(nb);
        raft::copy(h_indices.data(),   d_indices.data_handle(),   nb, res.get_stream());
        raft::copy(h_distances.data(), d_distances.data_handle(), nb, res.get_stream());
        res.sync_stream();

        for (int64_t q = 0; q < Q; q++) {
            for (int j = 0; j < top_k; j++) {
                CuvsSearchResult *r = &results[(size_t)q * top_k + j];
                if (j < bk) {
                    r->item_id  = h_indices[(size_t)q * bk + j];
                    r->distance = h_distances[(size_t)q * bk + j];
                } else {
                    r->item_id  = -1;            /* sentinel: no neighbor */
                    r->distance = 3.402823466e+38f;
                }
            }
        }
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_bf_search_batch] exception: %s\n", e.what());
        _pr.poison();
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_bf_search_batch] unknown exception\n");
        _pr.poison();
        return 1;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index serialize / deserialize
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_serialize(CuvsCagraIndex index, const char *path, int device_id)
{
    if (!index)
        return 1;

    try {
        CuvsCagraIndexImpl *impl = static_cast<CuvsCagraIndexImpl *>(index);
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        /* include_dataset=true: works now because impl->dataset keeps the
         * device memory alive that idx's view points to. */
        cuvs::neighbors::cagra::serialize(res, std::string(path), impl->idx, true);
        res.sync_stream();
        return 0;
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_serialize] exception: %s\n", e.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_serialize] unknown exception\n");
        return 1;
    }
}

extern "C" CuvsCagraIndex
cuvs_cagra_deserialize(const char *path, int dim, int device_id)
{
    try {
        PooledRes _pr(device_id); raft::device_resources &res = _pr.get();
        cuvs::neighbors::cagra::index<float, uint32_t> idx(res);
        cuvs::neighbors::cagra::deserialize(res, path, &idx);
        res.sync_stream();
        (void)dim;  /* dim is encoded in the serialized index */

        /* After deserialize with include_dataset=true at save time, the index
         * owns its own dataset (allocated internally during deserialize).
         * We pass an empty placeholder device_matrix so the impl struct's
         * field stays valid; idx's internal dataset is what matters. */
        auto empty = raft::make_device_matrix<float, int64_t>(res, (int64_t)0, (int64_t)0);
        res.sync_stream();
        return new CuvsCagraIndexImpl(std::move(empty), std::move(idx));
    } catch (const std::exception &e) {
        fprintf(stderr, "[cuvs_cagra_deserialize] exception: %s\n", e.what());
        return nullptr;
    } catch (...) {
        fprintf(stderr, "[cuvs_cagra_deserialize] unknown exception\n");
        return nullptr;
    }
}

/* ----------------------------------------------------------------
 * CAGRA index free
 * ---------------------------------------------------------------- */
extern "C" void
cuvs_cagra_free(CuvsCagraIndex index, int device_id)
{
    if (index) {
        if (cudaSetDevice(device_id) != cudaSuccess)
            fprintf(stderr, "[cuvs_cagra_free] cudaSetDevice(%d) failed\n", device_id);
        delete static_cast<CuvsCagraIndexImpl *>(index);
    }
}

/* ----------------------------------------------------------------
 * Phase 3I-1 — CPU HNSW fallback index
 *
 * cuvs_hnsw_serialize: convert a resident CAGRA GPU index to CPU HNSW
 * (hnswlib format) and write to disk.  Called once after a successful CAGRA
 * build; non-fatal if it fails (daemon continues with GPU-only serving).
 *
 * cuvs_hnsw_deserialize: load a .hnsw sidecar from disk.  Called lazily in
 * handle_search when use_cpu_hnsw=1 is requested and the index is not yet in
 * RAM.  hnswlib itself is CPU-threaded; the raft resource is only needed for
 * the cuVS API wrapper, not for GPU work.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_hnsw_serialize(CuvsCagraIndex cagra_idx, const char *path, int device_id)
{
    if (!cagra_idx || !path) return -1;
    CuvsCagraIndexImpl *cimpl = static_cast<CuvsCagraIndexImpl *>(cagra_idx);
    /* hnswlib aborts on very small graphs; skip serialization rather than crash.
     * The CAGRA index remains functional for GPU search. */
    if (cimpl->idx.size() < 16) {
        fprintf(stderr, "[cuvs_hnsw_serialize] skip: n_vecs=%zu < 16 (too small for hnswlib)\n",
                cimpl->idx.size());
        return 0;
    }
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::index_params hparams;
        /* CPU hierarchy produces an hnswlib-compatible file on disk,
         * required for both Phase 3I-1 (cuVS CPU search) and 3I-2
         * (pgvector bulk import via our own hnswlib parser). */
        hparams.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::CPU;
        auto hnsw = cuvs::neighbors::hnsw::from_cagra(
            res, hparams, cimpl->idx);
        res.sync_stream();
        cuvs::neighbors::hnsw::serialize(res, std::string(path), *hnsw);
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_serialize] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_serialize] unknown exception\n");
        return -1;
    }
}

extern "C" CuvsHnswIndex
cuvs_hnsw_deserialize(const char *path, int dim, uint32_t metric, int device_id)
{
    if (!path) return nullptr;
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::index_params hparams;
        hparams.hierarchy = cuvs::neighbors::hnsw::HnswHierarchy::CPU;
        cuvs::neighbors::hnsw::index<float> *loaded = nullptr;
        cuvs::neighbors::hnsw::deserialize(res, hparams, std::string(path),
                                           dim, cuvs_distance_type(metric),
                                           &loaded);
        if (!loaded) return nullptr;
        CuvsHnswIndexImpl *impl = new CuvsHnswIndexImpl;
        impl->idx    = loaded;
        impl->dim    = dim;
        impl->metric = metric;
        return impl;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_deserialize] %s\n", e.what());
        return nullptr;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_deserialize] unknown exception\n");
        return nullptr;
    }
}

/* Single-query CPU HNSW search.  ef <= 0 uses max(200, k). */
extern "C" int
cuvs_hnsw_search(CuvsHnswIndex hidx, const float *query, int dim,
                 int k, int ef, CuvsSearchResult *out)
{
    if (!hidx || !query || !out || k <= 0) return -1;
    CuvsHnswIndexImpl *impl = static_cast<CuvsHnswIndexImpl *>(hidx);
    PooledRes _pr(0);   /* raft API requires a resource; hnswlib ignores CUDA */
    try {
        raft::device_resources &res = _pr.get();
        cuvs::neighbors::hnsw::search_params sparams;
        sparams.ef = (ef > 0) ? ef : std::max(200, k);

        /* search() returns uint64_t neighbors (hnswlib labels). */
        std::vector<uint64_t> h_nb((size_t)k, UINT64_MAX);
        std::vector<float>    h_dist((size_t)k, 0.0f);

        auto q_view = raft::make_host_matrix_view<const float, int64_t>(
            query, (int64_t)1, (int64_t)impl->dim);
        auto n_view = raft::make_host_matrix_view<uint64_t, int64_t>(
            h_nb.data(), (int64_t)1, (int64_t)k);
        auto d_view = raft::make_host_matrix_view<float, int64_t>(
            h_dist.data(), (int64_t)1, (int64_t)k);

        cuvs::neighbors::hnsw::search(res, sparams, *impl->idx,
                                       q_view, n_view, d_view);
        for (int i = 0; i < k; i++) {
            out[i].item_id  = (int64_t)h_nb[i];   /* item_id is int64 in CuvsSearchResult */
            out[i].distance = h_dist[i];
        }
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_search] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_hnsw_search] unknown exception\n");
        return -1;
    }
}

extern "C" void
cuvs_hnsw_free(CuvsHnswIndex hidx)
{
    if (!hidx) return;
    CuvsHnswIndexImpl *impl = static_cast<CuvsHnswIndexImpl *>(hidx);
    delete impl->idx;   /* raw ptr returned by deserialize / released from unique_ptr */
    delete impl;
}

/* ----------------------------------------------------------------
 * Phase 3J: extract CAGRA adjacency list + corpus vectors from VRAM to CPU.
 * Used by handle_export_adjacency in the daemon to serve pg_cuvs_import_cagra.
 * ---------------------------------------------------------------- */
extern "C" int
cuvs_cagra_extract_adjacency(
    CuvsCagraIndex  handle,
    uint32_t      **adj_out,
    float         **vecs_out,
    size_t         *n_vecs_out,
    int            *graph_degree_out,
    int             device_id)
{
    if (!handle || !adj_out || !vecs_out || !n_vecs_out || !graph_degree_out)
        return -1;
    CuvsCagraIndexImpl *cimpl = static_cast<CuvsCagraIndexImpl *>(handle);
    PooledRes _pr(device_id);
    try {
        raft::device_resources &res = _pr.get();

        auto graph_view = cimpl->idx.graph();
        size_t N   = (size_t)graph_view.extent(0);
        size_t D   = (size_t)graph_view.extent(1);
        size_t dim = (size_t)cimpl->dataset.extent(1);

        uint32_t *adj  = (uint32_t *)malloc(N * D   * sizeof(uint32_t));
        float    *vecs = (float    *)malloc(N * dim  * sizeof(float));
        if (!adj || !vecs) {
            free(adj); free(vecs);
            fprintf(stderr, "[cuvs_cagra_extract_adjacency] malloc failed N=%zu D=%zu dim=%zu\n",
                    N, D, dim);
            return -1;
        }

        auto stream = raft::resource::get_cuda_stream(res);
        raft::copy(adj,  graph_view.data_handle(),      N * D,   stream);
        raft::copy(vecs, cimpl->dataset.data_handle(),  N * dim, stream);
        res.sync_stream();

        *adj_out          = adj;
        *vecs_out         = vecs;
        *n_vecs_out       = N;
        *graph_degree_out = (int)D;
        return 0;
    } catch (const std::exception &e) {
        _pr.poison();
        fprintf(stderr, "[cuvs_cagra_extract_adjacency] %s\n", e.what());
        return -1;
    } catch (...) {
        _pr.poison();
        fprintf(stderr, "[cuvs_cagra_extract_adjacency] unknown exception\n");
        return -1;
    }
}

/* ----------------------------------------------------------------
 * Warm-up: pay the one-time GPU init cost (CUDA primary context, RMM pool,
 * cuBLAS/cuSOLVER handles, kernel JIT) at daemon startup instead of on the
 * first client query. Runs a tiny brute-force search; also primes one entry
 * in the device_resources pool. Safe to call once per device at boot.
 * ---------------------------------------------------------------- */
extern "C" void
cuvs_warmup_device(int device_id)
{
    try {
        const int n = 512, dim = 16, k = 8;
        std::vector<float> corpus((size_t)n * dim), query(dim, 0.1f);
        for (size_t i = 0; i < corpus.size(); i++)
            corpus[i] = (float)((i * 2654435761u) % 1000) / 1000.0f;
        CuvsSearchResult out[8];
        /* brute force warms CUDA context / RMM / cuBLAS (gemm) */
        (void)cuvs_brute_force_search(corpus.data(), query.data(), n, dim, k,
                                      CUVS_METRIC_L2, out, device_id);
        /* cagra build+search warms the CAGRA-specific kernels so the first
         * real cagra query on a freshly booted daemon is not the one that
         * pays kernel load (~100 ms). */
        CuvsCagraIndex idx = cuvs_cagra_build(corpus.data(), n, dim, CUVS_METRIC_L2,
                                               0, 0, CUVS_CAGRA_BUILD_AUTO, device_id);
        if (idx) {
            (void)cuvs_cagra_search(idx, query.data(), dim, k, out, device_id);
            cuvs_cagra_free(idx, device_id);
        }
    } catch (...) {
        /* warm-up is best-effort; a failure here must not stop the daemon */
    }
}

extern "C" void
cuvs_warmup(void)
{
    cuvs_warmup_device(0);
}
