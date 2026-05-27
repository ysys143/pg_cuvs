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
    size_t namelen;
    const char *suffix = ".cagra";
    size_t suflen = strlen(suffix);

    if (!name)
        return -1;

    /* sscanf returns conversion count regardless of literal match — must
     * also verify the ".cagra" suffix to avoid matching .tids files. */
    namelen = strlen(name);
    if (namelen <= suflen)
        return -1;
    if (strcmp(name + namelen - suflen, suffix) != 0)
        return -1;
    if (sscanf(name, "%u_%u.cagra", db_oid, index_oid) == 2)
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
uint32_t
cuvs_crc32(const void *data, size_t len)
{
    static uint32_t table[256];
    static int      table_init = 0;
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;

    if (!table_init)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        table_init = 1;
    }

    for (size_t i = 0; i < len; i++)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
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
