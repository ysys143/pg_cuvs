/*
 * cuvs_wrapper_shim_cpu.c — CPU-reference shim for cuvs_wrapper.h (CI Tier 1).
 *
 * Replaces the real cuVS/CUDA wrapper (cuvs_wrapper.cu) with a pure-C, CUDA-free
 * implementation so the daemon + extension build and run on a hosted GitHub
 * runner with no GPU. Selected by `make PGCUVS_CPU_SHIM=1` (see Makefile).
 *
 * Design (design/CI_STRATEGY.md, ADR-067):
 *   - opaque handles (CuvsCagraIndex/BfIndex/IvfPqIndex/HnswIndex) = host copy of
 *     the corpus { float *vecs; int64_t n; int dim; uint32_t metric; }.
 *   - every search is EXACT brute-force k-NN → it is the ground truth, so SQL
 *     recall asserts pass and search_mode/IPC/fail-closed logic is exercised for
 *     real. Graph/PQ params are ignored (approximate-recall regression is a
 *     Tier-2-only concern).
 *   - VRAM accounting is a deterministic in-process counter so evict_lru / VRAM
 *     budget / OOM logic runs without a GPU (ADR-065: budget is daemon
 *     self-accounting, so a fake total is all the daemon needs).
 *
 * Metric conventions MATCH cuvs_wrapper.cu (cuvs_distance_type): L2 -> squared L2
 * (L2Expanded), COSINE -> 1 - cosine_sim (CosineExpanded), IP -> -dot (so
 * ascending sort puts the largest inner product first, as InnerProduct nearest).
 * Distances are smaller-is-nearer; ties broken by item_id for determinism.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "cuvs_wrapper.h"
#include "cuvs_ipc.h"   /* CUVS_METRIC_* */

/* ---- host index ---------------------------------------------------------- */
typedef struct ShimIndex {
    float   *vecs;   /* row-major [n*dim] */
    int64_t  n;
    int      dim;
    uint32_t metric;
} ShimIndex;

static ShimIndex *
shim_new(const float *vecs, int64_t n, int dim, uint32_t metric)
{
    ShimIndex *ix = (ShimIndex *) calloc(1, sizeof(ShimIndex));
    if (!ix) return NULL;
    ix->n = n; ix->dim = dim; ix->metric = metric;
    if (n > 0 && dim > 0) {
        size_t bytes = (size_t)n * (size_t)dim * sizeof(float);
        ix->vecs = (float *) malloc(bytes);
        if (!ix->vecs) { free(ix); return NULL; }
        if (vecs) memcpy(ix->vecs, vecs, bytes);
        else      memset(ix->vecs, 0, bytes);
    }
    return ix;
}

static void
shim_free(ShimIndex *ix)
{
    if (!ix) return;
    free(ix->vecs);
    free(ix);
}

/* ---- exact metric distance (smaller = nearer), matches cuvs_distance_type -- */
static float
shim_dist(const float *a, const float *b, int dim, uint32_t metric)
{
    if (metric == CUVS_METRIC_IP) {
        double dot = 0.0;
        for (int d = 0; d < dim; d++) dot += (double)a[d] * (double)b[d];
        return (float)(-dot);                      /* largest dot first */
    }
    if (metric == CUVS_METRIC_COSINE) {
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int d = 0; d < dim; d++) {
            dot += (double)a[d] * (double)b[d];
            na  += (double)a[d] * (double)a[d];
            nb  += (double)b[d] * (double)b[d];
        }
        double denom = sqrt(na) * sqrt(nb);
        if (denom == 0.0) return 1.0f;
        return (float)(1.0 - dot / denom);
    }
    /* L2Expanded = squared L2 */
    double s = 0.0;
    for (int d = 0; d < dim; d++) {
        double diff = (double)a[d] - (double)b[d];
        s += diff * diff;
    }
    return (float)s;
}

/* (distance, item_id) for sorting */
typedef struct { float dist; int64_t id; } ShimPair;

static int
shim_pair_cmp(const void *pa, const void *pb)
{
    const ShimPair *a = (const ShimPair *)pa, *b = (const ShimPair *)pb;
    if (a->dist < b->dist) return -1;
    if (a->dist > b->dist) return 1;
    if (a->id   < b->id)   return -1;   /* deterministic tiebreak */
    if (a->id   > b->id)   return 1;
    return 0;
}

