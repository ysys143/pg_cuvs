/*
 * cuvs_util.c — shared, dependency-free helpers for pg_cuvs.
 *
 * Compiled into both the PostgreSQL extension (.so) and the standalone
 * daemon, and linked directly by the unit-test binary. Contains no
 * PostgreSQL, CUDA, or cuVS dependencies.
 */

#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_STATUS_* defines (PG-free, CUDA-free) */

#include <string.h>
#include <stdlib.h>

/* ----------------------------------------------------------------
 * Index filename parsing: "<db_oid>_<index_oid>.cagra"
 * ---------------------------------------------------------------- */
int
cuvs_parse_index_filename(const char *name, uint32_t *db_oid, uint32_t *index_oid)
{
    int consumed = 0;

    if (!name)
        return -1;

    /* Require the ENTIRE name to match "<db>_<index>.cagra". The %n captures
     * how many chars were consumed once the ".cagra" literal matches; requiring
     * consumed == strlen(name) rejects trailing extensions (".cagra.bak") and,
     * importantly, Phase 3F shard artifacts ("<db>_<idx>.s000.cagra") whose
     * ".cagra" suffix would otherwise fool a plain suffix check. */
    if (sscanf(name, "%u_%u.cagra%n", db_oid, index_oid, &consumed) == 2
        && consumed == (int)strlen(name))
        return 0;
    return -1;
}

/* ----------------------------------------------------------------
 * IPC status code -> human-readable string
 * ---------------------------------------------------------------- */
const char *
cuvs_status_str(int status)
{
    switch (status)
    {
        case CUVS_STATUS_OK:           return "ok";
        case CUVS_STATUS_ERROR:        return "error";
        case CUVS_STATUS_OOM_FALLBACK: return "oom_fallback";
        case CUVS_STATUS_NOT_FOUND:    return "not_found";
        case CUVS_STATUS_UNAVAILABLE:  return "unavailable";
        case CUVS_STATUS_BUILD_FAILED:   return "build_failed";
        case CUVS_STATUS_PERSIST_FAILED: return "persist_failed";
        case CUVS_STATUS_DIM_MISMATCH:   return "dim_mismatch";
        case CUVS_STATUS_METRIC_MISMATCH: return "metric_mismatch";
        case CUVS_STATUS_STALE:          return "stale";
        default:                       return "unknown";
    }
}

/* ----------------------------------------------------------------
 * Opclass name -> CUVS_METRIC_*. Returns -1 for an unrecognized name.
 * Pure (no PG/CUDA); the PG glue resolves the index's opclass name and
 * calls this so build and search agree on the metric.
 * ---------------------------------------------------------------- */
int
cuvs_metric_from_opclass_name(const char *name)
{
    if (!name)
        return -1;
    if (strcmp(name, "vector_l2_ops") == 0)
        return CUVS_METRIC_L2;
    if (strcmp(name, "vector_cosine_ops") == 0)
        return CUVS_METRIC_COSINE;
    if (strcmp(name, "vector_ip_ops") == 0)
        return CUVS_METRIC_IP;
    return -1;
}

/* ----------------------------------------------------------------
 * Latency histogram: log2-spaced buckets + approximate percentiles.
 * ---------------------------------------------------------------- */
uint32_t
cuvs_lat_bucket_index(uint32_t us)
{
    uint32_t idx = 0;
    /* bucket 0 = {0}; bucket k>=1 covers [2^(k-1), 2^k). */
    while (us > 0 && idx < CUVS_LAT_BUCKETS - 1)
    {
        us >>= 1;
        idx++;
    }
    return idx;
}

/* Upper-edge latency (us) represented by a bucket: bucket 0 -> 0,
 * bucket k>=1 -> 2^k. Clamped index never exceeds CUVS_LAT_BUCKETS-1. */
