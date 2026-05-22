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
#include "cuvs_wrapper.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static int
save_index(IndexEntry *e)
{
    char idx_path[512], tids_path[512];
    index_file_path(idx_path, sizeof(idx_path), g_index_dir, e->db_oid, e->index_oid);
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);

    if (cuvs_cagra_serialize(e->handle, idx_path) != 0)
    {
        fprintf(stderr, "pg_cuvs_server: failed to serialize index %u/%u\n",
                e->db_oid, e->index_oid);
        return -1;
    }

    FILE *f = fopen(tids_path, "wb");
    if (!f)
        return -1;

    /* Header: n_vecs + dim + metric */
    fwrite(&e->n_vecs,  sizeof(int64_t),  1, f);
    fwrite(&e->dim,     sizeof(uint32_t), 1, f);
    fwrite(&e->metric,  sizeof(uint32_t), 1, f);
    fwrite(e->tids, sizeof(uint64_t), (size_t)e->n_vecs, f);
    fclose(f);

    fprintf(stderr, "pg_cuvs_server: saved index %u/%u (%lld vecs)\n",
            e->db_oid, e->index_oid, e->n_vecs);
    return 0;
}

static int
load_index(uint32_t db_oid, uint32_t index_oid)
{
    char idx_path[512], tids_path[512];
    index_file_path(idx_path, sizeof(idx_path), g_index_dir, db_oid, index_oid);
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, db_oid, index_oid);

    /* Read TIDs file first to get n_vecs and dim */
    FILE *f = fopen(tids_path, "rb");
    if (!f)
        return -1;

    int64_t  n_vecs;
    uint32_t dim, metric;
    fread(&n_vecs,  sizeof(int64_t),  1, f);
    fread(&dim,     sizeof(uint32_t), 1, f);
    fread(&metric,  sizeof(uint32_t), 1, f);

    uint64_t *tids = malloc((size_t)n_vecs * sizeof(uint64_t));
    if (!tids)
    {
        fclose(f);
        return -1;
    }
    fread(tids, sizeof(uint64_t), (size_t)n_vecs, f);
    fclose(f);

    /* VRAM preflight check */
    size_t needed = estimate_vram_bytes(n_vecs, (int)dim);
    size_t free_vram = gpu_free_vram_bytes();
    if (g_max_vram_bytes > 0 && total_vram_used() + needed > g_max_vram_bytes)
    {
        fprintf(stderr, "pg_cuvs_server: VRAM budget exceeded loading %u/%u, skip\n",
                db_oid, index_oid);
        free(tids);
        return -1;
    }
    if (needed > free_vram)
    {
        fprintf(stderr, "pg_cuvs_server: insufficient VRAM loading %u/%u, skip\n",
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

    fprintf(stderr, "pg_cuvs_server: loaded index %u/%u (%lld vecs, %zu MB VRAM)\n",
            db_oid, index_oid, n_vecs, needed / (1024*1024));
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
        /* Parse filenames of the form "<db_oid>_<index_oid>.cagra" */
        if (sscanf(ent->d_name, "%u_%u.cagra", &db_oid, &index_oid) == 2)
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

/* Evict the LRU index to free VRAM. Returns bytes freed, or 0. */
static size_t
evict_lru(void)
{
    IndexEntry *e = find_lru_index();
    if (!e)
        return 0;

    save_index(e);
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
send_error(int client_fd, const char *msg)
{
    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_ERROR;
    hdr.n_results = 0;
    strncpy(hdr.error, msg, sizeof(hdr.error) - 1);
    send_all(client_fd, &hdr, sizeof(hdr));
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

    int ret = cuvs_cagra_search(e->handle, query, (int)cmd->dim, k, raw);
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
 * Handle BUILD command
 * ---------------------------------------------------------------- */
static void
handle_build(int client_fd, const CuvsCmdFrame *cmd)
{
    /* Read index_dir from socket */
    char index_dir[256] = {0};
    if (recv_all(client_fd, index_dir, sizeof(index_dir)) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (shm_fd < 0)
    {
        send_error(client_fd, "shm_open failed");
        return;
    }

    void *mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs = (const float *)mem;
    const uint64_t *tids = (const uint64_t *)((const char *)mem + vec_bytes);

    pthread_mutex_lock(&g_index_mutex);

    /* Drop existing index for this OID if present */
    IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
    if (existing)
    {
        cuvs_cagra_free(existing->handle);
        free(existing->tids);
        int idx = (int)(existing - g_indexes);
        for (int i = idx; i < g_n_indexes - 1; i++)
            g_indexes[i] = g_indexes[i+1];
        g_n_indexes--;
    }

    /* VRAM check */
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

    CuvsCagraIndex handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim);
    if (!handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error(client_fd, "cuvs_cagra_build failed");
        return;
    }

    /* Store TID mapping */
    uint64_t *my_tids = malloc(tid_bytes);
    if (!my_tids)
    {
        cuvs_cagra_free(handle);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error(client_fd, "malloc tids failed");
        return;
    }
    memcpy(my_tids, tids, tid_bytes);
    munmap(mem, total);

    if (g_n_indexes >= MAX_INDEXES)
    {
        evict_lru();
    }

    IndexEntry *e = &g_indexes[g_n_indexes++];
    e->db_oid      = cmd->db_oid;
    e->index_oid   = cmd->index_oid;
    e->dim         = cmd->dim;
    e->metric      = cmd->metric;
    e->n_vecs      = cmd->n_vecs;
    e->handle      = handle;
    e->tids        = my_tids;
    e->vram_bytes  = needed;
    e->last_search = time(NULL);
    e->valid       = 1;

    /* Persist to disk */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    char idx_path[512], tids_path[512];
    index_file_path(idx_path, sizeof(idx_path), save_dir, e->db_oid, e->index_oid);
    tids_file_path(tids_path, sizeof(tids_path), save_dir, e->db_oid, e->index_oid);

    cuvs_cagra_serialize(handle, idx_path);

    FILE *f = fopen(tids_path, "wb");
    if (f)
    {
        fwrite(&e->n_vecs, sizeof(int64_t),  1, f);
        fwrite(&e->dim,    sizeof(uint32_t), 1, f);
        fwrite(&e->metric, sizeof(uint32_t), 1, f);
        fwrite(e->tids, sizeof(uint64_t), (size_t)e->n_vecs, f);
        fclose(f);
    }

    pthread_mutex_unlock(&g_index_mutex);

    fprintf(stderr, "pg_cuvs_server: built index %u/%u (%lld vecs, %zu MB VRAM)\n",
            cmd->db_oid, cmd->index_oid, cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* ----------------------------------------------------------------
 * Per-connection thread
 * ---------------------------------------------------------------- */
static void *
connection_thread(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);

    CuvsCmdFrame cmd;
    if (recv_all(client_fd, &cmd, sizeof(cmd)) < 0)
    {
        close(client_fd);
        return NULL;
    }

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
 * Signal handler — serialize indexes on SIGTERM
 * ---------------------------------------------------------------- */
static void
sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown = 1;

    fprintf(stderr, "pg_cuvs_server: SIGTERM received, serializing indexes...\n");

    pthread_mutex_lock(&g_index_mutex);
    for (int i = 0; i < g_n_indexes; i++)
    {
        if (g_indexes[i].valid)
            save_index(&g_indexes[i]);
    }
    pthread_mutex_unlock(&g_index_mutex);

    if (g_server_fd >= 0)
    {
        close(g_server_fd);
        unlink(g_socket_path);
    }

    fprintf(stderr, "pg_cuvs_server: shutdown complete\n");
    exit(0);
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

    fprintf(stderr, "pg_cuvs_server: starting (socket=%s index-dir=%s)\n",
            g_socket_path, g_index_dir);

    /* SIGTERM handler */
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT,  sigterm_handler);

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

    fprintf(stderr, "pg_cuvs_server: listening on %s\n", g_socket_path);

    /* Accept loop */
    while (!g_shutdown)
    {
        int client_fd = accept(g_server_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;
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

    return 0;
}
