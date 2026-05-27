/*
 * test_cuvs_util.c — standalone unit tests for src/cuvs_util.{h,c}.
 *
 * No framework, no PostgreSQL, no CUDA. Build + run via `make test-unit`.
 * Returns non-zero on any failure.
 */

#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_STATUS_* values */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT(cond, msg) do {                                          \
    if (cond) {                                                         \
        g_pass++;                                                       \
    } else {                                                            \
        g_fail++;                                                       \
        fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
    }                                                                   \
} while (0)

static void
test_tid_roundtrip(void)
{
    struct { uint32_t block; uint16_t offset; } cases[] = {
        { 0,          0 },
        { 0,          0xFFFF },
        { 1,          1 },
        { 0xFFFFFFFFu, 0 },
        { 0xFFFFFFFFu, 0xFFFF },
        { 123456,     7 },
        { 0x00ABCDEFu, 0x1234 },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        uint64_t enc = cuvs_tid_encode(cases[i].block, cases[i].offset);
        uint32_t block;
        uint16_t offset;
        cuvs_tid_decode(enc, &block, &offset);
        ASSERT(block == cases[i].block, "tid block roundtrip");
        ASSERT(offset == cases[i].offset, "tid offset roundtrip");
    }

    /* Explicit bit-layout check: block<<16 | offset */
    ASSERT(cuvs_tid_encode(0xABCD, 0x1234) == (((uint64_t)0xABCD << 16) | 0x1234),
           "tid encode bit layout");
}

static void
test_parse_index_filename(void)
{
    uint32_t db = 999, ix = 999;

    ASSERT(cuvs_parse_index_filename("16384_24576.cagra", &db, &ix) == 0,
           "valid filename parses");
    ASSERT(db == 16384 && ix == 24576, "valid filename values");

    db = ix = 999;
    ASSERT(cuvs_parse_index_filename("1_2.cagra", &db, &ix) == 0, "minimal valid");
    ASSERT(db == 1 && ix == 2, "minimal valid values");

    /* Invalid inputs */
    ASSERT(cuvs_parse_index_filename(NULL, &db, &ix) == -1, "null name");
    ASSERT(cuvs_parse_index_filename("16384_24576.tids", &db, &ix) == -1,
           "wrong suffix (.tids)");
    ASSERT(cuvs_parse_index_filename(".cagra", &db, &ix) == -1, "suffix only");
    ASSERT(cuvs_parse_index_filename("16384.cagra", &db, &ix) == -1,
           "missing underscore");
    ASSERT(cuvs_parse_index_filename("notanumber_x.cagra", &db, &ix) == -1,
           "non-numeric fields");
    ASSERT(cuvs_parse_index_filename("16384_24576.cagra.bak", &db, &ix) == -1,
           "trailing extension");
    ASSERT(cuvs_parse_index_filename("", &db, &ix) == -1, "empty string");

    /* More adversarial separators / missing fields */
    ASSERT(cuvs_parse_index_filename("16384__24576.cagra", &db, &ix) == -1,
           "double underscore");
    ASSERT(cuvs_parse_index_filename("16384_.cagra", &db, &ix) == -1,
           "missing second oid");
    ASSERT(cuvs_parse_index_filename("_24576.cagra", &db, &ix) == -1,
           "missing first oid");

    /* Boundary: 0_0 is a structurally valid name (OID 0 never occurs in
     * practice, but the parser must not special-case it). */
    db = ix = 999;
    ASSERT(cuvs_parse_index_filename("0_0.cagra", &db, &ix) == 0, "zero oids parse");
    ASSERT(db == 0 && ix == 0, "zero oids values");
}

static void
test_status_str(void)
{
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_OK), "ok") == 0, "status ok");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_ERROR), "error") == 0, "status error");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_OOM_FALLBACK), "oom_fallback") == 0,
           "status oom_fallback");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_NOT_FOUND), "not_found") == 0,
           "status not_found");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_UNAVAILABLE), "unavailable") == 0,
           "status unavailable");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_BUILD_FAILED), "build_failed") == 0,
           "status build_failed");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_PERSIST_FAILED), "persist_failed") == 0,
           "status persist_failed");
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_DIM_MISMATCH), "dim_mismatch") == 0,
           "status dim_mismatch");
    ASSERT(strcmp(cuvs_status_str(42), "unknown") == 0, "status out-of-range");
    ASSERT(strcmp(cuvs_status_str(-1), "unknown") == 0, "status negative");
}

