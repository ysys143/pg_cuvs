/*
 * cuvs_ipc.c — UDS client for pg_cuvs_server communication.
 *
 * Protocol: send CuvsCmdFrame over UDS, vector payload via shm_open,
 * receive CuvsReplyHeader + n×CuvsResult over UDS.
 *
 * Called from PostgreSQL backend process context. All paths that fail
 * to reach the daemon return an error code — pg_cuvs.c converts that
 * to a CPU fallback without crashing the backend.
 */

#include "cuvs_ipc.h"
#include "cuvs_util.h"   /* leveled logging macros (LOG_ERROR/WARN/INFO/DEBUG) */
#include "cuvs_build_corpus.h"   /* ADR-057: tiered corpus handoff + SCM_RIGHTS */

/* Hot-path IPC traces (LOG_DEBUG) are gated by PG_CUVS_DEBUG; enable via
 * -DPG_CUVS_DEBUG=1. Error paths (LOG_ERROR) always log unconditionally. */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>   /* 3S: interruptible reply wait */

/* Circuit breaker state machine moved to cuvs_util.c (structural commit). */

/* ----------------------------------------------------------------
 * Socket helpers
 * ---------------------------------------------------------------- */

/* Open a UDS connection. Returns fd on success, -1 on failure.
 * connect_timeout_ms: short for fail-fast on missing daemon
 * recv_timeout_sec: longer for receiving daemon response (build can be slow) */
static int
uds_connect_ex(const char *socket_path, int recv_timeout_sec)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    struct timeval tv = {.tv_sec = recv_timeout_sec, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int
uds_connect(const char *socket_path)
{
    return uds_connect_ex(socket_path, 30); /* default 30s for SEARCH */
}

static int
send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;   /* retry: a signal interrupted the write */
            return -1;
        }
        if (n == 0)
            return -1;      /* peer closed */
        sent += (size_t)n;
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
        if (n < 0)
        {
            if (errno == EINTR)
                continue;   /* retry: a signal interrupted the read. Large
                             * builds block here for minutes, so a stray
                             * backend signal must not abort the wait and
                             * spuriously fail a CREATE INDEX the daemon
                             * actually completed. */
            return -1;
        }
        if (n == 0)
            return -1;      /* peer closed */
        received += (size_t)n;
    }
    return 0;
}

/* 3S: query-cancel / statement_timeout hook. The backend registers a callback
 * that reports whether an interrupt is pending; the search reply-wait polls it. */
static int (*g_wait_cb)(void) = NULL;

void
cuvs_ipc_set_wait_callback(int (*cb)(void))
{
    g_wait_cb = cb;
}

/* Interruptible recv: like recv_all, but poll()s in short slices so a pending
 * PG cancel/statement_timeout (reported by g_wait_cb) can abort a long reply
 * wait instead of blocking the backend uninterruptibly. Returns 0 on full read,
 * -1 on error/EOF/overall-timeout, 1 if the wait callback asked to abort. */
static int
recv_all_interruptible(int fd, void *buf, size_t len, int timeout_sec)
{
    char  *p = buf;
    size_t got = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    while (got < len)
    {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 250 /* ms slice */);

        if (pr < 0)
        {
            if (errno == EINTR)
            {
                if (g_wait_cb && g_wait_cb())
                    return 1;   /* signal delivered a pending cancel */
                continue;
            }
            return -1;
        }
        if (pr == 0)            /* slice timed out: check cancel + overall deadline */
        {
            struct timespec now;
            if (g_wait_cb && g_wait_cb())
                return 1;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - start.tv_sec) >= timeout_sec)
                return -1;      /* daemon never replied within the budget */
            continue;
        }

        ssize_t n = read(fd, p + got, len - got);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;          /* peer closed */
        got += (size_t)n;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * shm helpers
 * ---------------------------------------------------------------- */