/* Exact top-k over ix, optionally excluding items whose bitset bit is set
 * (bit[id]=1 => EXCLUDE, cuVS convention). Pads tail with item_id=-1. */
static void
shim_topk(const ShimIndex *ix, const float *query, int dim, int top_k,
          const uint32_t *bitset, int64_t bitset_bits,
          CuvsSearchResult *results)
{
    int64_t n = (ix && dim == ix->dim) ? ix->n : 0;
    ShimPair *pairs = (n > 0) ? (ShimPair *) malloc((size_t)n * sizeof(ShimPair)) : NULL;
    int64_t m = 0;

    for (int64_t i = 0; i < n; i++) {
        if (bitset && i < bitset_bits &&
            (bitset[i >> 5] & (1u << (i & 31))) != 0)
            continue;   /* excluded */
        pairs[m].dist = shim_dist(ix->vecs + (size_t)i * dim, query, dim, ix->metric);
        pairs[m].id   = i;
        m++;
    }
    if (m > 0) qsort(pairs, (size_t)m, sizeof(ShimPair), shim_pair_cmp);

    for (int j = 0; j < top_k; j++) {
        if (j < m) { results[j].item_id = pairs[j].id;  results[j].distance = pairs[j].dist; }
        else       { results[j].item_id = -1;           results[j].distance = INFINITY;      }
    }
    free(pairs);
}

/* ---- serialize format (self-describing) --------------------------------- */
#define SHIM_MAGIC 0x53484d31u   /* "SHM1" */

static int
shim_write(const ShimIndex *ix, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t magic = SHIM_MAGIC;
    int ok = (fwrite(&magic, sizeof(magic), 1, f) == 1)
          && (fwrite(&ix->metric, sizeof(ix->metric), 1, f) == 1)
          && (fwrite(&ix->dim, sizeof(ix->dim), 1, f) == 1)
          && (fwrite(&ix->n, sizeof(ix->n), 1, f) == 1);
    if (ok && ix->n > 0)
        ok = (fwrite(ix->vecs, sizeof(float), (size_t)ix->n * ix->dim, f)
              == (size_t)ix->n * ix->dim);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static ShimIndex *
shim_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t magic = 0, metric = 0;
    int dim = 0; int64_t n = 0;
    if (fread(&magic, sizeof(magic), 1, f) != 1 || magic != SHIM_MAGIC ||
        fread(&metric, sizeof(metric), 1, f) != 1 ||
        fread(&dim, sizeof(dim), 1, f) != 1 ||
        fread(&n, sizeof(n), 1, f) != 1) { fclose(f); return NULL; }
    ShimIndex *ix = shim_new(NULL, n, dim, metric);
    if (ix && n > 0 &&
        fread(ix->vecs, sizeof(float), (size_t)n * dim, f) != (size_t)n * dim) {
        shim_free(ix); fclose(f); return NULL;
    }
    fclose(f);
    return ix;
}

/* ======================================================================== */
/* build / free                                                             */
/* ======================================================================== */
CuvsBfIndex
cuvs_bf_build(const float *vecs, int64_t n, int dim, uint32_t metric,
              uint32_t precision, int device_id)
{ (void)precision; (void)device_id; return (CuvsBfIndex) shim_new(vecs, n, dim, metric); }

void cuvs_bf_free(CuvsBfIndex index, int device_id)
{ (void)device_id; shim_free((ShimIndex *)index); }

CuvsCagraIndex
cuvs_cagra_build(const float *vecs, int64_t n_vecs, int dim, uint32_t metric,
                 int graph_degree, int intermediate_graph_degree,
                 uint32_t build_algo, int device_id)
{ (void)graph_degree; (void)intermediate_graph_degree; (void)build_algo; (void)device_id;
  return (CuvsCagraIndex) shim_new(vecs, n_vecs, dim, metric); }