static void
test_circuit_breaker(void)
{
    const uint32_t oid = 7;
    const int threshold = 3;

    cuvs_circuit_reset_all();
    ASSERT(cuvs_n_circuit_breakers == 0, "reset_all zeroes count");
    ASSERT(cuvs_circuit_is_open(oid) == 0, "fresh breaker is closed");

    /* Errors below threshold do not trip */
    cuvs_circuit_record_error(oid, threshold);
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "below threshold stays closed");

    /* Reaching threshold trips it open */
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 1, "threshold trips open");

    /* record_success clears the error count but not the open flag */
    cuvs_circuit_record_success(oid);
    ASSERT(cuvs_circuit_is_open(oid) == 1, "success leaves open flag set");

    /* reset clears both */
    cuvs_circuit_reset(oid);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "reset clears open flag");

    /* After reset, must accumulate threshold errors again to trip */
    cuvs_circuit_record_error(oid, threshold);
    cuvs_circuit_record_error(oid, threshold);
    ASSERT(cuvs_circuit_is_open(oid) == 0, "post-reset below threshold closed");

    /* Independent OIDs tracked separately */
    cuvs_circuit_reset_all();
    cuvs_circuit_record_error(1, threshold);
    cuvs_circuit_record_error(1, threshold);
    cuvs_circuit_record_error(1, threshold);
    ASSERT(cuvs_circuit_is_open(1) == 1, "oid 1 open");
    ASSERT(cuvs_circuit_is_open(2) == 0, "oid 2 unaffected");

    /* reset_all zeroes everything between cases */
    cuvs_circuit_reset_all();
    ASSERT(cuvs_circuit_is_open(1) == 0, "reset_all clears oid 1");
    ASSERT(cuvs_n_circuit_breakers == 0, "reset_all count back to zero");
}

static void
test_crc32(void)
{
    /* Standard CRC-32 (IEEE 802.3) check vectors. */
    ASSERT(cuvs_crc32("", 0) == 0x00000000u, "crc32 empty");
    ASSERT(cuvs_crc32("123456789", 9) == 0xCBF43926u, "crc32 check string");
    ASSERT(cuvs_crc32("a", 1) == 0xE8B7BE43u, "crc32 single byte");

    /* Sensitivity: a one-byte change must change the crc. */
    ASSERT(cuvs_crc32("123456789", 9) != cuvs_crc32("123456780", 9),
           "crc32 detects single-byte diff");
}

static void
test_tids_roundtrip(void)
{
    const int64_t  n = 5;
    const uint32_t dim = 128, metric = 1;
    uint64_t tids[5] = { 0, 1, 0xFFFFFFFFFFFFFFFFull, 42, 0xABCDEF01ull };

    FILE *f = tmpfile();
    ASSERT(f != NULL, "tmpfile open (write)");
    ASSERT(cuvs_tids_write(f, n, dim, metric, tids) == 0, "tids_write ok");
    rewind(f);

    CuvsTidsHeader hdr;
    uint64_t *out = NULL;
    ASSERT(cuvs_tids_read(f, &hdr, &out) == 0, "tids_read ok");
    ASSERT(hdr.magic == CUVS_TIDS_MAGIC, "rt magic");
    ASSERT(hdr.version == CUVS_TIDS_VERSION, "rt version");
    ASSERT(hdr.n_vecs == n, "rt n_vecs");
    ASSERT(hdr.dim == dim, "rt dim");
    ASSERT(hdr.metric == metric, "rt metric");
    ASSERT(hdr.reserved == 0, "rt reserved zero");
    if (out)
    {
        ASSERT(memcmp(out, tids, sizeof(tids)) == 0, "rt tids body identity");
        free(out);
    }
    fclose(f);
}

/* Write a valid .tids into a tmpfile, optionally let the caller corrupt the
 * raw bytes, then assert cuvs_tids_read rejects it. */