static uint32_t
lat_bucket_upper_us(int idx)
{
    if (idx <= 0)
        return 0;
    if (idx >= 31)              /* avoid UB: 1u<<31 fits, beyond would overflow */
        return 0x80000000u;
    return 1u << idx;
}

uint32_t
cuvs_lat_percentile(const uint32_t *buckets, int nbuckets, double q)
{
    uint64_t total = 0;
    uint64_t target;
    uint64_t cum = 0;

    for (int i = 0; i < nbuckets; i++)
        total += buckets[i];
    if (total == 0)
        return 0;

    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;

    /* Rank of the target sample (1-based): the smallest count whose
     * cumulative coverage reaches the q-quantile. */
    target = (uint64_t)(q * (double)total);
    if (target == 0)
        target = 1;

    for (int i = 0; i < nbuckets; i++)
    {
        cum += buckets[i];
        if (cum >= target)
            return lat_bucket_upper_us(i);
    }
    return lat_bucket_upper_us(nbuckets - 1);
}

/* ----------------------------------------------------------------
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) and versioned .tids I/O.
 * LE-only: see cuvs_util.h header comment.
 * ---------------------------------------------------------------- */
static uint32_t crc32_table[256];
static int      crc32_table_init = 0;

static void
crc32_ensure_table(void)
{
    if (crc32_table_init)
        return;
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

uint32_t
cuvs_crc32_stream_begin(void)
{
    crc32_ensure_table();
    return 0xFFFFFFFFu;
}

uint32_t
cuvs_crc32_stream_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    crc32_ensure_table();
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

uint32_t
cuvs_crc32_stream_end(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t
cuvs_crc32(const void *data, size_t len)
{
    return cuvs_crc32_stream_end(
        cuvs_crc32_stream_update(cuvs_crc32_stream_begin(), data, len));
}

int
cuvs_tids_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                const uint64_t *tids)
{
    CuvsTidsHeader hdr;
    hdr.magic      = CUVS_TIDS_MAGIC;
    hdr.version    = CUVS_TIDS_VERSION;
    hdr.n_vecs     = n_vecs;
    hdr.dim        = dim;
    hdr.metric     = metric;
    hdr.body_crc32 = cuvs_crc32(tids, (size_t)n_vecs * sizeof(uint64_t));
    hdr.reserved   = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(tids, sizeof(uint64_t), (size_t)n_vecs, f) != (size_t)n_vecs)
        return -1;
    return 0;
}

int
cuvs_tids_read(FILE *f, CuvsTidsHeader *hdr_out, uint64_t **tids_out)
{
    CuvsTidsHeader hdr;
    size_t body_bytes;
    uint64_t *tids;

    *tids_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_TIDS_MAGIC)
        return -1;
    if (hdr.version != CUVS_TIDS_VERSION)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    /* reserved must be 0 (forward-compat: a future format may use it, so a
     * v1 reader must reject files that set it rather than silently accept). */
    if (hdr.reserved != 0)
        return -1;

    body_bytes = (size_t)hdr.n_vecs * sizeof(uint64_t);
    tids = malloc(body_bytes);
    if (!tids)
        return -1;

    if (fread(tids, sizeof(uint64_t), (size_t)hdr.n_vecs, f) != (size_t)hdr.n_vecs)
    {
        free(tids);
        return -1;
    }
    if (cuvs_crc32(tids, body_bytes) != hdr.body_crc32)
    {
        free(tids);
        return -1;
    }

    if (hdr_out)
        *hdr_out = hdr;
    *tids_out = tids;
    return 0;
}

/* ----------------------------------------------------------------
 * Versioned .vectors sidecar I/O (Phase 3L brute-force mode).
 * Mirrors cuvs_tids_write/read; body is the row-major float matrix.
 * ---------------------------------------------------------------- */
