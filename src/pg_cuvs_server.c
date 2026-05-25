/*
 * pg_cuvs_server.c — GPU sidecar daemon for pg_cuvs.
 *
 * Standalone C program (NOT a PostgreSQL extension). Compiled separately
 * from the .so and linked against libcuvs + CUDA.
 *
 * Responsibilities:
 *   - Listen on UDS socket for SEARCH and BUILD commands
 *   - Manage CAGRA indexes in VRAM (LRU eviction)
 *   - Serialize indexes to disk on SIGTERM; reload on startup
 *   - Serve >=2 concurrent PG backends (pthread-per-connection)
 *
 * Usage:
 *   pg_cuvs_server --socket /tmp/.s.pg_cuvs.12345 \
 *                  --index-dir /var/lib/postgresql/16/main/cuvs_indexes \
 *                  --max-vram-mb 20480
 */

#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "cuvs_wrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Leveled logging (LOG_ERROR/WARN/INFO unconditional, LOG_DEBUG gated by
 * PG_CUVS_DEBUG) is provided by cuvs_util.h. Enable hot-path traces via
 * -DPG_CUVS_DEBUG=1 in the Makefile or env. */
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * Index registry
 * ---------------------------------------------------------------- */
#define MAX_INDEXES 64

typedef struct IndexEntry {
    uint32_t        db_oid;
    uint32_t        index_oid;
    uint32_t        dim;
    uint32_t        metric;
    int64_t         n_vecs;
    CuvsCagraIndex  handle;       /* cuVS opaque index */
    uint64_t       *tids;         /* TID array [n_vecs] */
    size_t          vram_bytes;   /* estimated VRAM usage */
    time_t          last_search;  /* for LRU eviction */
    int             valid;
} IndexEntry;

static IndexEntry  g_indexes[MAX_INDEXES];
static int         g_n_indexes   = 0;
static pthread_mutex_t g_index_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */
static char   g_socket_path[256] = "/tmp/.s.pg_cuvs.server";
static char   g_index_dir[256]   = "/tmp/pg_cuvs_indexes";
static size_t g_max_vram_bytes   = 0;  /* 0 = unlimited */
static int    g_server_fd        = -1;
static volatile int g_shutdown   = 0;

/* ----------------------------------------------------------------
 * VRAM helpers
 * ---------------------------------------------------------------- */
static size_t
estimate_vram_bytes(int64_t n_vecs, int dim)
{
    /* CAGRA graph: ~16 edges × 4 bytes per node, plus float vectors */
    return (size_t)n_vecs * ((size_t)dim * sizeof(float) + 16 * 4);
}

static size_t
total_vram_used(void)
{
    size_t total = 0;
    for (int i = 0; i < g_n_indexes; i++)
        if (g_indexes[i].valid)
            total += g_indexes[i].vram_bytes;
    return total;
}

static size_t
gpu_free_vram_bytes(void)
{
    return cuvs_vram_free_bytes();
}

/* ----------------------------------------------------------------
 * Index persistence helpers
 * ---------------------------------------------------------------- */
static void
index_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.cagra", dir, db_oid, index_oid);
}

static void
tids_file_path(char *out, size_t outlen,
               const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.tids", dir, db_oid, index_oid);
}

/* Forward decls used by save_index. */
static int write_tids_atomic(const char *tids_tmp,
                             int64_t n_vecs, uint32_t dim, uint32_t metric,
                             const uint64_t *tids);
static int fsync_path(const char *path);

/* save_index: persist a registry entry to disk atomically.
 * Same contract as handle_build's persistence path: tmp + rename + fsync.
 * Returns 0 on success, -1 on any failure (caller must NOT proceed to free
 * VRAM state, e.g., evict_lru must abort eviction on failure). */
static int
save_index(IndexEntry *e)
{
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_SAVE_INDEX")) {
        LOG_ERROR("save_index: CUVS_FAULT_SAVE_INDEX -> forced failure for %u/%u\n",
                e->db_oid, e->index_oid);
        return -1;
    }