CuvsCagraIndex
cuvs_cagra_build_multi(const float **vecs, const int64_t *n_each, int n_parts,
                       int64_t total, int dim, uint32_t metric,
                       int graph_degree, int intermediate_graph_degree,
                       uint32_t build_algo, int device_id)
{
    (void)graph_degree; (void)intermediate_graph_degree; (void)build_algo; (void)device_id;
    ShimIndex *ix = shim_new(NULL, total, dim, metric);
    if (!ix) return NULL;
    int64_t off = 0;
    for (int p = 0; p < n_parts; p++) {
        size_t rows = (size_t)n_each[p];
        if (rows == 0) continue;
        memcpy(ix->vecs + (size_t)off * dim, vecs[p], rows * (size_t)dim * sizeof(float));
        off += n_each[p];
    }
    return (CuvsCagraIndex) ix;
}

void cuvs_cagra_free(CuvsCagraIndex index, int device_id)
{ (void)device_id; shim_free((ShimIndex *)index); }

/* ======================================================================== */
/* search (exact)                                                           */
/* ======================================================================== */
int
cuvs_brute_force_search(const float *corpus_vecs, const float *query_vec,
                        int64_t n_corpus, int dim, int top_k, uint32_t metric,
                        CuvsSearchResult *results, int device_id)
{
    (void)device_id;
    ShimIndex tmp = { (float *)corpus_vecs, n_corpus, dim, metric };
    shim_topk(&tmp, query_vec, dim, top_k, NULL, 0, results);
    return 0;
}

int
cuvs_bf_search(CuvsBfIndex index, const float *query_vec, int dim, int top_k,
               CuvsSearchResult *results, int device_id)
{
    (void)device_id;
    ShimIndex *ix = (ShimIndex *)index;
    if (!ix) return 1;
    if (dim != ix->dim) return 2;
    shim_topk(ix, query_vec, dim, top_k, NULL, 0, results);
    return 0;
}

int
cuvs_bf_search_filtered(CuvsBfIndex index, const float *query_vec, int dim,
                        int top_k, const uint32_t *bitset_words,
                        int64_t bitset_bits, CuvsSearchResult *results,
                        int device_id)
{
    (void)device_id;
    ShimIndex *ix = (ShimIndex *)index;
    if (!ix) return 1;
    if (dim != ix->dim) return 2;
    shim_topk(ix, query_vec, dim, top_k, bitset_words, bitset_bits, results);
    return 0;
}

int
cuvs_cagra_search(CuvsCagraIndex index, const float *query_vec, int dim,
                  int top_k, CuvsSearchResult *results, int device_id)
{ return cuvs_bf_search((CuvsBfIndex)index, query_vec, dim, top_k, results, device_id); }

int
cuvs_cagra_search_filtered(CuvsCagraIndex index, const float *query_vec, int dim,
                           int top_k, const uint32_t *bitset_words,
                           int64_t bitset_bits, CuvsSearchResult *results,
                           int device_id)
{ return cuvs_bf_search_filtered((CuvsBfIndex)index, query_vec, dim, top_k,
                                 bitset_words, bitset_bits, results, device_id); }

static int
shim_search_batch(ShimIndex *ix, const float *queries, int n_queries, int dim,
                  int top_k, CuvsSearchResult *results)
{
    if (!ix) return 1;
    if (dim != ix->dim) return 2;
    for (int q = 0; q < n_queries; q++)
        shim_topk(ix, queries + (size_t)q * dim, dim, top_k, NULL, 0,
                  results + (size_t)q * top_k);
    return 0;
}

int cuvs_cagra_search_batch(CuvsCagraIndex index, const float *queries,
                            int n_queries, int dim, int top_k,
                            CuvsSearchResult *results, int device_id)
{ (void)device_id; return shim_search_batch((ShimIndex *)index, queries, n_queries, dim, top_k, results); }

int cuvs_bf_search_batch(CuvsBfIndex index, const float *queries, int n_queries,
                         int dim, int top_k, CuvsSearchResult *results, int device_id)
{ (void)device_id; return shim_search_batch((ShimIndex *)index, queries, n_queries, dim, top_k, results); }

/* ======================================================================== */
/* streaming / compaction                                                   */
/* ======================================================================== */
static int g_inject_extend_oom = 0;

void cuvs_set_inject_extend_oom(int enable) { g_inject_extend_oom = enable ? 1 : 0; }