int
cuvs_vectors_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                   uint32_t base_tids_crc32, const float *vecs)
{
    CuvsVectorsHeader hdr;
    size_t body_n = (size_t)n_vecs * (size_t)dim;

    memset(&hdr, 0, sizeof(hdr));   /* zero struct padding -> deterministic header */
    hdr.magic           = CUVS_VECTORS_MAGIC;
    hdr.version         = CUVS_VECTORS_VERSION;
    hdr.n_vecs          = n_vecs;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32(vecs, body_n * sizeof(float));
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(vecs, sizeof(float), body_n, f) != body_n)
        return -1;
    return 0;
}

/* ADR-059: write the .vectors sidecar from N partitions without a contiguous
 * host copy. The body crc32 is accumulated incrementally over the partitions in
 * concatenation order, then the body is written partition-by-partition. Output
 * is byte-identical to cuvs_vectors_write over the logically concatenated
 * corpus (Σ n_each rows). Empty partitions (n_each[i] <= 0) are skipped. */
int
cuvs_vectors_write_multi(FILE *f, const int64_t *n_each, int n_parts, uint32_t dim,
                         uint32_t metric, uint32_t base_tids_crc32,
                         const float *const *part_vecs)
{
    CuvsVectorsHeader hdr;
    int64_t  total = 0;
    uint32_t crc = cuvs_crc32_stream_begin();
    int      i;

    memset(&hdr, 0, sizeof(hdr));   /* zero struct padding -> deterministic header */

    for (i = 0; i < n_parts; i++)
    {
        size_t body_n;
        if (n_each[i] <= 0)
            continue;
        body_n = (size_t)n_each[i] * (size_t)dim;
        crc = cuvs_crc32_stream_update(crc, part_vecs[i], body_n * sizeof(float));
        total += n_each[i];
    }

    hdr.magic           = CUVS_VECTORS_MAGIC;
    hdr.version         = CUVS_VECTORS_VERSION;
    hdr.n_vecs          = total;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32_stream_end(crc);
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    for (i = 0; i < n_parts; i++)
    {
        size_t body_n;
        if (n_each[i] <= 0)
            continue;
        body_n = (size_t)n_each[i] * (size_t)dim;
        if (fwrite(part_vecs[i], sizeof(float), body_n, f) != body_n)
            return -1;
    }
    return 0;
}

int
cuvs_vectors_read(FILE *f, CuvsVectorsHeader *hdr_out, float **vecs_out)
{
    CuvsVectorsHeader hdr;
    size_t body_n;
    float *vecs;

    *vecs_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_VECTORS_MAGIC)
        return -1;
    if (hdr.version != CUVS_VECTORS_VERSION)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    if (hdr.dim == 0 || hdr.dim > CUVS_VECTORS_MAX_DIM)
        return -1;
    /* reserved must be 0 (forward-compat, like .tids). */
    if (hdr.reserved != 0)
        return -1;

    body_n = (size_t)hdr.n_vecs * (size_t)hdr.dim;
    vecs = malloc(body_n * sizeof(float));
    if (!vecs)
        return -1;

    if (fread(vecs, sizeof(float), body_n, f) != body_n)
    {
        free(vecs);
        return -1;
    }
    if (cuvs_crc32(vecs, body_n * sizeof(float)) != hdr.body_crc32)
    {
        free(vecs);
        return -1;
    }

    if (hdr_out)
        *hdr_out = hdr;
    *vecs_out = vecs;
    return 0;
}

/* ----------------------------------------------------------------
 * Versioned .shards manifest I/O (Phase 3F multi-GPU sharding).
 * ---------------------------------------------------------------- */
int
cuvs_shards_write(FILE *f, uint32_t shard_count, int64_t n_vecs,
                  uint32_t dim, uint32_t metric, uint32_t base_tids_crc32,
                  const CuvsShardRecord *recs)
{
    CuvsShardsHeader hdr;
    size_t body_bytes = (size_t)shard_count * sizeof(CuvsShardRecord);

    hdr.magic           = CUVS_SHARDS_MAGIC;
    hdr.version         = CUVS_SHARDS_VERSION;
    hdr.shard_count     = shard_count;
    hdr.base_tids_crc32 = base_tids_crc32;
    hdr.n_vecs          = n_vecs;
    hdr.dim             = dim;
    hdr.metric          = metric;
    hdr.body_crc32      = cuvs_crc32(recs, body_bytes);
    hdr.reserved        = 0;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (fwrite(recs, sizeof(CuvsShardRecord), shard_count, f) != shard_count)
        return -1;
    return 0;
}