#endif
    char idx_final[512],  idx_tmp[576];
    char tids_final[512], tids_tmp[576];
    index_file_path(idx_final,  sizeof(idx_final),  g_index_dir, e->db_oid, e->index_oid);
    tids_file_path(tids_final,  sizeof(tids_final), g_index_dir, e->db_oid, e->index_oid);
    snprintf(idx_tmp,  sizeof(idx_tmp),  "%s.tmp", idx_final);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);

    if (write_tids_atomic(tids_tmp, e->n_vecs, e->dim, e->metric, e->tids) != 0)
        return -1;

    if (cuvs_cagra_serialize(e->handle, idx_tmp) != 0) {
        LOG_ERROR("save_index: cuvs_cagra_serialize FAILED for %u/%u\n",
                e->db_oid, e->index_oid);
        unlink(tids_tmp);
        return -1;
    }
    if (fsync_path(idx_tmp) != 0)
        LOG_WARN("save_index: fsync %s failed errno=%d\n", idx_tmp, errno);

    if (rename(tids_tmp, tids_final) != 0) {
        LOG_ERROR("save_index: rename %s -> %s FAILED errno=%d (%s)\n",
                tids_tmp, tids_final, errno, strerror(errno));
        unlink(tids_tmp);
        unlink(idx_tmp);
        return -1;
    }
    if (rename(idx_tmp, idx_final) != 0) {
        LOG_ERROR("save_index: rename %s -> %s FAILED errno=%d (%s); "
                  "unlinking tids to avoid mismatch\n",
                idx_tmp, idx_final, errno, strerror(errno));
        unlink(tids_final);
        unlink(idx_tmp);
        return -1;
    }
    int dir_fd = open(g_index_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    LOG_INFO("pg_cuvs_server: saved index %u/%u (%lld vecs)\n",
            e->db_oid, e->index_oid, (long long)e->n_vecs);
    return 0;
}

static int
load_index(uint32_t db_oid, uint32_t index_oid)
{
    char idx_path[512], tids_path[512];
    index_file_path(idx_path, sizeof(idx_path), g_index_dir, db_oid, index_oid);
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, db_oid, index_oid);

    /* Read + validate the versioned, checksummed .tids sidecar. A reject
     * (bad magic/version/range/crc, legacy headerless format) means this
     * pair must be skipped; the index must be REINDEXed. */
    FILE *f = fopen(tids_path, "rb");
    if (!f)
        return -1;

    CuvsTidsHeader thdr;
    uint64_t *tids = NULL;
    if (cuvs_tids_read(f, &thdr, &tids) != 0)
    {
        fclose(f);
        LOG_ERROR("pg_cuvs_server: .tids validation failed for %u/%u, skip\n",
                db_oid, index_oid);
        return -1;
    }
    fclose(f);

    int64_t  n_vecs = thdr.n_vecs;
    uint32_t dim    = thdr.dim;
    uint32_t metric = thdr.metric;

    /* Header sanity: n_vecs already validated > 0 by cuvs_tids_read; require
     * a non-zero dimension before trusting the cagra side. */
    if (dim == 0)
    {
        LOG_ERROR("pg_cuvs_server: .tids header dim=0 for %u/%u, skip\n",
                db_oid, index_oid);
        free(tids);
        return -1;
    }

    /* VRAM preflight check */
    size_t needed = estimate_vram_bytes(n_vecs, (int)dim);
    size_t free_vram = gpu_free_vram_bytes();
    if (g_max_vram_bytes > 0 && total_vram_used() + needed > g_max_vram_bytes)
    {
        LOG_WARN("pg_cuvs_server: VRAM budget exceeded loading %u/%u, skip\n",
                db_oid, index_oid);
        free(tids);
        return -1;
    }
    if (needed > free_vram)
    {
        LOG_WARN("pg_cuvs_server: insufficient VRAM loading %u/%u, skip\n",
                db_oid, index_oid);
        free(tids);
        return -1;
    }

    CuvsCagraIndex handle = cuvs_cagra_deserialize(idx_path, (int)dim);
    if (!handle)
    {
        free(tids);
        return -1;
    }

    if (g_n_indexes >= MAX_INDEXES)
    {
        cuvs_cagra_free(handle);
        free(tids);
        return -1;
    }

    IndexEntry *e = &g_indexes[g_n_indexes++];
    e->db_oid      = db_oid;
    e->index_oid   = index_oid;
    e->dim         = dim;
    e->metric      = metric;
    e->n_vecs      = n_vecs;
    e->handle      = handle;
    e->tids        = tids;
    e->vram_bytes  = needed;
    e->last_search = time(NULL);
    e->valid       = 1;

    LOG_INFO("pg_cuvs_server: loaded index %u/%u (%lld vecs, %zu MB VRAM)\n",
            db_oid, index_oid, (long long)n_vecs, needed / (1024*1024));
    return 0;
}

