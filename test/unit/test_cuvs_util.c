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

int
main(void)
{
    test_tid_roundtrip();
    test_parse_index_filename();
    test_status_str();
    test_circuit_breaker();

    printf("[INFO] cuvs_util unit tests: %d passed, %d failed\n", g_pass, g_fail);
    if (g_fail == 0)
        printf("[OK] all tests passed\n");
    return g_fail == 0 ? 0 : 1;
}
