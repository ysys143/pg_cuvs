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
#include "cuvs_objstore.h"
#include <curl/curl.h>

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
    time_t          last_search;  /* for LRU eviction; also stats last_search_at */
    int             valid;
    int             stale;        /* 1 if heap writes happened since build (REINDEX needed) */
    time_t          stale_since;  /* when first marked stale; 0 if fresh */

    /* Phase 3B delta cache: resident GPU brute-force index over the pending
     * `.delta` vectors. Lazily (re)built in handle_search when the .delta file
     * appears/changes; freed on eviction. delta_idx==NULL means no GPU delta. */
    CuvsBfIndex     delta_idx;        /* resident brute-force index; NULL if none */
    uint64_t       *delta_tids;       /* host: delta item_id -> heap TID [n_delta] */
    int64_t         n_delta;          /* delta vectors in the cache */
    uint32_t        delta_generation; /* base .tids body_crc32 the cache was built on */
    int64_t         delta_mtime;      /* .delta st_mtime when the cache was built */
    size_t          delta_vram_bytes; /* estimated VRAM held by the delta cache */

    /* Search stats (pg_stat_gpu_search). Reset on (re)build/load — they
     * describe the currently resident index instance, not a persisted total. */
    uint64_t        search_count;     /* CUVS_STATUS_OK searches */
    uint64_t        error_count;      /* attributable non-OK searches */
    uint64_t        total_latency_us; /* sum of OK latencies (for avg) */
    uint32_t        lat_buckets[CUVS_LAT_BUCKETS];
    uint32_t        last_status;       /* CUVS_STATUS_* of most recent search */
    uint32_t        last_requested_k;  /* top-k of last OK search (reflects cuvs.k) */
    uint32_t        last_returned_k;   /* rows last OK search returned */
    char            last_error[128];
} IndexEntry;

/* Zero just the stat counters of a (re)initialized entry. The slot may carry
 * stale stats from a previously evicted index, and a rebuild starts fresh. */
static void
reset_entry_stats(IndexEntry *e)
{
    e->search_count     = 0;
    e->error_count      = 0;
    e->total_latency_us = 0;
    memset(e->lat_buckets, 0, sizeof(e->lat_buckets));
    e->last_status      = 0;
    e->last_requested_k = 0;
    e->last_returned_k  = 0;
    e->last_error[0]    = '\0';
}

/* Record a completed search on an entry. Caller MUST hold g_index_mutex. */
static void
record_search_stat(IndexEntry *e, uint32_t status, uint32_t latency_us,
                   const char *err)
{
    e->last_status = status;
    e->last_search = time(NULL);
    if (status == CUVS_STATUS_OK)
    {
        e->search_count++;
        e->total_latency_us += latency_us;
        e->lat_buckets[cuvs_lat_bucket_index(latency_us)]++;
    }
    else
    {
        e->error_count++;
        if (err)
        {
            strncpy(e->last_error, err, sizeof(e->last_error) - 1);
            e->last_error[sizeof(e->last_error) - 1] = '\0';
        }
    }
}

static IndexEntry  g_indexes[MAX_INDEXES];
static int         g_n_indexes   = 0;
static pthread_mutex_t g_index_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Daemon-global VRAM cache counters (mutated under g_index_mutex). Exposed via
 * CUVS_OP_CACHE_STATS / pg_stat_gpu_cache. Per-index counters can't track this
 * because eviction destroys the IndexEntry. */
static uint64_t g_cache_hits        = 0;
static uint64_t g_cache_misses      = 0;
static uint64_t g_cache_evictions   = 0;
static uint64_t g_cache_reloads     = 0;
static uint64_t g_cache_persist_fail = 0;

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */
static char   g_socket_path[256]  = "/tmp/.s.pg_cuvs.server";
static char   g_index_dir[256]    = "/tmp/pg_cuvs_indexes";
static size_t g_max_vram_bytes    = 0;   /* 0 = unlimited */
static int    g_server_fd         = -1;
static volatile int g_shutdown    = 0;
static char   g_snapshot_uri[512] = ""; /* "gs://bucket[/prefix]"; empty = disabled */
static char   g_cluster_id[128]   = ""; /* multi-node identifier for GCS path */
static char   g_gcs_key_file[512] = ""; /* service account JSON; empty = instance metadata */

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
            total += g_indexes[i].vram_bytes + g_indexes[i].delta_vram_bytes;
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