/* Scan index_dir on startup and load all valid .cagra/.tids pairs. */
static void
startup_load_indexes(void)
{
    mkdir(g_index_dir, 0700);

    DIR *dir = opendir(g_index_dir);
    if (!dir)
        return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        /* Parse filenames of the form "<db_oid>_<index_oid>.cagra" (also
         * rejects .tids files). See cuvs_parse_index_filename. */
        if (cuvs_parse_index_filename(ent->d_name, &db_oid, &index_oid) == 0)
            load_index(db_oid, index_oid);
    }
    closedir(dir);
}

/* ----------------------------------------------------------------
 * LRU eviction
 * ---------------------------------------------------------------- */
static IndexEntry *
find_lru_index(void)
{
    IndexEntry *lru = NULL;
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (!g_indexes[i].valid)
            continue;
        if (!lru || g_indexes[i].last_search < lru->last_search)
            lru = &g_indexes[i];
    }
    return lru;
}

/* Evict the LRU index to free VRAM. Returns bytes freed, or 0.
 * If save_index fails (disk full, permission, cuVS error), abort eviction —
 * we must not lose VRAM state when we couldn't persist it. */
static size_t
evict_lru(void)
{
    IndexEntry *e = find_lru_index();
    if (!e)
        return 0;

    if (save_index(e) != 0) {
        LOG_ERROR("evict_lru: save_index FAILED for %u/%u; aborting eviction "
                  "(VRAM still holds index)\n",
                e->db_oid, e->index_oid);
        return 0;
    }
    cuvs_cagra_free(e->handle);
    free(e->tids);
    size_t freed = e->vram_bytes;

    /* Remove from registry by shifting */
    int idx = (int)(e - g_indexes);
    for (int i = idx; i < g_n_indexes - 1; i++)
        g_indexes[i] = g_indexes[i+1];
    g_n_indexes--;

    return freed;
}

/* Ensure there is enough VRAM for `needed` bytes.
 * Returns 0 on success, -1 if even after full eviction there isn't enough. */
static int
ensure_vram(size_t needed)
{
    /* Layer 1: preflight check */
    if (g_max_vram_bytes > 0 && total_vram_used() + needed > g_max_vram_bytes)
    {
        /* Layer 2: LRU eviction loop */
        while (g_n_indexes > 0 && total_vram_used() + needed > g_max_vram_bytes)
            evict_lru();

        if (total_vram_used() + needed > g_max_vram_bytes)
            return -1;  /* Layer 3: caller falls back to CPU */
    }

    size_t free_vram = gpu_free_vram_bytes();
    while (g_n_indexes > 0 && needed > free_vram)
    {
        evict_lru();
        free_vram = gpu_free_vram_bytes();
    }

    return (needed <= free_vram) ? 0 : -1;
}

/* ----------------------------------------------------------------
 * Index lookup
 * ---------------------------------------------------------------- */
static IndexEntry *
find_index(uint32_t db_oid, uint32_t index_oid)
{
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (g_indexes[i].valid &&
            g_indexes[i].db_oid    == db_oid &&
            g_indexes[i].index_oid == index_oid)
            return &g_indexes[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------
 * Socket I/O helpers (same as client)
 * ---------------------------------------------------------------- */
static int
send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

static int
recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = read(fd, p + received, len - received);
        if (n <= 0)
            return -1;
        received += n;
    }
    return 0;
}