int
cuvs_shards_read(FILE *f, CuvsShardsHeader *hdr_out, CuvsShardRecord **recs_out)
{
    CuvsShardsHeader hdr;
    CuvsShardRecord *recs;
    size_t body_bytes;
    int64_t sum_n;

    *recs_out = NULL;

    if (fread(&hdr, sizeof(hdr), 1, f) != 1)
        return -1;
    if (hdr.magic != CUVS_SHARDS_MAGIC)
        return -1;
    if (hdr.version != CUVS_SHARDS_VERSION)
        return -1;
    if (hdr.reserved != 0)
        return -1;
    if (hdr.shard_count == 0 || hdr.shard_count > CUVS_SHARDS_MAX)
        return -1;
    if (hdr.n_vecs <= 0 || hdr.n_vecs > CUVS_TIDS_MAX_VECS)
        return -1;
    if (hdr.dim == 0)
        return -1;

    body_bytes = (size_t)hdr.shard_count * sizeof(CuvsShardRecord);
    recs = malloc(body_bytes);
    if (!recs)
        return -1;

    if (fread(recs, sizeof(CuvsShardRecord), hdr.shard_count, f) != hdr.shard_count)
    {
        free(recs);
        return -1;
    }
    if (cuvs_crc32(recs, body_bytes) != hdr.body_crc32)
    {
        free(recs);
        return -1;
    }

    /* Semantic invariants: a structurally valid manifest must describe a
     * complete, gap-free covering of [0, n_vecs). Reject otherwise (fail
     * closed) so a corrupt/incoherent manifest never yields a partial index. */
    sum_n = 0;
    for (uint32_t i = 0; i < hdr.shard_count; i++)
    {
        if (recs[i].reserved != 0)
            goto reject;
        if (recs[i].shard_id != i)              /* records ordered by shard id */
            goto reject;
        if (recs[i].tid_offset != sum_n)        /* contiguous from 0 */
            goto reject;
        if (recs[i].n_vecs <= 0 || recs[i].n_vecs > CUVS_TIDS_MAX_VECS)
            goto reject;
        if (recs[i].dim != hdr.dim || recs[i].metric != hdr.metric)
            goto reject;
        sum_n += recs[i].n_vecs;
    }
    if (sum_n != hdr.n_vecs)                     /* full coverage */
        goto reject;

    if (hdr_out)
        *hdr_out = hdr;
    *recs_out = recs;
    return 0;

reject:
    free(recs);
    return -1;
}

/* ----------------------------------------------------------------
 * Auto VRAM-based shard count (Phase 3G). Pure arithmetic, no CUDA — the
 * daemon supplies the runtime inputs (per-GPU VRAM budget, usable GPU count)
 * and this decides how many shards a logical index needs.
 *
 * Returns:
 *   1            -> unsharded (fits one GPU, or budget unknown/unlimited, or
 *                   corpus too small to shard). Preserves legacy behavior.
 *   2..max       -> split into this many contiguous shards.
 *   0            -> does not fit even when maximally sharded -> caller fails
 *                   closed (build error), never a partial/over-budget index.
 *
 * `needed` mirrors estimate_vram_bytes() in pg_cuvs_server.c exactly:
 * n_vecs * (dim*sizeof(float) + 16*4). Keep the two in sync.
 * ---------------------------------------------------------------- */