static void
test_tids_rejections(void)
{
    const int64_t  n = 4;
    const uint32_t dim = 16, metric = 0;
    uint64_t tids[4] = { 10, 20, 30, 40 };

    /* bad magic */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        rewind(f);
        uint32_t bad = 0xDEADBEEFu;
        fwrite(&bad, sizeof(bad), 1, f);   /* overwrite magic */
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject bad magic");
        ASSERT(o == NULL, "bad magic leaves out NULL");
        fclose(f);
    }

    /* bad version */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, sizeof(uint32_t), SEEK_SET);  /* version field */
        uint32_t badv = 999u;
        fwrite(&badv, sizeof(badv), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject bad version");
        fclose(f);
    }

    /* n_vecs <= 0 */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, offsetof(CuvsTidsHeader, n_vecs), SEEK_SET);
        int64_t zero = 0;
        fwrite(&zero, sizeof(zero), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject n_vecs<=0");
        fclose(f);
    }

    /* n_vecs > CAP */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, offsetof(CuvsTidsHeader, n_vecs), SEEK_SET);
        int64_t huge = CUVS_TIDS_MAX_VECS + 1;
        fwrite(&huge, sizeof(huge), 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject n_vecs>CAP");
        fclose(f);
    }

    /* truncated body: header claims n=4 but only 2 TIDs present */
    {
        FILE *f = tmpfile();
        CuvsTidsHeader h;
        h.magic = CUVS_TIDS_MAGIC; h.version = CUVS_TIDS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(tids, sizeof(tids)); h.reserved = 0;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(tids, sizeof(uint64_t), 2, f);   /* short body */
        rewind(f);
        CuvsTidsHeader hr; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &hr, &o) == -1, "reject truncated body");
        ASSERT(o == NULL, "truncated body frees alloc");
        fclose(f);
    }

    /* corrupted body: flip a byte so crc mismatches */
    {
        FILE *f = tmpfile();
        cuvs_tids_write(f, n, dim, metric, tids);
        fseek(f, sizeof(CuvsTidsHeader), SEEK_SET);  /* first body byte */
        unsigned char b;
        fread(&b, 1, 1, f);
        fseek(f, sizeof(CuvsTidsHeader), SEEK_SET);
        b ^= 0xFFu;
        fwrite(&b, 1, 1, f);
        rewind(f);
        CuvsTidsHeader h; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &h, &o) == -1, "reject crc mismatch");
        ASSERT(o == NULL, "crc mismatch frees alloc");
        fclose(f);
    }

    /* reserved != 0: valid otherwise, but the reserved field is set */
    {
        FILE *f = tmpfile();
        CuvsTidsHeader h;
        h.magic = CUVS_TIDS_MAGIC; h.version = CUVS_TIDS_VERSION;
        h.n_vecs = n; h.dim = dim; h.metric = metric;
        h.body_crc32 = cuvs_crc32(tids, sizeof(tids)); h.reserved = 1;
        fwrite(&h, sizeof(h), 1, f);
        fwrite(tids, sizeof(uint64_t), (size_t)n, f);
        rewind(f);
        CuvsTidsHeader hr; uint64_t *o = NULL;
        ASSERT(cuvs_tids_read(f, &hr, &o) == -1, "reject reserved != 0");
        ASSERT(o == NULL, "reserved!=0 leaves out NULL");
        fclose(f);
    }
}

#ifdef CUVS_TEST_HOOKS
static void
test_fault_hook(void)
{
    unsetenv("CUVS_FAULT_DUMMY");
    ASSERT(cuvs_fault("CUVS_FAULT_DUMMY") == 0, "fault unset returns 0");
    setenv("CUVS_FAULT_DUMMY", "1", 1);
    ASSERT(cuvs_fault("CUVS_FAULT_DUMMY") == 1, "fault set returns 1");
    unsetenv("CUVS_FAULT_DUMMY");
}
#endif

/* Write a delta header + body into a tmpfile and assert validate accepts the
 * good case and rejects bad magic/version/reserved/dim and a truncated body. */
static void
test_delta_format(void)
{
    const uint32_t dim = 4, metric = CUVS_METRIC_L2, base_crc = 0xCAFEBABEu;
    const int64_t  n = 3;
    size_t rec = cuvs_delta_record_bytes(dim);

    ASSERT(rec == sizeof(uint64_t) + 4 * sizeof(float), "delta record bytes");
    ASSERT(sizeof(CuvsDeltaHeader) == 32, "delta header is 32 bytes");

    /* init + round-trip a header through a FILE*. */
    CuvsDeltaHeader h;
    cuvs_delta_header_init(&h, dim, metric, base_crc);
    h.n_rows = n;
    ASSERT(h.magic == CUVS_DELTA_MAGIC, "init magic");
    ASSERT(h.version == CUVS_DELTA_VERSION, "init version");
    ASSERT(h.reserved == 0, "init reserved zero");
    ASSERT(h.base_tids_crc32 == base_crc, "init generation token");

    FILE *f = tmpfile();
    ASSERT(f != NULL, "delta tmpfile open");
    fwrite(&h, sizeof(h), 1, f);
    /* n records of {tid, vec[dim]} — content does not matter for validate. */
    for (int64_t i = 0; i < n; i++)
    {
        uint64_t tid = cuvs_tid_encode((uint32_t) i, (uint16_t) i);
        float    vec[4] = { (float) i, 0.0f, 0.0f, 0.0f };
        fwrite(&tid, sizeof(tid), 1, f);
        fwrite(vec, sizeof(float), dim, f);
    }
    rewind(f);

    CuvsDeltaHeader hr;
    ASSERT(cuvs_delta_read_header(f, &hr) == 0, "delta read_header ok");
    ASSERT(hr.n_rows == n && hr.dim == dim, "delta header round-trip");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec)) == 0,
           "validate accepts exact body size");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec) - 1) == -1,
           "validate rejects truncated body");
    ASSERT(cuvs_delta_validate(&hr, (int64_t) (n * (int64_t) rec) + rec) == -1,
           "validate rejects oversized body");
    fclose(f);

    /* field rejections */
    CuvsDeltaHeader b;
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.magic = 0xDEADBEEFu;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad magic");
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.version = 99u;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject bad version");
    cuvs_delta_header_init(&b, dim, metric, base_crc); b.n_rows = n;
    b.reserved = 1u;
    ASSERT(cuvs_delta_validate(&b, (int64_t)(n * (int64_t) rec)) == -1, "reject reserved != 0");
    cuvs_delta_header_init(&b, 0, metric, base_crc); b.n_rows = n;
    ASSERT(cuvs_delta_validate(&b, 0) == -1, "reject dim 0");

    /* generation mismatch is a header-field comparison the caller makes; here
     * we just confirm the token survives a round-trip so the gate can compare. */
    ASSERT(hr.base_tids_crc32 == base_crc, "generation token preserved on read");
}

