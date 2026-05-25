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

/* ----------------------------------------------------------------
 * Index filename parsing: "<db_oid>_<index_oid>.cagra"
 * ---------------------------------------------------------------- */
int
cuvs_parse_index_filename(const char *name, uint32_t *db_oid, uint32_t *index_oid)
{
    if (!name)
        return -1;

    /* sscanf returns conversion count regardless of literal match — must
     * also verify the ".cagra" suffix to avoid matching .tids files. */
    size_t namelen = strlen(name);
    const char *suffix = ".cagra";
    size_t suflen = strlen(suffix);
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
        default:                       return "unknown";
    }
}

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