/* Build a unique shm key from pid + sequence counter. */
static void
make_shm_key(char *key, size_t keylen)
{
    static int seq = 0;
    snprintf(key, keylen, "/pg_cuvs_%d_%d", (int)getpid(), seq++);
}

/* Write vectors + TIDs into a new shm segment. Returns shm fd or -1. */
static int
shm_write_build_payload(const char   *shm_key,
                        const float  *vecs,
                        const uint64_t *tids,
                        int64_t       n_vecs,
                        int           dim)
{
    size_t vec_bytes = (size_t)n_vecs * dim * sizeof(float);
    size_t tid_bytes = (size_t)n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    int fd = shm_open(shm_key, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return -1;

    /* Override umask: ensure other users (daemon) can read the shm segment */
    fchmod(fd, 0666);

    if (ftruncate(fd, (off_t)total) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    memcpy(mem, vecs, vec_bytes);
    memcpy((char *)mem + vec_bytes, tids, tid_bytes);

    munmap(mem, total);
    return fd;
}

/* Phase 3M: write [uint32 Q][uint32 dim][Q*dim float32] into a new shm segment
 * for a batch search request. Returns shm fd or -1. */
static int
shm_write_query_batch(const char *shm_key, const float *queries,
                      uint32_t n_queries, int dim)
{
    size_t hdr_bytes = 2 * sizeof(uint32_t);
    size_t vec_bytes = (size_t)n_queries * (size_t)dim * sizeof(float);
    size_t total     = hdr_bytes + vec_bytes;

    int fd = shm_open(shm_key, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return -1;
    fchmod(fd, 0666);
    if (ftruncate(fd, (off_t)total) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }
    ((uint32_t *)mem)[0] = n_queries;
    ((uint32_t *)mem)[1] = (uint32_t)dim;
    memcpy((char *)mem + hdr_bytes, queries, vec_bytes);
    munmap(mem, total);
    return fd;
}

/* Write query vector into a new shm segment. Returns shm fd or -1. */
static int
shm_write_query(const char *shm_key, const float *query_vec, int dim)
{
    size_t vec_bytes = (size_t)dim * sizeof(float);

    int fd = shm_open(shm_key, O_CREAT | O_RDWR, 0666);
    if (fd < 0)
        return -1;

    /* Override umask: ensure other users (daemon) can read the shm segment */
    fchmod(fd, 0666);

    if (ftruncate(fd, (off_t)vec_bytes) < 0)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    void *mem = mmap(NULL, vec_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key);
        close(fd);
        return -1;
    }

    memcpy(mem, query_vec, vec_bytes);
    munmap(mem, vec_bytes);
    return fd;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *query_vec,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch,
    int           parallel_fanout,
    uint32_t      use_cpu_hnsw,
    uint32_t      search_mode,
    uint32_t      bf_precision,
    uint32_t      bf_batch_wait_us,
    uint64_t     *tids_out,
    float        *dist_out,
    int          *n_out,
    uint32_t     *latency_us_out,
    int          *delta_merged_out)
{
    char shm_key[64];
    int  shm_fd = -1;
    int  sock   = -1;
    int  rc     = CUVS_STATUS_ERROR;

    if (latency_us_out)   *latency_us_out = 0;
    if (n_out)            *n_out = 0;
    if (delta_merged_out) *delta_merged_out = 0;

    make_shm_key(shm_key, sizeof(shm_key));

    shm_fd = shm_write_query(shm_key, query_vec, dim);
    if (shm_fd < 0)
        goto cleanup;       /* rc stays CUVS_STATUS_ERROR — our side issue */

    sock = uds_connect(socket_path);
    if (sock < 0) {
        rc = CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op              = CUVS_OP_SEARCH,
        .db_oid          = db_oid,
        .index_oid       = index_oid,
        .k               = (uint32_t)k,
        .metric          = metric,
        .dim             = (uint32_t)dim,
        .n_vecs          = 0,
        .shard_overfetch = shard_overfetch,
        .parallel_fanout = (uint32_t)(parallel_fanout ? 1 : 0),
        .use_cpu_hnsw    = use_cpu_hnsw,
        .search_mode     = search_mode,
        .bf_precision    = bf_precision,
        .bf_batch_wait_us = bf_batch_wait_us,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    /* 3S: wait for the reply header interruptibly so a query cancel /
     * statement_timeout aborts a long GPU search instead of blocking the backend.
     * On abort we close the socket (cleanup) -> the daemon's reply send fails and
     * its per-connection thread cleans up; the backend raises the interrupt. */
    CuvsReplyHeader hdr;
    {
        int wr = recv_all_interruptible(sock, &hdr, sizeof(hdr), 30 /* sec budget */);
        if (wr == 1) { rc = CUVS_STATUS_CANCELED; goto cleanup; }
        if (wr < 0)  goto cleanup;
    }

    rc = (int)hdr.status;
    if (latency_us_out)
        *latency_us_out = hdr.latency_us;
    if (delta_merged_out)
        *delta_merged_out = (hdr.delta_merged != 0);

    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n = (int)hdr.n_results;
        /* Defensive: never write more than the caller's k-sized buffers. */
        int n_write = (n > k) ? k : n;
        CuvsResult *results = malloc(n * sizeof(CuvsResult));
        if (!results)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }

        if (recv_all(sock, results, n * sizeof(CuvsResult)) < 0)
        {
            free(results);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }

        for (int i = 0; i < n_write; i++)
        {
            tids_out[i] = results[i].tid;
            dist_out[i] = results[i].distance;
        }
        *n_out = n_write;
        free(results);
    }
    else
    {
        *n_out = 0;
    }

cleanup:
    if (sock >= 0)
        close(sock);
    if (shm_fd >= 0)
        close(shm_fd);
    shm_unlink(shm_key);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_search_batch (Phase 3M)
 *
 * Q queries in one request shm ([Q][dim][Q*dim f32]); the daemon runs one
 * batched GPU dispatch and returns Q*K results via a daemon-allocated reply
 * shm ([Q][K][tids Q*K u64][dists Q*K f32]), with the reply shm key in
 * hdr.error[] and K in hdr.delta_merged (reusing the export_adjacency reply
 * mechanism). The backend owns both shm segments and unlinks them here.
 * tids_out/dist_out must hold Q*k; the daemon clamps K = min(k, corpus) and
 * *k_out reports the actual per-query stride.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_search_batch(
    const char   *socket_path,
    uint32_t      db_oid,
    uint32_t      index_oid,
    const float  *queries,
    uint32_t      n_queries,
    int           dim,
    int           k,
    uint32_t      metric,
    uint32_t      shard_overfetch,
    int           parallel_fanout,
    uint32_t      search_mode,
    uint32_t      bf_precision,
    uint64_t     *tids_out,
    float        *dist_out,
    uint32_t     *k_out,
    uint32_t     *latency_us_out)
{
    char   req_key[64];
    int    shm_fd   = -1;
    int    sock     = -1;
    int    reply_fd = -1;
    void  *rmem     = MAP_FAILED;
    size_t rtotal   = 0;
    int    rc       = CUVS_STATUS_ERROR;
    char   reply_key[128];

    reply_key[0] = '\0';
    if (k_out)          *k_out = 0;
    if (latency_us_out) *latency_us_out = 0;

    make_shm_key(req_key, sizeof(req_key));
    shm_fd = shm_write_query_batch(req_key, queries, n_queries, dim);
    if (shm_fd < 0)
        goto cleanup;       /* our-side error -> CUVS_STATUS_ERROR */

    /* A large batch can take longer than a single search; use a generous
     * timeout (mirrors export_adjacency). */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
    {
        rc = CUVS_STATUS_UNAVAILABLE;
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op              = CUVS_OP_SEARCH_BATCH,
        .db_oid          = db_oid,
        .index_oid       = index_oid,
        .k               = (uint32_t)k,
        .metric          = metric,
        .dim             = (uint32_t)dim,
        .n_vecs          = (int64_t)n_queries,   /* SEARCH_BATCH: Q query count */
        .shard_overfetch = shard_overfetch,
        .parallel_fanout = (uint32_t)(parallel_fanout ? 1 : 0),
        .search_mode     = search_mode,
        .bf_precision    = bf_precision,
    };
    strncpy(cmd.shm_key, req_key, sizeof(cmd.shm_key) - 1);
    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;
    rc = (int)hdr.status;
    if (latency_us_out)
        *latency_us_out = hdr.latency_us;
    if (hdr.status != CUVS_STATUS_OK)
        goto cleanup;

    /* Daemon packs: n_results=Q, delta_merged=K (per-query result count),
     * reply shm key in error[]. */
    uint32_t Q = hdr.n_results;
    uint32_t K = hdr.delta_merged;
    strncpy(reply_key, hdr.error, sizeof(reply_key) - 1);
    reply_key[sizeof(reply_key) - 1] = '\0';
    if (Q != n_queries || K == 0 || (int)K > k || reply_key[0] == '\0')
    {
        rc = CUVS_STATUS_ERROR;
        goto cleanup;
    }

    /* reply shm layout: [uint32 Q][uint32 K][tids Q*K u64][dists Q*K f32] */
    size_t hdr_bytes  = 2 * sizeof(uint32_t);
    size_t tids_bytes = (size_t)Q * (size_t)K * sizeof(uint64_t);
    size_t dist_bytes = (size_t)Q * (size_t)K * sizeof(float);
    rtotal = hdr_bytes + tids_bytes + dist_bytes;

    reply_fd = shm_open(reply_key, O_RDONLY, 0666);
    if (reply_fd < 0)
        goto cleanup;
    rmem = mmap(NULL, rtotal, PROT_READ, MAP_SHARED, reply_fd, 0);
    if (rmem == MAP_FAILED)
        goto cleanup;

    {
        const char *p = (const char *)rmem + hdr_bytes;
        memcpy(tids_out, p,              tids_bytes);
        memcpy(dist_out, p + tids_bytes, dist_bytes);
    }
    if (k_out)
        *k_out = K;
    rc = CUVS_STATUS_OK;

cleanup:
    if (rmem != MAP_FAILED) munmap(rmem, rtotal);
    if (reply_fd >= 0)      close(reply_fd);
    if (reply_key[0])       shm_unlink(reply_key);
    if (shm_fd >= 0)        close(shm_fd);
    shm_unlink(req_key);
    if (sock >= 0)          close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_stats
 * ---------------------------------------------------------------- */
int
cuvs_ipc_stats(
    const char     *socket_path,
    uint32_t        db_oid,
    uint32_t        index_oid,
    CuvsIndexStats *out,
    int             max,
    int            *n_out)
{
    int sock = -1;
    int rc   = CUVS_STATUS_ERROR;

    if (n_out) *n_out = 0;

    sock = uds_connect(socket_path);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down -> caller treats as empty */

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_STATUS,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    rc = (int)hdr.status;
    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n = (int)hdr.n_results;
        int n_copy = (n > max) ? max : n;
        CuvsIndexStats *buf = malloc((size_t)n * sizeof(CuvsIndexStats));
        if (!buf)
        {
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        if (recv_all(sock, buf, (size_t)n * sizeof(CuvsIndexStats)) < 0)
        {
            free(buf);
            rc = CUVS_STATUS_ERROR;
            goto cleanup;
        }
        for (int i = 0; i < n_copy; i++)
            out[i] = buf[i];
        if (n_out) *n_out = n_copy;
        free(buf);
    }

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_cache_stats
 * ---------------------------------------------------------------- */
int
cuvs_ipc_cache_stats(const char *socket_path, CuvsCacheStats *out,
                     int max, int *n_out)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    *n_out = 0;
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    memset(&cmd, 0, sizeof(cmd));
    cmd.op = CUVS_OP_CACHE_STATS;

    rc = CUVS_STATUS_ERROR;
    if (send_all(sock, &cmd, sizeof(cmd)) == 0
        && recv_all(sock, &hdr, sizeof(hdr)) == 0)
    {
        rc = (int) hdr.status;
        if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
        {
            int n = (int)hdr.n_results;
            if (n > max) n = max;
            if (recv_all(sock, out, (size_t)n * sizeof(*out)) < 0)
                rc = CUVS_STATUS_ERROR;
            else
                *n_out = n;
            /* drain any extra rows beyond max */
            for (int i = n; i < (int)hdr.n_results; i++)
            {
                CuvsCacheStats discard;
                recv_all(sock, &discard, sizeof(discard));
            }
        }
    }

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_shard_stats (Phase 3F)
 * ---------------------------------------------------------------- */
int
cuvs_ipc_shard_stats(const char *socket_path, uint32_t db_oid,
                     uint32_t index_oid, CuvsShardStats *out,
                     int max, int *n_out)
{
    int sock = -1;
    int rc   = CUVS_STATUS_ERROR;

    if (n_out) *n_out = 0;

    sock = uds_connect(socket_path);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down -> caller treats as empty */

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_SHARD_STATS,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    rc = (int)hdr.status;
    if (hdr.status == CUVS_STATUS_OK && hdr.n_results > 0)
    {
        int n      = (int)hdr.n_results;
        int n_copy = (n > max) ? max : n;
        for (int i = 0; i < n; i++)
        {
            CuvsShardStats row;
            if (recv_all(sock, &row, sizeof(row)) < 0)
            {
                rc = CUVS_STATUS_ERROR;
                if (n_out) *n_out = 0;
                goto cleanup;
            }
            if (i < n_copy)
                out[i] = row;       /* extra rows beyond max are drained */
        }
        if (n_out) *n_out = n_copy;
    }

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_mark_stale
 * ---------------------------------------------------------------- */
int
cuvs_ipc_mark_stale(const char *socket_path, uint32_t db_oid, uint32_t index_oid)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down — do not fail the write */

    memset(&cmd, 0, sizeof(cmd));
    cmd.op        = CUVS_OP_MARK_STALE;
    cmd.db_oid    = db_oid;
    cmd.index_oid = index_oid;

    rc = CUVS_STATUS_ERROR;
    if (send_all(sock, &cmd, sizeof(cmd)) == 0
        && recv_all(sock, &hdr, sizeof(hdr)) == 0)
        rc = (int) hdr.status;

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_drop (Phase 3G.1)
 * ---------------------------------------------------------------- */
int
cuvs_ipc_drop(const char *socket_path, uint32_t db_oid, uint32_t index_oid)
{
    int sock = uds_connect(socket_path);
    int rc;
    CuvsCmdFrame cmd;
    CuvsReplyHeader hdr;

    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;   /* daemon down — caller must not fail the DROP */

    memset(&cmd, 0, sizeof(cmd));
    cmd.op        = CUVS_OP_DROP_INDEX;
    cmd.db_oid    = db_oid;
    cmd.index_oid = index_oid;

    rc = CUVS_STATUS_ERROR;
    if (send_all(sock, &cmd, sizeof(cmd)) == 0
        && recv_all(sock, &hdr, sizeof(hdr)) == 0)
        rc = (int) hdr.status;

    close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const struct CuvsBuildCorpus *corpus,
    const float   *heap_vecs,
    const uint64_t *heap_tids,
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count,
    uint32_t       use_cpu_hnsw,
    uint32_t       graph_degree,
    uint32_t       intermediate_graph_degree,
    uint32_t       build_algo)
{
    char shm_key[64] = "";
    int  legacy_shm_fd = -1;   /* CORPUS_HEAP only: created + unlinked here */
    int  pass_fd = -1;         /* CORPUS_MEMFD only: passed via SCM_RIGHTS */
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd;
    char dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    /* Stage the payload by tier. */
    if (corpus->kind == CORPUS_MEMFD)
    {
        pass_fd = corpus->fd;                 /* anonymous; daemon mmaps the fd */
    }
    else if (corpus->kind == CORPUS_SHM)
    {
        strncpy(shm_key, corpus->shm_name, sizeof(shm_key) - 1);  /* daemon shm_open by name */
    }
    else  /* CORPUS_HEAP: copy into a fresh named shm, unlinked at cleanup */
    {
        make_shm_key(shm_key, sizeof(shm_key));
        legacy_shm_fd = shm_write_build_payload(shm_key, heap_vecs, heap_tids, n_vecs, dim);
        if (legacy_shm_fd < 0) {
            LOG_ERROR("[cuvs_ipc_build] shm_write FAILED errno=%d (%s)\n",
                    errno, strerror(errno));
            goto cleanup;
        }
    }
    LOG_DEBUG("[cuvs_ipc_build] tier=%d shm_key=%s pass_fd=%d socket=%s n_vecs=%lld dim=%d\n",
        (int)corpus->kind, shm_key, pass_fd, socket_path, (long long)n_vecs, dim);

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
        goto cleanup;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op           = CUVS_OP_BUILD;
    cmd.db_oid       = db_oid;
    cmd.index_oid    = index_oid;
    cmd.metric       = metric;
    cmd.dim          = (uint32_t)dim;
    cmd.n_vecs       = n_vecs;
    cmd.table_oid    = table_oid;
    cmd.relfilenode  = relfilenode;
    cmd.shard_count  = shard_count;
    cmd.use_cpu_hnsw = use_cpu_hnsw;  /* Phase 3I-1: 1 = serialize .hnsw sidecar */
    cmd.graph_degree              = graph_degree;              /* 3R; 0 = cuVS default */
    cmd.intermediate_graph_degree = intermediate_graph_degree; /* 3R; 0 = cuVS default */
    cmd.build_algo                = build_algo;                /* 3R; 0 = AUTO */
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);  /* "" for memfd tier */

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    /* index_dir (fixed 256B) carries the SCM_RIGHTS fd for the memfd tier; for
     * shm/heap pass_fd is -1 and only the bytes are sent. Daemon recvmsg's both. */
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, pass_fd, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)
        close(sock);
    /* Only the legacy heap-tier shm is owned here; the memfd/shm corpus lifetime
     * belongs to the caller (released via cuvs_corpus_close). */
    if (legacy_shm_fd >= 0)
    {
        close(legacy_shm_fd);
        shm_unlink(shm_key);
    }
    return rc;
}

/* ----------------------------------------------------------------
 * Public API: cuvs_ipc_build_multi (ADR-059)
 *
 * Like cuvs_ipc_build but references N worker named-shm partials instead of one
 * merged corpus. Sends the cmd frame (n_partials > 0), the index_dir frame (no
 * SCM_RIGHTS fd), then the CuvsPartialDesc list. The daemon mmaps each partial
 * and streams it straight to the GPU — no host merge. The caller owns the
 * partials and unlinks them after this returns.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build_multi(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const CuvsPartialDesc *partials,
    uint32_t       n_partials,
    int64_t        total,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count,
    uint32_t       use_cpu_hnsw,
    uint32_t       graph_degree,
    uint32_t       intermediate_graph_degree,
    uint32_t       build_algo)
{
    int  sock = -1;
    int  rc   = CUVS_STATUS_ERROR;
    CuvsCmdFrame cmd;
    char dir_buf[256] = {0};
    CuvsReplyHeader hdr;

    if (n_partials == 0 || partials == NULL)
    {
        LOG_ERROR("[cuvs_ipc_build_multi] no partials (n_partials=%u)\n", n_partials);
        return CUVS_STATUS_ERROR;
    }

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build_multi] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        return CUVS_STATUS_UNAVAILABLE;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.op           = CUVS_OP_BUILD;
    cmd.db_oid       = db_oid;
    cmd.index_oid    = index_oid;
    cmd.metric       = metric;
    cmd.dim          = (uint32_t)dim;
    cmd.n_vecs       = total;
    cmd.table_oid    = table_oid;
    cmd.relfilenode  = relfilenode;
    cmd.shard_count  = shard_count;
    cmd.use_cpu_hnsw = use_cpu_hnsw;
    cmd.n_partials   = n_partials;   /* daemon takes the multi-partial path */
    cmd.graph_degree              = graph_degree;              /* 3R */
    cmd.intermediate_graph_degree = intermediate_graph_degree; /* 3R */
    cmd.build_algo                = build_algo;                /* 3R */
    /* cmd.shm_key stays "" — no single corpus segment. */

    LOG_DEBUG("[cuvs_ipc_build_multi] n_partials=%u total=%lld dim=%d socket=%s\n",
        n_partials, (long long)total, dim, socket_path);

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    /* index_dir frame: no SCM_RIGHTS fd for the multi-partial path. */
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (cuvs_fd_send(sock, -1, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    /* Partial descriptor list (fixed-size records). */
    if (send_all(sock, partials, (size_t)n_partials * sizeof(CuvsPartialDesc)) < 0)
        goto cleanup;

    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)
        close(sock);
    return rc;   /* caller owns the partials' lifetime */
}

/* ----------------------------------------------------------------
 * Phase 3J: export CAGRA adjacency list + corpus vectors from daemon.
 *
 * Sends CUVS_OP_EXPORT_ADJACENCY; daemon packs adj+vecs+tids into a
 * shared memory segment and returns its key via the reply header.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_export_adjacency(
    const char  *socket_path,
    uint32_t     db_oid,
    uint32_t     index_oid,
    uint32_t   **adj_out,
    float      **vecs_out,
    uint64_t   **tids_out,
    size_t      *n_vecs_out,
    int         *graph_degree_out,
    int         *dim_out,
    uint32_t    *metric_out)
{
    int  sock = -1;
    int  shm_fd = -1;
    int  rc = CUVS_STATUS_ERROR;
    void *mem = MAP_FAILED;

    /* Long timeout: GPU→CPU copy for large indexes takes 10-30s. */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_EXPORT_ADJACENCY,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };
    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    if (hdr.status != CUVS_STATUS_OK) {
        rc = (int)hdr.status;
        goto cleanup;
    }

    /* Daemon packs: n_vecs=n_results, graph_degree=latency_us, dim=delta_merged,
     * shm_key in the error[] field (null-terminated, ≤64 chars). */
    size_t  n_vecs       = (size_t)hdr.n_results;
    int     graph_degree = (int)hdr.latency_us;
    int     dim          = (int)hdr.delta_merged;
    char    reply_shm_key[128];
    strncpy(reply_shm_key, hdr.error, sizeof(reply_shm_key) - 1);
    reply_shm_key[sizeof(reply_shm_key) - 1] = '\0';

    if (n_vecs == 0 || graph_degree <= 0 || dim <= 0 || reply_shm_key[0] == '\0') {
        LOG_ERROR("[export_adjacency] bad reply n=%zu gd=%d dim=%d key='%s'\n",
                  n_vecs, graph_degree, dim, reply_shm_key);
        goto cleanup;
    }

    /* shm layout: [uint32_t n_vecs][uint32_t graph_degree][uint32_t dim][uint32_t pad]
     *             [adj: n*gd*4][vecs: n*dim*4][tids: n*8] */
    size_t adj_bytes  = n_vecs * (size_t)graph_degree * sizeof(uint32_t);
    size_t vecs_bytes = n_vecs * (size_t)dim * sizeof(float);
    size_t tids_bytes = n_vecs * sizeof(uint64_t);
    size_t hdr_bytes  = 4 * sizeof(uint32_t);  /* n_vecs, gd, dim, pad */
    size_t total      = hdr_bytes + adj_bytes + vecs_bytes + tids_bytes;

    shm_fd = shm_open(reply_shm_key, O_RDONLY, 0666);
    if (shm_fd < 0) {
        LOG_ERROR("[export_adjacency] shm_open(%s) failed errno=%d\n",
                  reply_shm_key, errno);
        goto cleanup;
    }

    mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (mem == MAP_FAILED) {
        LOG_ERROR("[export_adjacency] mmap failed errno=%d\n", errno);
        goto cleanup;
    }

    /* Read metric from header slot [3] before the data section. */
    uint32_t src_metric = ((const uint32_t *)mem)[3];

    /* Copy out to caller-owned buffers. */
    const char *p = (const char *)mem + hdr_bytes;
    uint32_t *adj  = (uint32_t *)malloc(adj_bytes);
    float    *vecs = (float    *)malloc(vecs_bytes);
    uint64_t *tids = (uint64_t *)malloc(tids_bytes);
    if (!adj || !vecs || !tids) {
        free(adj); free(vecs); free(tids);
        LOG_ERROR("[export_adjacency] malloc failed\n");
        goto cleanup;
    }
    memcpy(adj,  p,                            adj_bytes);
    memcpy(vecs, p + adj_bytes,                vecs_bytes);
    memcpy(tids, p + adj_bytes + vecs_bytes,   tids_bytes);

    *adj_out          = adj;
    *vecs_out         = vecs;
    *tids_out         = tids;
    *n_vecs_out       = n_vecs;
    *graph_degree_out = graph_degree;
    *dim_out          = dim;
    if (metric_out) *metric_out = src_metric;
    rc = CUVS_STATUS_OK;

cleanup:
    if (mem != MAP_FAILED)  munmap(mem, total);
    if (shm_fd >= 0)        close(shm_fd);
    if (reply_shm_key[0])   shm_unlink(reply_shm_key);
    if (sock >= 0)          close(sock);
    return rc;
}

/* ----------------------------------------------------------------
 * Phase 3J: request daemon to run from_cagra() on a loaded CAGRA index
 * and serialize the resulting multi-level HNSW to /dev/shm (no disk I/O).
 * The path is returned in shm_path_out; caller must unlink after reading.
 * ---------------------------------------------------------------- */
int
cuvs_ipc_export_hnsw_shm(
    const char *socket_path,
    uint32_t    db_oid,
    uint32_t    index_oid,
    char       *shm_path_out,
    size_t      shm_path_len)
{
    int sock = -1;
    int rc   = CUVS_STATUS_ERROR;

    /* from_cagra() can take 30s+ for large indexes */
    sock = uds_connect_ex(socket_path, 120);
    if (sock < 0)
        return CUVS_STATUS_UNAVAILABLE;

    CuvsCmdFrame cmd = {
        .op        = CUVS_OP_EXPORT_HNSW_SHM,
        .db_oid    = db_oid,
        .index_oid = index_oid,
    };
    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    if (hdr.status != CUVS_STATUS_OK) {
        rc = (int)hdr.status;
        goto cleanup;
    }

    /* Daemon puts the /dev/shm path in hdr.error[] */
    if (hdr.error[0] == '\0') {
        LOG_ERROR("[export_hnsw_shm] empty path in reply\n");
        goto cleanup;
    }
    strncpy(shm_path_out, hdr.error, shm_path_len - 1);
    shm_path_out[shm_path_len - 1] = '\0';
    rc = CUVS_STATUS_OK;

cleanup:
    if (sock >= 0) close(sock);
    return rc;
}