/* Sidecar marking an index stale; persists staleness across daemon restarts. */
static void
stale_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.stale", dir, db_oid, index_oid);
}

/* Pending-insert delta sidecar (Phase 3B); written by the PG backend, replayed
 * here into a resident GPU brute-force cache. */
static void
delta_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.delta", dir, db_oid, index_oid);
}

/* Phase 3C: heap compatibility sidecar; written by the extension at build time.
 * Format: "<relfilenode> <table_oid>\n" */
static void
relfilenode_file_path(char *out, size_t outlen,
                      const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.relfilenode", dir, db_oid, index_oid);
}

static int
read_relfilenode_sidecar(const char *dir, uint32_t db_oid, uint32_t index_oid,
                         uint32_t *rfn_out, uint32_t *table_oid_out)
{
    char path[512];
    relfilenode_file_path(path, sizeof(path), dir, db_oid, index_oid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = (fscanf(f, "%u %u", rfn_out, table_oid_out) == 2);
    fclose(f);
    return ok ? 0 : -1;
}

/* ----------------------------------------------------------------
 * Phase 3B delta cache: resident GPU brute-force over the pending `.delta`.
 * ----------------------------------------------------------------
 * The merge comparator needs the metric; handle_search is serialized by
 * g_index_mutex, so a file-scope value set right before qsort is safe. */
static uint32_t g_merge_metric;

static int
delta_cand_cmp(const void *a, const void *b)
{
    const CuvsResult *x = (const CuvsResult *) a;
    const CuvsResult *y = (const CuvsResult *) b;

    if (g_merge_metric == CUVS_METRIC_IP)   /* larger inner product = nearer */
    {
        if (x->distance > y->distance) return -1;
        if (x->distance < y->distance) return 1;
        return 0;
    }
    if (x->distance < y->distance) return -1;   /* smaller distance = nearer */
    if (x->distance > y->distance) return 1;
    return 0;
}

/* Release an entry's GPU delta cache. Caller holds g_index_mutex. */
static void
free_delta_cache(IndexEntry *e)
{
    if (e->delta_idx) { cuvs_bf_free(e->delta_idx); e->delta_idx = NULL; }
    if (e->delta_tids) { free(e->delta_tids); e->delta_tids = NULL; }
    e->n_delta          = 0;
    e->delta_generation = 0;
    e->delta_mtime      = 0;
    e->delta_vram_bytes = 0;
}

/* Make e's GPU delta cache match the current `.delta` file. Cheap when nothing
 * changed (one stat + mtime compare). On a change it rebuilds the resident
 * brute-force index; any failure (corrupt / generation mismatch / no spare
 * VRAM) leaves the cache empty so the search runs base-only and the backend
 * CPU-merges. Best-effort, non-evicting: the delta must never evict a base
 * CAGRA index. Caller holds g_index_mutex. */
static void
refresh_delta_cache(IndexEntry *e)
{
    char            path[512], tids_path[512];
    struct stat     st;
    FILE           *f;
    CuvsDeltaHeader hdr;
    CuvsTidsHeader  th;
    uint32_t        base_crc;
    size_t          got, rec_bytes, needed;
    float          *vecs = NULL;
    uint64_t       *tids = NULL;
    char           *rec  = NULL;

    delta_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_delta_cache(e);            /* .delta gone (REINDEX) -> base-only */
        return;
    }
    if ((int64_t) st.st_mtime == e->delta_mtime)
        return;                         /* unchanged -> reuse current cache */

    /* The file changed. Drop the old cache and record the new mtime up front so
     * a persistent failure below does not retry on every search. */
    free_delta_cache(e);
    e->delta_mtime = (int64_t) st.st_mtime;

    f = fopen(path, "rb");
    if (!f)
        return;
    if (cuvs_delta_read_header(f, &hdr) != 0
        || cuvs_delta_validate(&hdr, (int64_t) st.st_size - (int64_t) sizeof(hdr)) != 0
        || hdr.dim != e->dim || hdr.metric != e->metric)
    {
        fclose(f);
        return;
    }

    /* Generation: the delta must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { fclose(f); return; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != hdr.base_tids_crc32)
    {
        fclose(f);
        return;                         /* stale delta from an old base -> base-only */
    }
    e->delta_generation = hdr.base_tids_crc32;
    if (hdr.n_rows == 0)
    {
        fclose(f);
        return;                         /* empty delta -> base-only */
    }

    /* VRAM: best-effort, NON-evicting (never evict a base index for the delta). */
    needed = (size_t) hdr.n_rows * ((size_t) hdr.dim * sizeof(float) + sizeof(uint64_t));
    if ((g_max_vram_bytes > 0 && total_vram_used() + needed > g_max_vram_bytes)
        || needed > gpu_free_vram_bytes())
    {
        fclose(f);
        return;                         /* no spare VRAM -> backend CPU-merges */
    }

    rec_bytes = cuvs_delta_record_bytes(hdr.dim);
    vecs = malloc((size_t) hdr.n_rows * hdr.dim * sizeof(float));
    tids = malloc((size_t) hdr.n_rows * sizeof(uint64_t));
    rec  = malloc(rec_bytes);
    if (vecs && tids && rec && fseek(f, (long) sizeof(hdr), SEEK_SET) == 0)
    {
        int ok = 1;
        for (int64_t i = 0; i < hdr.n_rows; i++)
        {
            if (fread(rec, 1, rec_bytes, f) != rec_bytes) { ok = 0; break; }
            memcpy(&tids[i], rec, sizeof(uint64_t));
            memcpy(&vecs[(size_t) i * hdr.dim], rec + sizeof(uint64_t),
                   (size_t) hdr.dim * sizeof(float));
        }
        if (ok)
        {
            e->delta_idx = cuvs_bf_build(vecs, hdr.n_rows, (int) hdr.dim, hdr.metric);
            if (e->delta_idx)
            {
                e->delta_tids       = tids;  tids = NULL;   /* ownership moved */
                e->n_delta          = hdr.n_rows;
                e->delta_vram_bytes = needed;
                LOG_INFO("pg_cuvs_server: delta cache %u/%u built (%lld rows, %zu KB VRAM)\n",
                        e->db_oid, e->index_oid, (long long) e->n_delta, needed / 1024);
            }
        }
    }
    free(vecs);
    free(tids);
    free(rec);
    fclose(f);
}