int
cuvs_auto_shard_count(int64_t n_vecs, int dim, size_t per_gpu_budget_bytes,
                      int n_gpus, int max_shards)
{
    size_t needed;
    int    cap;
    int    want;

    if (n_vecs <= 0 || dim <= 0 || n_gpus <= 0)
        return 1;
    if (per_gpu_budget_bytes == 0)
        return 1;                       /* unlimited/unknown budget: don't auto-shard */

    needed = (size_t) n_vecs * ((size_t) dim * sizeof(float) + 16 * 4);
    if (needed <= per_gpu_budget_bytes)
        return 1;                       /* fits a single GPU */

    /* Cap shard count by usable GPUs, the sanity max, and the >=2-vectors-per-
     * shard floor (CAGRA aborts on a 1-vector shard). */
    cap = n_gpus;
    if (max_shards > 0 && cap > max_shards)
        cap = max_shards;
    if ((int64_t) cap > n_vecs / 2)
        cap = (int) (n_vecs / 2);
    if (cap < 2)
        return 0;                       /* can't split enough to fit */

    /* ceil(needed / per_gpu_budget_bytes) shards of ~equal size. */
    want = (int) ((needed + per_gpu_budget_bytes - 1) / per_gpu_budget_bytes);
    if (want < 2)
        want = 2;
    if (want > cap)
        return 0;                       /* even cap shards each exceed budget */
    return want;
}

/* ----------------------------------------------------------------
 * Versioned .delta pending-insert sidecar I/O (Phase 3A).
 * ---------------------------------------------------------------- */
void
cuvs_delta_header_init(CuvsDeltaHeader *h, uint32_t dim, uint32_t metric,
                       uint32_t base_tids_crc32)
{
    h->magic           = CUVS_DELTA_MAGIC;
    h->version         = CUVS_DELTA_VERSION;
    h->n_rows          = 0;
    h->dim             = dim;
    h->metric          = metric;
    h->base_tids_crc32 = base_tids_crc32;
    h->reserved        = 0;
}

int
cuvs_delta_validate(const CuvsDeltaHeader *h, int64_t body_bytes)
{
    if (h->magic != CUVS_DELTA_MAGIC)
        return -1;
    if (h->version != CUVS_DELTA_VERSION)
        return -1;
    if (h->reserved != 0)
        return -1;
    if (h->dim == 0)
        return -1;
    if (h->n_rows < 0 || h->n_rows > CUVS_TIDS_MAX_VECS)
        return -1;
    if (body_bytes < 0)
        return -1;
    /* Body must be exactly n_rows fixed-width records — catches truncation. */
    if ((uint64_t) body_bytes
        != (uint64_t) h->n_rows * cuvs_delta_record_bytes(h->dim))
        return -1;
    return 0;
}

int
cuvs_delta_read_header(FILE *f, CuvsDeltaHeader *out)
{
    CuvsDeltaHeader h;

    if (fread(&h, sizeof(h), 1, f) != 1)
        return -1;
    if (h.magic != CUVS_DELTA_MAGIC)
        return -1;
    if (h.version != CUVS_DELTA_VERSION)
        return -1;
    if (h.reserved != 0)
        return -1;
    if (out)
        *out = h;
    return 0;
}

/* ----------------------------------------------------------------
 * Tombstone sidecar helpers (Phase 3A-4)
 * ---------------------------------------------------------------- */
void
cuvs_tombstone_header_init(CuvsTombstoneHeader *h, uint32_t base_tids_crc32)
{
    h->magic           = CUVS_TOMBSTONE_MAGIC;
    h->version         = CUVS_TOMBSTONE_VERSION;
    h->n_entries       = 0;
    h->base_tids_crc32 = base_tids_crc32;
    h->reserved        = 0;
    h->_pad0           = 0;
}