static void
send_error_code(int client_fd, int status, const char *msg)
{
    CuvsReplyHeader hdr = {0};
    hdr.status = status;
    hdr.n_results = 0;
    strncpy(hdr.error, msg, sizeof(hdr.error) - 1);
    send_all(client_fd, &hdr, sizeof(hdr));
}

static void
send_error(int client_fd, const char *msg)
{
    send_error_code(client_fd, CUVS_STATUS_ERROR, msg);
}

/* ----------------------------------------------------------------
 * Handle SEARCH command
 * ---------------------------------------------------------------- */
static void
handle_search(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e)
    {
        /* Try to load from disk */
        if (load_index(cmd->db_oid, cmd->index_oid) == 0)
            e = find_index(cmd->db_oid, cmd->index_oid);
    }

    if (!e)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NOT_FOUND;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    e->last_search = time(NULL);

    /* Map shm query vector */
    size_t vec_bytes = (size_t)cmd->dim * sizeof(float);
    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed");
        return;
    }

    float *query = mmap(NULL, vec_bytes, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (query == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed");
        return;
    }

    int k = (int)cmd->k;
    CuvsSearchResult *raw = malloc(k * sizeof(CuvsSearchResult));
    if (!raw)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    LOG_DEBUG("[handle_search] calling cuvs_cagra_search k=%d dim=%u...\n",
            k, cmd->dim);
    int ret = cuvs_cagra_search(e->handle, query, (int)cmd->dim, k, raw);
    LOG_DEBUG("[handle_search] cuvs_cagra_search rc=%d\n", ret);
    munmap(query, vec_bytes);

    if (ret != 0)
    {
        free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "CAGRA search failed", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Map item_ids to TIDs */
    CuvsResult *results = malloc(k * sizeof(CuvsResult));
    if (!results)
    {
        free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    int n_valid = 0;
    for (int i = 0; i < k; i++)
    {
        int64_t item_id = raw[i].item_id;
        if (item_id < 0 || item_id >= e->n_vecs)
            continue;
        results[n_valid].tid      = e->tids[item_id];
        results[n_valid].distance = raw[i].distance;
        n_valid++;
    }
    free(raw);

    pthread_mutex_unlock(&g_index_mutex);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)n_valid;
    hdr.latency_us = latency_us;

    send_all(client_fd, &hdr, sizeof(hdr));
    if (n_valid > 0)
        send_all(client_fd, results, (size_t)n_valid * sizeof(CuvsResult));

    free(results);
}

/* ----------------------------------------------------------------
 * Atomic tids file write helper.
 * Writes to <tids_tmp> path, fsyncs, returns 0 on success, -1 on failure.
 * On failure the tmp file is unlinked.
 * ---------------------------------------------------------------- */
static int
write_tids_atomic(const char *tids_tmp,
                  int64_t n_vecs, uint32_t dim, uint32_t metric,
                  const uint64_t *tids)
{
    FILE *f = fopen(tids_tmp, "wb");
    if (!f) {
        LOG_ERROR("write_tids_atomic: fopen %s FAILED errno=%d (%s)\n",
                tids_tmp, errno, strerror(errno));
        return -1;
    }
    int ok = (cuvs_tids_write(f, n_vecs, dim, metric, tids) == 0);
    if (ok && fflush(f) != 0) {
        LOG_ERROR("write_tids_atomic: fflush %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (ok && fsync(fileno(f)) != 0) {
        LOG_ERROR("write_tids_atomic: fsync %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (fclose(f) != 0) {
        LOG_ERROR("write_tids_atomic: fclose %s FAILED errno=%d\n",
                tids_tmp, errno);
        ok = 0;
    }
    if (!ok) {
        unlink(tids_tmp);
        return -1;
    }
    return 0;
}

/* fsync a file by path (re-opens it RDONLY). Best-effort; returns 0/-1. */
static int
fsync_path(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    close(fd);
    return rc;
}

/* ----------------------------------------------------------------
 * Handle BUILD command
 *
 * Atomicity contract (B-1 + B-2 + B-3):
 *   Either CREATE INDEX succeeds with VRAM + disk both committed, or it
 *   fails with no state change (existing index, if any, untouched).
 *
 * Sequence:
 *   1. Build new VRAM index (do NOT drop existing yet).
 *   2. Write tids.tmp atomically (fwrite count + fsync + fclose checked).
 *   3. Serialize cagra.tmp and fsync.
 *   4. Atomic rename: tmp -> final.
 *   5. fsync directory.
 *   6. Swap registry: free existing, install new.
 *
 * Any failure before step 4 → unlink tmps, free new resources, error reply.
 * Failure between renames is logged; partial state may persist on disk but
 * registry is rolled back so memory state stays consistent.
 * ---------------------------------------------------------------- */
static void
handle_build(int client_fd, const CuvsCmdFrame *cmd)
{
    LOG_DEBUG("[handle_build] reading index_dir...\n");
    char index_dir[256] = {0};
    if (recv_all(client_fd, index_dir, sizeof(index_dir)) < 0)
    {
        LOG_ERROR("[handle_build] recv index_dir FAILED errno=%d\n", errno);
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    LOG_DEBUG("[handle_build] got index_dir=%s\n", index_dir);

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    LOG_DEBUG("[handle_build] shm_open(%s)...\n", cmd->shm_key);
    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        LOG_ERROR("[handle_build] shm_open FAILED errno=%d (%s)\n", errno, strerror(errno));
        send_error(client_fd, "shm_open failed");
        return;
    }
    LOG_DEBUG("[handle_build] shm_open OK fd=%d\n", shm_fd);

    void *mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    pthread_mutex_lock(&g_index_mutex);

    /* VRAM accounting: cuvs_cagra_build allocates BEFORE we free the old
     * index, so peak VRAM = existing + new. Ask for full 'needed' from
     * ensure_vram regardless of same-OID replacement. */
    size_t needed = estimate_vram_bytes(cmd->n_vecs, (int)cmd->dim);
    if (ensure_vram(needed) < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "VRAM exhausted", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    LOG_DEBUG("[handle_build] calling cuvs_cagra_build n_vecs=%lld dim=%u...\n",
            (long long)cmd->n_vecs, cmd->dim);
    CuvsCagraIndex new_handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim);
    if (!new_handle)
    {
        LOG_ERROR("[handle_build] cuvs_cagra_build returned NULL\n");
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_cagra_build failed");
        return;
    }
    LOG_DEBUG("[handle_build] cuvs_cagra_build OK\n");

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        cuvs_cagra_free(new_handle);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);
    munmap(mem, total);

    /* --- Disk persistence: tmp -> rename pattern --- */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    char idx_final[512],  idx_tmp[576];
    char tids_final[512], tids_tmp[576];
    index_file_path(idx_final,  sizeof(idx_final),  save_dir, cmd->db_oid, cmd->index_oid);
    tids_file_path(tids_final,  sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(idx_tmp,  sizeof(idx_tmp),  "%s.tmp", idx_final);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);

#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_TIDS_WRITE")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_TIDS_WRITE\n");
        goto persist_fail;
    }
#endif
    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
        goto persist_fail;
    LOG_DEBUG("[handle_build] tids.tmp written + fsynced\n");

#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_SERIALIZE")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_SERIALIZE\n");
        goto persist_fail;
    }
#endif
    LOG_DEBUG("[handle_build] cuvs_cagra_serialize(%s)...\n", idx_tmp);
    if (cuvs_cagra_serialize(new_handle, idx_tmp) != 0) {
        LOG_ERROR("[handle_build] cuvs_cagra_serialize FAILED (path=%s)\n", idx_tmp);
        goto persist_fail;
    }
    if (fsync_path(idx_tmp) != 0) {
        LOG_ERROR("[handle_build] fsync %s failed errno=%d; aborting commit\n", idx_tmp, errno);
        goto persist_fail;
    }
    LOG_DEBUG("[handle_build] cagra.tmp written + fsynced\n");

    /* Commit point: atomic renames. tids first, then cagra (the file
     * startup_load_indexes scans for). If second rename fails, log and
     * unlink whatever we renamed to leave consistent state. */
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_RENAME_TIDS")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_RENAME_TIDS\n");
        goto persist_fail;
    }