/* Forward decls used by save_index. */
static int write_tids_atomic(const char *tids_tmp,
                             int64_t n_vecs, uint32_t dim, uint32_t metric,
                             const uint64_t *tids);
static int fsync_path(const char *path);
static int ensure_vram(size_t needed);   /* defined after the LRU section */

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

    /* VRAM make-room: evict LRU resident indexes to fit (tiered cache). Same
     * path as build, so a search miss on a non-resident index reloads it by
     * evicting the least-recently-used one. If it still won't fit after full
     * eviction, skip (caller falls back to CPU). */
    size_t needed = estimate_vram_bytes(n_vecs, (int)dim);
    if (ensure_vram(needed) != 0)
    {
        LOG_WARN("pg_cuvs_server: insufficient VRAM loading %u/%u even after "
                 "eviction, skip\n", db_oid, index_oid);
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
    /* Delta cache starts empty; this slot may carry stale delta fields from a
     * previously evicted index. It is lazily (re)built in handle_search. */
    e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
    e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
    reset_entry_stats(e);

    /* Restore staleness from the .stale sidecar so a daemon restart does not
     * silently resurrect a write-stale index as fresh. */
    {
        char stale_path[512];
        struct stat st;
        stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
        if (stat(stale_path, &st) == 0)
        {
            e->stale = 1;
            e->stale_since = st.st_mtime;
        }
        else
        {
            e->stale = 0;
            e->stale_since = 0;
        }
    }

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

    /* Phase 3C: second pass — for indexes that have a .relfilenode sidecar
     * but no local .cagra, try downloading from GCS. The .relfilenode sidecar
     * serves as the registry: its presence means the extension built this index
     * here and the daemon should maintain it. */
    if (g_snapshot_uri[0] == '\0')
        return;

    dir = opendir(g_index_dir);
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        /* Match "<db_oid>_<index_oid>.relfilenode" */
        if (sscanf(ent->d_name, "%u_%u.relfilenode", &db_oid, &index_oid) != 2)
            continue;

        /* Skip if already resident in the registry (loaded in first pass) */
        int already_loaded = 0;
        for (int i = 0; i < g_n_indexes; i++)
        {
            if (g_indexes[i].valid &&
                g_indexes[i].db_oid    == db_oid &&
                g_indexes[i].index_oid == index_oid)
            {
                already_loaded = 1;
                break;
            }
        }
        if (already_loaded)
            continue;

        uint32_t local_rfn = 0, local_table_oid = 0;
        read_relfilenode_sidecar(g_index_dir, db_oid, index_oid,
                                 &local_rfn, &local_table_oid);

        LOG_INFO("objstore: local .cagra missing for %u/%u, trying GCS download\n",
                 db_oid, index_oid);

        int rc = cuvs_objstore_download(
            g_snapshot_uri,
            g_cluster_id,
            g_gcs_key_file,
            g_index_dir,
            db_oid,
            index_oid,
            local_rfn,   /* 0 = skip compat check */
            NULL         /* build_timestamp_out */
        );

        if (rc == 0)
        {
            LOG_INFO("objstore: download succeeded for %u/%u, loading\n",
                     db_oid, index_oid);
            load_index(db_oid, index_oid);
        }
        else
        {
            LOG_WARN("objstore: download failed for %u/%u; will rebuild on first query\n",
                     db_oid, index_oid);
        }
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
        g_cache_persist_fail++;
        return 0;
    }
    free_delta_cache(e);   /* release the GPU delta cache with the index */
    cuvs_cagra_free(e->handle);
    free(e->tids);
    size_t freed = e->vram_bytes;

    /* Remove from registry by shifting */
    int idx = (int)(e - g_indexes);
    for (int i = idx; i < g_n_indexes - 1; i++)
        g_indexes[i] = g_indexes[i+1];
    g_n_indexes--;

    g_cache_evictions++;
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
        /* Layer 2: LRU eviction loop. Stop if evict_lru makes no progress
         * (returns 0, e.g. save_index keeps failing) to avoid an infinite loop. */
        while (g_n_indexes > 0 && total_vram_used() + needed > g_max_vram_bytes)
            if (evict_lru() == 0)
                break;

        if (total_vram_used() + needed > g_max_vram_bytes)
            return -1;  /* Layer 3: caller falls back to CPU */
    }

    size_t free_vram = gpu_free_vram_bytes();
    while (g_n_indexes > 0 && needed > free_vram)
    {
        if (evict_lru() == 0)
            break;
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
    if (e)
    {
        g_cache_hits++;             /* already VRAM-resident */
    }
    else
    {
        g_cache_misses++;           /* not resident */
        /* Try to (re)load from disk, evicting LRU to fit if needed. */
        if (load_index(cmd->db_oid, cmd->index_oid) == 0)
        {
            g_cache_reloads++;
            e = find_index(cmd->db_oid, cmd->index_oid);
        }
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

    /* Stale: heap writes happened since the build, so the CAGRA graph is missing
     * rows. Don't search a stale graph — tell the backend to use the CPU path
     * (correct, current results) until a REINDEX rebuilds the graph. */
    if (e->stale)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_STALE;
        strncpy(hdr.error, "index stale (writes since build)", sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_STALE, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* The metric is baked into the CAGRA graph at build time. A query carrying
     * a different opclass metric means this index was built before the
     * build-metric fix (always-L2) and must be REINDEXed — otherwise we'd
     * silently return results from a wrong-metric graph. Reject it. */
    if (cmd->metric != e->metric)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_METRIC_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "index built with metric %u but queried with metric %u; REINDEX required",
                 e->metric, cmd->metric);
        record_search_stat(e, CUVS_STATUS_METRIC_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

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

    /* Reject a dimension-mismatched query BEFORE it reaches cuVS. cuVS throws
     * a RAFT failure on mismatch, and the resulting sticky CUDA error has
     * aborted the entire daemon (SIGABRT), taking down every backend. This is
     * a user error: distinct status, no CPU fallback. */
    if (cmd->dim != e->dim)
    {
        munmap(query, vec_bytes);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        record_search_stat(e, CUVS_STATUS_DIM_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
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

    if (ret != 0)
    {
        munmap(query, vec_bytes);
        free(raw);
        CuvsReplyHeader hdr = {0};
        /* ret==2 is the wrapper's own dim-mismatch guard (defense in depth in
         * case the pre-check above is ever bypassed). Everything else maps to
         * OOM_FALLBACK so the backend retries on CPU. */
        hdr.status = (ret == 2) ? CUVS_STATUS_DIM_MISMATCH : CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error,
                (ret == 2) ? "query dim != index dim" : "CAGRA search failed",
                sizeof(hdr.error) - 1);
        record_search_stat(e, hdr.status, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    CuvsResult *results = malloc(k * sizeof(CuvsResult));
    if (!results)
    {
        munmap(query, vec_bytes);
        free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    /* Phase 3B: refresh the resident GPU delta cache from the current .delta;
     * if present, brute-force it on the GPU and merge with the base candidates
     * by distance (same cuVS scale, metric-aware order). The query stays mmap'd
     * until after the delta search. */
    int delta_merged = 0;
    int n_valid      = 0;
    refresh_delta_cache(e);
    if (e->delta_idx && e->n_delta > 0)
    {
        int eff_k = (int) ((int64_t) k < e->n_delta ? (int64_t) k : e->n_delta);
        CuvsSearchResult *draw = malloc((size_t) eff_k * sizeof(CuvsSearchResult));
        CuvsResult       *cand = malloc((size_t) (k + eff_k) * sizeof(CuvsResult));
        if (draw && cand
            && cuvs_bf_search(e->delta_idx, query, (int) cmd->dim, eff_k, draw) == 0)
        {
            int n_total = 0;
            for (int i = 0; i < k; i++)            /* base candidates */
            {
                int64_t id = raw[i].item_id;
                if (id < 0 || id >= e->n_vecs) continue;
                cand[n_total].tid      = e->tids[id];
                cand[n_total].distance = raw[i].distance;
                n_total++;
            }
            for (int i = 0; i < eff_k; i++)        /* delta candidates */
            {
                int64_t id = draw[i].item_id;
                if (id < 0 || id >= e->n_delta) continue;
                cand[n_total].tid      = e->delta_tids[id];
                cand[n_total].distance = draw[i].distance;
                n_total++;
            }
            g_merge_metric = e->metric;            /* read by delta_cand_cmp under the mutex */
            qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
            n_valid = (n_total < k) ? n_total : k;
            memcpy(results, cand, (size_t) n_valid * sizeof(CuvsResult));
            delta_merged = 1;
        }
        free(draw);
        free(cand);
    }
    munmap(query, vec_bytes);

    if (!delta_merged)
    {
        /* Base-only: no usable delta cache — map item_ids to TIDs. */
        for (int i = 0; i < k; i++)
        {
            int64_t item_id = raw[i].item_id;
            if (item_id < 0 || item_id >= e->n_vecs)
                continue;
            results[n_valid].tid      = e->tids[item_id];
            results[n_valid].distance = raw[i].distance;
            n_valid++;
        }
    }
    free(raw);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);

    record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)n_valid;

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status       = CUVS_STATUS_OK;
    hdr.n_results    = (uint32_t)n_valid;
    hdr.latency_us   = latency_us;
    hdr.delta_merged = (uint32_t)delta_merged;

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
 * Phase 3C: detached upload thread — spawned after OK reply so that
 * CREATE INDEX returns immediately. Upload failure is non-fatal; the
 * local artifact is already durable at this point.
 * ---------------------------------------------------------------- */
typedef struct {
    char     snapshot_uri[512];
    char     cluster_id[128];
    char     gcs_key_file[512];
    char     cagra_path[512];
    char     tids_path[512];
    uint32_t db_oid;
    uint32_t table_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t metric;
    uint32_t dim;
    int64_t  vector_count;
} UploadThreadArgs;

static void *
objstore_upload_thread(void *arg)
{
    UploadThreadArgs *a = (UploadThreadArgs *)arg;
    int rc = cuvs_objstore_upload(
        a->snapshot_uri,
        a->cluster_id,
        a->gcs_key_file,
        a->cagra_path,
        a->tids_path,
        a->db_oid,
        a->table_oid,
        a->index_oid,
        a->relfilenode,
        a->metric,
        a->dim,
        a->vector_count,
        0   /* base_generation: not yet tracked at upload time */
    );
    if (rc == 0)
        LOG_INFO("objstore: uploaded index %u/%u to %s\n",
                 a->db_oid, a->index_oid, a->snapshot_uri);
    else
        LOG_WARN("objstore: upload FAILED for index %u/%u (non-fatal, local artifact intact)\n",
                 a->db_oid, a->index_oid);
    free(a);
    return NULL;
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

    /* Reject degenerate or overflowing payload sizes before any allocation.
     * n_vecs*dim*4 can wrap size_t on a 32-bit-ish product (e.g. n_vecs ~2^31,
     * dim ~2^20), producing a tiny mmap that the build then reads past. */
    /* CAGRA cannot build a neighborhood graph from a single point; cuVS
     * ABORTS the process (uncatchable) on n_vecs==1, taking down the daemon
     * for every backend. Require >= 2 and reject cleanly here. (The backend
     * already short-circuits n_vecs==0 without an IPC call.) */
    if (cmd->n_vecs < 2 || cmd->dim == 0)
    {
        LOG_ERROR("[handle_build] invalid payload n_vecs=%lld dim=%u\n",
                  (long long)cmd->n_vecs, cmd->dim);
        send_error(client_fd, "CAGRA build needs at least 2 vectors");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            LOG_ERROR("[handle_build] payload size overflow n_vecs=%lld dim=%u\n",
                      (long long)cmd->n_vecs, cmd->dim);
            send_error(client_fd, "build payload size overflow");
            return;
        }
    }

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
    CuvsCagraIndex new_handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric);
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

    /* A rebuild produces a fresh graph reflecting the current heap — clear any
     * persisted staleness marker. */
    {
        char stale_path[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir,
                        cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
    }

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
        existing->stale       = 0;     /* rebuilt -> fresh */
        existing->stale_since = 0;
        /* The rebuilt base obsoletes the old delta cache (the backend unlinks
         * .delta on a successful build); drop the resident GPU delta now. */
        free_delta_cache(existing);
        reset_entry_stats(existing);   /* fresh index instance */
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
        e->stale       = 0;
        e->stale_since = 0;
        e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
        e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
        reset_entry_stats(e);
    }

    pthread_mutex_unlock(&g_index_mutex);
    LOG_DEBUG("[handle_build] sending OK reply\n");

    LOG_INFO("pg_cuvs_server: built index %u/%u (%lld vecs, %zu MB VRAM)\n",
            cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader hdr_ok = {0};
    hdr_ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));

    /* Phase 3C: upload to GCS in a detached thread after replying OK.
     * CREATE INDEX has already returned; upload failure is non-fatal. */
    if (g_snapshot_uri[0] != '\0' && cmd->relfilenode != 0)
    {
        UploadThreadArgs *ua = malloc(sizeof(*ua));
        if (ua)
        {
            strncpy(ua->snapshot_uri, g_snapshot_uri, sizeof(ua->snapshot_uri) - 1);
            strncpy(ua->cluster_id,   g_cluster_id,   sizeof(ua->cluster_id)   - 1);
            strncpy(ua->gcs_key_file, g_gcs_key_file, sizeof(ua->gcs_key_file) - 1);
            strncpy(ua->cagra_path,   idx_final,       sizeof(ua->cagra_path)   - 1);
            strncpy(ua->tids_path,    tids_final,      sizeof(ua->tids_path)    - 1);
            ua->db_oid      = cmd->db_oid;
            ua->table_oid   = cmd->table_oid;
            ua->index_oid   = cmd->index_oid;
            ua->relfilenode = cmd->relfilenode;
            ua->metric      = cmd->metric;
            ua->dim         = cmd->dim;
            ua->vector_count = cmd->n_vecs;

            pthread_t upload_tid;
            pthread_attr_t upload_attr;
            pthread_attr_init(&upload_attr);
            pthread_attr_setdetachstate(&upload_attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&upload_tid, &upload_attr, objstore_upload_thread, ua) != 0)
            {
                LOG_WARN("objstore: failed to spawn upload thread for %u/%u\n",
                         cmd->db_oid, cmd->index_oid);
                free(ua);
            }
            pthread_attr_destroy(&upload_attr);
        }
    }
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
 * Handle STATS command (CUVS_OP_STATUS)
 *
 * Returns per-index search statistics for the requesting database. The
 * daemon is the source of truth: counters are cross-backend and survive
 * individual PG sessions (but reset on index rebuild/reload). With
 * cmd->index_oid == 0, every resident index in cmd->db_oid is returned;
 * otherwise just that one. Reply is the standard header (n_results = count)
 * followed by n_results × CuvsIndexStats.
 * ---------------------------------------------------------------- */
static void
handle_stats(int client_fd, const CuvsCmdFrame *cmd)
{
    pthread_mutex_lock(&g_index_mutex);

    /* Bounded by MAX_INDEXES; gather under the lock, send after unlocking. */
    CuvsIndexStats stats[MAX_INDEXES];
    uint32_t n = 0;

    for (int i = 0; i < g_n_indexes && n < MAX_INDEXES; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid)
            continue;
        if (e->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
            continue;

        CuvsIndexStats *s = &stats[n++];
        memset(s, 0, sizeof(*s));
        s->db_oid           = e->db_oid;
        s->index_oid        = e->index_oid;
        s->dim              = e->dim;
        s->metric           = e->metric;
        s->n_vecs           = e->n_vecs;
        s->vram_bytes       = e->vram_bytes;
        s->resident         = 1;
        s->last_status      = e->last_status;
        s->last_requested_k = e->last_requested_k;
        s->last_returned_k  = e->last_returned_k;
        s->search_count     = e->search_count;
        s->error_count      = e->error_count;
        s->total_latency_us = e->total_latency_us;
        s->p50_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.50);
        s->p95_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.95);
        s->p99_us = cuvs_lat_percentile(e->lat_buckets, CUVS_LAT_BUCKETS, 0.99);
        s->last_search_at   = (int64_t)e->last_search;
        s->stale            = (uint32_t)e->stale;
        s->stale_since      = (int64_t)e->stale_since;
        strncpy(s->last_error, e->last_error, sizeof(s->last_error) - 1);
    }

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = n;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n > 0)
        send_all(client_fd, stats, (size_t)n * sizeof(CuvsIndexStats));
}