int
cuvs_tombstone_validate(const CuvsTombstoneHeader *h, int64_t body_bytes)
{
    if (h->magic != CUVS_TOMBSTONE_MAGIC)   return -1;
    if (h->version != CUVS_TOMBSTONE_VERSION) return -1;
    if (h->reserved != 0)                    return -1;
    if (h->n_entries < 0 || h->n_entries > CUVS_TIDS_MAX_VECS) return -1;
    if (body_bytes < 0)                      return -1;
    if ((uint64_t)body_bytes != (uint64_t)h->n_entries * sizeof(CuvsTombstoneRecord))
        return -1;
    return 0;
}

int
cuvs_tombstone_read_header(FILE *f, CuvsTombstoneHeader *out)
{
    CuvsTombstoneHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1)
        return -1;
    if (h.magic != CUVS_TOMBSTONE_MAGIC)
        return -1;
    if (h.version != CUVS_TOMBSTONE_VERSION)
        return -1;
    if (h.reserved != 0)
        return -1;
    if (out)
        *out = h;
    return 0;
}

#ifdef CUVS_TEST_HOOKS
int
cuvs_fault(const char *name)
{
    return getenv(name) != NULL;
}
#endif

/* ----------------------------------------------------------------
 * Circuit breaker state (process-local)
 * ---------------------------------------------------------------- */
CuvsCircuitBreaker cuvs_circuit_breakers[CUVS_MAX_TRACKED_INDEXES];
int                cuvs_n_circuit_breakers = 0;

static CuvsCircuitBreaker *
find_or_create_breaker(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
            return &cuvs_circuit_breakers[i];

    if (cuvs_n_circuit_breakers < CUVS_MAX_TRACKED_INDEXES)
    {
        CuvsCircuitBreaker *b = &cuvs_circuit_breakers[cuvs_n_circuit_breakers++];
        b->index_oid        = index_oid;
        b->consecutive_errors = 0;
        b->open             = 0;
        return b;
    }
    return NULL;
}

void
cuvs_circuit_record_error(uint32_t index_oid, int threshold)
{
    CuvsCircuitBreaker *b = find_or_create_breaker(index_oid);
    if (!b)
        return;
    b->consecutive_errors++;
    if (b->consecutive_errors >= threshold)
        b->open = 1;
}

void
cuvs_circuit_record_success(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
        {
            cuvs_circuit_breakers[i].consecutive_errors = 0;
            return;
        }
}

void
cuvs_circuit_reset(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
    {
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
        {
            cuvs_circuit_breakers[i].consecutive_errors = 0;
            cuvs_circuit_breakers[i].open = 0;
            return;
        }
    }
}

int
cuvs_circuit_is_open(uint32_t index_oid)
{
    for (int i = 0; i < cuvs_n_circuit_breakers; i++)
        if (cuvs_circuit_breakers[i].index_oid == index_oid)
            return cuvs_circuit_breakers[i].open;
    return 0;
}

void
cuvs_circuit_reset_all(void)
{
    memset(cuvs_circuit_breakers, 0, sizeof(cuvs_circuit_breakers));
    cuvs_n_circuit_breakers = 0;
}

/* ----------------------------------------------------------------
 * Phase 3L-9: brute-force micro-batch grouping (pure).
 * ---------------------------------------------------------------- */
void
cuvs_bf_batch_group(const CuvsBfKey *keys, int n,
                    int *group_id_out, int *n_groups_out)
{
    int ng = 0;
    for (int i = 0; i < n; i++)
    {
        int g = -1;
        /* Reuse the group of the first earlier request with an identical key. */
        for (int j = 0; j < i; j++)
        {
            if (keys[j].db_oid    == keys[i].db_oid &&
                keys[j].index_oid == keys[i].index_oid &&
                keys[j].precision == keys[i].precision &&
                keys[j].dim       == keys[i].dim)
            {
                g = group_id_out[j];
                break;
            }
        }
        if (g < 0)
            g = ng++;               /* new key -> next dense, first-seen group id */
        group_id_out[i] = g;
    }
    if (n_groups_out)
        *n_groups_out = ng;
}