#endif
    if (rename(tids_tmp, tids_final) != 0) {
        LOG_ERROR("[handle_build] rename %s -> %s FAILED errno=%d (%s)\n",
                tids_tmp, tids_final, errno, strerror(errno));
        goto persist_fail;
    }
#ifdef CUVS_TEST_HOOKS
    if (cuvs_fault("CUVS_FAULT_RENAME_CAGRA")) {
        LOG_ERROR("[handle_build] fault injection: CUVS_FAULT_RENAME_CAGRA\n");
        unlink(tids_final);
        goto persist_fail;
    }
#endif
    if (rename(idx_tmp, idx_final) != 0) {
        LOG_ERROR("[handle_build] rename %s -> %s FAILED errno=%d (%s); "
                  "unlinking tids to avoid mismatch\n",
                idx_tmp, idx_final, errno, strerror(errno));
        unlink(tids_final);
        goto persist_fail;
    }
    /* fsync directory so the rename(s) are durable. */
    int dir_fd = open(save_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    LOG_DEBUG("[handle_build] disk commit OK\n");

    /* --- Swap into registry --- */
    IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
    if (existing) {
        cuvs_cagra_free(existing->handle);
        free(existing->tids);
        existing->dim         = cmd->dim;
        existing->metric      = cmd->metric;
        existing->n_vecs      = cmd->n_vecs;
        existing->handle      = new_handle;
        existing->tids        = new_tids;
        existing->vram_bytes  = needed;
        existing->last_search = time(NULL);
        existing->valid       = 1;
    } else {
        if (g_n_indexes >= MAX_INDEXES)
            evict_lru();
        if (g_n_indexes >= MAX_INDEXES) {
            /* No slot available — roll back disk state and fail. */
            LOG_ERROR("[handle_build] registry full; rolling back disk commit\n");
            unlink(idx_final);
            unlink(tids_final);
            cuvs_cagra_free(new_handle);
            free(new_tids);
            pthread_mutex_unlock(&g_index_mutex);
            send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "index registry full");
            return;
        }
        IndexEntry *e = &g_indexes[g_n_indexes++];
        e->db_oid      = cmd->db_oid;
        e->index_oid   = cmd->index_oid;
        e->dim         = cmd->dim;
        e->metric      = cmd->metric;
        e->n_vecs      = cmd->n_vecs;
        e->handle      = new_handle;
        e->tids        = new_tids;
        e->vram_bytes  = needed;
        e->last_search = time(NULL);
        e->valid       = 1;
    }

    pthread_mutex_unlock(&g_index_mutex);
    LOG_DEBUG("[handle_build] sending OK reply\n");

    LOG_INFO("pg_cuvs_server: built index %u/%u (%lld vecs, %zu MB VRAM)\n",
            cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader hdr_ok = {0};
    hdr_ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
    return;

persist_fail:
    /* Disk persistence failed before commit. Existing entry (if any) remains
     * intact in registry. Clean up new resources and tmp files. */
    unlink(idx_tmp);
    unlink(tids_tmp);
    cuvs_cagra_free(new_handle);
    free(new_tids);
    pthread_mutex_unlock(&g_index_mutex);
    send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "disk persistence failed");
}