/* ----------------------------------------------------------------
 * Handle MARK_STALE command (CUVS_OP_MARK_STALE)
 *
 * A backend write hook (aminsert/ambulkdelete) flags an index stale after a
 * heap write. We set the in-memory flag AND touch a .stale sidecar so the
 * staleness survives a daemon restart. Idempotent.
 * ---------------------------------------------------------------- */
static void
handle_mark_stale(int client_fd, const CuvsCmdFrame *cmd)
{
    char stale_path[512];
    time_t now = time(NULL);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e && !e->stale)
    {
        e->stale = 1;
        e->stale_since = now;
    }

    /* Persist the marker even if the index is not currently resident, so a
     * later load_index picks it up. */
    stale_file_path(stale_path, sizeof(stale_path), g_index_dir,
                    cmd->db_oid, cmd->index_oid);
    int fd = open(stale_path, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0)
        close(fd);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* ----------------------------------------------------------------
 * Handle CACHE_STATS command (CUVS_OP_CACHE_STATS): daemon-global counters.
 * ---------------------------------------------------------------- */
static void
handle_cache_stats(int client_fd)
{
    CuvsCacheStats cs;
    memset(&cs, 0, sizeof(cs));

    pthread_mutex_lock(&g_index_mutex);
    cs.hits             = g_cache_hits;
    cs.misses           = g_cache_misses;
    cs.evictions        = g_cache_evictions;
    cs.reloads          = g_cache_reloads;
    cs.persist_failures = g_cache_persist_fail;
    cs.resident_count   = (uint32_t) g_n_indexes;
    cs.vram_used_bytes  = total_vram_used();
    cs.vram_budget_bytes = g_max_vram_bytes;
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = 1;
    send_all(client_fd, &hdr, sizeof(hdr));
    send_all(client_fd, &cs, sizeof(cs));
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
        case CUVS_OP_STATUS:
            handle_stats(client_fd, &cmd);
            break;
        case CUVS_OP_MARK_STALE:
            handle_mark_stale(client_fd, &cmd);
            break;
        case CUVS_OP_CACHE_STATS:
            handle_cache_stats(client_fd);
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
        else if (strcmp(argv[i], "--snapshot-uri") == 0 && i+1 < argc)
            strncpy(g_snapshot_uri, argv[++i], sizeof(g_snapshot_uri) - 1);
        else if (strcmp(argv[i], "--cluster-id") == 0 && i+1 < argc)
            strncpy(g_cluster_id, argv[++i], sizeof(g_cluster_id) - 1);
        else if (strcmp(argv[i], "--gcs-key-file") == 0 && i+1 < argc)
            strncpy(g_gcs_key_file, argv[++i], sizeof(g_gcs_key_file) - 1);
    }

    LOG_INFO("pg_cuvs_server: starting (socket=%s index-dir=%s snapshot=%s)\n",
            g_socket_path, g_index_dir,
            g_snapshot_uri[0] ? g_snapshot_uri : "disabled");

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

    /* Phase 3C: initialize libcurl once for the process lifetime. Must be called
     * before any curl_easy_* or download/upload operations. */
    if (g_snapshot_uri[0] != '\0')
        curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Load persisted indexes */
    startup_load_indexes();

    /* Warm up the GPU now so the first client query doesn't pay the one-time
     * CUDA context / RMM / cuBLAS / kernel init (hundreds of ms). */
    {
        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        cuvs_warmup();
        clock_gettime(CLOCK_MONOTONIC, &w1);
        double ms = (w1.tv_sec - w0.tv_sec) * 1000.0 +
                    (w1.tv_nsec - w0.tv_nsec) / 1e6;
        LOG_INFO("pg_cuvs_server: GPU warm-up done in %.0f ms\n", ms);
    }

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