static void
test_lat_histogram(void)
{
    /* Bucket boundaries: 0->0, [2^(k-1),2^k)->k. */
    ASSERT(cuvs_lat_bucket_index(0) == 0, "bucket 0 us -> 0");
    ASSERT(cuvs_lat_bucket_index(1) == 1, "bucket 1 us -> 1");
    ASSERT(cuvs_lat_bucket_index(2) == 2, "bucket 2 us -> 2");
    ASSERT(cuvs_lat_bucket_index(3) == 2, "bucket 3 us -> 2 ([2,4))");
    ASSERT(cuvs_lat_bucket_index(4) == 3, "bucket 4 us -> 3");
    ASSERT(cuvs_lat_bucket_index(1000) == 10, "bucket 1000 us -> 10 ([512,1024))");
    /* Huge latency clamps to the last bucket, never overruns the array. */
    ASSERT(cuvs_lat_bucket_index(0xFFFFFFFFu) == CUVS_LAT_BUCKETS - 1,
           "bucket UINT32_MAX clamps to last");

    uint32_t hist[CUVS_LAT_BUCKETS];

    /* Empty histogram -> 0 for every quantile. */
    memset(hist, 0, sizeof(hist));
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 0, "empty p50 -> 0");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 0, "empty p99 -> 0");

    /* Single sample in bucket 5 -> upper edge 2^5 = 32. */
    memset(hist, 0, sizeof(hist));
    hist[5] = 1;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 32u, "single p50 -> 32");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 32u, "single p99 -> 32");

    /* 100 samples all in bucket 10 -> every quantile is the bucket upper (1024). */
    memset(hist, 0, sizeof(hist));
    hist[10] = 100;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 1024u, "uniform p50 -> 1024");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.95) == 1024u, "uniform p95 -> 1024");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 1024u, "uniform p99 -> 1024");

    /* Skewed: 90 fast (bucket 3 -> 8us), 10 slow (bucket 10 -> 1024us).
     * p50 falls in the fast bulk; p95/p99 cross into the slow tail. */
    memset(hist, 0, sizeof(hist));
    hist[3] = 90;
    hist[10] = 10;
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.50) == 8u, "skewed p50 -> 8");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.95) == 1024u, "skewed p95 -> 1024 (tail)");
    ASSERT(cuvs_lat_percentile(hist, CUVS_LAT_BUCKETS, 0.99) == 1024u, "skewed p99 -> 1024 (tail)");
}

static void
test_metric_from_opclass(void)
{
    ASSERT(cuvs_metric_from_opclass_name("vector_l2_ops")     == CUVS_METRIC_L2,     "l2 opclass -> L2");
    ASSERT(cuvs_metric_from_opclass_name("vector_cosine_ops") == CUVS_METRIC_COSINE, "cosine opclass -> COSINE");
    ASSERT(cuvs_metric_from_opclass_name("vector_ip_ops")     == CUVS_METRIC_IP,     "ip opclass -> IP");
    ASSERT(cuvs_metric_from_opclass_name("bogus_ops")         == -1, "unknown opclass -> -1");
    ASSERT(cuvs_metric_from_opclass_name("")                  == -1, "empty opclass -> -1");
    ASSERT(cuvs_metric_from_opclass_name(NULL)                == -1, "NULL opclass -> -1");
    /* status string for the new metric-mismatch code. */
    ASSERT(strcmp(cuvs_status_str(CUVS_STATUS_METRIC_MISMATCH), "metric_mismatch") == 0,
           "status_str METRIC_MISMATCH");
}

int
main(void)
{
    test_tid_roundtrip();
    test_parse_index_filename();
    test_status_str();
    test_circuit_breaker();
    test_crc32();
    test_tids_roundtrip();
    test_tids_rejections();
    test_delta_format();
    test_lat_histogram();
    test_metric_from_opclass();
#ifdef CUVS_TEST_HOOKS
    test_fault_hook();
#endif

    printf("[INFO] cuvs_util unit tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("[OK] all tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