/* ----------------------------------------------------------------
 * Per-connection thread
 * ---------------------------------------------------------------- */
static void *
connection_thread(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

    LOG_DEBUG("pg_cuvs_server: client connected (fd=%d)\n", client_fd);
    fflush(stderr);

    CuvsCmdFrame cmd;
    if (recv_all(client_fd, &cmd, sizeof(cmd)) < 0)
    {
        LOG_ERROR("pg_cuvs_server: recv_all CMD failed (errno=%d)\n", errno);
        fflush(stderr);
        close(client_fd);
        return NULL;
    }

    LOG_DEBUG("pg_cuvs_server: received cmd op=%u db=%u idx=%u dim=%u n_vecs=%lld shm=%s\n",
            cmd.op, cmd.db_oid, cmd.index_oid, cmd.dim, (long long)cmd.n_vecs, cmd.shm_key);
    fflush(stderr);

    switch (cmd.op)
    {
        case CUVS_OP_SEARCH:
            handle_search(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD:
            handle_build(client_fd, &cmd);
            break;
        default:
            send_error(client_fd, "unknown op");
            break;
    }

    close(client_fd);
    return NULL;
}

/* ----------------------------------------------------------------
 * Signal handling — async-signal-safe flag + deferred graceful shutdown
 *
 * sigterm_handler does the minimum legal in a signal handler: set the
 * volatile flag. All heavy work (mutex, CUDA serialize, file I/O) is
 * deferred to graceful_shutdown(), run from the main thread after the
 * accept loop is interrupted. This avoids deadlock (the handler could
 * fire while a connection thread holds g_index_mutex) and undefined
 * behavior from non-async-signal-safe calls.
 * ---------------------------------------------------------------- */
static void
sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* Graceful shutdown — main-thread context, safe to use mutex/CUDA/file I/O.
 * Locking g_index_mutex waits for any in-flight connection thread to finish
 * its critical section before we serialize. Does NOT call exit(); the caller
 * returns from main() normally. */
static void
graceful_shutdown(void)
{
    LOG_INFO("pg_cuvs_server: SIGTERM received, serializing indexes...\n");

    pthread_mutex_lock(&g_index_mutex);
    int saved = 0, failed = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (!g_indexes[i].valid)
            continue;
        if (save_index(&g_indexes[i]) == 0) {
            saved++;
        } else {
            failed++;
            LOG_ERROR("sigterm: save_index FAILED for %u/%u "
                      "(operator must REINDEX after restart)\n",
                    g_indexes[i].db_oid, g_indexes[i].index_oid);
        }
    }
    pthread_mutex_unlock(&g_index_mutex);
    LOG_INFO("sigterm: %d indexes saved, %d failed\n", saved, failed);

    if (g_server_fd >= 0)
    {
        close(g_server_fd);
        unlink(g_socket_path);
    }

    LOG_INFO("pg_cuvs_server: shutdown complete\n");
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    /* Parse arguments */
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket") == 0 && i+1 < argc)
            strncpy(g_socket_path, argv[++i], sizeof(g_socket_path) - 1);
        else if (strcmp(argv[i], "--index-dir") == 0 && i+1 < argc)
            strncpy(g_index_dir, argv[++i], sizeof(g_index_dir) - 1);
        else if (strcmp(argv[i], "--max-vram-mb") == 0 && i+1 < argc)
            g_max_vram_bytes = (size_t)atol(argv[++i]) * 1024 * 1024;
    }

    LOG_INFO("pg_cuvs_server: starting (socket=%s index-dir=%s)\n",
            g_socket_path, g_index_dir);

    /* Install SIGTERM/SIGINT via sigaction WITHOUT SA_RESTART so a signal
     * interrupts a blocked accept() (returns -1/EINTR) and we can break out
     * of the loop to run graceful_shutdown() in the main thread. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Load persisted indexes */
    startup_load_indexes();

    /* Create UDS socket */
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    unlink(g_socket_path);  /* remove stale socket if present */

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        return 1;
    }

    chmod(g_socket_path, 0660);

    if (listen(g_server_fd, 32) < 0)
    {
        perror("listen");
        return 1;
    }

    LOG_INFO("pg_cuvs_server: listening on %s\n", g_socket_path);

    /* Accept loop */
    while (!g_shutdown)
    {
        int client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            /* sigaction installed handlers WITHOUT SA_RESTART, so a SIGTERM/
             * SIGINT during accept() returns EINTR. We were signalled (or
             * g_shutdown was set between iterations) -> stop accepting. */
            if (errno == EINTR)
                break;
            if (!g_shutdown)
                perror("accept");
            break;
        }

        int *fd_ptr = malloc(sizeof(int));
        *fd_ptr = client_fd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, connection_thread, fd_ptr);
        pthread_attr_destroy(&attr);
    }

    /* Deferred graceful shutdown in the main thread (safe for mutex / CUDA
     * serialize / file I/O). In-flight detached connection threads holding
     * g_index_mutex are waited on by the lock inside graceful_shutdown. */
    graceful_shutdown();

    return 0;
}
