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
        .op        = CUVS_OP_SEARCH,
        .db_oid    = db_oid,
        .index_oid = index_oid,
        .k         = (uint32_t)k,
        .metric    = metric,
        .dim       = (uint32_t)dim,
        .n_vecs    = 0,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

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
 * Public API: cuvs_ipc_build
 * ---------------------------------------------------------------- */
int
cuvs_ipc_build(
    const char    *socket_path,
    uint32_t       db_oid,
    uint32_t       index_oid,
    const float   *vecs,
    const uint64_t *tids,
    int64_t        n_vecs,
    int            dim,
    uint32_t       metric,
    const char    *index_dir,
    uint32_t       table_oid,
    uint32_t       relfilenode,
    uint32_t       shard_count)
{
    char shm_key[64];
    int  shm_fd = -1;
    int  sock   = -1;
    int  rc     = CUVS_STATUS_ERROR;

    make_shm_key(shm_key, sizeof(shm_key));
    LOG_DEBUG("[cuvs_ipc_build] shm_key=%s socket=%s n_vecs=%lld dim=%d\n",
        shm_key, socket_path, (long long)n_vecs, dim);

    shm_fd = shm_write_build_payload(shm_key, vecs, tids, n_vecs, dim);
    if (shm_fd < 0) {
        LOG_ERROR("[cuvs_ipc_build] shm_write FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        goto cleanup;
    }

    sock = uds_connect_ex(socket_path, 600);  /* BUILD can take minutes */
    if (sock < 0) {
        LOG_ERROR("[cuvs_ipc_build] uds_connect FAILED errno=%d (%s)\n",
                errno, strerror(errno));
        rc = CUVS_STATUS_UNAVAILABLE;   /* daemon not reachable */
        goto cleanup;
    }

    CuvsCmdFrame cmd = {
        .op          = CUVS_OP_BUILD,
        .db_oid      = db_oid,
        .index_oid   = index_oid,
        .k           = 0,
        .metric      = metric,
        .dim         = (uint32_t)dim,
        .n_vecs      = n_vecs,
        .table_oid   = table_oid,
        .relfilenode = relfilenode,
        .shard_count = shard_count,
    };
    strncpy(cmd.shm_key, shm_key, sizeof(cmd.shm_key) - 1);

    /*
     * Pack index_dir into the reserved area of the command. We reuse
     * the shm_key field convention: append a second null-terminated
     * string immediately after CuvsCmdFrame in a wrapper struct.
     */
    if (send_all(sock, &cmd, sizeof(cmd)) < 0)
        goto cleanup;

    /* Send index_dir as a fixed 256-byte field. */
    char dir_buf[256] = {0};
    if (index_dir)
        strncpy(dir_buf, index_dir, sizeof(dir_buf) - 1);
    if (send_all(sock, dir_buf, sizeof(dir_buf)) < 0)
        goto cleanup;

    CuvsReplyHeader hdr;
    if (recv_all(sock, &hdr, sizeof(hdr)) < 0)
        goto cleanup;

    rc = (int)hdr.status;

cleanup:
    if (sock >= 0)
        close(sock);
    if (shm_fd >= 0)
        close(shm_fd);
    shm_unlink(shm_key);
    return rc;
}