int
cuvs_cagra_extend(CuvsCagraIndex index, const float *new_vecs, int64_t n_new,
                  int dim, uint32_t max_chunk_size, int device_id)
{
    (void)max_chunk_size; (void)device_id;
    ShimIndex *ix = (ShimIndex *)index;
    if (!ix || !new_vecs || n_new <= 0) return 1;
    if (dim != ix->dim) return 2;
    if (g_inject_extend_oom) { g_inject_extend_oom = 0; return 1; }  /* simulate OOM */
    size_t old = (size_t)ix->n * dim, add = (size_t)n_new * dim;
    float *grown = (float *) realloc(ix->vecs, (old + add) * sizeof(float));
    if (!grown) return 1;
    ix->vecs = grown;
    memcpy(ix->vecs + old, new_vecs, add * sizeof(float));
    ix->n += n_new;
    return 0;
}

CuvsCagraIndex
cuvs_cagra_compact(CuvsCagraIndex index, const uint32_t *keep_bits_words,
                   int64_t n_total, uint32_t metric, int device_id)
{
    (void)device_id;
    ShimIndex *ix = (ShimIndex *)index;
    if (!ix) return NULL;
    ShimIndex *out = shim_new(NULL, 0, ix->dim, metric);
    if (!out) return NULL;
    int64_t kept = 0;
    for (int64_t i = 0; i < n_total && i < ix->n; i++)
        if (keep_bits_words[i >> 5] & (1u << (i & 31))) kept++;
    out->vecs = (kept > 0) ? (float *) malloc((size_t)kept * ix->dim * sizeof(float)) : NULL;
    int64_t w = 0;
    for (int64_t i = 0; i < n_total && i < ix->n; i++)
        if (keep_bits_words[i >> 5] & (1u << (i & 31))) {
            memcpy(out->vecs + (size_t)w * ix->dim, ix->vecs + (size_t)i * ix->dim,
                   (size_t)ix->dim * sizeof(float));
            w++;
        }
    out->n = kept;
    return (CuvsCagraIndex) out;
}

/* ======================================================================== */
/* serialize / deserialize                                                  */
/* ======================================================================== */
int cuvs_cagra_serialize(CuvsCagraIndex index, const char *path, int device_id)
{ (void)device_id; return shim_write((ShimIndex *)index, path); }

CuvsCagraIndex cuvs_cagra_deserialize(const char *path, int dim, int device_id)
{ (void)dim; (void)device_id; return (CuvsCagraIndex) shim_read(path); }

int cuvs_ivfpq_build(const float *vecs, int64_t n_vecs, int dim, uint32_t metric,
                     uint32_t n_lists, uint32_t pq_bits, uint32_t pq_dim,
                     int device_id, CuvsIvfPqIndex *out)
{ (void)n_lists; (void)pq_bits; (void)pq_dim; (void)device_id;
  if (!out) return 1;
  *out = (CuvsIvfPqIndex) shim_new(vecs, n_vecs, dim, metric);
  return *out ? 0 : 1; }

int cuvs_ivfpq_search(CuvsIvfPqIndex index, const float *query_vec, int dim,
                      int top_k, uint32_t n_probes, CuvsSearchResult *results,
                      int device_id)
{ (void)n_probes; return cuvs_bf_search((CuvsBfIndex)index, query_vec, dim, top_k, results, device_id); }

int cuvs_ivfpq_serialize(CuvsIvfPqIndex index, const char *path, int device_id)
{ (void)device_id; return shim_write((ShimIndex *)index, path); }

CuvsIvfPqIndex cuvs_ivfpq_deserialize(const char *path, int device_id)
{ (void)device_id; return (CuvsIvfPqIndex) shim_read(path); }

void cuvs_ivfpq_free(CuvsIvfPqIndex index, int device_id)
{ (void)device_id; shim_free((ShimIndex *)index); }

/* ======================================================================== */
/* CPU HNSW (Phase 3I-1) — exact kNN over the same host copy                 */
/* ======================================================================== */
int cuvs_hnsw_serialize(CuvsCagraIndex cagra_idx, const char *path, int device_id)
{ (void)device_id; return shim_write((ShimIndex *)cagra_idx, path); }

CuvsHnswIndex cuvs_hnsw_deserialize(const char *path, int dim, uint32_t metric, int device_id)
{ (void)dim; (void)metric; (void)device_id; return (CuvsHnswIndex) shim_read(path); }

int cuvs_hnsw_search(CuvsHnswIndex hidx, const float *query, int dim, int k,
                     int ef, CuvsSearchResult *out)
{ (void)ef;
  ShimIndex *ix = (ShimIndex *)hidx;
  if (!ix) return 1;
  if (dim != ix->dim) return 2;
  shim_topk(ix, query, dim, k, NULL, 0, out);
  return 0; }

void cuvs_hnsw_free(CuvsHnswIndex hidx) { shim_free((ShimIndex *)hidx); }

/* 3J export: real exact-kNN adjacency so the exported pgvector HNSW is correct. */
int
cuvs_cagra_extract_adjacency(CuvsCagraIndex handle, uint32_t **adj_out,
                             float **vecs_out, size_t *n_vecs_out,
                             int *graph_degree_out, int device_id)
{
    (void)device_id;
    ShimIndex *ix = (ShimIndex *)handle;
    if (!ix) return -1;
    int gd = (ix->n > 32) ? 32 : (ix->n > 1 ? (int)ix->n - 1 : 1);
    if (gd < 1) gd = 1;
    uint32_t *adj  = (uint32_t *) malloc((size_t)ix->n * gd * sizeof(uint32_t));
    float    *vecs = (float *) malloc((size_t)ix->n * ix->dim * sizeof(float));
    if (!adj || !vecs) { free(adj); free(vecs); return -1; }
    memcpy(vecs, ix->vecs, (size_t)ix->n * ix->dim * sizeof(float));

    CuvsSearchResult *nbr = (CuvsSearchResult *) malloc((size_t)(gd + 1) * sizeof(CuvsSearchResult));
    for (int64_t i = 0; i < ix->n; i++) {
        shim_topk(ix, ix->vecs + (size_t)i * ix->dim, ix->dim, gd + 1, NULL, 0, nbr);
        int w = 0;
        for (int j = 0; j < gd + 1 && w < gd; j++) {
            if (nbr[j].item_id == i || nbr[j].item_id < 0) continue;  /* skip self/pad */
            adj[(size_t)i * gd + w++] = (uint32_t) nbr[j].item_id;
        }
        while (w < gd) adj[(size_t)i * gd + w++] = (uint32_t) i;       /* pad */
    }
    free(nbr);
    *adj_out = adj; *vecs_out = vecs; *n_vecs_out = (size_t)ix->n; *graph_degree_out = gd;
    return 0;
}

/* ======================================================================== */
/* VRAM / device accounting (deterministic fake)                            */
/* ======================================================================== */
static size_t
shim_vram_total(void)
{
    const char *e = getenv("CUVS_SHIM_VRAM_MB");
    long mb = (e && e[0]) ? atol(e) : 40960;   /* default ~40 GB */
    if (mb < 1) mb = 40960;
    return (size_t)mb * 1024 * 1024;
}

static int64_t g_shim_vram_free = -1;   /* -1 = uninitialized */

static size_t shim_vram_free_get(void)
{
    if (g_shim_vram_free < 0) g_shim_vram_free = (int64_t) shim_vram_total();
    return (size_t) g_shim_vram_free;
}

int cuvs_detect_gpus(CuvsGpuDeviceInfo *out, int max_devices)
{
    if (max_devices < 1) return 0;
    out[0].device_id = 0;
    out[0].total_vram_bytes = shim_vram_total();
    snprintf(out[0].name, sizeof(out[0].name), "shim-cpu (PGCUVS_CPU_SHIM)");
    return 1;
}

int    cuvs_gpu_available(void) { return 1; }
size_t cuvs_vram_free_bytes_on(int device_id) { (void)device_id; return shim_vram_free_get(); }
size_t cuvs_vram_free_bytes(void) { return shim_vram_free_get(); }
void   cuvs_warmup_device(int device_id) { (void)device_id; }
void   cuvs_warmup(void) {}

int cuvs_eat_vram(int64_t leave_bytes, int device_id)
{ (void)device_id; if (leave_bytes < 0) leave_bytes = 0; g_shim_vram_free = leave_bytes; return 0; }

int cuvs_free_vram(int device_id)
{ (void)device_id; g_shim_vram_free = (int64_t) shim_vram_total(); return 0; }
