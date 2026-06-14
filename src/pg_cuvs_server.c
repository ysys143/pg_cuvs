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

/* The Makefile builds with -D_POSIX_C_SOURCE=200809L, which hides glibc's
 * __USE_MISC extensions (e.g. MAP_ANONYMOUS, used by the ADR-059 multi-shard
 * fallback). Re-enable them; additive alongside _POSIX_C_SOURCE. Must precede
 * any system header. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "cuvs_ipc.h"
#include "cuvs_util.h"
#include "cuvs_wrapper.h"
#include "cuvs_objstore.h"
#include "cuvs_build_corpus.h"   /* ADR-057: cuvs_fd_recv (SCM_RIGHTS build payload) */
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
 *
 * The registry is a soft LRU working-set cap, NOT a hard wall: up to
 * g_max_indexes indexes stay resident; beyond that the LRU is evicted to disk
 * (durable) and reloaded on demand by the search-miss path. Sized once at
 * startup from --max-indexes (default 1024, was a hard 64) — allocated with
 * calloc and never realloc'd, so &g_indexes[i] addresses and the compacting-shift
 * removal stay valid for the daemon's life.
 * ---------------------------------------------------------------- */
#define CUVS_DEFAULT_MAX_INDEXES 1024

/* Phase 3D: per-index warmup lifecycle state. */
typedef enum WarmupState {
    WARMUP_HOT          = 0,
    WARMUP_COLD         = 1,
    WARMUP_QUEUED       = 2,
    WARMUP_DOWNLOADING  = 3,
    WARMUP_LOADING      = 4,
    WARMUP_FAILED       = 5,
} WarmupState;

/* Phase 3F: one shard of a sharded logical CAGRA index. A shard is a standalone
 * CAGRA artifact resident on one GPU, covering the contiguous global TID range
 * [tid_offset, tid_offset + n_vecs). Shard-local item_ids map to global TIDs via
 * IndexEntry.tids[tid_offset + item_id]. */
typedef struct ShardEntry {
    uint32_t        shard_id;
    CuvsCagraIndex  handle;        /* cuVS opaque index for this shard */
    int64_t         tid_offset;    /* global TID start offset */
    int64_t         n_vecs;        /* vectors in this shard */
    uint32_t        gpu_device_id; /* CUDA device this shard lives on */
    size_t          vram_bytes;    /* estimated VRAM held by this shard */
    uint64_t        search_count;  /* OK searches dispatched to this shard */
    uint64_t        error_count;   /* failed shard searches */
    uint32_t        last_status;   /* CUVS_STATUS_* of the last shard search */
    int             valid;         /* 1 once the shard is built/loaded */

    /* Phase 3L: resident GPU brute-force index over this shard's vector range
     * [tid_offset, tid_offset + n_vecs). Lazily built on the first BF search;
     * NULL until then. Freed with the shard. */
    CuvsBfIndex     bf_idx;
    size_t          bf_vram_bytes; /* estimated VRAM held by bf_idx */
} ShardEntry;

typedef struct IndexEntry {
    uint32_t        db_oid;
    uint32_t        index_oid;
    uint32_t        dim;
    uint32_t        metric;
    int64_t         n_vecs;
    CuvsCagraIndex  handle;       /* cuVS opaque index (unsharded only; NULL if sharded) */
    uint64_t       *tids;         /* global TID array [n_vecs] (shared by all shards) */

    /* Phase 3F multi-GPU sharding. shard_count <= 1 => unsharded: use `handle`
     * and `gpu_device_id`. shard_count >= 2 => sharded: `handle` is NULL,
     * `shards`[shard_count] holds the per-GPU CAGRA artifacts, and
     * `gpu_device_id` is set to 0xFFFFFFFF (no single device) so LRU eviction
     * (which matches on exact device id) never partially evicts the index. */
    int             shard_count;  /* 0/1 = unsharded; >=2 = sharded */
    ShardEntry     *shards;       /* [shard_count] when sharded; NULL otherwise */
    int             inflight;     /* Phase 3G.4: in-flight lock-free sharded searches;
                                   * >0 blocks whole-unit eviction of this entry */
    size_t          vram_bytes;   /* estimated VRAM usage */
    time_t          last_search;  /* for LRU eviction; also stats last_search_at */
    int             valid;
    int             stale;        /* 1 if heap writes happened since build (REINDEX needed) */
    time_t          stale_since;  /* when first marked stale; 0 if fresh */

    /* ADR-073: this is a standalone `flat` index — GPU exact brute-force only,
     * NO CAGRA graph. handle == NULL and shard_count == 0 (breaks the old
     * "handle==NULL => sharded" assumption). Every search MUST take the
     * brute-force path (main_bf_idx over .vectors); the cagra `handle` is never
     * dereferenced. Set in handle_build_flat and load_index's flat branch; reset
     * to 0 by reset_entry_stats for every cagra/ivfpq/sharded (re)init. */
    int             is_flat;

    /* Phase 3B delta cache: resident GPU brute-force index over the pending
     * `.delta` vectors. Lazily (re)built in handle_search when the .delta file
     * appears/changes; freed on eviction. delta_idx==NULL means no GPU delta. */
    CuvsBfIndex     delta_idx;        /* resident brute-force index; NULL if none */
    uint64_t       *delta_tids;       /* host: delta item_id -> heap TID [n_delta] */
    int64_t         n_delta;          /* delta vectors in the cache */
    uint32_t        delta_generation; /* base .tids body_crc32 the cache was built on */
    int64_t         delta_mtime;      /* .delta st_mtime when the cache was built */
    size_t          delta_vram_bytes; /* estimated VRAM held by the delta cache */
    float          *delta_vecs_host; /* host copy for incremental rebuild (Phase 3A-4) */
    int64_t         delta_n_cached;  /* rows in current GPU BF + host buffer */

    /* Phase 3L: resident GPU brute-force index over the full base vector matrix
     * (the `.vectors` sidecar), used when a search request sets search_mode=BF.
     * Lazily (re)built in handle_search when `.vectors` appears/changes; freed
     * on eviction. main_bf_idx==NULL means no resident BF index. */
    CuvsBfIndex     main_bf_idx;        /* unsharded BF index; NULL if none */
    int64_t         main_bf_n;          /* vectors in the BF index */
    size_t          main_bf_vram_bytes; /* estimated VRAM held by main_bf_idx */
    int64_t         main_bf_mtime;      /* .vectors st_mtime when the BF was built */
    uint32_t        main_bf_generation; /* base .tids body_crc32 the BF was built on */
    uint32_t        bf_precision;       /* precision of the resident BF index(es):
                                         * 0=float32, 1=float16 (cuvs.bf_precision) */

    /* 3O pre-filter: TID-sorted reverse map for query-time BITSET construction.
     * rev_tids[i] is sorted ascending; rev_item_ids[i] is the corresponding item_id.
     * Built once at load time (O(n log n)); NULL until built or on malloc failure. */
    uint64_t       *rev_tids;           /* sorted TID values [n_vecs] */
    int32_t        *rev_item_ids;       /* item_id for rev_tids[i] [n_vecs] */

    /* Search stats (pg_stat_gpu_search). Reset on (re)build/load — they
     * describe the currently resident index instance, not a persisted total. */
    uint64_t        search_count;     /* CUVS_STATUS_OK searches */
    uint64_t        error_count;      /* attributable non-OK searches */
    uint64_t        total_latency_us; /* sum of OK latencies (for avg) */
    uint32_t        lat_buckets[CUVS_LAT_BUCKETS];
    uint32_t        last_status;       /* CUVS_STATUS_* of most recent search */
    uint32_t        last_requested_k;  /* top-k of last OK search (reflects cuvs.k) */
    uint32_t        last_returned_k;   /* rows last OK search returned */
    uint64_t        delta_merged_count; /* searches where daemon merged delta on GPU */
    uint64_t        bf_batch_count;     /* Phase 3L-9: coalesced BF batch dispatches */
    char            last_error[128];
    WarmupState      warmup_state;     /* Phase 3D: WARMUP_HOT for resident indexes */
    /* Phase 3D: warmup observability — populated when an index is hydrated from
     * GCS by the warmup worker; stays 0 for locally-built indexes (never warmed). */
    time_t          last_warmup_at;
    uint32_t        warmup_duration_ms;
    uint64_t        download_count;
    uint64_t        cache_miss_count;
    uint32_t        gpu_device_id;    /* Phase 3E: which CUDA device this index lives on */

    /* Phase 3I-1: CPU HNSW fallback. Loaded lazily when use_cpu_hnsw=1. */
    CuvsHnswIndex   hnsw_idx;        /* NULL until first cpu_hnsw search request */
    uint32_t        last_search_mode; /* 0=gpu_cagra, 1=cpu_hnsw, 2=cpu_fallback, 3=gpu_bf, 4=cagra_prefilter, 5=ivfpq */

    /* 3P: IVF-PQ sidecar (mutually exclusive with CAGRA handle for same entry) */
    CuvsIvfPqIndex  ivfpq_handle;       /* NULL if not loaded */
    int64_t         ivfpq_n_vecs;       /* corpus size (mirrors n_vecs) */
    size_t          ivfpq_vram_bytes;   /* estimated VRAM for IVF-PQ PQ codes */
    /* 3Q: streaming update counters */
    int64_t         n_extended;         /* vectors added via EXTEND since last build/compact */
    /* 4C: compaction observability */
    uint64_t        compact_count;      /* cuvsCagraMerge compact ops since last build */
    time_t          last_compact_at;    /* epoch seconds of last compact; 0 if never */
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
    e->last_requested_k    = 0;
    e->last_returned_k    = 0;
    e->delta_merged_count = 0;
    e->bf_batch_count     = 0;
    e->last_error[0]      = '\0';
    e->last_warmup_at     = 0;   /* Phase 3D warmup observability */
    e->warmup_duration_ms = 0;
    e->download_count     = 0;
    e->cache_miss_count   = 0;
    /* ADR-073: default to non-flat. Every cagra/ivfpq/sharded (re)init funnels
     * through reset_entry_stats, so this clears a reused slot's stale is_flat;
     * the flat build/load paths set is_flat = 1 AFTER calling reset_entry_stats. */
    e->is_flat            = 0;
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

static int         g_max_indexes = CUVS_DEFAULT_MAX_INDEXES;  /* registry capacity; --max-indexes */
static IndexEntry *g_indexes      = NULL;   /* calloc(g_max_indexes) at startup */
static int         g_n_indexes   = 0;
static pthread_mutex_t g_index_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Phase 3D: cold index registry — indexes known from .relfilenode sidecars but
 * not yet VRAM-resident. Protected by g_index_mutex. When warmup completes,
 * the entry is removed here and load_index() adds it to g_indexes. */
typedef struct ColdIndexEntry {
    uint32_t    db_oid;
    uint32_t    index_oid;
    uint32_t    relfilenode;
    uint32_t    table_oid;
    WarmupState warmup_state;
    time_t      last_warmup_at;
    uint32_t    warmup_duration_ms;
    uint64_t    download_count;
    uint64_t    cache_miss_count;
    uint32_t    gpu_device_id;
    int         valid;
} ColdIndexEntry;

static ColdIndexEntry *g_cold_indexes = NULL;   /* calloc(g_max_indexes) at startup */
static int            g_n_cold_indexes = 0;

/* Daemon-global VRAM cache counters (mutated under g_index_mutex). Exposed via
 * CUVS_OP_CACHE_STATS / pg_stat_gpu_cache. Per-index counters can't track this
 * because eviction destroys the IndexEntry. */
static uint64_t g_cache_hits[CUVS_MAX_GPUS];
static uint64_t g_cache_misses[CUVS_MAX_GPUS];
static uint64_t g_cache_evictions[CUVS_MAX_GPUS];
static uint64_t g_cache_reloads[CUVS_MAX_GPUS];
static uint64_t g_cache_persist_fail[CUVS_MAX_GPUS];

/* ADR-070 Bug #2: VRAM reserved by in-flight builds that have released
 * g_index_mutex for the (multi-minute) GPU build. The built index is not yet in
 * g_indexes, so total_vram_used() adds this so concurrent admission/eviction
 * still accounts for the build's VRAM. Mutated under g_index_mutex. */
static size_t   g_pending_build_vram[CUVS_MAX_GPUS];

/* Phase 3E: per-device GPU state. */
static CuvsGpuDeviceInfo g_gpus[CUVS_MAX_GPUS];
static int      g_n_gpus          = 0;
static int      g_allowed_gpus[CUVS_MAX_GPUS];
static int      g_n_allowed_gpus  = 0;   /* 0 = use all detected */
static size_t   g_max_vram_per_gpu[CUVS_MAX_GPUS];

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */
static char   g_socket_path[256]  = "/tmp/.s.pg_cuvs.server";
static char   g_index_dir[256]    = "/tmp/pg_cuvs_indexes";
static size_t g_max_vram_bytes    = 0;   /* 0 = use default fraction; >0 = explicit --max-vram-mb */

/* ADR-065: default per-device budget when --max-vram-mb is omitted — a fraction
 * of total VRAM, leaving headroom for the untracked cuVS workspace + CUDA
 * context so an unset budget cannot OOM the device. */
#define CUVS_DEFAULT_VRAM_FRACTION 0.90
static int    g_server_fd         = -1;
static volatile int g_shutdown    = 0;
static char   g_snapshot_uri[512] = ""; /* "gs://bucket[/prefix]"; empty = disabled */
static char   g_cluster_id[128]   = ""; /* multi-node identifier for GCS path */
static char   g_gcs_key_file[512] = ""; /* service account JSON; empty = instance metadata */

/* ----------------------------------------------------------------
 * Phase 3D: background warmup thread pool + queue
 * ---------------------------------------------------------------- */
typedef struct WarmupJob {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t table_oid;
    int      from_cache_miss;
} WarmupJob;

#define WARMUP_QUEUE_MAX 64

static WarmupJob       g_warmup_queue[WARMUP_QUEUE_MAX];
static int             g_warmup_head  = 0;
static int             g_warmup_tail  = 0;
static int             g_warmup_count = 0;
static pthread_mutex_t g_warmup_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_warmup_cond  = PTHREAD_COND_INITIALIZER;
static int             g_warmup_nthreads = 2;
static pthread_t       g_warmup_tids[8];

/* ----------------------------------------------------------------
 * Phase 3L-9: brute-force micro-batch worker (single consumer).
 *
 * When a connection thread runs an unsharded brute_force search with
 * cmd->bf_batch_wait_us > 0, it enqueues a request here (instead of dispatching
 * immediately) and blocks on the request's `done` flag. One dedicated worker
 * thread coalesces queued requests that share a (db,index,precision,dim) key
 * into a single cuvs_bf_search_batch GPU dispatch, then wakes each producer.
 *
 * Lock order (no thread ever holds both): a producer takes g_index_mutex for
 * the cheap stale/metric/dim preamble, RELEASES it, then takes g_bf_mtx to
 * enqueue; the worker moves the queue out under g_bf_mtx, RELEASES it, then
 * takes g_index_mutex for the GPU work. Gated entirely by bf_batch_wait_us>0 —
 * with the default 0 nothing is ever enqueued and this subsystem is inert.
 * ---------------------------------------------------------------- */
#define CUVS_BF_BATCH_MAX 256          /* cap on concurrently queued BF requests */

typedef struct CuvsBfRequest {
    CuvsBfKey    key;          /* db_oid, index_oid, precision, dim */
    const float *query;        /* producer-owned, dim floats, valid until done */
    int          k;            /* requested top-k */
    uint32_t     wait_us;      /* this request's batch window hint */
    CuvsResult  *out;          /* producer-owned [k]; worker writes n_out results */
    int          n_out;        /* results written by the worker */
    int          status;       /* CUVS_STATUS_* set by the worker */
    int          done;         /* 0 = pending, 1 = worker finished this request */
} CuvsBfRequest;

static pthread_mutex_t g_bf_mtx       = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_bf_cond      = PTHREAD_COND_INITIALIZER; /* signaled on enqueue */
static pthread_cond_t  g_bf_done_cond = PTHREAD_COND_INITIALIZER; /* signaled on completion */
static CuvsBfRequest  *g_bf_queue[CUVS_BF_BATCH_MAX];
static int             g_bf_queue_n   = 0;
static pthread_t       g_bf_worker_tid;
static int             g_bf_worker_started = 0;

static int
warmup_enqueue(uint32_t db_oid, uint32_t index_oid,
               uint32_t relfilenode, uint32_t table_oid,
               int from_cache_miss)
{
    pthread_mutex_lock(&g_warmup_mutex);

    /* Duplicate check: skip if already queued or downloading. */
    for (int i = 0; i < g_warmup_count; i++)
    {
        int idx = (g_warmup_head + i) % WARMUP_QUEUE_MAX;
        if (g_warmup_queue[idx].db_oid == db_oid &&
            g_warmup_queue[idx].index_oid == index_oid)
        {
            pthread_mutex_unlock(&g_warmup_mutex);
            return -1;
        }
    }

    if (g_warmup_count >= WARMUP_QUEUE_MAX)
    {
        pthread_mutex_unlock(&g_warmup_mutex);
        return -1;
    }

    WarmupJob *j = &g_warmup_queue[g_warmup_tail];
    j->db_oid         = db_oid;
    j->index_oid      = index_oid;
    j->relfilenode    = relfilenode;
    j->table_oid      = table_oid;
    j->from_cache_miss = from_cache_miss;
    g_warmup_tail = (g_warmup_tail + 1) % WARMUP_QUEUE_MAX;
    g_warmup_count++;

    pthread_cond_signal(&g_warmup_cond);
    pthread_mutex_unlock(&g_warmup_mutex);
    return 0;
}

static int
warmup_dequeue(WarmupJob *out)
{
    pthread_mutex_lock(&g_warmup_mutex);
    while (g_warmup_count == 0 && !g_shutdown)
        pthread_cond_wait(&g_warmup_cond, &g_warmup_mutex);

    if (g_shutdown)
    {
        pthread_mutex_unlock(&g_warmup_mutex);
        return -1;
    }

    *out = g_warmup_queue[g_warmup_head];
    g_warmup_head = (g_warmup_head + 1) % WARMUP_QUEUE_MAX;
    g_warmup_count--;
    pthread_mutex_unlock(&g_warmup_mutex);
    return 0;
}

/* Forward declarations for warmup worker. */
static int  load_index(uint32_t db_oid, uint32_t index_oid);
static void build_rev_tid_map(IndexEntry *e);  /* 3O: TID→item_id reverse map */

static void *
warmup_worker_thread(void *arg)
{
    (void)arg;
    while (!g_shutdown)
    {
        WarmupJob job;
        if (warmup_dequeue(&job) != 0)
            break;

        LOG_INFO("warmup: downloading %u/%u from GCS\n",
                 job.db_oid, job.index_oid);

        /* Update cold entry state -> DOWNLOADING (under g_index_mutex). */
        pthread_mutex_lock(&g_index_mutex);
        for (int i = 0; i < g_n_cold_indexes; i++)
        {
            ColdIndexEntry *ce = &g_cold_indexes[i];
            if (ce->valid && ce->db_oid == job.db_oid &&
                ce->index_oid == job.index_oid)
            {
                ce->warmup_state = WARMUP_DOWNLOADING;
                break;
            }
        }
        pthread_mutex_unlock(&g_index_mutex);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int rc = cuvs_objstore_download(
            g_snapshot_uri, g_cluster_id, g_gcs_key_file,
            g_index_dir, job.db_oid, job.index_oid,
            job.relfilenode, NULL);

        if (rc != 0)
        {
            LOG_WARN("warmup: GCS download failed for %u/%u\n",
                     job.db_oid, job.index_oid);
            pthread_mutex_lock(&g_index_mutex);
            for (int i = 0; i < g_n_cold_indexes; i++)
            {
                ColdIndexEntry *ce = &g_cold_indexes[i];
                if (ce->valid && ce->db_oid == job.db_oid &&
                    ce->index_oid == job.index_oid)
                {
                    ce->warmup_state = WARMUP_FAILED;
                    break;
                }
            }
            pthread_mutex_unlock(&g_index_mutex);
            continue;
        }

        /* Download succeeded — load into VRAM. */
        pthread_mutex_lock(&g_index_mutex);

        /* Mark LOADING. */
        ColdIndexEntry *target_ce = NULL;
        for (int i = 0; i < g_n_cold_indexes; i++)
        {
            ColdIndexEntry *ce = &g_cold_indexes[i];
            if (ce->valid && ce->db_oid == job.db_oid &&
                ce->index_oid == job.index_oid)
            {
                ce->warmup_state = WARMUP_LOADING;
                target_ce = ce;
                break;
            }
        }

        if (load_index(job.db_oid, job.index_oid) != 0)
        {
            LOG_WARN("warmup: VRAM load failed for %u/%u\n",
                     job.db_oid, job.index_oid);
            if (target_ce)
                target_ce->warmup_state = WARMUP_FAILED;
            pthread_mutex_unlock(&g_index_mutex);
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t dur_ms = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000 +
                                     (t1.tv_nsec - t0.tv_nsec) / 1000000);

        /* Propagate warmup stats to the hot entry. */
        IndexEntry *he = NULL;
        for (int i = 0; i < g_n_indexes; i++)
        {
            if (g_indexes[i].valid && g_indexes[i].db_oid == job.db_oid &&
                g_indexes[i].index_oid == job.index_oid)
            {
                he = &g_indexes[i];
                break;
            }
        }
        if (he)
        {
            he->warmup_state       = WARMUP_HOT;
            he->last_warmup_at     = time(NULL);
            he->warmup_duration_ms = dur_ms;
            he->download_count     = (target_ce ? target_ce->download_count : 0) + 1;
            he->cache_miss_count   = (target_ce ? target_ce->cache_miss_count : 0);
        }

        /* Remove from cold registry (shift down). */
        if (target_ce)
        {
            int ci = (int)(target_ce - g_cold_indexes);
            for (int i = ci; i < g_n_cold_indexes - 1; i++)
                g_cold_indexes[i] = g_cold_indexes[i + 1];
            g_n_cold_indexes--;
        }

        pthread_mutex_unlock(&g_index_mutex);

        /* Clean up stale delta/tombstone sidecars from a previous generation. */
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/%u_%u.delta",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
            snprintf(path, sizeof(path), "%s/%u_%u.tombstone",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
            snprintf(path, sizeof(path), "%s/%u_%u.stale",
                     g_index_dir, job.db_oid, job.index_oid);
            unlink(path);
        }

        LOG_INFO("warmup: %u/%u loaded into VRAM in %u ms\n",
                 job.db_oid, job.index_oid, dur_ms);
    }
    return NULL;
}

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
total_vram_used(int device_id)
{
    size_t total = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid)
            continue;
        if (e->shard_count >= 2)
        {
            /* Sharded: sum only the shards resident on this device. Include the
             * per-shard resident BF index (Phase 3L): it is freed on eviction
             * (free_index_shards) so it must be accounted, else eviction
             * over-commits VRAM. */
            for (int s = 0; s < e->shard_count; s++)
                if (e->shards[s].valid &&
                    e->shards[s].gpu_device_id == (uint32_t)device_id)
                    total += e->shards[s].vram_bytes
                           + e->shards[s].bf_vram_bytes;
        }
        else if (e->gpu_device_id == (uint32_t)device_id)
        {
            /* Include the resident main BF index (Phase 3L). NOT ivfpq_vram_bytes:
             * an IVF-PQ entry sets both vram_bytes and ivfpq_vram_bytes to the
             * same `needed`, so counting the latter would double-count. */
            total += e->vram_bytes + e->delta_vram_bytes + e->main_bf_vram_bytes;
        }
    }
    /* ADR-070 Bug #2: include VRAM reserved by in-flight builds on this device
     * (released the mutex for the GPU build; not yet in g_indexes). */
    if (device_id >= 0 && device_id < CUVS_MAX_GPUS)
        total += g_pending_build_vram[device_id];
    return total;
}

static size_t
gpu_free_vram_bytes(int device_id)
{
    return cuvs_vram_free_bytes_on(device_id);
}

/* Phase 3E: return the allowed GPU index (in g_allowed_gpus) for a device_id,
 * or the device_id itself if no restriction is configured. */
static int
is_gpu_allowed(int device_id)
{
    if (g_n_allowed_gpus == 0)
        return 1;
    for (int i = 0; i < g_n_allowed_gpus; i++)
        if (g_allowed_gpus[i] == device_id)
            return 1;
    return 0;
}

static int
n_usable_gpus(void)
{
    return (g_n_allowed_gpus > 0) ? g_n_allowed_gpus : g_n_gpus;
}

static int
usable_gpu(int i)
{
    return (g_n_allowed_gpus > 0) ? g_allowed_gpus[i] : i;
}

/* Pick the GPU with the most VRAM headroom for a new index of `needed` bytes.
 * Returns device_id or -1 if no device can fit even in principle (needed >
 * budget). Does NOT reject based on current usage -- ensure_vram handles
 * eviction to make room. */
static int
pick_gpu_for_index(size_t needed)
{
    int best = -1;
    size_t best_headroom = 0;
    int n = n_usable_gpus();
    for (int i = 0; i < n; i++)
    {
        int dev = usable_gpu(i);
        size_t budget = g_max_vram_per_gpu[dev];
        if (budget > 0 && needed > budget)
            continue;
        size_t used = total_vram_used(dev);
        size_t headroom = (budget > 0) ? (budget - used) : gpu_free_vram_bytes(dev);
        if (best < 0 || headroom > best_headroom) {
            best_headroom = headroom;
            best = dev;
        }
    }
    return best;
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

/* Phase 3I-1: CPU HNSW fallback sidecar. */
static void
hnsw_file_path(char *out, size_t outlen,
               const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.hnsw", dir, db_oid, index_oid);
}

/* 3P: IVF-PQ serialized index. */
static void
ivfpq_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.ivfpq", dir, db_oid, index_oid);
}

/* Sidecar marking an index stale; persists staleness across daemon restarts. */
static void
stale_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.stale", dir, db_oid, index_oid);
}

/* Phase 3F: manifest sidecar marking a logical index as sharded; the commit
 * marker for a sharded build (renamed last). */
static void
shards_manifest_path(char *out, size_t outlen,
                     const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.shards", dir, db_oid, index_oid);
}

/* Phase 3F: per-shard CAGRA artifact, e.g. "<db>_<idx>.s000.cagra". */
static void
shard_cagra_path(char *out, size_t outlen,
                 const char *dir, uint32_t db_oid, uint32_t index_oid,
                 uint32_t shard_id)
{
    snprintf(out, outlen, "%s/%u_%u.s%03u.cagra", dir, db_oid, index_oid, shard_id);
}

/* Pending-insert delta sidecar (Phase 3B); written by the PG backend, replayed
 * here into a resident GPU brute-force cache. */
static void
delta_file_path(char *out, size_t outlen,
                const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.delta", dir, db_oid, index_oid);
}

/* Phase 3L: raw vector matrix sidecar for GPU brute-force search; written at
 * build time alongside `.cagra`/`.tids`, loaded into a resident CuvsBfIndex. */
static void
vectors_file_path(char *out, size_t outlen,
                  const char *dir, uint32_t db_oid, uint32_t index_oid)
{
    snprintf(out, outlen, "%s/%u_%u.vectors", dir, db_oid, index_oid);
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

/* GPU that hosts the delta brute-force cache for an entry. Unsharded indexes
 * use their single device; a sharded index has no single device (gpu_device_id
 * is the 0xFFFFFFFF sentinel), so the global delta cache lives on shard 0's GPU
 * (Phase 3G.3). Caller holds g_index_mutex (reads e->shards). */
static uint32_t
delta_gpu_of(const IndexEntry *e)
{
    if (e->shard_count >= 2 && e->shards)
        return e->shards[0].gpu_device_id;
    return e->gpu_device_id;
}

/* Release an entry's GPU delta cache. Caller holds g_index_mutex. */
static void
free_delta_cache(IndexEntry *e)
{
    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, delta_gpu_of(e)); e->delta_idx = NULL; }
    if (e->delta_tids) { free(e->delta_tids); e->delta_tids = NULL; }
    if (e->delta_vecs_host) { free(e->delta_vecs_host); e->delta_vecs_host = NULL; }
    e->n_delta          = 0;
    e->delta_n_cached   = 0;
    e->delta_generation = 0;
    e->delta_mtime      = 0;
    e->delta_vram_bytes = 0;
}

/* Phase 3L: release an entry's resident GPU brute-force index over the full
 * base vector matrix. Unsharded only — sharded per-shard bf_idx is freed in
 * free_index_shards. Caller holds g_index_mutex. */
static void
free_main_bf_cache(IndexEntry *e)
{
    if (e->main_bf_idx) { cuvs_bf_free(e->main_bf_idx, delta_gpu_of(e)); e->main_bf_idx = NULL; }
    e->main_bf_n          = 0;
    e->main_bf_vram_bytes = 0;
    e->main_bf_mtime      = 0;
    e->main_bf_generation = 0;
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
    uint32_t        dg   = delta_gpu_of(e);   /* delta cache GPU (shard 0 if sharded) */

    delta_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_delta_cache(e);            /* .delta gone (REINDEX) -> base-only */
        return;
    }
    if ((int64_t) st.st_mtime == e->delta_mtime)
        return;                         /* unchanged -> reuse current cache */

    /* The file changed. Save previous state for incremental check, then
     * selectively clear: free the GPU index but keep host buffers if the
     * generation might still match. Record new mtime up front so a persistent
     * failure below does not retry on every search. */
    uint32_t prev_gen    = e->delta_generation;
    int64_t  prev_cached = e->delta_n_cached;
    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, dg); e->delta_idx = NULL; }
    e->n_delta          = 0;
    e->delta_vram_bytes = 0;
    e->delta_mtime      = (int64_t) st.st_mtime;

    f = fopen(path, "rb");
    if (!f)
        goto fail_cleanup;
    if (cuvs_delta_read_header(f, &hdr) != 0
        || cuvs_delta_validate(&hdr, (int64_t) st.st_size - (int64_t) sizeof(hdr)) != 0
        || hdr.dim != e->dim || hdr.metric != e->metric)
    {
        fclose(f);
        goto fail_cleanup;
    }

    /* Generation: the delta must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { fclose(f); goto fail_cleanup; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != hdr.base_tids_crc32)
    {
        fclose(f);
        goto fail_cleanup;              /* stale delta from an old base -> base-only */
    }
    e->delta_generation = hdr.base_tids_crc32;
    if (hdr.n_rows == 0)
    {
        fclose(f);
        goto fail_cleanup;              /* empty delta -> base-only */
    }

    /* VRAM: best-effort, NON-evicting (never evict a base index for the delta). */
    needed = (size_t) hdr.n_rows * ((size_t) hdr.dim * sizeof(float) + sizeof(uint64_t));
    if ((g_max_vram_per_gpu[dg] > 0
         && total_vram_used(dg) + needed > g_max_vram_per_gpu[dg])
        || needed > gpu_free_vram_bytes(dg))
    {
        fclose(f);
        goto fail_cleanup;              /* no spare VRAM -> backend CPU-merges */
    }

    rec_bytes = cuvs_delta_record_bytes(hdr.dim);

    /* Phase 3A-4 incremental path: if the generation matches, the file just
     * grew (append-only), and we still have the host-side buffers, read only
     * the new records and rebuild the GPU BF from the full host buffer. */
    if (hdr.base_tids_crc32 == prev_gen
        && hdr.n_rows > prev_cached
        && e->delta_vecs_host != NULL && e->delta_tids != NULL)
    {
        int64_t new_rows = hdr.n_rows - prev_cached;
        float    *new_vecs = realloc(e->delta_vecs_host,
                                     (size_t) hdr.n_rows * hdr.dim * sizeof(float));
        uint64_t *new_tids = realloc(e->delta_tids,
                                     (size_t) hdr.n_rows * sizeof(uint64_t));
        rec = malloc(rec_bytes);
        if (new_vecs && new_tids && rec)
        {
            e->delta_vecs_host = new_vecs;
            e->delta_tids      = new_tids;
            long seek_off = (long) sizeof(hdr) + (long)(prev_cached * rec_bytes);
            if (fseek(f, seek_off, SEEK_SET) == 0)
            {
                int ok = 1;
                for (int64_t i = 0; i < new_rows; i++)
                {
                    int64_t idx = prev_cached + i;
                    if (fread(rec, 1, rec_bytes, f) != rec_bytes) { ok = 0; break; }
                    memcpy(&e->delta_tids[idx], rec, sizeof(uint64_t));
                    memcpy(&e->delta_vecs_host[(size_t) idx * hdr.dim],
                           rec + sizeof(uint64_t),
                           (size_t) hdr.dim * sizeof(float));
                }
                if (ok)
                {
                    if (e->delta_idx) { cuvs_bf_free(e->delta_idx, dg); e->delta_idx = NULL; }
                    e->delta_idx = cuvs_bf_build(e->delta_vecs_host, hdr.n_rows,
                                                 (int) hdr.dim, hdr.metric,
                                                 0 /* float32: CPU-exact equivalence */, dg);
                    if (e->delta_idx)
                    {
                        e->n_delta          = hdr.n_rows;
                        e->delta_n_cached   = hdr.n_rows;
                        e->delta_vram_bytes = needed;
                        LOG_INFO("pg_cuvs_server: delta cache %u/%u incremental (%lld -> %lld rows)\n",
                                e->db_oid, e->index_oid,
                                (long long) prev_cached, (long long) hdr.n_rows);
                    }
                }
            }
        }
        free(rec);
        fclose(f);
        return;
    }

    /* Full rebuild path */
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
            e->delta_idx = cuvs_bf_build(vecs, hdr.n_rows, (int) hdr.dim, hdr.metric,
                                         0 /* float32: CPU-exact equivalence */, dg);
            if (e->delta_idx)
            {
                e->delta_tids       = tids;  tids = NULL;
                e->delta_vecs_host  = vecs;  vecs = NULL;
                e->n_delta          = hdr.n_rows;
                e->delta_n_cached   = hdr.n_rows;
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
    return;

fail_cleanup:
    if (e->delta_tids) { free(e->delta_tids); e->delta_tids = NULL; }
    if (e->delta_vecs_host) { free(e->delta_vecs_host); e->delta_vecs_host = NULL; }
    e->delta_n_cached   = 0;
    e->delta_generation = 0;
}

/* Phase 3L: make e's resident main brute-force index match the current
 * `.vectors` sidecar and the requested precision. Lazily (re)built on the first
 * brute_force search and whenever the sidecar's mtime or the precision changes.
 * Leaves main_bf_idx == NULL (so the caller fails closed with NO_VECTORS) when
 * the sidecar is missing, stale (generation mismatch with the current .tids),
 * corrupt, shape-mismatched, or cannot fit. Best-effort, NON-evicting:
 * brute_force must never evict a base CAGRA index. Caller holds g_index_mutex.
 * Unsharded only — sharded BF uses per-shard caches (refresh_shard_bf_caches). */
static void
refresh_main_bf_cache(IndexEntry *e, uint32_t want_precision)
{
    char              path[512], tids_path[512];
    struct stat       st;
    FILE             *f;
    CuvsVectorsHeader vh;
    CuvsTidsHeader    th;
    float            *vecs = NULL;
    size_t            got, needed;
    uint32_t          dg = delta_gpu_of(e);   /* unsharded: the index's GPU */

    vectors_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_main_bf_cache(e);          /* no sidecar -> brute_force unavailable */
        return;
    }
    if (e->main_bf_idx
        && (int64_t) st.st_mtime == e->main_bf_mtime
        && e->bf_precision == want_precision)
        return;                         /* unchanged + same precision -> reuse */

    /* Sidecar changed or precision toggled: drop the old index and rebuild.
     * Record the new mtime/precision up front so a persistent failure below
     * does not retry the (re)build on every search. */
    free_main_bf_cache(e);
    e->main_bf_mtime = (int64_t) st.st_mtime;
    e->bf_precision  = want_precision;

    f = fopen(path, "rb");
    if (!f)
        return;
    if (cuvs_vectors_read(f, &vh, &vecs) != 0)
    {
        fclose(f);
        return;                         /* corrupt / short read -> unavailable */
    }
    fclose(f);
    if (vh.dim != e->dim || vh.metric != e->metric || vh.n_vecs != e->n_vecs)
    {
        free(vecs);
        return;                         /* shape mismatch -> unavailable */
    }

    /* Generation: the sidecar must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { free(vecs); return; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != vh.base_tids_crc32)
    {
        free(vecs);
        return;                         /* stale sidecar (torn build) -> unavailable */
    }

    /* VRAM: best-effort, NON-evicting (never evict a base index for BF). */
    needed = (size_t) vh.n_vecs * (size_t) vh.dim * (want_precision == 1 ? 2 : 4);
    if ((g_max_vram_per_gpu[dg] > 0
         && total_vram_used(dg) + needed > g_max_vram_per_gpu[dg])
        || needed > gpu_free_vram_bytes(dg))
    {
        free(vecs);
        return;                         /* no spare VRAM -> brute_force unavailable */
    }

    e->main_bf_idx = cuvs_bf_build(vecs, vh.n_vecs, (int) vh.dim, vh.metric,
                                   want_precision, dg);
    free(vecs);
    if (e->main_bf_idx)
    {
        e->main_bf_n          = vh.n_vecs;
        e->main_bf_generation = vh.base_tids_crc32;
        e->main_bf_vram_bytes = needed;
        LOG_INFO("pg_cuvs_server: main BF cache %u/%u built (%lld vecs, %s, %zu MB VRAM)\n",
                 e->db_oid, e->index_oid, (long long) vh.n_vecs,
                 want_precision == 1 ? "float16" : "float32", needed / (1024 * 1024));
    }
}

/* Phase 3L: free every shard's resident brute-force index. Caller holds the
 * mutex. */
static void
free_shard_bf_caches(IndexEntry *e)
{
    if (!e->shards) return;
    for (int s = 0; s < e->shard_count; s++)
        if (e->shards[s].bf_idx)
        {
            cuvs_bf_free(e->shards[s].bf_idx, e->shards[s].gpu_device_id);
            e->shards[s].bf_idx = NULL;
            e->shards[s].bf_vram_bytes = 0;
        }
}

/* Phase 3L: ensure every shard of e has a resident brute-force index over its
 * vector range, built at want_precision by slicing the single global `.vectors`
 * sidecar at each shard's tid_offset. Returns 1 if all shards are ready, 0 if
 * the sidecar is missing / stale (generation mismatch) / shape-mismatched / a
 * shard won't fit — in which case the caller fails closed with NO_VECTORS.
 * Lazily (re)builds when the sidecar mtime or the precision changes; reuses
 * main_bf_mtime/bf_precision as the generation tracker (main_bf_idx is unused
 * for sharded). Best-effort, NON-evicting, all-or-nothing. Caller holds mutex. */
static int
refresh_shard_bf_caches(IndexEntry *e, uint32_t want_precision)
{
    char              path[512], tids_path[512];
    struct stat       st;
    FILE             *f;
    CuvsVectorsHeader vh;
    CuvsTidsHeader    th;
    float            *vecs = NULL;
    size_t            got;

    if (e->shard_count < 2 || !e->shards)
        return 0;

    vectors_file_path(path, sizeof(path), g_index_dir, e->db_oid, e->index_oid);
    if (stat(path, &st) != 0)
    {
        free_shard_bf_caches(e);        /* sidecar gone -> brute_force unavailable */
        e->main_bf_mtime = 0;
        return 0;
    }
    if ((int64_t) st.st_mtime == e->main_bf_mtime && e->bf_precision == want_precision)
    {
        int ready = 1;
        for (int s = 0; s < e->shard_count; s++)
            if (!e->shards[s].bf_idx) { ready = 0; break; }
        if (ready)
            return 1;                   /* unchanged + same precision + all built */
    }

    /* (Re)build: drop existing per-shard BF, load once, slice, build each. */
    free_shard_bf_caches(e);
    e->main_bf_mtime = (int64_t) st.st_mtime;
    e->bf_precision  = want_precision;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (cuvs_vectors_read(f, &vh, &vecs) != 0) { fclose(f); return 0; }
    fclose(f);
    if (vh.dim != e->dim || vh.metric != e->metric || vh.n_vecs != e->n_vecs)
    {
        free(vecs);
        return 0;
    }

    /* Generation: the sidecar must belong to the current base build's .tids. */
    tids_file_path(tids_path, sizeof(tids_path), g_index_dir, e->db_oid, e->index_oid);
    {
        FILE *tf = fopen(tids_path, "rb");
        if (!tf) { free(vecs); return 0; }
        got = fread(&th, 1, sizeof(th), tf);
        fclose(tf);
    }
    if (got != sizeof(th) || th.magic != CUVS_TIDS_MAGIC
        || th.body_crc32 != vh.base_tids_crc32)
    {
        free(vecs);
        return 0;
    }

    /* Build each shard's BF over its contiguous [tid_offset, +n_vecs) range.
     * VRAM best-effort, NON-evicting per shard GPU; any failure rolls back all
     * shards and reports unavailable (fail closed). */
    int ok = 1;
    for (int s = 0; s < e->shard_count; s++)
    {
        ShardEntry *sh  = &e->shards[s];
        int         dev = (int) sh->gpu_device_id;
        size_t      needed = (size_t) sh->n_vecs * (size_t) vh.dim
                             * (want_precision == 1 ? 2 : 4);
        if ((g_max_vram_per_gpu[dev] > 0
             && total_vram_used(dev) + needed > g_max_vram_per_gpu[dev])
            || needed > gpu_free_vram_bytes(dev))
        {
            ok = 0;
            break;
        }
        sh->bf_idx = cuvs_bf_build(vecs + (size_t) sh->tid_offset * vh.dim,
                                   sh->n_vecs, (int) vh.dim, vh.metric,
                                   want_precision, dev);
        if (!sh->bf_idx) { ok = 0; break; }
        sh->bf_vram_bytes = needed;
    }
    free(vecs);

    if (!ok)
    {
        free_shard_bf_caches(e);
        return 0;
    }
    LOG_INFO("pg_cuvs_server: sharded BF cache %u/%u built (%d shards, %s)\n",
             e->db_oid, e->index_oid, e->shard_count,
             want_precision == 1 ? "float16" : "float32");
    return 1;
}

/* Forward decls used by save_index. */
static int crc32_file(const char *path, uint32_t *out);
static void free_index_shards(IndexEntry *e);
static IndexEntry *find_index(uint32_t db_oid, uint32_t index_oid);
static int write_tids_atomic(const char *tids_tmp,
                             int64_t n_vecs, uint32_t dim, uint32_t metric,
                             const uint64_t *tids);
static int fsync_path(const char *path);
static int ensure_vram(size_t needed, int device_id);   /* defined after the LRU section */
static size_t evict_lru(int device_id);                 /* soft-cap slot eviction (load_index) */

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
    /* Defensive (ADR-070): save_index serializes a CAGRA handle. An IVF-PQ or
     * sharded entry has e->handle == NULL; serializing it would deref NULL.
     * Callers must route those to their own (save-free) eviction paths; fail
     * closed here rather than crash if one ever reaches us. */
    if (e->handle == NULL) {
        LOG_ERROR("save_index: %u/%u has no CAGRA handle (ivfpq/sharded?); refusing\n",
                  e->db_oid, e->index_oid);
        return -1;
    }

    char idx_final[512],  idx_tmp[576];
    char tids_final[512], tids_tmp[576];
    index_file_path(idx_final,  sizeof(idx_final),  g_index_dir, e->db_oid, e->index_oid);
    tids_file_path(tids_final,  sizeof(tids_final), g_index_dir, e->db_oid, e->index_oid);
    snprintf(idx_tmp,  sizeof(idx_tmp),  "%s.tmp", idx_final);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);

    if (write_tids_atomic(tids_tmp, e->n_vecs, e->dim, e->metric, e->tids) != 0)
        return -1;

    if (cuvs_cagra_serialize(e->handle, idx_tmp, e->gpu_device_id) != 0) {
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

/* Phase 3F: reload a sharded logical index from its `.shards` manifest. The
 * caller has already read + validated the global `.tids` (thdr, tids). On
 * success registers a sharded IndexEntry, takes ownership of `tids`, and
 * returns 0. Returns 1 when no manifest exists (caller uses the unsharded
 * path; `tids` untouched). Returns -1 (corrupt/mismatch) or -2 (VRAM) on
 * failure WITHOUT consuming `tids` — fail closed, never a partial index.
 * Caller holds g_index_mutex. */
static int
load_index_sharded(uint32_t db_oid, uint32_t index_oid,
                   const CuvsTidsHeader *thdr, uint64_t *tids)
{
    char manifest_path[512];
    shards_manifest_path(manifest_path, sizeof(manifest_path), g_index_dir, db_oid, index_oid);

    FILE *mf = fopen(manifest_path, "rb");
    if (!mf)
        return 1;   /* no manifest => unsharded */

    CuvsShardsHeader shdr;
    CuvsShardRecord *recs = NULL;
    if (cuvs_shards_read(mf, &shdr, &recs) != 0)
    {
        fclose(mf);
        LOG_ERROR("pg_cuvs_server: .shards validation failed for %u/%u, skip (fail closed)\n",
                  db_oid, index_oid);
        return -1;
    }
    fclose(mf);

    /* Generation + geometry must match the .tids we already validated. */
    if (shdr.base_tids_crc32 != thdr->body_crc32 ||
        shdr.n_vecs != thdr->n_vecs ||
        shdr.dim != thdr->dim ||
        shdr.metric != thdr->metric)
    {
        LOG_ERROR("pg_cuvs_server: .shards generation/geometry mismatch for %u/%u, skip\n",
                  db_oid, index_oid);
        free(recs);
        return -1;
    }

    /* Soft cap: evict the LRU to free a slot so a sharded reload succeeds. */
    if (g_n_indexes >= g_max_indexes)
        evict_lru(usable_gpu(0));
    if (g_n_indexes >= g_max_indexes)
    {
        free(recs);
        return -1;
    }

    int         sc      = (int)shdr.shard_count;
    ShardEntry *shards  = calloc((size_t)sc, sizeof(ShardEntry));
    if (!shards)
    {
        free(recs);
        return -1;
    }

    int n_gpus  = n_usable_gpus();
    int ok      = 1;
    int rc_fail = -1;
    int i;
    for (i = 0; i < sc; i++)
    {
        size_t needed = estimate_vram_bytes(recs[i].n_vecs, (int)recs[i].dim);
        int    dev    = usable_gpu(i % n_gpus);   /* re-place on reload */
        if (ensure_vram(needed, dev) != 0)
        {
            int alt = pick_gpu_for_index(needed);
            if (alt < 0 || ensure_vram(needed, alt) != 0)
            {
                rc_fail = -2;   /* VRAM pressure */
                ok = 0;
                break;
            }
            dev = alt;
        }

        char sp[512];
        shard_cagra_path(sp, sizeof(sp), g_index_dir, db_oid, index_oid, recs[i].shard_id);

        /* Verify shard artifact CRC before deserialize (fail closed). */
        uint32_t acrc = 0;
        if (crc32_file(sp, &acrc) != 0 || acrc != recs[i].artifact_crc32)
        {
            LOG_ERROR("pg_cuvs_server: shard %u artifact crc mismatch for %u/%u, skip\n",
                      recs[i].shard_id, db_oid, index_oid);
            ok = 0;
            break;
        }

        CuvsCagraIndex h = cuvs_cagra_deserialize(sp, (int)recs[i].dim, dev);
        if (!h)
        {
            ok = 0;
            break;
        }

        shards[i].shard_id      = recs[i].shard_id;
        shards[i].handle        = h;
        shards[i].tid_offset    = recs[i].tid_offset;
        shards[i].n_vecs        = recs[i].n_vecs;
        shards[i].gpu_device_id = (uint32_t)dev;
        shards[i].vram_bytes    = needed;
        shards[i].valid         = 1;
    }

    if (!ok)
    {
        for (int j = 0; j < i && j < sc; j++)
            if (shards[j].valid && shards[j].handle)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
        free(shards);
        free(recs);
        return rc_fail;   /* fail closed: no partial sharded index registered */
    }

    IndexEntry *e = &g_indexes[g_n_indexes++];
    memset(e, 0, sizeof(*e));
    e->db_oid        = db_oid;
    e->index_oid     = index_oid;
    e->dim           = thdr->dim;
    e->metric        = thdr->metric;
    e->n_vecs        = thdr->n_vecs;
    e->handle        = NULL;
    e->tids          = tids;          /* take ownership */
    build_rev_tid_map(e);             /* 3O: TID → item_id reverse map */
    e->vram_bytes    = 0;
    e->last_search   = time(NULL);
    e->valid         = 1;
    e->warmup_state  = WARMUP_HOT;
    e->gpu_device_id = 0xFFFFFFFFu;    /* sharded: not LRU-evictable */
    e->shard_count   = sc;
    e->shards        = shards;
    reset_entry_stats(e);

    {
        char stale_path[512];
        struct stat st;
        stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
        if (stat(stale_path, &st) == 0) { e->stale = 1; e->stale_since = st.st_mtime; }
        else { e->stale = 0; e->stale_since = 0; }
    }

    free(recs);
    LOG_INFO("pg_cuvs_server: loaded sharded index %u/%u (%lld vecs, %d shards)\n",
             db_oid, index_oid, (long long)thdr->n_vecs, sc);
    for (int j = 0; j < sc; j++)
        LOG_INFO("  shard %d -> GPU %u (%lld vecs)\n",
                 j, e->shards[j].gpu_device_id, (long long)e->shards[j].n_vecs);
    return 0;
}

/* ----------------------------------------------------------------
 * 3O: Build sorted (TID → item_id) reverse map for BITSET prefilter.
 * Called after e->tids is populated; safe to call with n_vecs==0.
 * ---------------------------------------------------------------- */
typedef struct { uint64_t tid; int32_t item_id; } RevTidPair;
static int
cmp_rev_tid(const void *a, const void *b)
{
    uint64_t ta = ((const RevTidPair *)a)->tid;
    uint64_t tb = ((const RevTidPair *)b)->tid;
    return (ta > tb) - (ta < tb);
}

static void
build_rev_tid_map(IndexEntry *e)
{
    if (!e->tids || e->n_vecs <= 0)
        return;

    RevTidPair *pairs   = malloc((size_t)e->n_vecs * sizeof(RevTidPair));
    uint64_t   *rt      = malloc((size_t)e->n_vecs * sizeof(uint64_t));
    int32_t    *ri      = malloc((size_t)e->n_vecs * sizeof(int32_t));

    if (!pairs || !rt || !ri) {
        free(pairs); free(rt); free(ri);
        e->rev_tids     = NULL;
        e->rev_item_ids = NULL;
        LOG_WARN("[3O] build_rev_tid_map: malloc failed for index %u/%u\n",
                 e->db_oid, e->index_oid);
        return;
    }

    for (int64_t i = 0; i < e->n_vecs; i++) {
        pairs[i].tid     = e->tids[i];
        pairs[i].item_id = (int32_t)i;
    }
    qsort(pairs, (size_t)e->n_vecs, sizeof(RevTidPair), cmp_rev_tid);
    for (int64_t i = 0; i < e->n_vecs; i++) {
        rt[i] = pairs[i].tid;
        ri[i] = pairs[i].item_id;
    }
    free(pairs);

    e->rev_tids     = rt;
    e->rev_item_ids = ri;
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

    /* Phase 3F: if a `.shards` manifest exists, this is a sharded logical index.
     * Load via the shard path (which takes ownership of tids on success). A
     * present-but-invalid manifest fails closed (no unsharded fallback — there
     * is no single .cagra to fall back to). */
    {
        int src = load_index_sharded(db_oid, index_oid, &thdr, tids);
        if (src == 0)
            return 0;            /* sharded load OK; tids ownership transferred */
        if (src < 0)
        {
            free(tids);
            return src;          /* -1 corrupt/mismatch, -2 VRAM: fail closed */
        }
        /* src == 1: no manifest -> unsharded path below (tids still owned here) */
    }

    /* ADR-073: flat index — .tids + .vectors present, but NO .cagra and NO
     * .ivfpq. Register brute-force-only (handle == NULL, is_flat = 1). The
     * resident BF over .vectors is (re)built lazily + non-evictingly on the first
     * search (refresh_main_bf_cache), so we reserve no graph VRAM here. Detected
     * before the cagra VRAM make-room so a flat load never over-reserves or evicts
     * a cagra index for a graph it will not allocate. */
    {
        char fcagra[512], fivfpq[512], fvecs[512];
        index_file_path(fcagra, sizeof(fcagra), g_index_dir, db_oid, index_oid);
        ivfpq_file_path(fivfpq, sizeof(fivfpq), g_index_dir, db_oid, index_oid);
        vectors_file_path(fvecs, sizeof(fvecs), g_index_dir, db_oid, index_oid);
        if (access(fcagra, F_OK) != 0 && access(fivfpq, F_OK) != 0
            && access(fvecs, F_OK) == 0)
        {
            int fgpu = pick_gpu_for_index(0);
            if (fgpu < 0) fgpu = usable_gpu(0);

            /* Soft cap: free a slot if the registry is full (search-miss reload). */
            if (g_n_indexes >= g_max_indexes)
                evict_lru(fgpu);
            if (g_n_indexes >= g_max_indexes)
            {
                free(tids);
                return -1;
            }

            IndexEntry *e = &g_indexes[g_n_indexes++];
            e->db_oid       = db_oid;
            e->index_oid    = index_oid;
            e->dim          = dim;
            e->metric       = metric;
            e->n_vecs       = n_vecs;
            e->handle       = NULL;          /* no CAGRA graph — brute-force only */
            e->ivfpq_handle = NULL;
            e->ivfpq_n_vecs = 0;
            e->ivfpq_vram_bytes = 0;
            e->tids         = tids;          /* ownership transferred */
            e->rev_tids     = NULL;
            e->rev_item_ids = NULL;
            build_rev_tid_map(e);            /* 3O: TID -> item_id reverse map */
            e->vram_bytes   = 0;             /* BF is lazy; tracked in main_bf_vram_bytes */
            e->last_search  = time(NULL);
            e->valid        = 1;
            e->gpu_device_id = (uint32_t) fgpu;
            e->shard_count  = 0;
            e->shards       = NULL;
            e->inflight     = 0;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx     = NULL;
            e->n_extended   = 0;
            e->compact_count = 0;
            e->last_compact_at = 0;
            e->warmup_state = WARMUP_HOT;
            reset_entry_stats(e);
            e->is_flat      = 1;             /* MUST be after reset_entry_stats */
            e->last_search_mode = 3;         /* gpu_bf */

            /* Restore staleness from the .stale sidecar (same as cagra). */
            {
                char stale_path[512];
                struct stat st;
                stale_file_path(stale_path, sizeof(stale_path), g_index_dir, db_oid, index_oid);
                if (stat(stale_path, &st) == 0) { e->stale = 1; e->stale_since = st.st_mtime; }
                else { e->stale = 0; e->stale_since = 0; }
            }

            LOG_INFO("pg_cuvs_server: loaded flat index %u/%u (%lld vecs, BF built lazily)\n",
                     db_oid, index_oid, (long long) n_vecs);
            return 0;
        }
    }

    /* VRAM make-room: evict LRU resident indexes to fit (tiered cache). Same
     * path as build, so a search miss on a non-resident index reloads it by
     * evicting the least-recently-used one. If it still won't fit after full
     * eviction, skip (caller falls back to CPU). */
    size_t needed = estimate_vram_bytes(n_vecs, (int)dim);
    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        LOG_WARN("pg_cuvs_server: no GPU can fit %u/%u (%zu bytes), skip\n",
                 db_oid, index_oid, needed);
        free(tids);
        return -2;  /* VRAM: index too large for any GPU budget */
    }
    if (ensure_vram(needed, target_gpu) != 0)
    {
        LOG_WARN("pg_cuvs_server: VRAM exhausted on GPU %d loading %u/%u "
                 "(budget %zu MB, used %zu MB, needed %zu MB)\n",
                 target_gpu, db_oid, index_oid,
                 g_max_vram_per_gpu[target_gpu] / (1024*1024),
                 total_vram_used(target_gpu) / (1024*1024),
                 needed / (1024*1024));
        free(tids);
        return -2;  /* VRAM: exhausted after eviction */
    }

    CuvsCagraIndex handle       = cuvs_cagra_deserialize(idx_path, (int)dim, target_gpu);
    CuvsIvfPqIndex ivfpq_handle = NULL;
    if (!handle)
    {
        /* No .cagra sidecar — try .ivfpq (3P IVF-PQ index). */
        char ivfpq_path[512];
        ivfpq_file_path(ivfpq_path, sizeof(ivfpq_path), g_index_dir, db_oid, index_oid);
        if (access(ivfpq_path, F_OK) == 0)
            ivfpq_handle = cuvs_ivfpq_deserialize(ivfpq_path, target_gpu);
        if (!ivfpq_handle)
        {
            free(tids);
            return -1;
        }
    }

    /* Soft cap: if the registry is full, evict the LRU to free a slot so this
     * reload (search-miss auto-reload) succeeds. ensure_vram above only evicts
     * under VRAM pressure; with spare VRAM but full slots it would not, leaving
     * an evicted index unable to return. */
    if (g_n_indexes >= g_max_indexes)
        evict_lru(target_gpu);
    if (g_n_indexes >= g_max_indexes)
    {
        if (handle)       cuvs_cagra_free(handle, target_gpu);
        if (ivfpq_handle) cuvs_ivfpq_free(ivfpq_handle, target_gpu);
        free(tids);
        return -1;
    }

    IndexEntry *e = &g_indexes[g_n_indexes++];
    e->db_oid       = db_oid;
    e->index_oid    = index_oid;
    e->dim          = dim;
    e->metric       = metric;
    e->n_vecs       = n_vecs;
    e->handle       = handle;
    e->ivfpq_handle = ivfpq_handle;
    e->ivfpq_n_vecs = ivfpq_handle ? n_vecs : 0;
    e->tids         = tids;
    build_rev_tid_map(e);   /* 3O: TID → item_id reverse map */
    e->vram_bytes  = needed;
    e->last_search = time(NULL);
    e->valid       = 1;
    e->gpu_device_id = (uint32_t)target_gpu;
    e->shard_count = 0;     /* unsharded; slot may be reused post-eviction */
    e->shards      = NULL;
    /* Delta cache starts empty; this slot may carry stale delta fields from a
     * previously evicted index. It is lazily (re)built in handle_search. */
    e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
    e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
    e->delta_vecs_host = NULL; e->delta_n_cached = 0;
    /* Phase 3L: main BF cache starts empty; lazily built on a BF search. */
    e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
    e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
    /* 3P: ivfpq_vram_bytes already set above; ensure it's 0 for CAGRA entries */
    if (!e->ivfpq_handle) e->ivfpq_vram_bytes = 0;
    e->warmup_state = WARMUP_HOT;
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
         * rejects .tids files and Phase 3F shard artifacts). See
         * cuvs_parse_index_filename. */
        if (cuvs_parse_index_filename(ent->d_name, &db_oid, &index_oid) == 0)
            load_index(db_oid, index_oid);
    }
    closedir(dir);

    /* Phase 3F: sharded indexes have no base "<db>_<idx>.cagra" — discover them
     * via the ".shards" manifest. load_index dedups against already-loaded
     * entries and branches to the sharded path when the manifest is present. */
    dir = opendir(g_index_dir);
    if (dir)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            uint32_t db_oid, index_oid;
            char tail[16] = {0};
            /* "<db>_<idx>.shards"; %15s after the dot must equal "shards". */
            if (sscanf(ent->d_name, "%u_%u.%15s", &db_oid, &index_oid, tail) == 3
                && strcmp(tail, "shards") == 0
                && !find_index(db_oid, index_oid))
                load_index(db_oid, index_oid);
        }
        closedir(dir);
    }

    /* ADR-073: flat indexes have a "<db>_<idx>.vectors" + ".tids" but NO ".cagra"
     * and NO ".shards", so both passes above miss them and they would be
     * unsearchable after a daemon restart. Discover them via the .vectors sidecar.
     * Dedup against already-loaded entries (a cagra index also writes a .vectors
     * sidecar; it is already loaded by the .cagra pass, and even if reloaded here
     * load_index sees its .cagra and does NOT take the flat branch). */
    dir = opendir(g_index_dir);
    if (dir)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            uint32_t db_oid, index_oid;
            char tail[16] = {0};
            if (sscanf(ent->d_name, "%u_%u.%15s", &db_oid, &index_oid, tail) == 3
                && strcmp(tail, "vectors") == 0
                && !find_index(db_oid, index_oid))
                load_index(db_oid, index_oid);
        }
        closedir(dir);
    }

    /* Phase 3D: second pass — for indexes that have a .relfilenode sidecar
     * but no local .cagra, register as COLD and enqueue background download.
     * The daemon opens its UDS socket before these downloads complete, so
     * queries for COLD indexes get NOT_FOUND -> CPU fallback until warmup
     * finishes. */
    if (g_snapshot_uri[0] == '\0')
        return;

    dir = opendir(g_index_dir);
    if (!dir)
        return;

    while ((ent = readdir(dir)) != NULL)
    {
        uint32_t db_oid, index_oid;
        if (sscanf(ent->d_name, "%u_%u.relfilenode", &db_oid, &index_oid) != 2)
            continue;

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

        if (g_n_cold_indexes < g_max_indexes)
        {
            ColdIndexEntry *ce = &g_cold_indexes[g_n_cold_indexes++];
            ce->db_oid            = db_oid;
            ce->index_oid         = index_oid;
            ce->relfilenode       = local_rfn;
            ce->table_oid         = local_table_oid;
            ce->warmup_state      = WARMUP_QUEUED;
            ce->last_warmup_at    = 0;
            ce->warmup_duration_ms = 0;
            ce->download_count    = 0;
            ce->cache_miss_count  = 0;
            ce->valid             = 1;
        }

        warmup_enqueue(db_oid, index_oid, local_rfn, local_table_oid, 0);
        LOG_INFO("warmup: enqueued background download for %u/%u\n",
                 db_oid, index_oid);
    }
    closedir(dir);
}

/* ----------------------------------------------------------------
 * LRU eviction
 * ---------------------------------------------------------------- */
/* Is `e` (partly) resident on device_id? Unsharded: its single GPU. Sharded: any
 * of its shards (Phase 3G.4). */
static int
entry_on_device(const IndexEntry *e, int device_id)
{
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
            if (e->shards[s].valid &&
                e->shards[s].gpu_device_id == (uint32_t)device_id)
                return 1;
        return 0;
    }
    return e->gpu_device_id == (uint32_t)device_id;
}

static IndexEntry *
find_lru_index(int device_id)
{
    IndexEntry *lru = NULL;
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid)
            continue;
        /* Phase 3G.4: sharded indexes are evictable as a WHOLE UNIT, but never
         * while a lock-free fanout is mid-flight on them (inflight>0) — that
         * would free shard handles out from under the search. */
        if (e->shard_count >= 2 && e->inflight > 0)
            continue;
        if (!entry_on_device(e, device_id))
            continue;
        if (!lru || e->last_search < lru->last_search)
            lru = e;
    }
    return lru;
}

/* Evict the LRU index on a specific GPU to free VRAM. Returns bytes freed, or 0. */
static size_t
evict_lru(int device_id)
{
    IndexEntry *e = find_lru_index(device_id);
    if (!e)
        return 0;

    size_t freed;
    if (e->shard_count >= 2)
    {
        /* Sharded: every artifact (.tids/.shards/.sNNN.cagra/.delta/.tombstone)
         * is already durable on disk, so eviction needs no save — just free the
         * whole logical index (all shards across their GPUs) as a unit. A later
         * query reloads it from the .shards manifest. `freed` counts only this
         * device's shards (what ensure_vram is trying to reclaim here); freeing
         * also releases the index's shards on other GPUs (bonus). */
        freed = 0;
        for (int s = 0; s < e->shard_count; s++)
            if (e->shards[s].valid &&
                e->shards[s].gpu_device_id == (uint32_t)device_id)
                freed += e->shards[s].vram_bytes;
        free_delta_cache(e);
        free_main_bf_cache(e);
        free_index_shards(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }
    else if (e->ivfpq_handle)
    {
        /* 3P IVF-PQ: its `.ivfpq`/`.tids` sidecars are durable on disk, so
         * eviction needs no save — a later query reloads via load_index (like
         * the sharded path). save_index() assumes a CAGRA handle, but an IVF-PQ
         * entry's `e->handle` is NULL — calling cuvs_cagra_serialize(NULL) here
         * SEGV'd the daemon (ADR-070 — pre-existing; IVF-PQ was never evictable).
         * The ivfpq_handle itself is freed by the shared cleanup below. */
        free_delta_cache(e);
        free_main_bf_cache(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
        freed = e->vram_bytes;   /* IVF-PQ sets vram_bytes == ivfpq_vram_bytes */
    }
    else if (e->is_flat)
    {
        /* ADR-073: flat's .tids + .vectors are durable on disk, so eviction needs
         * no save — and MUST NOT call save_index (handle == NULL → it refuses,
         * which would make the flat index un-evictable and pin VRAM). A later
         * query reloads via load_index's flat branch and rebuilds the resident BF
         * lazily. The only VRAM a flat entry holds is the resident main_bf_idx. */
        freed = e->main_bf_vram_bytes;
        free_delta_cache(e);
        free_main_bf_cache(e);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
    }
    else
    {
        if (save_index(e) != 0) {
            LOG_ERROR("evict_lru: save_index FAILED for %u/%u on GPU %u; aborting eviction "
                      "(VRAM still holds index)\n",
                    e->db_oid, e->index_oid, e->gpu_device_id);
            g_cache_persist_fail[device_id]++;
            return 0;
        }
        free_delta_cache(e);
        free_main_bf_cache(e);
        cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;
        freed = e->vram_bytes;
    }

    /* Phase 3I-1: free CPU HNSW sidecar (RAM-only; no VRAM). */
    if (e->hnsw_idx) { cuvs_hnsw_free(e->hnsw_idx); e->hnsw_idx = NULL; }
    /* 3P: free IVF-PQ GPU index. */
    if (e->ivfpq_handle) { cuvs_ivfpq_free(e->ivfpq_handle, e->gpu_device_id); e->ivfpq_handle = NULL; }

    int idx = (int)(e - g_indexes);
    for (int i = idx; i < g_n_indexes - 1; i++)
        g_indexes[i] = g_indexes[i+1];
    g_n_indexes--;

    g_cache_evictions[device_id]++;
    return freed;
}

/* Ensure there is enough VRAM on `device_id` for `needed` bytes. */
static int
ensure_vram(size_t needed, int device_id)
{
    size_t budget = g_max_vram_per_gpu[device_id];
    if (budget > 0 && total_vram_used(device_id) + needed > budget)
    {
        while (g_n_indexes > 0 && total_vram_used(device_id) + needed > budget)
            if (evict_lru(device_id) == 0)
                break;

        if (total_vram_used(device_id) + needed > budget)
            return -1;
    }

    size_t free_vram = gpu_free_vram_bytes(device_id);
    while (g_n_indexes > 0 && needed > free_vram)
    {
        if (evict_lru(device_id) == 0)
            break;
        free_vram = gpu_free_vram_bytes(device_id);
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

/* Phase 3F: bump a per-GPU cache counter for every GPU a logical index
 * occupies — each shard's GPU for a sharded index, or the single GPU for an
 * unsharded one. Guards the sharded sentinel gpu_device_id (0xFFFFFFFF) so it
 * never indexes the per-GPU counter arrays out of bounds. Caller holds the
 * mutex. */
static void
cache_counter_bump(uint64_t *counters, const IndexEntry *e)
{
    if (e->shard_count >= 2)
    {
        for (int s = 0; s < e->shard_count; s++)
        {
            uint32_t g = e->shards[s].gpu_device_id;
            if (g < CUVS_MAX_GPUS)
                counters[g]++;
        }
    }
    else if (e->gpu_device_id < CUVS_MAX_GPUS)
    {
        counters[e->gpu_device_id]++;
    }
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

/* Phase 3G: one shard's search, run on its own thread for parallel fanout.
 * Each shard lives on a distinct GPU and uses the per-device cuVS resource pool
 * (cuvs_wrapper.cu), which is independently locked per device — concurrent
 * dispatch across devices is safe, and two shards on the same device serialize
 * correctly on that device's pool mutex. `cudaSetDevice` is per-thread, set
 * inside cuvs_cagra_search via PooledRes, so each worker binds its own device. */
typedef struct ShardSearchArg {
    CuvsCagraIndex    handle;
    CuvsBfIndex       bf_idx;      /* Phase 3L: per-shard BF index (used when use_bf) */
    int               use_bf;      /* Phase 3L: 1 = brute_force, 0 = CAGRA */
    const float      *query;
    int               dim;
    int               sk;          /* per-shard request k (k + overfetch, clamped to n_vecs) */
    uint32_t          gpu_device_id;
    int64_t           tid_offset;  /* snapshot: global TID start of this shard */
    int64_t           shard_n_vecs;/* snapshot: vectors in this shard */
    CuvsSearchResult *out;         /* caller-allocated [sk]; NULL when sk==0 */
    int               rc;          /* 0 = OK (also for skipped sk==0 shards) */
    int               threaded;    /* 1 if dispatched on its own pthread */
} ShardSearchArg;

static void *
shard_search_worker(void *p)
{
    ShardSearchArg *a = (ShardSearchArg *) p;
    if (a->use_bf)
        a->rc = cuvs_bf_search(a->bf_idx, a->query, a->dim, a->sk, a->out,
                               (int) a->gpu_device_id);
    else
        a->rc = cuvs_cagra_search(a->handle, a->query, a->dim, a->sk, a->out,
                                  (int) a->gpu_device_id);
    return NULL;
}

/* ----------------------------------------------------------------
 * ADR-064: Streaming / out-of-core filtered brute force.
 *
 * Reuses the 3O reverse map (heapTID -> item_id) to gather ONLY the
 * filter-passing vectors from the `.vectors` sidecar via pread (item_id order,
 * random access: offset = sizeof(header) + item_id*dim*4), runs GPU brute force
 * in chunks of `cmd->n_vecs` vectors, and merges a running exact top-k. The
 * whole sidecar is never resident — only one chunk at a time. Chunk size is a
 * footprint knob only; the merged result is identical for any chunking.
 * ---------------------------------------------------------------- */

/* pread `chunk_n` rows (item_ids[0..chunk_n)) from the sidecar body into the
 * contiguous host buffer `out` (row j = item_ids[j]). 0 on success, -1 on any
 * short read / pread error. */
static int
gather_chunk_pread(int fd, uint32_t dim, const int32_t *item_ids,
                   int chunk_n, float *out)
{
    const size_t row_bytes = (size_t)dim * sizeof(float);
    for (int j = 0; j < chunk_n; j++)
    {
        off_t  off = (off_t)sizeof(CuvsVectorsHeader)
                   + (off_t)item_ids[j] * (off_t)row_bytes;
        char  *dst = (char *)(out + (size_t)j * dim);
        size_t got = 0;
        while (got < row_bytes)
        {
            ssize_t r = pread(fd, dst + got, row_bytes - got, off + (off_t)got);
            if (r <= 0)
                return -1;
            got += (size_t)r;
        }
    }
    return 0;
}

/* Insert (tid, dist) into a running top-k kept ascending by distance (smaller =
 * better, matching cuVS L2/IP-as-distance). `*pn` is the current fill (<= k). */
static void
topk_insert(CuvsResult *top, int *pn, int k, uint64_t tid, float dist)
{
    if (*pn >= k && dist >= top[k - 1].distance)
        return;                                /* not better than current worst */
    int pos = (*pn < k) ? (*pn)++ : k - 1;     /* append, or evict worst if full */
    while (pos > 0 && top[pos - 1].distance > dist)
    {
        top[pos] = top[pos - 1];
        pos--;
    }
    top[pos].tid      = tid;
    top[pos].distance = dist;
}

static void
handle_search_stream_bf(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e && load_index(cmd->db_oid, cmd->index_oid) == 0)
        e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NOT_FOUND;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    e->last_search = time(NULL);

    /* Stale (heap writes since build): defer to the backend CPU path, same as
     * the in-VRAM filtered path does. */
    if (e->stale)
    {
        record_search_stat(e, CUVS_STATUS_STALE, 0, "index stale (writes since build)");
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_STALE;
        strncpy(hdr.error, "index stale (writes since build)", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Need the 3O reverse map (heapTID -> item_id), built at build/load time.
     * Genuinely absent only if build_rev_tid_map hit a malloc failure -> the
     * backend falls back to its in-VRAM filtered path. */
    if (e->rev_tids == NULL || e->rev_item_ids == NULL || e->n_vecs <= 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (cmd->metric != e->metric || cmd->dim != e->dim)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = (cmd->metric != e->metric) ? CUVS_STATUS_METRIC_MISMATCH
                                                : CUVS_STATUS_DIM_MISMATCH;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Map shm query vector. */
    size_t vec_bytes = (size_t)cmd->dim * sizeof(float);
    int qfd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (qfd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed");
        return;
    }
    float *query = mmap(NULL, vec_bytes, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (query == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed");
        return;
    }

    /* Map filter TIDs. */
    uint64_t *flt_tids = NULL;
    void     *flt_mem  = MAP_FAILED;
    size_t    flt_bytes = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
    if (cmd->n_filter_tids > 0 && cmd->filter_shm_key[0] != '\0')
    {
        int ffd = shm_open(cmd->filter_shm_key, O_RDONLY, 0);
        if (ffd >= 0)
        {
            flt_mem = mmap(NULL, flt_bytes, PROT_READ, MAP_SHARED, ffd, 0);
            close(ffd);
            if (flt_mem != MAP_FAILED)
                flt_tids = (uint64_t *) flt_mem;
        }
    }
    if (!flt_tids)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;   /* nothing to gather */
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* TID -> item_id via 3O reverse map (binary search); collect, drop misses. */
    int64_t  nv       = e->n_vecs;
    int32_t *item_ids = malloc((size_t)cmd->n_filter_tids * sizeof(int32_t));
    int      n_items  = 0;
    if (item_ids)
    {
        for (uint32_t fi = 0; fi < cmd->n_filter_tids; fi++)
        {
            uint64_t want = flt_tids[fi];
            int64_t  lo = 0, hi = nv - 1;
            while (lo <= hi)
            {
                int64_t mid = lo + (hi - lo) / 2;
                if (e->rev_tids[mid] == want)
                {
                    int32_t iid = e->rev_item_ids[mid];
                    if (iid >= 0 && (int64_t)iid < nv)
                        item_ids[n_items++] = iid;
                    break;
                }
                if (e->rev_tids[mid] < want) lo = mid + 1;
                else                          hi = mid - 1;
            }
        }
    }
    if (flt_mem != MAP_FAILED)
        munmap(flt_mem, flt_bytes);

    int k = (int)cmd->k;
    if (k < 1) k = 1;
    int chunk_cap = (cmd->n_vecs > 0) ? (int)cmd->n_vecs : 1;
    int dev       = delta_gpu_of(e);

    /* Open + validate the .vectors sidecar (item_id-ordered float32 body). */
    char vpath[512];
    vectors_file_path(vpath, sizeof(vpath), g_index_dir, e->db_oid, e->index_oid);
    int vfd = open(vpath, O_RDONLY);
    CuvsVectorsHeader vh;
    int ok = (vfd >= 0);
    if (ok)
    {
        ssize_t hr = pread(vfd, &vh, sizeof(vh), 0);
        ok = (hr == (ssize_t)sizeof(vh)
              && vh.magic == CUVS_VECTORS_MAGIC
              && vh.dim == e->dim && vh.metric == e->metric
              && vh.n_vecs == e->n_vecs);
    }
    if (!ok || !item_ids)
    {
        if (vfd >= 0) close(vfd);
        free(item_ids);
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = ok ? CUVS_STATUS_ERROR : CUVS_STATUS_NO_VECTORS;
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* Chunk buffers (sized to the cap, reused across chunks). */
    int cap = (chunk_cap < n_items) ? chunk_cap : (n_items > 0 ? n_items : 1);
    float            *chunkbuf  = malloc((size_t)cap * (size_t)e->dim * sizeof(float));
    CuvsSearchResult *chunk_res = malloc((size_t)k * sizeof(CuvsSearchResult));
    CuvsResult       *top       = malloc((size_t)k * sizeof(CuvsResult));
    int pn = 0;
    int failed = (!chunkbuf || !chunk_res || !top);

    for (int base = 0; !failed && base < n_items; base += chunk_cap)
    {
        int chunk_n = (n_items - base < chunk_cap) ? (n_items - base) : chunk_cap;
        if (gather_chunk_pread(vfd, e->dim, item_ids + base, chunk_n, chunkbuf) != 0)
        { failed = 1; break; }

        int sk = (k < chunk_n) ? k : chunk_n;
        if (cuvs_brute_force_search(chunkbuf, query, chunk_n, (int)e->dim,
                                    sk, e->metric, chunk_res, dev) != 0)
        { failed = 1; break; }

        for (int j = 0; j < sk; j++)
        {
            int64_t row = chunk_res[j].item_id;
            if (row < 0 || row >= chunk_n) continue;
            int32_t gid = item_ids[base + row];
            if (gid < 0 || (int64_t)gid >= e->n_vecs) continue;
            topk_insert(top, &pn, k, e->tids[gid], chunk_res[j].distance);
        }
    }

    close(vfd);
    munmap(query, vec_bytes);
    free(item_ids);
    free(chunkbuf);
    free(chunk_res);

    if (failed)
    {
        free(top);
        record_search_stat(e, CUVS_STATUS_ERROR, 0, "stream_bf failed");
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "stream_bf failed");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t lat = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                              (t1.tv_nsec - t0.tv_nsec) / 1000);
    record_search_stat(e, CUVS_STATUS_OK, lat, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)pn;
    e->last_search_mode = 6;   /* stream_bf */
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)pn;
    hdr.latency_us = lat;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (pn > 0)
        send_all(client_fd, top, (size_t)pn * sizeof(CuvsResult));
    free(top);
}

/* ----------------------------------------------------------------
 * Handle SEARCH_BF_TRANSIENT command (CUVS_OP_SEARCH_BF_TRANSIENT, ADR-073 B)
 *
 * One-shot exact GPU brute force over a per-query corpus the backend supplies —
 * NO resident IndexEntry. The corpus [float32 vecs n×dim][uint64_t tids n] arrives
 * as an SCM_RIGHTS memfd (or a named shm in filter_shm_key); the query is in
 * shm_key. VRAM admission is NON-EVICTING: a transient corpus must NEVER evict a
 * resident flat/cagra index. The short search runs under g_index_mutex so
 * total_vram_used() is consistent without a reservation counter. Touches no
 * IndexEntry (no find_index/load_index/record_search_stat).
 * ---------------------------------------------------------------- */
static void
handle_search_bf_transient(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Second frame: corpus memfd via SCM_RIGHTS (passed_fd), else shm by name. */
    char dir_buf[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, dir_buf, sizeof(dir_buf), &passed_fd) < 0)
    {
        send_error(client_fd, "recv corpus frame failed");
        return;
    }

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        if (passed_fd >= 0) close(passed_fd);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "transient BF needs >= 1 vector");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            if (passed_fd >= 0) close(passed_fd);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "transient BF payload overflow");
            return;
        }
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* Map the corpus (memfd tier via SCM_RIGHTS; else named shm). */
    void *mem;
    if (passed_fd >= 0)
    {
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);
    }
    else
    {
        int cfd = shm_open(cmd->filter_shm_key, O_RDONLY, 0);
        if (cfd < 0) { send_error(client_fd, "corpus shm_open failed"); return; }
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, cfd, 0);
        close(cfd);
    }
    if (mem == MAP_FAILED) { send_error(client_fd, "corpus mmap failed"); return; }

    const float    *vecs = (const float *)mem;
    const uint64_t *tids = (const uint64_t *)((const char *)mem + vec_bytes);

    /* Map the query shm. */
    size_t q_bytes = (size_t)cmd->dim * sizeof(float);
    int qfd = shm_open(cmd->shm_key, O_RDONLY, 0);
    if (qfd < 0) { munmap(mem, total); send_error(client_fd, "query shm_open failed"); return; }
    float *query = mmap(NULL, q_bytes, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (query == MAP_FAILED) { munmap(mem, total); send_error(client_fd, "query mmap failed"); return; }

    int k  = (int)cmd->k; if (k < 1) k = 1;
    int sk = (k < (int)cmd->n_vecs) ? k : (int)cmd->n_vecs;

    /* --- VRAM admission (NON-EVICTING). Hold g_index_mutex across the short
     * search so total_vram_used() stays consistent (no reservation counter). --- */
    pthread_mutex_lock(&g_index_mutex);

    size_t needed = vec_bytes;   /* corpus uploaded to the GPU as float32 */
    int    dev    = pick_gpu_for_index(needed);
    if (dev < 0
        || (g_max_vram_per_gpu[dev] > 0
            && total_vram_used(dev) + needed > g_max_vram_per_gpu[dev])
        || needed > gpu_free_vram_bytes(dev))
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(query, q_bytes);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        snprintf(hdr.error, sizeof(hdr.error),
                 "transient BF corpus %zu MB exceeds free GPU VRAM; reduce the WHERE "
                 "selectivity, build a flat index, or use a cagra/ivfpq index",
                 needed / (1024 * 1024));
        LOG_WARN("[handle_search_bf_transient] db=%u %s\n", cmd->db_oid, hdr.error);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    CuvsSearchResult *res = malloc((size_t)sk * sizeof(CuvsSearchResult));
    CuvsResult       *out = malloc((size_t)sk * sizeof(CuvsResult));
    int rc = (!res || !out)
           ? -1
           : cuvs_brute_force_search(vecs, query, cmd->n_vecs, (int)cmd->dim,
                                     sk, cmd->metric, res, dev);

    pthread_mutex_unlock(&g_index_mutex);

    munmap(query, q_bytes);

    if (rc != 0)
    {
        free(res); free(out);
        munmap(mem, total);
        send_error(client_fd, "transient BF search failed");
        return;
    }

    /* Map corpus rows -> TIDs (tids still mapped). */
    int pn = 0;
    for (int j = 0; j < sk; j++)
    {
        int64_t row = res[j].item_id;
        if (row < 0 || row >= cmd->n_vecs)
            continue;
        out[pn].tid      = tids[row];
        out[pn].distance = res[j].distance;
        pn++;
    }
    munmap(mem, total);
    free(res);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t lat = (uint32_t)((t1.tv_sec - t0.tv_sec) * 1000000 +
                              (t1.tv_nsec - t0.tv_nsec) / 1000);

    CuvsReplyHeader hdr = {0};
    hdr.status     = CUVS_STATUS_OK;
    hdr.n_results  = (uint32_t)pn;
    hdr.latency_us = lat;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (pn > 0)
        send_all(client_fd, out, (size_t)pn * sizeof(CuvsResult));
    free(out);
}

/* ----------------------------------------------------------------
 * Handle SEARCH command
 * ---------------------------------------------------------------- */
static void
handle_search(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

#ifdef CUVS_TEST_HOOKS
    /* 3S test seam: delay the reply (before taking the index lock so other
     * searches are unaffected) to exercise backend cancel + daemon disconnect. */
    {
        const char *d = getenv("CUVS_FAULT_SEARCH_DELAY_MS");
        long ms = (d && d[0]) ? atol(d) : 0;
        if (ms > 0)
        {
            LOG_INFO("[handle_search] fault: delaying reply %ld ms\n", ms);
            usleep((useconds_t) ms * 1000);
        }
    }
#endif

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        cache_counter_bump(g_cache_hits, e);
    }
    else
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid);
        if (load_rc == 0)
        {
            e = find_index(cmd->db_oid, cmd->index_oid);
            if (e)
            {
                cache_counter_bump(g_cache_reloads, e);
                cache_counter_bump(g_cache_misses, e);
            }
        }
        else
        {
            g_cache_misses[usable_gpu(0)]++;
        }

        if (!e)
        {
            /* Distinguish VRAM exhaustion (-2) from file-not-found (-1). */
            int reply_status;
            if (load_rc == -2)
            {
                reply_status = CUVS_STATUS_OOM_FALLBACK;
            }
            else
            {
                reply_status = CUVS_STATUS_NOT_FOUND;
                /* Phase 3D: check cold registry for GCS download. */
                if (g_snapshot_uri[0] != '\0')
                {
                    for (int i = 0; i < g_n_cold_indexes; i++)
                    {
                        ColdIndexEntry *ce = &g_cold_indexes[i];
                        if (ce->valid && ce->db_oid == cmd->db_oid &&
                            ce->index_oid == cmd->index_oid)
                        {
                            ce->cache_miss_count++;
                            if (ce->warmup_state == WARMUP_COLD ||
                                ce->warmup_state == WARMUP_FAILED)
                            {
                                ce->warmup_state = WARMUP_QUEUED;
                                warmup_enqueue(ce->db_oid, ce->index_oid,
                                               ce->relfilenode, ce->table_oid, 1);
                            }
                            break;
                        }
                    }
                }
            }

            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t)reply_status;
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
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

    /* Phase 3L: GPU brute-force (exact) search, unsharded. The resident BF index
     * over the `.vectors` sidecar is (re)built lazily at the requested precision.
     * If it is unavailable (sidecar missing / stale generation / won't fit) we
     * fail closed with NO_VECTORS so the backend raises a clear "REINDEX to
     * enable brute_force" error rather than silently returning ANN results.
     * Delta is NOT merged here — `.vectors` already covers the full base corpus
     * exactly. Sharded BF runs in the fanout block below.
     *
     * ADR-073: a flat index (e->is_flat) FORCES this brute-force path regardless
     * of cmd->search_mode. flat has handle == NULL, so the cagra dispatch below
     * would deref NULL; the backend's flat_gettuple already sends search_mode=1,
     * and this is the daemon-side defense-in-depth that also covers any other
     * caller (e.g. a batch path that did not set search_mode). */
    if ((cmd->search_mode == 1 || e->is_flat) && e->shard_count < 2)
    {
        /* Phase 3L-9: micro-batched path. When bf_batch_wait_us>0 and the batch
         * worker is up, hand this request to the worker instead of dispatching
         * inline: release g_index_mutex (lock order is index-then-bf), enqueue,
         * and block until the worker fills our result. The query shm stays mapped
         * (req.query points at it) until we reply. On a full queue we degrade to
         * the immediate path below by re-acquiring the lock and re-finding e. */
        if (cmd->bf_batch_wait_us > 0 && g_bf_worker_started)
        {
            CuvsBfKey     key  = { cmd->db_oid, cmd->index_oid, cmd->bf_precision, cmd->dim };
            CuvsResult   *rout = malloc((size_t) k * sizeof(CuvsResult));
            int           enqueued = 0;
            CuvsBfRequest req;

            pthread_mutex_unlock(&g_index_mutex);   /* release BEFORE taking g_bf_mtx */

            if (rout)
            {
                req.key     = key;   req.query  = query;  req.k = k;
                req.wait_us = cmd->bf_batch_wait_us;
                req.out     = rout;  req.n_out  = 0;
                req.status  = CUVS_STATUS_ERROR; req.done = 0;

                pthread_mutex_lock(&g_bf_mtx);
                if (g_bf_queue_n < CUVS_BF_BATCH_MAX)
                {
                    g_bf_queue[g_bf_queue_n++] = &req;
                    enqueued = 1;
                    pthread_cond_signal(&g_bf_cond);
                    while (!req.done)                       /* spurious-wakeup safe */
                        pthread_cond_wait(&g_bf_done_cond, &g_bf_mtx);
                }
                pthread_mutex_unlock(&g_bf_mtx);
            }

            if (enqueued)
            {
                munmap(query, vec_bytes);
                CuvsReplyHeader hdr = {0};
                hdr.status = (uint32_t) req.status;
                if (req.status == CUVS_STATUS_OK)
                {
                    hdr.n_results = (uint32_t) req.n_out;
                    send_all(client_fd, &hdr, sizeof(hdr));
                    if (req.n_out > 0)
                        send_all(client_fd, rout, (size_t) req.n_out * sizeof(CuvsResult));
                }
                else
                {
                    if (req.status == CUVS_STATUS_NO_VECTORS)
                        strncpy(hdr.error,
                                "brute_force requested but the .vectors sidecar is "
                                "missing or stale; REINDEX to enable it",
                                sizeof(hdr.error) - 1);
                    send_all(client_fd, &hdr, sizeof(hdr));
                }
                free(rout);
                return;
            }

            /* Could not enqueue (queue full / malloc fail): degrade to the
             * immediate path. Re-acquire the lock and re-find the entry. */
            free(rout);
            pthread_mutex_lock(&g_index_mutex);
            e = find_index(cmd->db_oid, cmd->index_oid);
            if (!e)
            {
                munmap(query, vec_bytes);
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr = {0};
                hdr.status = CUVS_STATUS_NOT_FOUND;
                send_all(client_fd, &hdr, sizeof(hdr));
                return;
            }
            /* fall through to the immediate path with e + the still-mapped query */
        }

        refresh_main_bf_cache(e, cmd->bf_precision);
        if (!e->main_bf_idx)
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* 3O: GPU BITSET prefilter path.
         * Converts filter TIDs → bitset via rev map, then calls cuVS filtered BF.
         * Falls through to D-wedge below on any allocation or search failure. */
        if (cmd->use_prefilter && cmd->n_filter_tids > 0 && e->rev_tids != NULL)
        {
            size_t    flt_bytes  = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
            void     *flt_mem    = MAP_FAILED;
            uint64_t *flt_tids   = NULL;
            uint32_t *bitset     = NULL;
            CuvsSearchResult *praw     = NULL;
            CuvsResult       *presults = NULL;
            int pn = 0, did_prefilter = 0, used_cagra = 0;

            if (cmd->filter_shm_key[0] != '\0')
            {
                int ffd = shm_open(cmd->filter_shm_key, O_RDONLY, 0);
                if (ffd >= 0) {
                    flt_mem = mmap(NULL, flt_bytes, PROT_READ, MAP_SHARED, ffd, 0);
                    close(ffd);
                    if (flt_mem != MAP_FAILED)
                        flt_tids = (uint64_t *) flt_mem;
                }
            }

            if (flt_tids) {
                int64_t  nv      = e->main_bf_n;
                uint32_t nwords  = (uint32_t)((nv + 31) / 32);
                bitset   = malloc((size_t)nwords * sizeof(uint32_t));
                praw     = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsSearchResult));
                presults = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsResult));

                if (bitset && praw && presults) {
                    /* pg_cuvs convention: bit=1 = exclude. Start all excluded;
                     * the cuVS wrapper inverts this to cuVS's bit=1 = include. */
                    memset(bitset, 0xFF, (size_t)nwords * sizeof(uint32_t));
                    /* clear bits for filter items (include them) */
                    for (uint32_t fi = 0; fi < cmd->n_filter_tids; fi++) {
                        uint64_t want = flt_tids[fi];
                        int64_t lo = 0, hi = nv - 1;
                        while (lo <= hi) {
                            int64_t mid = lo + (hi - lo) / 2;
                            if (e->rev_tids[mid] == want) {
                                int32_t iid = e->rev_item_ids[mid];
                                if (iid >= 0 && (int64_t)iid < nv)
                                    bitset[iid >> 5] &= ~(1u << (iid & 31));
                                break;
                            }
                            if (e->rev_tids[mid] < want) lo = mid + 1;
                            else                          hi = mid - 1;
                        }
                    }

                    /* CAGRA prefilter first (approx, faster); fall back to BF. */
                    int pret = 1;
                    if (e->handle != NULL) {
                        pret = cuvs_cagra_search_filtered(
                            e->handle, query, (int)cmd->dim, k,
                            bitset, nv, praw, e->gpu_device_id);
                        if (pret == 0) used_cagra = 1;
                    }
                    if (pret != 0 && e->main_bf_idx != NULL)
                        pret = cuvs_bf_search_filtered(
                            e->main_bf_idx, query, (int)cmd->dim, k,
                            bitset, nv, praw, delta_gpu_of(e));
                    if (pret == 0) {
                        did_prefilter = 1;
                        munmap(query, vec_bytes);  /* unmap before D-wedge path runs */
                        for (int i = 0; i < k; i++) {
                            int64_t id = praw[i].item_id;
                            if (id < 0 || id >= e->n_vecs) break;
                            presults[pn].tid      = e->tids[id];
                            presults[pn].distance = praw[i].distance;
                            pn++;
                        }
                    }
                }
            }

            free(bitset);
            free(praw);
            if (flt_mem != MAP_FAILED) munmap(flt_mem, flt_bytes);

            if (did_prefilter) {
                clock_gettime(CLOCK_MONOTONIC, &t1);
                uint32_t lat3o = (uint32_t)(
                    (t1.tv_sec - t0.tv_sec) * 1000000 +
                    (t1.tv_nsec - t0.tv_nsec) / 1000);
                record_search_stat(e, CUVS_STATUS_OK, lat3o, NULL);
                e->last_requested_k = cmd->k;
                e->last_returned_k  = (uint32_t) pn;
                e->last_search_mode = used_cagra ? 4 : 3; /* 4=cagra_prefilter, 3=gpu_bf_prefilter */
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr3o = {0};
                hdr3o.status     = CUVS_STATUS_OK;
                hdr3o.n_results  = (uint32_t) pn;
                hdr3o.latency_us = lat3o;
                send_all(client_fd, &hdr3o, sizeof(hdr3o));
                if (pn > 0)
                    send_all(client_fd, presults, (size_t) pn * sizeof(CuvsResult));
                free(presults);
                return;
            }
            free(presults);
            /* 3O failed (search error or malloc); fall through to D-wedge. */
        }

        /* D-wedge: overfetch 4x when post-filtering so top-k survive. */
        int64_t bk_target = (cmd->n_filter_tids > 0)
            ? (int64_t)k * 4
            : (int64_t)k;
        int bk = (int)(bk_target < e->main_bf_n ? bk_target : e->main_bf_n);
        CuvsSearchResult *raw     = malloc((size_t)(bk > 0 ? bk : 1) * sizeof(CuvsSearchResult));
        CuvsResult       *results = malloc((size_t)(k  > 0 ? k  : 1) * sizeof(CuvsResult));
        if (!raw || !results)
        {
            free(raw); free(results);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        int ret = (bk > 0)
            ? cuvs_bf_search(e->main_bf_idx, query, (int) cmd->dim, bk, raw, delta_gpu_of(e))
            : 0;
        munmap(query, vec_bytes);
        if (ret != 0)
        {
            free(raw); free(results);
            CuvsReplyHeader hdr = {0};
            hdr.status = (ret == 2) ? CUVS_STATUS_DIM_MISMATCH : CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error,
                    (ret == 2) ? "query dim != index dim" : "brute_force search failed",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, hdr.status, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* D-wedge spike: open filter shm and mmap the sorted TID array. */
        uint64_t *filter_tids = NULL;
        void     *filter_mem  = MAP_FAILED;
        size_t    filter_bytes = 0;
        if (cmd->n_filter_tids > 0 && cmd->filter_shm_key[0] != '\0')
        {
            filter_bytes = (size_t)cmd->n_filter_tids * sizeof(uint64_t);
            int ffd = shm_open(cmd->filter_shm_key, O_RDONLY, 0);
            if (ffd >= 0)
            {
                filter_mem = mmap(NULL, filter_bytes, PROT_READ, MAP_SHARED, ffd, 0);
                close(ffd);
                if (filter_mem != MAP_FAILED)
                    filter_tids = (uint64_t *)filter_mem;
            }
            /* On shm failure: fall through to unfiltered (spike graceful degrade). */
        }

        int n_valid = 0;
        for (int i = 0; i < bk && n_valid < k; i++)
        {
            int64_t id = raw[i].item_id;
            if (id < 0 || id >= e->n_vecs) continue;
            uint64_t tid = e->tids[id];
            if (filter_tids)
            {
                uint32_t lo = 0, hi = cmd->n_filter_tids;
                int found = 0;
                while (lo < hi)
                {
                    uint32_t mid = lo + (hi - lo) / 2;
                    if (filter_tids[mid] == tid) { found = 1; break; }
                    if (filter_tids[mid] < tid)  lo = mid + 1;
                    else                          hi = mid;
                }
                if (!found) continue;
            }
            results[n_valid].tid      = tid;
            results[n_valid].distance = raw[i].distance;
            n_valid++;
        }
        free(raw);
        if (filter_mem != MAP_FAILED)
            munmap(filter_mem, filter_bytes);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t latency_us = (uint32_t)(
            (t1.tv_sec - t0.tv_sec) * 1000000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000);

        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
        e->last_requested_k = cmd->k;
        e->last_returned_k  = (uint32_t) n_valid;
        e->last_search_mode = 3; /* gpu_bf */

        pthread_mutex_unlock(&g_index_mutex);

        CuvsReplyHeader hdr = {0};
        hdr.status       = CUVS_STATUS_OK;
        hdr.n_results    = (uint32_t) n_valid;
        hdr.latency_us   = latency_us;
        hdr.delta_merged = 0;
        send_all(client_fd, &hdr, sizeof(hdr));
        if (n_valid > 0)
            send_all(client_fd, results, (size_t) n_valid * sizeof(CuvsResult));
        free(results);
        return;
    }

    /* Phase 3F/3G: sharded fanout. Search every shard for its top-(k+overfetch),
     * map each shard-local id to a global TID via the shard's tid_offset, merge
     * all candidates by distance (metric-aware), and return the global top-k.
     * One IPC request per logical index; the reply format is unchanged. Phase 3G
     * dispatches the per-shard searches concurrently (one pthread per shard) when
     * cuvs.parallel_fanout is on, cutting latency from sum(shard_i) toward
     * max(shard_i); off runs them sequentially (A/B baseline / kill switch).
     * cuvs.shard_overfetch widens each shard's request for recall at scale.
     * Delta is left to the backend CPU merge (delta_merged=0). Fail closed: any
     * shard search failure returns OOM_FALLBACK (CPU), never a partial result. */
    if (e->shard_count >= 2)
    {
        int sc        = e->shard_count;
        int overfetch = (int) cmd->shard_overfetch;
        int eff_k     = k + overfetch;
        if (eff_k < k) eff_k = k;            /* guard against overflow */
        int parallel  = (cmd->parallel_fanout != 0);
        int use_bf    = (cmd->search_mode == 1);   /* Phase 3L: brute_force over shards */
        uint32_t snap_db  = e->db_oid;
        uint32_t snap_idx = e->index_oid;

        /* Phase 3L: brute_force over a sharded index — ensure every shard has a
         * resident BF index over its range. Fail closed (NO_VECTORS) if the
         * global .vectors sidecar is missing/stale, exactly like the unsharded
         * path. Done under the lock before the in-flight window opens. */
        if (use_bf && !refresh_shard_bf_caches(e, cmd->bf_precision))
        {
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        ShardSearchArg *args    = calloc((size_t) sc, sizeof(ShardSearchArg));
        pthread_t      *threads = calloc((size_t) sc, sizeof(pthread_t));
        if (!args || !threads)
        {
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Snapshot each shard's descriptor and allocate its result buffer, then
         * drop the lock for the GPU dispatch. The shard handles and the `tids`
         * block stay valid lock-free by construction (ADR-022): sharded entries
         * are non-evictable, and same-index REINDEX/DROP is serialized against
         * this search by PostgreSQL's AccessExclusiveLock, so nothing frees them
         * mid-search. We must NOT reuse `e` after unlocking, though: a concurrent
         * eviction of some other index compacts g_indexes[] by value and can move
         * this struct — so we re-find by OID after the parallel window. */
        int64_t total_sk = 0;
        int     alloc_ok = 1;
        for (int s = 0; s < sc; s++)
        {
            ShardEntry *se = &e->shards[s];
            int sk = (int) (se->n_vecs < (int64_t) eff_k ? se->n_vecs : eff_k);
            if (sk < 0) sk = 0;
            args[s].handle        = se->handle;
            args[s].bf_idx        = se->bf_idx;   /* Phase 3L: NULL unless use_bf */
            args[s].use_bf        = use_bf;
            args[s].query         = query;
            args[s].dim           = (int) cmd->dim;
            args[s].sk            = sk;
            args[s].gpu_device_id = se->gpu_device_id;
            args[s].tid_offset    = se->tid_offset;
            args[s].shard_n_vecs  = se->n_vecs;
            args[s].rc            = 0;       /* skipped (sk==0) shards = OK / no candidates */
            if (sk > 0)
            {
                args[s].out = malloc((size_t) sk * sizeof(CuvsSearchResult));
                if (!args[s].out) { alloc_ok = 0; break; }
                total_sk += sk;
            }
        }
        if (!alloc_ok)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Phase 3G.4: mark this entry in-flight so a concurrent eviction (now
         * that sharded indexes are evictable) cannot free our shard handles
         * while we search them lock-free. Decremented after the re-lock below. */
        e->inflight++;

        pthread_mutex_unlock(&g_index_mutex);   /* ---- lock-free GPU dispatch ---- */

        /* Parallel spawns one worker per non-empty shard; sequential runs them
         * inline (cuvs.parallel_fanout = off). A pthread_create failure degrades
         * that shard to inline execution (no leak, still correct). */
        for (int s = 0; s < sc; s++)
        {
            if (args[s].sk <= 0) continue;
            if (parallel &&
                pthread_create(&threads[s], NULL, shard_search_worker, &args[s]) == 0)
                args[s].threaded = 1;
            else
                shard_search_worker(&args[s]);
        }
        for (int s = 0; s < sc; s++)
            if (args[s].threaded)
                pthread_join(threads[s], NULL);

        /* Keep `query` mapped past the join: the GPU delta search below
         * (Phase 3G.3) still reads it; munmap after that. Re-acquire the lock to
         * update counters, refresh+search the delta cache, and merge. Re-find by
         * OID since the struct may have moved (eviction compaction) while we were
         * unlocked. The merge (g_merge_metric + qsort) and the small delta BF
         * search run here under the lock; only the base shard fanout was lock-free. */
        pthread_mutex_lock(&g_index_mutex);
        e = find_index(snap_db, snap_idx);
        if (!e)
        {
            /* Cannot happen for the supported workload (ADR-022 invariant);
             * fail closed to CPU rather than risk touching freed memory. */
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error, "sharded index vanished mid-search; CPU fallback",
                    sizeof(hdr.error) - 1);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* Lock-free window closed: we hold the mutex again and the entry can't be
         * evicted under us now, so drop the in-flight refcount here (covers every
         * exit path below). */
        if (e->inflight > 0) e->inflight--;

        /* Record per-shard outcomes; any shard failure fails the whole query
         * closed (CPU fallback), never a partial ANN result. */
        int shard_err = 0;
        for (int s = 0; s < sc; s++)
        {
            if (args[s].rc != 0)
            {
                e->shards[s].error_count++;
                e->shards[s].last_status = CUVS_STATUS_OOM_FALLBACK;
                shard_err = 1;
            }
            else if (args[s].sk > 0)
            {
                e->shards[s].search_count++;
                e->shards[s].last_status = CUVS_STATUS_OK;
            }
        }

        if (shard_err)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads);
            munmap(query, vec_bytes);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error, "sharded search failed on a shard; CPU fallback",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }

        /* cand holds base candidates (<= total_sk) plus up to k delta candidates. */
        CuvsResult *cand = malloc((size_t) (total_sk + (int64_t) k) * sizeof(CuvsResult));
        CuvsResult *out  = malloc((size_t) k * sizeof(CuvsResult));
        if (!cand || !out)
        {
            for (int s = 0; s < sc; s++) free(args[s].out);
            free(args); free(threads); free(cand); free(out);
            munmap(query, vec_bytes);
            pthread_mutex_unlock(&g_index_mutex);
            send_error(client_fd, "malloc failed");
            return;
        }

        /* Collect candidates, mapping each shard-local id to a global TID. Uses
         * the snapshot tid_offset/n_vecs (stable per the invariant) and the
         * re-found entry's tids/n_vecs. */
        int n_total = 0;
        for (int s = 0; s < sc; s++)
        {
            int64_t toff = args[s].tid_offset;
            for (int j = 0; j < args[s].sk; j++)
            {
                int64_t id = args[s].out[j].item_id;
                if (id < 0 || id >= args[s].shard_n_vecs) continue;
                int64_t gid = toff + id;
                if (gid < 0 || gid >= e->n_vecs) continue;
                cand[n_total].tid      = e->tids[gid];
                cand[n_total].distance = args[s].out[j].distance;
                n_total++;
            }
            free(args[s].out);
        }
        free(args); free(threads);

        /* Phase 3G.3: shard-aware GPU delta cache. Refresh the resident delta
         * brute-force index (on shard 0's GPU) from the current `.delta`; if
         * present, search it and fold its candidates into the same global merge,
         * so the daemon serves INSERT/UPDATE rows on the GPU path (delta_merged=1)
         * instead of forcing the backend CPU merge. If the cache is unavailable
         * (no VRAM / corrupt / generation mismatch), delta_merged stays 0 and the
         * backend CPU-merges as before — fail-open to correctness. */
        int delta_merged = 0;
        /* Phase 3L: brute_force already searches the full base corpus exactly via
         * the per-shard .vectors caches; the pending .delta is a separate concern
         * and is NOT merged in BF mode (consistent with the unsharded BF path). */
        if (!use_bf)
            refresh_delta_cache(e);
        if (!use_bf && e->delta_idx && e->n_delta > 0)
        {
            int eff_dk = (int) ((int64_t) k < e->n_delta ? (int64_t) k : e->n_delta);
            CuvsSearchResult *draw = malloc((size_t) eff_dk * sizeof(CuvsSearchResult));
            if (draw && cuvs_bf_search(e->delta_idx, query, (int) cmd->dim, eff_dk,
                                       draw, delta_gpu_of(e)) == 0)
            {
                for (int i = 0; i < eff_dk; i++)
                {
                    int64_t id = draw[i].item_id;
                    if (id < 0 || id >= e->n_delta) continue;
                    cand[n_total].tid      = e->delta_tids[id];
                    cand[n_total].distance = draw[i].distance;
                    n_total++;
                }
                delta_merged = 1;
                e->delta_merged_count++;
            }
            free(draw);
        }
        munmap(query, vec_bytes);            /* base + delta searches done with query */

        g_merge_metric = e->metric;          /* read by delta_cand_cmp under the lock */
        qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
        int n_valid = (n_total < k) ? n_total : k;
        memcpy(out, cand, (size_t) n_valid * sizeof(CuvsResult));
        free(cand);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        uint32_t latency_us = (uint32_t)(
            (t1.tv_sec - t0.tv_sec) * 1000000 +
            (t1.tv_nsec - t0.tv_nsec) / 1000);
        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
        e->last_requested_k = cmd->k;
        e->last_returned_k  = (uint32_t) n_valid;
        e->last_search_mode = use_bf ? 3 : 0;   /* Phase 3L: 3=gpu_bf, 0=gpu_cagra */
        pthread_mutex_unlock(&g_index_mutex);

        CuvsReplyHeader hdr = {0};
        hdr.status       = CUVS_STATUS_OK;
        hdr.n_results    = (uint32_t) n_valid;
        hdr.latency_us   = latency_us;
        hdr.delta_merged = (uint32_t) delta_merged;
        send_all(client_fd, &hdr, sizeof(hdr));
        if (n_valid > 0)
            send_all(client_fd, out, (size_t) n_valid * sizeof(CuvsResult));
        free(out);
        return;
    }

    /* Phase 3I-1: CPU HNSW path — bypass GPU CAGRA when requested. */
    if (cmd->use_cpu_hnsw)
    {
        /* Lazy-load the .hnsw sidecar into RAM (held in e->hnsw_idx). */
        if (!e->hnsw_idx)
        {
            char hp[512];
            hnsw_file_path(hp, sizeof(hp), g_index_dir, e->db_oid, e->index_oid);
            e->hnsw_idx = cuvs_hnsw_deserialize(hp, (int)e->dim, e->metric,
                                                 (int)e->gpu_device_id);
            if (!e->hnsw_idx)
                LOG_WARN("[handle_search] HNSW sidecar not loadable for %u/%u; "
                         "using GPU CAGRA\n", e->db_oid, e->index_oid);
        }
        if (e->hnsw_idx)
        {
            CuvsSearchResult *hraw = malloc((size_t)k * sizeof(CuvsSearchResult));
            if (!hraw)
            {
                munmap(query, vec_bytes);
                pthread_mutex_unlock(&g_index_mutex);
                send_error(client_fd, "malloc failed");
                return;
            }
            int hret = cuvs_hnsw_search(e->hnsw_idx, query, (int)cmd->dim,
                                         k, 0, hraw);
            if (hret == 0)
            {
                CuvsResult *hresults = malloc((size_t)k * sizeof(CuvsResult));
                int hn_valid = 0;
                if (hresults)
                {
                    for (int i = 0; i < k; i++)
                    {
                        int64_t id = hraw[i].item_id;
                        if (id < 0 || id >= e->n_vecs) continue;
                        hresults[hn_valid].tid      = e->tids[id];
                        hresults[hn_valid].distance = hraw[i].distance;
                        hn_valid++;
                    }
                }
                free(hraw);
                munmap(query, vec_bytes);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                uint32_t hlat = (uint32_t)((t1.tv_sec  - t0.tv_sec)  * 1000000 +
                                           (t1.tv_nsec - t0.tv_nsec) / 1000);
                record_search_stat(e, CUVS_STATUS_OK, hlat, NULL);
                e->last_requested_k  = cmd->k;
                e->last_returned_k   = (uint32_t)hn_valid;
                e->last_search_mode  = 1; /* cpu_hnsw */
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr = {0};
                hdr.status     = CUVS_STATUS_OK;
                hdr.n_results  = (uint32_t)hn_valid;
                hdr.latency_us = hlat;
                send_all(client_fd, &hdr, sizeof(hdr));
                if (hn_valid > 0 && hresults)
                    send_all(client_fd, hresults,
                             (size_t)hn_valid * sizeof(CuvsResult));
                free(hresults);
                return;
            }
            /* HNSW search failed -> fall through to GPU CAGRA. */
            free(hraw);
            LOG_WARN("[handle_search] cuvs_hnsw_search failed for %u/%u; "
                     "retrying with GPU CAGRA\n", e->db_oid, e->index_oid);
        }
    }

    CuvsSearchResult *raw = malloc(k * sizeof(CuvsSearchResult));
    if (!raw)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    /* ADR-073 defense-in-depth: the cagra dispatch requires a non-NULL graph
     * handle. A flat index (is_flat, handle==NULL) must never reach here — the
     * brute-force gate above returns first — but guard structurally so any future
     * path landing here with handle==NULL fails closed instead of dereferencing. */
    if (e->handle == NULL)
    {
        free(raw);
        munmap(query, vec_bytes);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NO_VECTORS;
        strncpy(hdr.error,
                "index has no CAGRA graph (flat/brute-force only)",
                sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    LOG_DEBUG("[handle_search] calling cuvs_cagra_search k=%d dim=%u...\n",
            k, cmd->dim);
    int ret = cuvs_cagra_search(e->handle, query, (int)cmd->dim, k, raw, e->gpu_device_id);
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
            && cuvs_bf_search(e->delta_idx, query, (int) cmd->dim, eff_k, draw, e->gpu_device_id) == 0)
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
            e->delta_merged_count++;
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
    e->last_search_mode = 0; /* gpu_cagra */

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
 * Handle SEARCH_BATCH command (CUVS_OP_SEARCH_BATCH, Phase 3M)
 *
 * Q queries arrive in one request shm ([Q][dim][Q*dim f32]); the daemon runs
 * one batched GPU dispatch (cuvs_*_search_batch) for unsharded indexes, or a
 * per-query shard fanout+merge for sharded indexes, and returns Q*K results
 * (K = min(k, n_vecs)) in a daemon-allocated reply shm
 * ([Q][K][tids Q*K][dists Q*K]) whose key travels back in reply.error[]. The
 * single-query handle_search path is unchanged. Held under g_index_mutex for
 * the whole call (no lock-free window): correctness over latency for batches.
 * ---------------------------------------------------------------- */
static void
handle_search_batch(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    /* --- Resolve the index (find or reload), mirroring handle_search. --- */
    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        cache_counter_bump(g_cache_hits, e);
    }
    else
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid);
        if (load_rc == 0)
            e = find_index(cmd->db_oid, cmd->index_oid);
        if (!e)
        {
            int st = (load_rc == -2) ? CUVS_STATUS_OOM_FALLBACK : CUVS_STATUS_NOT_FOUND;
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr = {0};
            hdr.status = (uint32_t) st;
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        cache_counter_bump(g_cache_reloads, e);
        cache_counter_bump(g_cache_misses, e);
    }
    e->last_search = time(NULL);

    /* --- Stale / metric / dim gates (same statuses as handle_search). --- */
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
    if (cmd->dim != e->dim)
    {
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        record_search_stat(e, CUVS_STATUS_DIM_MISMATCH, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    uint32_t Q      = (uint32_t) cmd->n_vecs;   /* SEARCH_BATCH: Q in n_vecs */
    int      dim    = (int) cmd->dim;
    int      k      = (int) cmd->k;
    /* ADR-073 FATAL fix: a flat index (handle==NULL) MUST use brute-force here.
     * Without the e->is_flat term a flat batch search falls to the else branch
     * below — cuvs_cagra_search_batch(e->handle, ...) — and dereferences NULL,
     * crashing the daemon for every backend. */
    int      use_bf = (cmd->search_mode == 1 || e->is_flat);
    int      K      = (int) ((int64_t) k < e->n_vecs ? (int64_t) k : e->n_vecs);

    if (Q == 0 || K <= 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "empty batch or empty index");
        return;
    }

    /* --- Map the request shm: [Q][dim][Q*dim f32]. --- */
    size_t qhdr_bytes = 2 * sizeof(uint32_t);
    size_t qtotal     = qhdr_bytes + (size_t) Q * (size_t) dim * sizeof(float);
    int    qfd        = shm_open(cmd->shm_key, O_RDONLY, 0666);
    if (qfd < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed for batch queries");
        return;
    }
    void *qmem = mmap(NULL, qtotal, PROT_READ, MAP_SHARED, qfd, 0);
    close(qfd);
    if (qmem == MAP_FAILED)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed for batch queries");
        return;
    }
    const float *queries = (const float *) ((const char *) qmem + qhdr_bytes);

    /* --- brute_force: ensure the resident BF cache(s); fail closed if absent. --- */
    if (use_bf)
    {
        int ready;
        if (e->shard_count >= 2)
            ready = refresh_shard_bf_caches(e, cmd->bf_precision);
        else
        {
            refresh_main_bf_cache(e, cmd->bf_precision);
            ready = (e->main_bf_idx != NULL);
        }
        if (!ready)
        {
            munmap(qmem, qtotal);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_NO_VECTORS;
            strncpy(hdr.error,
                    "brute_force requested but the .vectors sidecar is missing or "
                    "stale; REINDEX to enable it",
                    sizeof(hdr.error) - 1);
            record_search_stat(e, CUVS_STATUS_NO_VECTORS, 0, hdr.error);
            pthread_mutex_unlock(&g_index_mutex);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
    }

    /* --- Run the batched search into out[Q*K] (global TIDs). --- */
    CuvsResult *out = malloc((size_t) Q * (size_t) K * sizeof(CuvsResult));
    if (!out)
    {
        munmap(qmem, qtotal);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }
    int ok = 1;

    if (e->shard_count < 2)
    {
        /* Unsharded: one GPU dispatch for the whole batch. */
        CuvsSearchResult *raw = malloc((size_t) Q * (size_t) K * sizeof(CuvsSearchResult));
        if (!raw)
        {
            ok = 0;
        }
        else
        {
            int ret = use_bf
                ? cuvs_bf_search_batch(e->main_bf_idx, queries, (int) Q, dim, K, raw, delta_gpu_of(e))
                : cuvs_cagra_search_batch(e->handle, queries, (int) Q, dim, K, raw, e->gpu_device_id);
            if (ret != 0)
                ok = 0;
            else
                for (size_t i = 0; i < (size_t) Q * (size_t) K; i++)
                {
                    int64_t id = raw[i].item_id;
                    out[i].tid      = (id < 0 || id >= e->n_vecs) ? 0 : e->tids[id];
                    out[i].distance = raw[i].distance;
                }
            free(raw);
        }
    }
    else
    {
        /* Sharded: per-query fanout + global merge, sequential under the lock. */
        int sc        = e->shard_count;
        int overfetch = (int) cmd->shard_overfetch;
        int eff_k     = k + overfetch;
        if (eff_k < k) eff_k = k;
        CuvsSearchResult *sraw = malloc((size_t) eff_k * sizeof(CuvsSearchResult));
        CuvsResult       *cand = malloc((size_t) sc * (size_t) eff_k * sizeof(CuvsResult));
        if (!sraw || !cand)
        {
            ok = 0;
        }
        else
        {
            for (uint32_t q = 0; q < Q && ok; q++)
            {
                const float *query = queries + (size_t) q * dim;
                int n_total = 0;
                for (int s = 0; s < sc; s++)
                {
                    ShardEntry *sh = &e->shards[s];
                    int sk = (int) (sh->n_vecs < (int64_t) eff_k ? sh->n_vecs : eff_k);
                    if (sk <= 0) continue;
                    int ret = use_bf
                        ? cuvs_bf_search(sh->bf_idx, query, dim, sk, sraw, sh->gpu_device_id)
                        : cuvs_cagra_search(sh->handle, query, dim, sk, sraw, sh->gpu_device_id);
                    if (ret != 0) { ok = 0; break; }
                    for (int j = 0; j < sk; j++)
                    {
                        int64_t id = sraw[j].item_id;
                        if (id < 0 || id >= sh->n_vecs) continue;
                        int64_t gid = sh->tid_offset + id;
                        if (gid < 0 || gid >= e->n_vecs) continue;
                        cand[n_total].tid      = e->tids[gid];
                        cand[n_total].distance = sraw[j].distance;
                        n_total++;
                    }
                }
                if (!ok) break;
                g_merge_metric = e->metric;
                qsort(cand, (size_t) n_total, sizeof(CuvsResult), delta_cand_cmp);
                int nv = (n_total < K) ? n_total : K;
                for (int j = 0; j < K; j++)
                {
                    if (j < nv)
                        out[(size_t) q * K + j] = cand[j];
                    else
                    {
                        out[(size_t) q * K + j].tid      = 0;
                        out[(size_t) q * K + j].distance = 3.402823466e+38f;
                    }
                }
            }
        }
        free(sraw);
        free(cand);
    }

    munmap(qmem, qtotal);

    if (!ok)
    {
        free(out);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "batch search failed on the GPU; retry on CPU",
                sizeof(hdr.error) - 1);
        record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, hdr.error);
        pthread_mutex_unlock(&g_index_mutex);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t) (
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);
    record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t) K;
    e->last_search_mode = use_bf ? 3 : 0;

    /* --- Allocate the reply shm: [Q][K][tids Q*K][dists Q*K]. --- */
    static int bsr_seq = 0;
    char rkey[64];
    snprintf(rkey, sizeof(rkey), "/pg_cuvs_bsr_%d_%d",
             (int) getpid(), __atomic_fetch_add(&bsr_seq, 1, __ATOMIC_RELAXED));
    size_t rhdr   = 2 * sizeof(uint32_t);
    size_t rtids  = (size_t) Q * (size_t) K * sizeof(uint64_t);
    size_t rdists = (size_t) Q * (size_t) K * sizeof(float);
    size_t rtotal = rhdr + rtids + rdists;

    int rfd = shm_open(rkey, O_CREAT | O_RDWR, 0666);
    if (rfd < 0)
    {
        free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "shm_open failed for batch reply");
        return;
    }
    fchmod(rfd, 0666);
    if (ftruncate(rfd, (off_t) rtotal) != 0)
    {
        close(rfd); shm_unlink(rkey); free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "ftruncate failed for batch reply");
        return;
    }
    void *rmem = mmap(NULL, rtotal, PROT_WRITE, MAP_SHARED, rfd, 0);
    close(rfd);
    if (rmem == MAP_FAILED)
    {
        shm_unlink(rkey); free(out);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "mmap failed for batch reply");
        return;
    }
    ((uint32_t *) rmem)[0] = Q;
    ((uint32_t *) rmem)[1] = (uint32_t) K;
    {
        uint64_t *tp = (uint64_t *) ((char *) rmem + rhdr);
        float    *dp = (float *)    ((char *) rmem + rhdr + rtids);
        for (size_t i = 0; i < (size_t) Q * (size_t) K; i++)
        {
            tp[i] = out[i].tid;
            dp[i] = out[i].distance;
        }
    }
    munmap(rmem, rtotal);
    free(out);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status       = CUVS_STATUS_OK;
    hdr.n_results    = Q;
    hdr.latency_us   = latency_us;
    hdr.delta_merged = (uint32_t) K;   /* per-query result stride */
    strncpy(hdr.error, rkey, sizeof(hdr.error) - 1);
    send_all(client_fd, &hdr, sizeof(hdr));
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

/* Phase 3F: streaming CRC-32 of a whole file (chunked; never loads the entire
 * artifact into RAM). Returns 0 and sets *out on success, -1 on open/read error. */
static int
crc32_file(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    uint32_t crc = cuvs_crc32_stream_begin();
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc = cuvs_crc32_stream_update(crc, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err)
        return -1;
    *out = cuvs_crc32_stream_end(crc);
    return 0;
}

/* Phase 3F: free all GPU shard handles of a sharded entry and the shards array.
 * Caller holds g_index_mutex. Leaves shard_count=0/shards=NULL so the slot can
 * be reused as an unsharded entry. Does NOT free e->tids (caller owns that). */
static void
free_index_shards(IndexEntry *e)
{
    if (!e->shards)
    {
        e->shard_count = 0;
        return;
    }
    for (int s = 0; s < e->shard_count; s++)
    {
        if (e->shards[s].valid && e->shards[s].handle)
            cuvs_cagra_free(e->shards[s].handle, e->shards[s].gpu_device_id);
        /* Phase 3L: release this shard's resident brute-force index, if any. */
        if (e->shards[s].bf_idx)
            cuvs_bf_free(e->shards[s].bf_idx, e->shards[s].gpu_device_id);
    }
    free(e->shards);
    e->shards = NULL;
    e->shard_count = 0;
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
    uint32_t base_generation;   /* this build's .tids body_crc32 (3C manifest) */
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
        a->base_generation   /* this build's .tids body_crc32 */
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

/* Phase 3G.2: sharded variant — uploads the whole artifact set (.tids + .shards
 * + N .sNNN.cagra) located under index_dir. Detached + non-fatal, like the
 * unsharded path above. */
typedef struct {
    char     snapshot_uri[512];
    char     cluster_id[128];
    char     gcs_key_file[512];
    char     index_dir[512];
    uint32_t db_oid;
    uint32_t table_oid;
    uint32_t index_oid;
    uint32_t relfilenode;
    uint32_t metric;
    uint32_t dim;
    int64_t  vector_count;
    uint32_t shard_count;
    uint32_t base_generation;   /* this build's .tids body_crc32 (3C manifest) */
} ShardedUploadArgs;

static void *
objstore_upload_sharded_thread(void *arg)
{
    ShardedUploadArgs *a = (ShardedUploadArgs *)arg;
    int rc = cuvs_objstore_upload_sharded(
        a->snapshot_uri, a->cluster_id, a->gcs_key_file, a->index_dir,
        a->db_oid, a->table_oid, a->index_oid, a->relfilenode,
        a->metric, a->dim, a->vector_count,
        a->base_generation,   /* this build's .tids body_crc32 */
        a->shard_count);
    if (rc == 0)
        LOG_INFO("objstore: uploaded sharded index %u/%u (%u shards) to %s\n",
                 a->db_oid, a->index_oid, a->shard_count, a->snapshot_uri);
    else
        LOG_WARN("objstore: sharded upload FAILED for %u/%u (non-fatal, local artifacts intact)\n",
                 a->db_oid, a->index_oid);
    free(a);
    return NULL;
}

/* ----------------------------------------------------------------
 * Phase 3F: sharded BUILD path (cuvs.shard_count >= 2).
 *
 * Splits the corpus into `shard_count` contiguous build-order ranges, builds a
 * standalone CAGRA artifact per shard placed round-robin across usable GPUs,
 * and commits all-or-nothing: every shard `.cagra.tmp` + global `.tids.tmp` +
 * `.shards.tmp` are fsynced, then renamed (shard cagras, then .tids, then the
 * `.shards` manifest LAST as the commit marker). Self-contained: owns `mem`
 * (munmap) and the client reply. Fail-closed: any failure rolls back all
 * artifacts and resources and returns an error (CREATE INDEX/REINDEX aborts).
 *
 * Builds into LOCAL arrays and registers the entry only on full success — this
 * avoids holding an IndexEntry* across ensure_vram, whose eviction can shift
 * g_indexes[] and invalidate the pointer.
 * ---------------------------------------------------------------- */
static void
build_sharded(int client_fd, const CuvsCmdFrame *cmd, const char *index_dir,
              const float *vecs, const uint64_t *tids_in,
              size_t total, void *mem, int shard_count)
{
    int64_t  n_vecs    = cmd->n_vecs;
    uint32_t dim       = cmd->dim;
    uint32_t metric    = cmd->metric;
    size_t   tid_bytes = (size_t)n_vecs * sizeof(uint64_t);

    /* Clamp shard count so every shard holds >= 2 vectors (CAGRA aborts on 1). */
    int sc = shard_count;
    if (sc > CUVS_SHARDS_MAX)
        sc = CUVS_SHARDS_MAX;
    if ((int64_t)sc > n_vecs / 2)
        sc = (int)(n_vecs / 2);
    if (sc < 2)
    {
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED,
                        "shard_count too high for corpus (need >= 2 vectors per shard)");
        return;
    }
    if (sc != shard_count)
        LOG_WARN("[build_sharded] clamped shard_count %d -> %d for %lld vecs\n",
                 shard_count, sc, (long long)n_vecs);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Host copy of the global TID array (shared by all shards), taken before
     * we drop the shm mapping. */
    uint64_t        *new_tids    = malloc(tid_bytes);
    ShardEntry      *shards      = calloc((size_t)sc, sizeof(ShardEntry));
    CuvsShardRecord *recs        = calloc((size_t)sc, sizeof(CuvsShardRecord));
    char           (*shard_tmp)[576]   = malloc((size_t)sc * sizeof(*shard_tmp));
    char           (*shard_final)[512] = malloc((size_t)sc * sizeof(*shard_final));
    if (!new_tids || !shards || !recs || !shard_tmp || !shard_final)
    {
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "sharded build alloc failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    pthread_mutex_lock(&g_index_mutex);

    /* --- Build + place each shard over a contiguous range. --- */
    int64_t base   = n_vecs / sc;
    int64_t rem    = n_vecs % sc;
    int64_t off    = 0;
    int     n_gpus = n_usable_gpus();
    int     ok     = 1;
    int     i      = 0;
    int     n_reserved = 0;   /* ADR-070 Bug #2: shards with an active VRAM reservation */
    for (i = 0; i < sc; i++)
    {
        int64_t shard_n = base + (i < rem ? 1 : 0);
        size_t  needed  = estimate_vram_bytes(shard_n, (int)dim);
        int     dev     = usable_gpu(i % n_gpus);   /* round-robin spread */

        shard_cagra_path(shard_final[i], 512, save_dir,
                         cmd->db_oid, cmd->index_oid, (uint32_t)i);
        snprintf(shard_tmp[i], 576, "%s.tmp", shard_final[i]);

        if (ensure_vram(needed, dev) != 0)
        {
            int alt = pick_gpu_for_index(needed);
            if (alt < 0 || ensure_vram(needed, alt) != 0)
            {
                LOG_WARN("[build_sharded] shard %d (%lld vecs) won't fit on any GPU\n",
                         i, (long long)shard_n);
                ok = 0;
                break;
            }
            dev = alt;
        }

        CuvsCagraIndex h = cuvs_cagra_build(vecs + (size_t)off * dim, shard_n,
                                            (int)dim, metric,
                                            (int)cmd->graph_degree,
                                            (int)cmd->intermediate_graph_degree,
                                            cmd->build_algo, dev);
        if (!h)
        {
            LOG_ERROR("[build_sharded] cuvs_cagra_build failed for shard %d\n", i);
            ok = 0;
            break;
        }

        shards[i].shard_id      = (uint32_t)i;
        shards[i].handle        = h;
        shards[i].tid_offset    = off;
        shards[i].n_vecs        = shard_n;
        shards[i].gpu_device_id = (uint32_t)dev;
        shards[i].vram_bytes    = needed;
        shards[i].valid         = 1;

        if (cuvs_cagra_serialize(h, shard_tmp[i], dev) != 0
            || fsync_path(shard_tmp[i]) != 0)
        {
            LOG_ERROR("[build_sharded] serialize/fsync failed for shard %d\n", i);
            ok = 0;
            break;
        }

        uint32_t acrc = 0;
        if (crc32_file(shard_tmp[i], &acrc) != 0)
        {
            ok = 0;
            break;
        }
        recs[i].shard_id       = (uint32_t)i;
        recs[i].gpu_device_id  = (uint32_t)dev;
        recs[i].tid_offset     = off;
        recs[i].n_vecs         = shard_n;
        recs[i].dim            = dim;
        recs[i].metric         = metric;
        recs[i].artifact_crc32 = acrc;
        recs[i].reserved       = 0;

        off += shard_n;
    }

    /* ADR-070 Bug #2: builds done (or aborted) — release every shard reservation.
     * The lock is held continuously from here through the registry insert below,
     * so the shards' VRAM is accounted by the registry entry (success) or freed
     * (error paths) at the next unlock; no reservation must outlive this point.
     * shards[r].gpu_device_id/vram_bytes were set before each build, so this also
     * covers a shard whose build failed. */
    for (int r = 0; r < n_reserved; r++)
        g_pending_build_vram[shards[r].gpu_device_id] -= shards[r].vram_bytes;
    n_reserved = 0;

    /* Phase 3L: persist one global `.vectors` sidecar (same build-order layout
     * as the global `.tids`) while `vecs` is still mapped. Per-shard BF indexes
     * are sliced from this at load time by tid_offset. Best-effort: a failure
     * leaves brute_force unavailable but never fails the sharded build. */
    char vecs_final[512], vecs_tmp[576];
    int  vectors_committed = 0;
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
    uint32_t base_gen = cuvs_crc32(new_tids, tid_bytes);   /* .tids body_crc32 — also the 3C manifest base_generation */
    if (ok)
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok && cuvs_vectors_write(vf, n_vecs, dim, metric, base_gen, vecs) != 0) vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (vok) {
            vectors_committed = 1;
        } else {
            if (vf) unlink(vecs_tmp);
            unlink(vecs_final);   /* drop any stale prior sidecar */
            LOG_WARN("[build_sharded] .vectors sidecar not written for %u/%u; "
                     "brute_force unavailable until REINDEX\n",
                     cmd->db_oid, cmd->index_oid);
        }
    }

    munmap(mem, total);   /* vecs no longer needed past the build loop */

    if (!ok)
    {
        for (int j = 0; j <= i && j < sc; j++)
        {
            if (shards[j].valid && shards[j].handle)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            unlink(shard_tmp[j]);
        }
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "sharded build failed");
        return;
    }

    /* --- Persist global .tids.tmp + .shards.tmp (fsynced). --- */
    char tids_final[512], tids_tmp[576], shards_finalp[512], shards_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    shards_manifest_path(shards_finalp, sizeof(shards_finalp), save_dir,
                         cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp,   sizeof(tids_tmp),   "%s.tmp", tids_final);
    snprintf(shards_tmp, sizeof(shards_tmp), "%s.tmp", shards_finalp);

    uint32_t base_crc = cuvs_crc32(new_tids, tid_bytes);

    int persisted = 0;
    if (write_tids_atomic(tids_tmp, n_vecs, dim, metric, new_tids) == 0)
    {
        FILE *mf = fopen(shards_tmp, "wb");
        if (mf)
        {
            int wok = (cuvs_shards_write(mf, (uint32_t)sc, n_vecs, dim, metric,
                                         base_crc, recs) == 0);
            if (wok && fflush(mf) != 0) wok = 0;
            if (wok && fsync(fileno(mf)) != 0) wok = 0;
            if (fclose(mf) != 0) wok = 0;
            if (wok) persisted = 1;
            else unlink(shards_tmp);
        }
        if (!persisted)
            unlink(tids_tmp);
    }

    if (!persisted)
    {
        for (int j = 0; j < sc; j++)
        {
            cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            unlink(shard_tmp[j]);
        }
        unlink(vecs_tmp);   /* Phase 3L: drop our un-committed sidecar tmp */
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "sharded persist failed");
        return;
    }

    /* --- Commit: rename shard cagras, then .tids, then .shards LAST. --- */
    int commit_ok = 1, renamed = 0;
    for (int j = 0; j < sc; j++)
    {
        if (rename(shard_tmp[j], shard_final[j]) != 0) { commit_ok = 0; break; }
        renamed++;
    }
    if (commit_ok && rename(tids_tmp, tids_final) != 0) commit_ok = 0;
    if (commit_ok && rename(shards_tmp, shards_finalp) != 0) commit_ok = 0;

    if (!commit_ok)
    {
        for (int j = 0; j < renamed; j++) unlink(shard_final[j]);
        unlink(tids_final); unlink(shards_finalp);
        unlink(tids_tmp);   unlink(shards_tmp);
        unlink(vecs_tmp);   /* Phase 3L: .vectors not yet renamed; drop tmp */
        for (int j = renamed; j < sc; j++) unlink(shard_tmp[j]);
        for (int j = 0; j < sc; j++)
            cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "sharded commit rename failed");
        return;
    }

    int dir_fd = open(save_dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    /* Phase 3L: commit the global .vectors sidecar (non-fatal, like unsharded).
     * The generation token guards a torn post-commit, pre-vectors crash. */
    if (vectors_committed) {
        if (rename(vecs_tmp, vecs_final) != 0) {
            LOG_WARN("[build_sharded] rename %s -> %s failed errno=%d; brute_force unavailable\n",
                     vecs_tmp, vecs_final, errno);
            unlink(vecs_tmp); unlink(vecs_final);
            vectors_committed = 0;
        } else {
            int vdir_fd = open(save_dir, O_RDONLY);
            if (vdir_fd >= 0) { fsync(vdir_fd); close(vdir_fd); }
        }
    }

    /* Clear staleness marker and any legacy unsharded .cagra for this OID so a
     * reload sees only the sharded artifacts. Also remove orphan shard files
     * left by a prior build with a HIGHER shard count (e.g. REINDEX 4->2). */
    {
        char stale_path[512], legacy_cagra[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
        index_file_path(legacy_cagra, sizeof(legacy_cagra), save_dir,
                        cmd->db_oid, cmd->index_oid);
        unlink(legacy_cagra);
        for (uint32_t s = (uint32_t)sc; s < CUVS_SHARDS_MAX; s++)
        {
            char sp[512];
            shard_cagra_path(sp, sizeof(sp), save_dir, cmd->db_oid, cmd->index_oid, s);
            if (unlink(sp) != 0)
                break;   /* contiguous ids: first miss => no more orphans */
        }
    }

    /* --- Register the entry (no more eviction past this point). --- */
    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (e)
    {
        free_delta_cache(e);
        if (e->shard_count >= 2) free_index_shards(e);
        else if (e->handle) cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
    }
    else
    {
        if (g_n_indexes >= g_max_indexes)
            evict_lru(usable_gpu(0));
        if (g_n_indexes >= g_max_indexes)
        {
            /* Registry full but artifacts are durable on disk: a later search
             * reloads from the .shards manifest. Free GPU resources now. */
            for (int j = 0; j < sc; j++)
                cuvs_cagra_free(shards[j].handle, shards[j].gpu_device_id);
            pthread_mutex_unlock(&g_index_mutex);
            free(new_tids); free(shards); free(recs); free(shard_tmp); free(shard_final);
            LOG_WARN("[build_sharded] registry full; %u/%u persisted, will reload on demand\n",
                     cmd->db_oid, cmd->index_oid);
            CuvsReplyHeader hdr_ok = {0};
            hdr_ok.status = CUVS_STATUS_OK;
            send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
            return;
        }
        e = &g_indexes[g_n_indexes++];
        memset(e, 0, sizeof(*e));
    }

    e->db_oid       = cmd->db_oid;
    e->index_oid    = cmd->index_oid;
    e->dim          = dim;
    e->metric       = metric;
    e->n_vecs       = n_vecs;
    e->handle       = NULL;
    e->tids         = new_tids;
    e->vram_bytes   = 0;
    e->last_search  = time(NULL);
    e->valid        = 1;
    e->stale        = 0;
    e->stale_since  = 0;
    e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
    e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
    e->delta_vecs_host = NULL; e->delta_n_cached = 0;
    /* Phase 3L: main BF cache starts empty; sharded uses per-shard bf_idx. */
    e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
    e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
    e->warmup_state  = WARMUP_HOT;
    e->gpu_device_id = 0xFFFFFFFFu;   /* sharded: no single device, not LRU-evictable */
    e->shard_count   = sc;
    e->shards        = shards;        /* transfer ownership of the local array */
    e->inflight      = 0;             /* Phase 3G.4 */
    reset_entry_stats(e);

    LOG_INFO("pg_cuvs_server: built sharded index %u/%u (%lld vecs, %d shards)\n",
             cmd->db_oid, cmd->index_oid, (long long)n_vecs, sc);
    for (int j = 0; j < sc; j++)
        LOG_INFO("  shard %d -> GPU %u (%lld vecs, %zu MB)\n",
                 j, e->shards[j].gpu_device_id, (long long)e->shards[j].n_vecs,
                 e->shards[j].vram_bytes / (1024*1024));

    pthread_mutex_unlock(&g_index_mutex);

    /* new_tids ownership moved to e->tids; do not free it here. */
    free(recs); free(shard_tmp); free(shard_final);

    CuvsReplyHeader hdr_ok = {0};
    hdr_ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));

    /* Phase 3G.2: snapshot the whole sharded artifact set to GCS (detached,
     * non-fatal). The local artifacts are already durable; upload failure only
     * costs a replica the warmup fast-path. */
    if (g_snapshot_uri[0] != '\0' && cmd->relfilenode != 0)
    {
        ShardedUploadArgs *sa = malloc(sizeof(*sa));
        if (sa)
        {
            strncpy(sa->snapshot_uri, g_snapshot_uri, sizeof(sa->snapshot_uri) - 1);
            strncpy(sa->cluster_id,   g_cluster_id,   sizeof(sa->cluster_id)   - 1);
            strncpy(sa->gcs_key_file, g_gcs_key_file, sizeof(sa->gcs_key_file) - 1);
            strncpy(sa->index_dir,    save_dir,        sizeof(sa->index_dir)    - 1);
            sa->snapshot_uri[sizeof(sa->snapshot_uri) - 1] = '\0';
            sa->cluster_id[sizeof(sa->cluster_id) - 1]     = '\0';
            sa->gcs_key_file[sizeof(sa->gcs_key_file) - 1] = '\0';
            sa->index_dir[sizeof(sa->index_dir) - 1]       = '\0';
            sa->db_oid       = cmd->db_oid;
            sa->table_oid    = cmd->table_oid;
            sa->index_oid    = cmd->index_oid;
            sa->relfilenode  = cmd->relfilenode;
            sa->metric       = cmd->metric;
            sa->dim          = cmd->dim;
            sa->vector_count = cmd->n_vecs;
            sa->shard_count  = (uint32_t) sc;
            sa->base_generation = base_gen;

            pthread_t up_tid;
            pthread_attr_t up_attr;
            pthread_attr_init(&up_attr);
            pthread_attr_setdetachstate(&up_attr, PTHREAD_CREATE_DETACHED);
            if (pthread_create(&up_tid, &up_attr, objstore_upload_sharded_thread, sa) != 0)
            {
                LOG_WARN("objstore: failed to spawn sharded upload thread for %u/%u\n",
                         cmd->db_oid, cmd->index_oid);
                free(sa);
            }
            pthread_attr_destroy(&up_attr);
        }
    }
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
/* ADR-059: upper bound on parallel-build worker partials accepted from a single
 * BUILD frame (defensive against a malformed n_partials). A real build has at
 * most max_parallel_maintenance_workers + 1 participants. */
#define CUVS_BUILD_MAX_PARTIALS 256

static void handle_build_multi(int client_fd, const CuvsCmdFrame *cmd,
                               const char *index_dir);
static void finish_build_commit(int client_fd, const CuvsCmdFrame *cmd,
                                const char *save_dir, CuvsCagraIndex new_handle,
                                uint64_t *new_tids, int target_gpu, size_t needed,
                                int vectors_committed, const char *vecs_tmp,
                                const char *vecs_final, uint32_t base_generation);

static void
handle_build(int client_fd, const CuvsCmdFrame *cmd)
{
    LOG_DEBUG("[handle_build] reading index_dir...\n");
    char index_dir[256] = {0};
    /* ADR-057: the backend may pass the corpus as an SCM_RIGHTS memfd alongside
     * index_dir (memfd tier); passed_fd is -1 for the shm/heap tiers. */
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        LOG_ERROR("[handle_build] recv index_dir FAILED errno=%d\n", errno);
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    LOG_DEBUG("[handle_build] got index_dir=%s passed_fd=%d\n", index_dir, passed_fd);

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
        if (passed_fd >= 0) close(passed_fd);
        send_error(client_fd, "CAGRA build needs at least 2 vectors");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            LOG_ERROR("[handle_build] payload size overflow n_vecs=%lld dim=%u\n",
                      (long long)cmd->n_vecs, cmd->dim);
            if (passed_fd >= 0) close(passed_fd);
            send_error(client_fd, "build payload size overflow");
            return;
        }
    }

    /* ADR-059: the parallel-build leader can hand off N worker named-shm
     * partials instead of one merged corpus. That path is self-contained
     * (receives the descriptor list, mmaps each partial, builds via direct
     * multi-H2D). The multi path never carries an SCM_RIGHTS fd. */
    if (cmd->n_partials > 0)
    {
        if (passed_fd >= 0)
            close(passed_fd);
        handle_build_multi(client_fd, cmd, index_dir);
        return;
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* ADR-057: memfd tier hands over the corpus as a passed fd (no /dev/shm
     * name, so a crashed build can never leave an orphan); the shm/heap tiers
     * name it in cmd->shm_key. Either way the mapping outlives the fd close. */
    void *mem;
    if (passed_fd >= 0)
    {
        LOG_INFO("[handle_build] corpus via memfd(SCM_RIGHTS) fd=%d total=%zu\n", passed_fd, total);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);   /* mapping holds the memory; daemon's ref is dropped here */
    }
    else
    {
        LOG_INFO("[handle_build] corpus via shm_open(%s)\n", cmd->shm_key);
        int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
        if (shm_fd < 0)
        {
            LOG_ERROR("[handle_build] shm_open FAILED errno=%d (%s)\n", errno, strerror(errno));
            send_error(client_fd, "shm_open failed");
            return;
        }
        LOG_DEBUG("[handle_build] shm_open OK fd=%d\n", shm_fd);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
    }
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    /* Phase 3F/3G: resolve the shard count, then dispatch.
     *   cmd->shard_count == 0 -> Phase 3G auto: derive from VRAM. Resolves to 1
     *      (unsharded) whenever the index fits one GPU, so small-index behavior
     *      is byte-identical to pre-3G. 0 from the helper means it won't fit even
     *      maximally sharded -> fail closed (CREATE INDEX aborts).
     *   cmd->shard_count == 1 -> forced unsharded.
     *   cmd->shard_count >= 2 -> forced N (build_sharded clamps to the corpus).
     * The build_sharded path is self-contained (owns munmap + reply); the
     * unsharded fall-through below is byte-identical to pre-3F behavior. */
    int resolved_sc = (int)cmd->shard_count;
    if (cmd->shard_count == 0)
    {
        /* Per-GPU budget: the smallest positive budget across usable GPUs (most
         * conservative; 0 if all are unlimited, which the helper treats as
         * "don't auto-shard"). */
        size_t budget = 0;
        int ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b > 0 && (budget == 0 || b < budget))
                budget = b;
        }
        resolved_sc = cuvs_auto_shard_count(cmd->n_vecs, (int)cmd->dim, budget,
                                            ng, CUVS_SHARDS_MAX);
        if (resolved_sc == 0)
        {
            munmap(mem, total);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            strncpy(hdr.error,
                    "index too large for GPU VRAM even when fully sharded",
                    sizeof(hdr.error) - 1);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        LOG_INFO("[handle_build] auto shard count: %u/%u %lld vecs dim %u -> %d shard(s)\n",
                 cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, cmd->dim, resolved_sc);
    }

    if (resolved_sc >= 2)
    {
        build_sharded(client_fd, cmd, index_dir, vecs, tids_in, total, mem, resolved_sc);
        return;
    }

    pthread_mutex_lock(&g_index_mutex);

    /* VRAM accounting: cuvs_cagra_build allocates BEFORE we free the old
     * index, so peak VRAM = existing + new. Ask for full 'needed' from
     * ensure_vram regardless of same-OID replacement. */
    size_t needed = estimate_vram_bytes(cmd->n_vecs, (int)cmd->dim);
    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        LOG_WARN("[handle_build] index %u/%u (%zu bytes) too large for any GPU budget\n",
                 cmd->db_oid, cmd->index_oid, needed);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "index too large for any GPU VRAM budget", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (ensure_vram(needed, target_gpu) < 0)
    {
        LOG_WARN("[handle_build] VRAM exhausted on GPU %d for %u/%u "
                 "(budget %zu MB, used %zu MB, needed %zu MB)\n",
                 target_gpu, cmd->db_oid, cmd->index_oid,
                 g_max_vram_per_gpu[target_gpu] / (1024*1024),
                 total_vram_used(target_gpu) / (1024*1024),
                 needed / (1024*1024));
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        snprintf(hdr.error, sizeof(hdr.error),
                 "VRAM exhausted on GPU %d after eviction", target_gpu);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    /* ADR-070 Bug #2: reserve the VRAM, then release g_index_mutex for the
     * multi-minute GPU build so concurrent searches/stats/drops are not blocked.
     * total_vram_used() counts g_pending_build_vram, so concurrent admission and
     * eviction stay correct while we build unlocked. vecs/mem are per-request
     * (not shared state), so reading them unlocked is safe. */
    g_pending_build_vram[target_gpu] += needed;
    pthread_mutex_unlock(&g_index_mutex);

    LOG_DEBUG("[handle_build] calling cuvs_cagra_build n_vecs=%lld dim=%u gpu=%d...\n",
            (long long)cmd->n_vecs, cmd->dim, target_gpu);
    CuvsCagraIndex new_handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric,
                                                 (int)cmd->graph_degree,
                                                 (int)cmd->intermediate_graph_degree,
                                                 cmd->build_algo, target_gpu);
    if (!new_handle && cuvs_last_build_was_oom())
    {
        /* ADR-070 Bug #3: the build hit a VRAM OOM (estimate_vram_bytes excludes
         * CAGRA build-time scratch, so admission can pass yet the build OOM).
         * Briefly retake the lock to evict an LRU index, then retry once (only if
         * eviction freed VRAM). evict_lru now safely handles IVF-PQ/sharded LRUs. */
        pthread_mutex_lock(&g_index_mutex);
        int evicted = (evict_lru(target_gpu) > 0);
        pthread_mutex_unlock(&g_index_mutex);
        if (evicted)
        {
            LOG_WARN("[handle_build] build OOM on GPU %d; evicted LRU, retrying once\n",
                     target_gpu);
            new_handle = cuvs_cagra_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric,
                                          (int)cmd->graph_degree,
                                          (int)cmd->intermediate_graph_degree,
                                          cmd->build_algo, target_gpu);
        }
    }

    /* Reacquire for the durable-commit + registry tail (finish_build_commit
     * expects the lock held). Release the build reservation now: on success the
     * registry entry below carries vram_bytes; on failure the VRAM is freed. */
    pthread_mutex_lock(&g_index_mutex);
    g_pending_build_vram[target_gpu] -= needed;

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
        cuvs_cagra_free(new_handle, target_gpu);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    /* --- Disk persistence: tmp -> rename pattern --- */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Phase 3L: persist the raw vector matrix as a `.vectors` sidecar for GPU
     * brute-force search, written from `vecs` (the shm payload) while it is
     * still mapped — munmap follows. Best-effort: a write failure leaves
     * brute_force unavailable for this index but never fails the CAGRA build.
     * The generation token (this build's .tids body_crc32) lets the daemon
     * reject a stale sidecar after a torn build. Renamed at the commit point. */
    uint32_t tids_gen = cuvs_crc32(new_tids, (size_t)cmd->n_vecs * sizeof(uint64_t));
    char vecs_final[512], vecs_tmp[576];
    int  vectors_committed = 0;
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok && cuvs_vectors_write(vf, cmd->n_vecs, cmd->dim, cmd->metric, tids_gen, vecs) != 0)
            vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (vok) {
            vectors_committed = 1;   /* tmp ready; renamed at the commit point */
        } else {
            if (vf) unlink(vecs_tmp);
            unlink(vecs_final);      /* drop any stale prior sidecar */
            LOG_WARN("[handle_build] .vectors sidecar not written for %u/%u; "
                     "brute_force unavailable until REINDEX\n",
                     cmd->db_oid, cmd->index_oid);
        }
    }
    munmap(mem, total);

    /* ADR-059: the persist/commit/registry/reply sequence is shared with the
     * multi-partial build path (handle_build_multi). */
    finish_build_commit(client_fd, cmd, save_dir, new_handle, new_tids,
                        target_gpu, needed, vectors_committed, vecs_tmp, vecs_final,
                        tids_gen);
}

/* Persist a freshly built CAGRA index (tmp+rename), swap it into the registry,
 * and reply OK — the durable commit tail shared by the single-corpus and ADR-059
 * multi-partial build paths. On entry the caller holds g_index_mutex, has built
 * new_handle, assembled the contiguous new_tids, and staged the .vectors sidecar
 * (vectors_committed + vecs_tmp/vecs_final). This releases g_index_mutex. */
static void
finish_build_commit(int client_fd, const CuvsCmdFrame *cmd, const char *save_dir,
                    CuvsCagraIndex new_handle, uint64_t *new_tids, int target_gpu,
                    size_t needed, int vectors_committed, const char *vecs_tmp,
                    const char *vecs_final, uint32_t base_generation)
{
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
    if (cuvs_cagra_serialize(new_handle, idx_tmp, target_gpu) != 0) {
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

    /* Phase 3L: commit the .vectors sidecar. Non-fatal — the index is already
     * durably committed above (cagra + tids renamed); a rename failure only
     * disables brute_force. The generation token guards a torn post-cagra,
     * pre-vectors crash: a stale sidecar is rejected at load. */
    if (vectors_committed) {
        if (rename(vecs_tmp, vecs_final) != 0) {
            LOG_WARN("[handle_build] rename %s -> %s failed errno=%d; brute_force unavailable\n",
                     vecs_tmp, vecs_final, errno);
            unlink(vecs_tmp);
            unlink(vecs_final);
            vectors_committed = 0;
        } else {
            int vdir_fd = open(save_dir, O_RDONLY);
            if (vdir_fd >= 0) { fsync(vdir_fd); close(vdir_fd); }
        }
    }

    /* A rebuild produces a fresh graph reflecting the current heap — clear any
     * persisted staleness marker. */
    {
        char stale_path[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir,
                        cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
    }

    /* Phase 3F: this unsharded build replaces any prior sharded artifacts for
     * the same OID. Remove the .shards manifest FIRST (so a reload can never
     * take the sharded path), then the contiguous shard .cagra files. */
    {
        char shards_path[512];
        shards_manifest_path(shards_path, sizeof(shards_path), save_dir,
                             cmd->db_oid, cmd->index_oid);
        unlink(shards_path);
        for (uint32_t s = 0; s < CUVS_SHARDS_MAX; s++)
        {
            char sp[512];
            shard_cagra_path(sp, sizeof(sp), save_dir, cmd->db_oid, cmd->index_oid, s);
            if (unlink(sp) != 0)
                break;   /* contiguous ids: first miss => no more shards */
        }
    }

    LOG_DEBUG("[handle_build] disk commit OK\n");

    /* Phase 3I-1: write CPU HNSW fallback sidecar — only when the backend
     * requested it (cuvs.cpu_hnsw_fallback=on or a 3I import_hnsw flow).
     * Skipping this for CAGRA-only builds avoids ~30s of from_cagra() CPU
     * work that would never be used. */
    if (cmd->use_cpu_hnsw)
    {
        char hnsw_path[512];
        hnsw_file_path(hnsw_path, sizeof(hnsw_path), save_dir,
                       cmd->db_oid, cmd->index_oid);
        if (cuvs_hnsw_serialize(new_handle, hnsw_path, target_gpu) != 0)
            LOG_WARN("[handle_build] HNSW fallback sidecar not saved for %u/%u\n",
                     cmd->db_oid, cmd->index_oid);
        else
            LOG_INFO("pg_cuvs_server: saved HNSW fallback %u/%u\n",
                     cmd->db_oid, cmd->index_oid);
    }

    /* --- Swap into registry --- */
    IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
    if (existing) {
        /* Phase 3F: a prior sharded instance is replaced by this unsharded
         * build — free all its shard handles, not a (NULL) single handle. */
        if (existing->shard_count >= 2)
            free_index_shards(existing);
        else
            cuvs_cagra_free(existing->handle, existing->gpu_device_id);
        free(existing->tids);
        free(existing->rev_tids);     existing->rev_tids     = NULL;
        free(existing->rev_item_ids); existing->rev_item_ids = NULL;
        existing->shard_count = 0;
        existing->shards      = NULL;
        existing->gpu_device_id = (uint32_t)target_gpu;
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
        /* Phase 3I-1: drop cached CPU HNSW (new sidecar was just written). */
        if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
        /* 3P: CAGRA rebuild clears any prior IVF-PQ handle on the same slot. */
        if (existing->ivfpq_handle) { cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id); existing->ivfpq_handle = NULL; }
        existing->last_search_mode = 0;
        reset_entry_stats(existing);   /* fresh index instance */
        existing->n_extended = 0;      /* REINDEX resets the extend counter */
        existing->compact_count++;     /* every rebuild counts as a compaction */
        existing->last_compact_at = time(NULL);
        build_rev_tid_map(existing);   /* rebuild map for new tids/n_vecs */
    } else {
        if (g_n_indexes >= g_max_indexes)
            evict_lru(target_gpu);
        if (g_n_indexes >= g_max_indexes) {
            /* Registry full and the LRU couldn't be evicted (e.g. all entries
             * inflight on other GPUs). The artifacts are already durable on
             * disk, so DON'T roll back — free GPU resources and reply OK; the
             * first query reloads from disk (load_index evicts for a slot).
             * Mirrors build_sharded's defer-to-reload. */
            LOG_WARN("[handle_build] registry full; %u/%u persisted, will reload on demand\n",
                     cmd->db_oid, cmd->index_oid);
            (void) vectors_committed;   /* .vectors stays on disk for the reload */
            cuvs_cagra_free(new_handle, target_gpu);
            free(new_tids);
            pthread_mutex_unlock(&g_index_mutex);
            CuvsReplyHeader hdr_ok = {0};
            hdr_ok.status = CUVS_STATUS_OK;
            send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
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
        e->delta_vecs_host = NULL; e->delta_n_cached = 0;
        /* Phase 3L: main BF cache starts empty; lazily built on a BF search. */
        e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
        e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
        e->warmup_state = WARMUP_HOT;
        e->gpu_device_id = (uint32_t)target_gpu;
        e->shard_count = 0;     /* unsharded; slot may be reused post-eviction */
        e->shards      = NULL;
        e->hnsw_idx     = NULL;  /* Phase 3I-1: loaded lazily on first cpu_hnsw request */
        e->ivfpq_handle = NULL;  /* 3P: CAGRA build never sets an IVF-PQ handle */
        e->ivfpq_n_vecs = 0;
        e->ivfpq_vram_bytes = 0;
        e->last_search_mode = 0;
        e->n_extended        = 0;   /* slot may be reused; must not carry stale counter */
        e->compact_count     = 0;
        e->last_compact_at   = 0;
        e->rev_tids = NULL;  /* evicted slot may have stale freed ptr; NULL it */
        e->rev_item_ids = NULL;
        /* 3O: build the heapTID->item_id reverse map at build time (not only on
         * load). Without this a freshly-built resident index has no map, so 3O
         * prefilter and ADR-064 streaming BF silently fall back until a restart
         * or in-place REINDEX. e->tids/e->n_vecs are set above. This covers both
         * handle_build and handle_build_multi (both finalize through here). */
        build_rev_tid_map(e);
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
            ua->base_generation = base_generation;

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
     * intact in registry. Clean up new resources and tmp files. The prior
     * `.vectors` (if any) is left intact — it still matches the surviving
     * (un-renamed) prior `.tids`/`.cagra`; only our new tmp is removed. */
    unlink(idx_tmp);
    unlink(tids_tmp);
    unlink(vecs_tmp);
    cuvs_cagra_free(new_handle, target_gpu);
    free(new_tids);
    pthread_mutex_unlock(&g_index_mutex);
    send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "disk persistence failed");
}

/* ----------------------------------------------------------------
 * ADR-059: multi-partial CAGRA build.
 *
 * The parallel-build leader hands off N worker named-shm partials (each
 * [vectors][tids] for its scanned share) instead of one merged corpus. We mmap
 * each partial and, for the common single-shard case, build via
 * cuvs_cagra_build_multi — streaming each partial straight to the GPU with no
 * host-side corpus copy (eliminating the leader merge that capped ADR-058). The
 * global TID array (small) is assembled host-side for persistence; the .vectors
 * sidecar is streamed from the partials. Multi-shard falls back to a contiguous
 * host assembly + build_sharded. The leader owns the partials' lifetime and
 * unlinks them after we reply.
 * ---------------------------------------------------------------- */
static void
handle_build_multi(int client_fd, const CuvsCmdFrame *cmd, const char *index_dir)
{
    uint32_t       n_parts   = cmd->n_partials;
    CuvsPartialDesc *descs    = NULL;
    const float  **part_vecs  = NULL;
    int64_t       *n_each     = NULL;
    void         **maps       = NULL;
    size_t        *map_lens   = NULL;
    uint64_t      *new_tids   = NULL;
    int            nmapped    = 0;
    int64_t        total      = cmd->n_vecs;
    size_t         per_vec    = (size_t)cmd->dim * sizeof(float);
    size_t         vec_bytes  = (size_t)total * per_vec;
    size_t         tid_bytes  = (size_t)total * sizeof(uint64_t);
    const char    *err        = NULL;
    int            status     = CUVS_STATUS_ERROR;
    int            resolved_sc;
    const char    *save_dir;

    if (n_parts == 0 || n_parts > CUVS_BUILD_MAX_PARTIALS)
    {
        LOG_ERROR("[handle_build_multi] invalid n_partials=%u\n", n_parts);
        send_error(client_fd, "invalid n_partials");
        return;
    }

    descs     = malloc((size_t)n_parts * sizeof(*descs));
    part_vecs = malloc((size_t)n_parts * sizeof(*part_vecs));
    n_each    = malloc((size_t)n_parts * sizeof(*n_each));
    maps      = malloc((size_t)n_parts * sizeof(*maps));
    map_lens  = malloc((size_t)n_parts * sizeof(*map_lens));
    if (!descs || !part_vecs || !n_each || !maps || !map_lens)
    {
        err = "malloc (partial bookkeeping) failed";
        goto fail;
    }

    if (recv_all(client_fd, descs, (size_t)n_parts * sizeof(*descs)) < 0)
    {
        err = "recv partial descriptors failed";
        goto fail;
    }

    /* Validate the list before touching shm: name format + Σ n_vecs == corpus
     * size (so positional (vector,tid) pairing across partials is complete). */
    {
        int64_t sum = 0;
        for (uint32_t i = 0; i < n_parts; i++)
        {
            if (descs[i].n_vecs < 0)  { err = "negative partial n_vecs"; goto fail; }
            if (descs[i].n_vecs == 0) continue;
            if (strncmp(descs[i].shm_name, "/pg_cuvs_bld_", 13) != 0)
            {
                err = "partial shm name not /pg_cuvs_bld_*";
                goto fail;
            }
            sum += descs[i].n_vecs;
        }
        if (sum != total)
        {
            LOG_ERROR("[handle_build_multi] sum partial n_vecs %lld != total %lld\n",
                      (long long)sum, (long long)total);
            err = "partial n_vecs sum != corpus size";
            goto fail;
        }
    }

    /* mmap each non-empty partial (host) after verifying its exact size. */
    for (uint32_t i = 0; i < n_parts; i++)
    {
        size_t pvb, ptb, plen;
        int    pfd;
        struct stat st;
        void  *pm;

        if (descs[i].n_vecs == 0)
            continue;
        pvb  = (size_t)descs[i].n_vecs * per_vec;
        ptb  = (size_t)descs[i].n_vecs * sizeof(uint64_t);
        plen = pvb + ptb;

        pfd = shm_open(descs[i].shm_name, O_RDONLY, 0);
        if (pfd < 0)              { err = "shm_open partial failed"; goto fail; }
        if (fstat(pfd, &st) != 0) { close(pfd); err = "fstat partial failed"; goto fail; }
        if ((size_t)st.st_size != plen)
        {
            LOG_ERROR("[handle_build_multi] partial %s size %lld != expected %zu\n",
                      descs[i].shm_name, (long long)st.st_size, plen);
            close(pfd);
            err = "partial size mismatch";
            goto fail;
        }
        pm = mmap(NULL, plen, PROT_READ, MAP_SHARED, pfd, 0);
        close(pfd);
        if (pm == MAP_FAILED)     { err = "mmap partial failed"; goto fail; }

        maps[nmapped]      = pm;
        map_lens[nmapped]  = plen;
        part_vecs[nmapped] = (const float *)pm;
        n_each[nmapped]    = descs[i].n_vecs;
        nmapped++;
    }
    if (nmapped == 0)             { err = "no non-empty partials"; goto fail; }

    LOG_INFO("[handle_build_multi] %d partial(s), total=%lld dim=%u (direct multi-H2D)\n",
             nmapped, (long long)total, cmd->dim);

    /* Assemble the global TID array (small: total*8 bytes). */
    new_tids = malloc(tid_bytes);
    if (!new_tids)                { err = "malloc tids failed"; goto fail; }
    {
        size_t toff = 0;
        for (int j = 0; j < nmapped; j++)
        {
            size_t pvb = (size_t)n_each[j] * per_vec;
            size_t ptb = (size_t)n_each[j] * sizeof(uint64_t);
            memcpy((char *)new_tids + toff, (const char *)maps[j] + pvb, ptb);
            toff += ptb;
        }
    }

    /* Resolve shard count — identical policy to the single-corpus path. */
    resolved_sc = (int)cmd->shard_count;
    if (cmd->shard_count == 0)
    {
        size_t budget = 0;
        int ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b > 0 && (budget == 0 || b < budget))
                budget = b;
        }
        resolved_sc = cuvs_auto_shard_count(total, (int)cmd->dim, budget, ng, CUVS_SHARDS_MAX);
        if (resolved_sc == 0)
        {
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "index too large for GPU VRAM even when fully sharded";
            goto fail;
        }
    }

    save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    /* Multi-shard fallback: partials cross shard boundaries, so assemble a
     * contiguous [vecs][tids] corpus (anonymous mmap, released by build_sharded's
     * munmap) and reuse the existing sharded build. Rare large-index path. */
    if (resolved_sc >= 2)
    {
        size_t corpus_bytes = vec_bytes + tid_bytes;
        void  *mem = mmap(NULL, corpus_bytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        size_t voff = 0;
        if (mem == MAP_FAILED)    { err = "mmap merged corpus failed"; goto fail; }
        for (int j = 0; j < nmapped; j++)
        {
            size_t pvb = (size_t)n_each[j] * per_vec;
            memcpy((char *)mem + voff, maps[j], pvb);
            voff += pvb;
        }
        memcpy((char *)mem + vec_bytes, new_tids, tid_bytes);

        for (int j = 0; j < nmapped; j++)
            munmap(maps[j], map_lens[j]);    /* copied out; release partials */
        free(new_tids); new_tids = NULL;
        free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);

        build_sharded(client_fd, cmd, index_dir,
                      (const float *)mem,
                      (const uint64_t *)((char *)mem + vec_bytes),
                      corpus_bytes, mem, resolved_sc);
        return;   /* build_sharded owns the reply + munmap(mem) */
    }

    /* Single shard: direct multi-H2D — the ADR-059 win (no host corpus copy). */
    pthread_mutex_lock(&g_index_mutex);
    {
        size_t needed = estimate_vram_bytes(total, (int)cmd->dim);
        int    target_gpu = pick_gpu_for_index(needed);
        CuvsCagraIndex new_handle;
        uint32_t tids_gen;
        char vecs_final[512], vecs_tmp[576];
        int  vectors_committed = 0;

        if (target_gpu < 0)
        {
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "index too large for any GPU VRAM budget";
            goto fail;
        }
        if (ensure_vram(needed, target_gpu) < 0)
        {
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_OOM_FALLBACK;
            err = "VRAM exhausted after eviction";
            goto fail;
        }

        /* ADR-070 Bug #2: reserve VRAM, then release the lock for the multi-H2D
         * GPU build so concurrent searches/stats/drops aren't blocked. Partials
         * (part_vecs/maps) are per-request, so reading them unlocked is safe;
         * total_vram_used() counts g_pending_build_vram during the window. */
        g_pending_build_vram[target_gpu] += needed;
        pthread_mutex_unlock(&g_index_mutex);

        new_handle = cuvs_cagra_build_multi(part_vecs, n_each, nmapped, total,
                                            (int)cmd->dim, cmd->metric,
                                            (int)cmd->graph_degree,
                                            (int)cmd->intermediate_graph_degree,
                                            cmd->build_algo, target_gpu);
        if (!new_handle && cuvs_last_build_was_oom())
        {
            /* ADR-070 Bug #3: VRAM OOM (scratch not covered by ensure_vram) —
             * briefly retake the lock to evict an LRU index, then retry once. */
            pthread_mutex_lock(&g_index_mutex);
            int evicted = (evict_lru(target_gpu) > 0);
            pthread_mutex_unlock(&g_index_mutex);
            if (evicted)
            {
                LOG_WARN("[handle_build_multi] build OOM on GPU %d; evicted LRU, retrying once\n",
                         target_gpu);
                new_handle = cuvs_cagra_build_multi(part_vecs, n_each, nmapped, total,
                                                    (int)cmd->dim, cmd->metric,
                                                    (int)cmd->graph_degree,
                                                    (int)cmd->intermediate_graph_degree,
                                                    cmd->build_algo, target_gpu);
            }
        }

        /* Reacquire for the durable-commit + registry tail (finish_build_commit
         * expects the lock held). Release the reservation: on success the registry
         * entry carries vram_bytes; on failure the VRAM is freed. */
        pthread_mutex_lock(&g_index_mutex);
        g_pending_build_vram[target_gpu] -= needed;

        if (!new_handle)
        {
            pthread_mutex_unlock(&g_index_mutex);
            status = CUVS_STATUS_BUILD_FAILED;
            err = "cuvs_cagra_build_multi failed";
            goto fail;
        }

        /* .vectors sidecar streamed from the partials (no contiguous host copy). */
        tids_gen = cuvs_crc32(new_tids, tid_bytes);
        vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
        snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);
        {
            FILE *vf = fopen(vecs_tmp, "wb");
            int vok = (vf != NULL);
            if (vok && cuvs_vectors_write_multi(vf, n_each, nmapped, cmd->dim,
                                                cmd->metric, tids_gen, part_vecs) != 0)
                vok = 0;
            if (vok && fflush(vf) != 0) vok = 0;
            if (vok && fsync(fileno(vf)) != 0) vok = 0;
            if (vf && fclose(vf) != 0) vok = 0;
            if (vok)
                vectors_committed = 1;
            else
            {
                if (vf) unlink(vecs_tmp);
                unlink(vecs_final);
                LOG_WARN("[handle_build_multi] .vectors sidecar not written for %u/%u; "
                         "brute_force unavailable until REINDEX\n",
                         cmd->db_oid, cmd->index_oid);
            }
        }

        /* Done with the partials; the GPU build holds its own device copy. */
        for (int j = 0; j < nmapped; j++)
            munmap(maps[j], map_lens[j]);
        free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);

        finish_build_commit(client_fd, cmd, save_dir, new_handle, new_tids,
                            target_gpu, needed, vectors_committed, vecs_tmp, vecs_final,
                            tids_gen);
        return;   /* new_tids ownership passed to the registry */
    }

fail:
    for (int j = 0; j < nmapped; j++)
        munmap(maps[j], map_lens[j]);
    free(new_tids);
    free(descs); free(part_vecs); free(n_each); free(maps); free(map_lens);
    if (status == CUVS_STATUS_ERROR)
        send_error(client_fd, err ? err : "multi-partial build failed");
    else
        send_error_code(client_fd, status, err ? err : "multi-partial build failed");
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

    /* Bounded by g_max_indexes; gather under the lock, send after unlocking.
     * Heap-allocated — g_max_indexes can be large, so never a stack VLA. */
    CuvsIndexStats *stats = malloc((size_t)g_max_indexes * sizeof(CuvsIndexStats));
    uint32_t n = 0;

    if (!stats)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OK;   /* empty stats, not an error */
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    for (int i = 0; i < g_n_indexes && n < (uint32_t)g_max_indexes; i++)
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
        if (e->shard_count >= 2)
        {
            uint64_t vb = 0;
            for (int s2 = 0; s2 < e->shard_count; s2++)
                vb += e->shards[s2].vram_bytes;
            s->vram_bytes = vb;     /* logical index VRAM = sum of shards */
        }
        else
            s->vram_bytes = e->vram_bytes;
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
        s->delta_rows         = e->n_delta;
        s->delta_generation   = e->delta_generation;
        s->delta_vram_bytes   = e->delta_vram_bytes;
        s->delta_merged_count = e->delta_merged_count;
        s->delta_search_mode  = (e->delta_idx && e->n_delta > 0) ? 2 : 0;
        s->warmup_state       = (uint32_t)e->warmup_state;
        s->last_warmup_at     = (int64_t)e->last_warmup_at;       /* Phase 3D: retained post-hydration */
        s->warmup_duration_ms = e->warmup_duration_ms;
        s->download_count     = (uint32_t)e->download_count;
        s->cache_miss_count   = e->cache_miss_count;
        s->gpu_device_id      = e->gpu_device_id;   /* 0xFFFFFFFF when sharded */
        s->shard_count        = (uint32_t)e->shard_count;
        s->search_mode        = e->last_search_mode; /* Phase 3I-1 */
        s->bf_batch_count     = e->bf_batch_count;   /* Phase 3L-9 */
        s->n_extended         = e->n_extended;
        s->compact_count      = e->compact_count;
        s->last_compact_at    = (int64_t)e->last_compact_at;
    }

    /* Phase 3D: also emit cold (not-yet-resident) entries so operators can
     * see warmup progress in pg_stat_gpu_search. */
    for (int i = 0; i < g_n_cold_indexes && n < (uint32_t)g_max_indexes; i++)
    {
        ColdIndexEntry *ce = &g_cold_indexes[i];
        if (!ce->valid)
            continue;
        if (ce->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && ce->index_oid != cmd->index_oid)
            continue;

        CuvsIndexStats *s = &stats[n++];
        memset(s, 0, sizeof(*s));
        s->db_oid             = ce->db_oid;
        s->index_oid          = ce->index_oid;
        s->resident           = 0;
        s->warmup_state       = (uint32_t)ce->warmup_state;
        s->last_warmup_at     = (int64_t)ce->last_warmup_at;
        s->warmup_duration_ms = ce->warmup_duration_ms;
        s->download_count     = (uint32_t)ce->download_count;
        s->cache_miss_count   = ce->cache_miss_count;
        s->gpu_device_id      = 0xFFFFFFFF;
        s->shard_count        = 0;   /* cold entries are reported unsharded */
    }

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = n;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n > 0)
        send_all(client_fd, stats, (size_t)n * sizeof(CuvsIndexStats));
    free(stats);
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
 * Handle DROP_INDEX command (CUVS_OP_DROP_INDEX, Phase 3G.1)
 *
 * Fired by the backend's object_access_hook when a DROP INDEX commits. Free the
 * whole logical index from VRAM (sharded or not), remove it from the resident +
 * cold registries, and unlink ALL of its on-disk artifacts so a daemon restart
 * does not reload a dropped index as a zombie. DROP holds AccessExclusiveLock on
 * the index, so no search can be in-flight on it — safe to free under the mutex
 * (this is why sharded indexes need no inflight guard here; see ADR-022/023).
 * Idempotent: a missing index or missing files are not errors.
 * ---------------------------------------------------------------- */
static void
handle_drop(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;
    char     p[512];

    pthread_mutex_lock(&g_index_mutex);

    /* Free the resident entry (if any) and compact the registry. */
    IndexEntry *e = find_index(db, idx);
    if (e)
    {
        free_delta_cache(e);
        /* ADR-073: free the resident main BF cache — flat's ONLY GPU allocation
         * (handle is NULL), and a cagra index's secondary BF cache. NULL-safe.
         * (Note: e->ivfpq_handle is still not freed here — a pre-existing IVF-PQ
         * VRAM-on-drop leak, out of scope for ADR-073.) */
        free_main_bf_cache(e);
        if (e->shard_count >= 2) free_index_shards(e);
        else if (e->handle)      cuvs_cagra_free(e->handle, e->gpu_device_id);
        free(e->tids);
        free(e->rev_tids);     e->rev_tids     = NULL;
        free(e->rev_item_ids); e->rev_item_ids = NULL;

        int slot = (int)(e - g_indexes);
        for (int i = slot; i < g_n_indexes - 1; i++)
            g_indexes[i] = g_indexes[i + 1];
        g_n_indexes--;
    }

    /* Drop a cold-registry entry too (Phase 3D), if present. */
    for (int i = 0; i < g_n_cold_indexes; i++)
        if (g_cold_indexes[i].valid &&
            g_cold_indexes[i].db_oid == db && g_cold_indexes[i].index_oid == idx)
        {
            for (int j = i; j < g_n_cold_indexes - 1; j++)
                g_cold_indexes[j] = g_cold_indexes[j + 1];
            g_n_cold_indexes--;
            break;
        }

    /* Unlink every per-index artifact so a restart won't reload a zombie. */
    index_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .cagra */
    tids_file_path(p, sizeof(p), g_index_dir, db, idx);        unlink(p); /* .tids */
    shards_manifest_path(p, sizeof(p), g_index_dir, db, idx);  unlink(p); /* .shards */
    delta_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .delta */
    vectors_file_path(p, sizeof(p), g_index_dir, db, idx);     unlink(p); /* .vectors */
    stale_file_path(p, sizeof(p), g_index_dir, db, idx);       unlink(p); /* .stale */
    relfilenode_file_path(p, sizeof(p), g_index_dir, db, idx); unlink(p); /* .relfilenode */
    snprintf(p, sizeof(p), "%s/%u_%u.tombstone", g_index_dir, db, idx); unlink(p);
    /* Shard artifacts have contiguous ids; stop at the first missing one. */
    for (uint32_t s = 0; s < CUVS_SHARDS_MAX; s++)
    {
        shard_cagra_path(p, sizeof(p), g_index_dir, db, idx, s);
        if (unlink(p) != 0)
            break;
    }

    LOG_INFO("pg_cuvs_server: dropped index %u/%u (freed + artifacts unlinked)\n", db, idx);

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status = CUVS_STATUS_OK;
    send_all(client_fd, &hdr, sizeof(hdr));
}

/* ----------------------------------------------------------------
 * Handle EXPORT_ADJACENCY command (CUVS_OP_EXPORT_ADJACENCY, Phase 3J).
 *
 * Copies the CAGRA graph adjacency list + corpus vectors from GPU VRAM
 * to a shared memory segment, then replies with the key so the backend
 * can read the data and write pgvector HNSW pages directly (no .hnsw file).
 *
 * Reply header reuse:
 *   n_results  = n_vecs
 *   latency_us = graph_degree
 *   delta_merged = dim
 *   error[]    = shm_key (null-terminated)
 * ---------------------------------------------------------------- */
static void
handle_export_adjacency(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(db, idx);
    if (!e || !e->valid || !e->handle || e->shard_count >= 2)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "index not loaded or sharded (call after CREATE INDEX USING cagra)");
        return;
    }

    /* Hold the mutex while extracting from VRAM to prevent eviction. */
    uint32_t *adj  = NULL;
    float    *vecs = NULL;
    size_t    n_vecs;
    int       graph_degree;
    int       gpu  = (int)e->gpu_device_id;

    if (cuvs_cagra_extract_adjacency(e->handle, &adj, &vecs, &n_vecs,
                                      &graph_degree, gpu) != 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(adj); free(vecs);
        send_error(client_fd, "cuvs_cagra_extract_adjacency failed");
        return;
    }

    uint32_t dim     = e->dim;
    uint64_t *tids   = e->tids;   /* owned by daemon; copy below */
    int64_t   n_vecs_i = e->n_vecs;
    pthread_mutex_unlock(&g_index_mutex);

    /* Pack into shared memory:
     *   [uint32_t n_vecs][uint32_t graph_degree][uint32_t dim][uint32_t pad]
     *   [uint32_t adj[n_vecs * graph_degree]]
     *   [float    vecs[n_vecs * dim]]
     *   [uint64_t tids[n_vecs]]
     */
    size_t adj_bytes  = (size_t)n_vecs * (size_t)graph_degree * sizeof(uint32_t);
    size_t vecs_bytes = (size_t)n_vecs * (size_t)dim           * sizeof(float);
    size_t tids_bytes = (size_t)n_vecs * sizeof(uint64_t);
    size_t hdr_bytes  = 4 * sizeof(uint32_t);
    size_t total      = hdr_bytes + adj_bytes + vecs_bytes + tids_bytes;

    /* Generate a unique shm key for this reply. */
    static int adj_seq = 0;
    char shm_key[64];
    snprintf(shm_key, sizeof(shm_key), "/pg_cuvs_adj_%d_%d",
             (int)getpid(), __atomic_fetch_add(&adj_seq, 1, __ATOMIC_RELAXED));

    int shm_fd = shm_open(shm_key, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0)
    {
        free(adj); free(vecs);
        send_error(client_fd, "shm_open failed for export");
        return;
    }
    if (ftruncate(shm_fd, (off_t)total) != 0)
    {
        close(shm_fd); shm_unlink(shm_key); free(adj); free(vecs);
        send_error(client_fd, "ftruncate failed for export");
        return;
    }
    void *mem = mmap(NULL, total, PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (mem == MAP_FAILED)
    {
        shm_unlink(shm_key); free(adj); free(vecs);
        send_error(client_fd, "mmap failed for export");
        return;
    }

    /* Write header: [n_vecs][graph_degree][dim][metric] */
    uint32_t *hp = (uint32_t *)mem;
    hp[0] = (uint32_t)n_vecs;
    hp[1] = (uint32_t)graph_degree;
    hp[2] = (uint32_t)dim;
    hp[3] = e->metric;  /* CUVS_METRIC_* for source metric validation */

    char *dp = (char *)mem + hdr_bytes;
    memcpy(dp, adj,  adj_bytes);   dp += adj_bytes;
    memcpy(dp, vecs, vecs_bytes);  dp += vecs_bytes;
    memcpy(dp, tids, tids_bytes);

    munmap(mem, total);
    free(adj);
    free(vecs);

    LOG_INFO("[handle_export_adjacency] %u/%u N=%zu D=%d dim=%u shm=%s\n",
             db, idx, n_vecs, graph_degree, dim, shm_key);

    /* Reply: encode n_vecs/graph_degree/dim in header fields; shm_key in error[] */
    CuvsReplyHeader reply = {0};
    reply.status       = CUVS_STATUS_OK;
    reply.n_results    = (uint32_t)n_vecs;
    reply.latency_us   = (uint32_t)graph_degree;
    reply.delta_merged = (uint32_t)dim;
    strncpy(reply.error, shm_key, sizeof(reply.error) - 1);
    send_all(client_fd, &reply, sizeof(reply));
    (void)n_vecs_i;
}

/* ----------------------------------------------------------------
 * Handle EXPORT_HNSW_SHM command (CUVS_OP_EXPORT_HNSW_SHM, Phase 3J).
 *
 * Runs from_cagra() on the loaded CAGRA index and writes the resulting
 * multi-level HNSW to /dev/shm/ (RAM-backed, no disk I/O).  Returns the
 * /dev/shm path in reply.error[] so the backend can open and parse it,
 * then unlink after reading.
 * ---------------------------------------------------------------- */
static void
handle_export_hnsw_shm(int client_fd, const CuvsCmdFrame *cmd)
{
    uint32_t db  = cmd->db_oid;
    uint32_t idx = cmd->index_oid;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(db, idx);
    if (!e || !e->valid || !e->handle || e->shard_count >= 2)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "index not loaded or sharded");
        return;
    }
    CuvsCagraIndex handle = e->handle;
    int            gpu    = (int)e->gpu_device_id;
    pthread_mutex_unlock(&g_index_mutex);

    /* Generate unique /dev/shm path */
    static int hnsw_shm_seq = 0;
    char shm_path[256];
    snprintf(shm_path, sizeof(shm_path), "/dev/shm/pg_cuvs_hnsw_%d_%d",
             (int)getpid(),
             __atomic_fetch_add(&hnsw_shm_seq, 1, __ATOMIC_RELAXED));

    /* Serialize HNSW to /dev/shm (reuses existing cuvs_hnsw_serialize) */
    if (cuvs_hnsw_serialize(handle, shm_path, gpu) != 0)
    {
        send_error(client_fd, "from_cagra/serialize failed");
        return;
    }

    LOG_INFO("[handle_export_hnsw_shm] %u/%u → %s\n", db, idx, shm_path);

    CuvsReplyHeader reply = {0};
    reply.status = CUVS_STATUS_OK;
    strncpy(reply.error, shm_path, sizeof(reply.error) - 1);
    send_all(client_fd, &reply, sizeof(reply));
}

/* ----------------------------------------------------------------
 * Handle CACHE_STATS command (CUVS_OP_CACHE_STATS): per-GPU counters.
 * Phase 3E: one CuvsCacheStats row per usable GPU device.
 * ---------------------------------------------------------------- */
static void
handle_cache_stats(int client_fd)
{
    CuvsCacheStats rows[CUVS_MAX_GPUS];
    int n_rows = 0;

    pthread_mutex_lock(&g_index_mutex);
    int n = n_usable_gpus();
    for (int gi = 0; gi < n && n_rows < CUVS_MAX_GPUS; gi++)
    {
        int dev = usable_gpu(gi);
        CuvsCacheStats *cs = &rows[n_rows++];
        memset(cs, 0, sizeof(*cs));
        cs->gpu_device_id    = (uint32_t)dev;
        cs->hits             = g_cache_hits[dev];
        cs->misses           = g_cache_misses[dev];
        cs->evictions        = g_cache_evictions[dev];
        cs->reloads          = g_cache_reloads[dev];
        cs->persist_failures = g_cache_persist_fail[dev];
        cs->resident_count   = 0;
        for (int j = 0; j < g_n_indexes; j++)
        {
            IndexEntry *ej = &g_indexes[j];
            if (!ej->valid)
                continue;
            if (ej->shard_count >= 2)
            {
                /* Count each shard resident on this device. */
                for (int s = 0; s < ej->shard_count; s++)
                    if (ej->shards[s].valid &&
                        ej->shards[s].gpu_device_id == (uint32_t)dev)
                    {
                        cs->resident_count++;
                        /* Phase 3L: per-shard resident brute-force index VRAM. */
                        if (ej->shards[s].bf_idx)
                        {
                            cs->bf_vram_bytes += ej->shards[s].bf_vram_bytes;
                            cs->bf_precision = ej->bf_precision;
                        }
                    }
            }
            else if (ej->gpu_device_id == (uint32_t)dev)
            {
                cs->resident_count++;
                /* Phase 3L: unsharded resident brute-force index VRAM. */
                if (ej->main_bf_idx)
                {
                    cs->bf_vram_bytes += ej->main_bf_vram_bytes;
                    cs->bf_precision = ej->bf_precision;
                }
            }
        }
        cs->vram_used_bytes  = total_vram_used(dev);
        cs->vram_budget_bytes = g_max_vram_per_gpu[dev];
    }
    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = (uint32_t)n_rows;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n_rows > 0)
        send_all(client_fd, rows, (size_t)n_rows * sizeof(CuvsCacheStats));
}

/* ----------------------------------------------------------------
 * Handle SHARD_STATS command (CUVS_OP_SHARD_STATS): per-shard rows for every
 * resident sharded index in the requesting database (Phase 3F).
 * ---------------------------------------------------------------- */
static void
handle_shard_stats(int client_fd, const CuvsCmdFrame *cmd)
{
    pthread_mutex_lock(&g_index_mutex);

    /* Count shard rows first so we can size the reply buffer. */
    int total = 0;
    for (int i = 0; i < g_n_indexes; i++)
    {
        IndexEntry *e = &g_indexes[i];
        if (!e->valid || e->shard_count < 2)
            continue;
        if (e->db_oid != cmd->db_oid)
            continue;
        if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
            continue;
        total += e->shard_count;
    }

    CuvsShardStats *rows = (total > 0) ? malloc((size_t)total * sizeof(CuvsShardStats))
                                       : NULL;
    int n = 0;
    if (rows)
    {
        for (int i = 0; i < g_n_indexes; i++)
        {
            IndexEntry *e = &g_indexes[i];
            if (!e->valid || e->shard_count < 2)
                continue;
            if (e->db_oid != cmd->db_oid)
                continue;
            if (cmd->index_oid != 0 && e->index_oid != cmd->index_oid)
                continue;
            for (int s = 0; s < e->shard_count; s++)
            {
                ShardEntry *se = &e->shards[s];
                CuvsShardStats *r = &rows[n++];
                r->db_oid        = e->db_oid;
                r->index_oid     = e->index_oid;
                r->shard_id      = se->shard_id;
                r->gpu_device_id = se->gpu_device_id;
                r->n_vecs        = se->n_vecs;
                r->tid_offset    = se->tid_offset;
                r->vram_bytes    = se->vram_bytes;
                r->search_count  = se->search_count;
                r->error_count   = se->error_count;
                r->resident      = (uint32_t)(se->valid ? 1 : 0);
                r->last_status   = se->last_status;
            }
        }
    }

    pthread_mutex_unlock(&g_index_mutex);

    CuvsReplyHeader hdr = {0};
    hdr.status    = CUVS_STATUS_OK;
    hdr.n_results = (uint32_t)n;
    send_all(client_fd, &hdr, sizeof(hdr));
    if (n > 0)
        send_all(client_fd, rows, (size_t)n * sizeof(CuvsShardStats));
    free(rows);
}

/* ----------------------------------------------------------------
 * ADR-073: handle_build_flat — build a standalone flat (vectors-only) index.
 *
 * Persists .tids + .vectors ONLY (no .cagra graph) and registers a brute-force-
 * only entry (handle == NULL, is_flat = 1). It does NOT call finish_build_commit
 * (which serializes a CAGRA handle). The corpus handoff mirrors handle_build
 * (memfd via SCM_RIGHTS, or named shm) because the backend builds the flat corpus
 * with the same cuvs_corpus tiers. The resident GPU BF over .vectors is built
 * lazily on the first search (refresh_main_bf_cache). n_vecs >= 1 is allowed
 * (brute-force needs no neighborhood graph), reconciled with the load validator
 * (cuvs_tids_read requires n > 0).
 * ---------------------------------------------------------------- */
static void
handle_build_flat(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        if (passed_fd >= 0) close(passed_fd);
        send_error(client_fd, "flat build needs at least 1 vector");
        return;
    }
    {
        size_t per_vec = (size_t)cmd->dim * sizeof(float) + sizeof(uint64_t);
        if ((size_t)cmd->n_vecs > SIZE_MAX / per_vec)
        {
            if (passed_fd >= 0) close(passed_fd);
            send_error(client_fd, "build payload size overflow");
            return;
        }
    }

    size_t vec_bytes = (size_t)cmd->n_vecs * cmd->dim * sizeof(float);
    size_t tid_bytes = (size_t)cmd->n_vecs * sizeof(uint64_t);
    size_t total     = vec_bytes + tid_bytes;

    /* Corpus handoff (memfd tier via SCM_RIGHTS, else named shm) — same as
     * handle_build. */
    void *mem;
    if (passed_fd >= 0)
    {
        LOG_INFO("[handle_build_flat] corpus via memfd(SCM_RIGHTS) fd=%d total=%zu\n",
                 passed_fd, total);
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, passed_fd, 0);
        close(passed_fd);
    }
    else
    {
        LOG_INFO("[handle_build_flat] corpus via shm_open(%s)\n", cmd->shm_key);
        int shm_fd = shm_open(cmd->shm_key, O_RDONLY, 0);
        if (shm_fd < 0)
        {
            send_error(client_fd, "shm_open failed");
            return;
        }
        mem = mmap(NULL, total, PROT_READ, MAP_SHARED, shm_fd, 0);
        close(shm_fd);
    }
    if (mem == MAP_FAILED)
    {
        send_error(client_fd, "mmap failed");
        return;
    }

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    /* ADR-073 VRAM no-fit gate: the resident BF over .vectors needs N*dim*bytes of
     * VRAM at search time (non-evicting). If the corpus cannot fit ANY GPU's
     * configured budget even as float16, fail the build with a DISTINCT, actionable
     * message — far clearer than letting every later search fail with the generic
     * "sidecar missing or stale". A float32-only overflow is a non-fatal advisory.
     * Budget-only check (config, not live free VRAM); dynamic shortfalls still
     * surface at search time via refresh_main_bf_cache. */
    {
        size_t need_f32 = vec_bytes;
        size_t need_f16 = vec_bytes / 2;
        size_t max_budget = 0;
        int    unlimited = 0;
        int    ng = n_usable_gpus();
        for (int i = 0; i < ng; i++)
        {
            size_t b = g_max_vram_per_gpu[usable_gpu(i)];
            if (b == 0) { unlimited = 1; break; }   /* an unlimited GPU can hold it */
            if (b > max_budget) max_budget = b;
        }
        if (!unlimited && max_budget > 0 && need_f16 > max_budget)
        {
            munmap(mem, total);
            CuvsReplyHeader hdr = {0};
            hdr.status = CUVS_STATUS_OOM_FALLBACK;
            snprintf(hdr.error, sizeof(hdr.error),
                     "flat corpus %zu MB (float16 %zu MB) exceeds the GPU VRAM budget "
                     "(%zu MB); shard the table or use a cagra/ivfpq index",
                     need_f32 / (1024*1024), need_f16 / (1024*1024),
                     max_budget / (1024*1024));
            LOG_WARN("[handle_build_flat] %u/%u %s\n", cmd->db_oid, cmd->index_oid, hdr.error);
            send_all(client_fd, &hdr, sizeof(hdr));
            return;
        }
        if (!unlimited && max_budget > 0 && need_f32 > max_budget)
            LOG_WARN("[handle_build_flat] %u/%u corpus %zu MB exceeds VRAM budget as "
                     "float32; use WITH (precision='float16') for brute-force search\n",
                     cmd->db_oid, cmd->index_oid, need_f32 / (1024*1024));
    }

    /* --- Disk persistence: .tids + .vectors (tmp + rename). Our own commit,
     * NOT finish_build_commit (which serializes a .cagra graph). --- */
    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);

    /* Generation token: this build's .tids body CRC, embedded in .vectors so the
     * loader rejects a torn (stale) sidecar after a crash. */
    uint32_t tids_gen = cuvs_crc32(new_tids, (size_t)cmd->n_vecs * sizeof(uint64_t));

    char tids_final[512], tids_tmp[576];
    char vecs_final[512], vecs_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    vectors_file_path(vecs_final, sizeof(vecs_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp, sizeof(tids_tmp), "%s.tmp", tids_final);
    snprintf(vecs_tmp, sizeof(vecs_tmp), "%s.tmp", vecs_final);

    /* .vectors (float32 body + generation token) while the corpus is still mapped. */
    {
        FILE *vf = fopen(vecs_tmp, "wb");
        int vok = (vf != NULL);
        if (vok && cuvs_vectors_write(vf, cmd->n_vecs, cmd->dim, cmd->metric, tids_gen, vecs) != 0)
            vok = 0;
        if (vok && fflush(vf) != 0) vok = 0;
        if (vok && fsync(fileno(vf)) != 0) vok = 0;
        if (vf && fclose(vf) != 0) vok = 0;
        if (!vok)
        {
            unlink(vecs_tmp);
            free(new_tids);
            munmap(mem, total);
            send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .vectors persist failed");
            return;
        }
    }
    munmap(mem, total);

    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
    {
        unlink(vecs_tmp);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .tids persist failed");
        return;
    }

    /* Commit: rename .vectors then .tids. On a failure, unlink both so a partial
     * (e.g. .vectors but no .tids) never registers — load_index needs .tids. */
    if (rename(vecs_tmp, vecs_final) != 0)
    {
        unlink(vecs_tmp); unlink(tids_tmp);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .vectors rename failed");
        return;
    }
    if (rename(tids_tmp, tids_final) != 0)
    {
        unlink(tids_tmp); unlink(vecs_final);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "flat .tids rename failed");
        return;
    }
    {
        int dir_fd = open(save_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    /* Remove any stale CAGRA / IVF-PQ / .shards / .stale artifacts for this OID so
     * load_index unambiguously takes the flat branch. */
    {
        char p[512];
        index_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        ivfpq_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        stale_file_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid);   unlink(p);
        shards_manifest_path(p, sizeof(p), save_dir, cmd->db_oid, cmd->index_oid); unlink(p);
    }

    /* Register (or replace) in the hot registry — handle == NULL, is_flat = 1. */
    pthread_mutex_lock(&g_index_mutex);
    {
        int fgpu = pick_gpu_for_index(0);
        if (fgpu < 0) fgpu = usable_gpu(0);
        IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
        if (existing)
        {
            if (existing->handle)       cuvs_cagra_free(existing->handle, existing->gpu_device_id);
            if (existing->ivfpq_handle) cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id);
            free(existing->tids);
            free_delta_cache(existing);
            free_main_bf_cache(existing);
            if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
            existing->handle         = NULL;
            existing->ivfpq_handle   = NULL;
            existing->ivfpq_n_vecs   = 0;
            existing->ivfpq_vram_bytes = 0;
            existing->tids           = new_tids;
            existing->dim            = cmd->dim;
            existing->metric         = cmd->metric;
            existing->n_vecs         = cmd->n_vecs;
            existing->vram_bytes     = 0;
            existing->shard_count    = 0;
            existing->shards         = NULL;
            existing->inflight       = 0;
            existing->gpu_device_id  = (uint32_t) fgpu;
            existing->last_search    = time(NULL);
            existing->valid          = 1;
            existing->stale          = 0;
            existing->stale_since    = 0;
            existing->n_extended     = 0;
            existing->compact_count  = 0;
            existing->last_compact_at = 0;
            existing->warmup_state   = WARMUP_HOT;
            existing->last_search_mode = 3; /* gpu_bf */
            free(existing->rev_tids);     existing->rev_tids     = NULL;
            free(existing->rev_item_ids); existing->rev_item_ids = NULL;
            build_rev_tid_map(existing);
            reset_entry_stats(existing);
            existing->is_flat        = 1;   /* MUST be after reset_entry_stats */
        }
        else
        {
            if (g_n_indexes >= g_max_indexes)
                evict_lru(fgpu);
            if (g_n_indexes >= g_max_indexes)
            {
                /* Soft cap: artifacts durable on disk; reload on demand. */
                LOG_WARN("[handle_build_flat] registry full; %u/%u persisted, reload on demand\n",
                         cmd->db_oid, cmd->index_oid);
                free(new_tids);
                pthread_mutex_unlock(&g_index_mutex);
                CuvsReplyHeader hdr_ok = {0};
                hdr_ok.status = CUVS_STATUS_OK;
                send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
                return;
            }
            IndexEntry *e = &g_indexes[g_n_indexes++];
            e->db_oid          = cmd->db_oid;
            e->index_oid       = cmd->index_oid;
            e->dim             = cmd->dim;
            e->metric          = cmd->metric;
            e->n_vecs          = cmd->n_vecs;
            e->handle          = NULL;
            e->ivfpq_handle    = NULL;
            e->ivfpq_n_vecs    = 0;
            e->ivfpq_vram_bytes = 0;
            e->tids            = new_tids;
            e->rev_tids        = NULL;
            e->rev_item_ids    = NULL;
            build_rev_tid_map(e);
            e->vram_bytes      = 0;
            e->last_search     = time(NULL);
            e->valid           = 1;
            e->stale           = 0;
            e->stale_since     = 0;
            e->shard_count     = 0;
            e->shards          = NULL;
            e->inflight        = 0;
            e->gpu_device_id   = (uint32_t) fgpu;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx        = NULL;
            e->n_extended      = 0;
            e->compact_count   = 0;
            e->last_compact_at = 0;
            e->warmup_state    = WARMUP_HOT;
            e->last_search_mode = 3; /* gpu_bf */
            reset_entry_stats(e);
            e->is_flat         = 1;   /* MUST be after reset_entry_stats */
        }
    }
    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("pg_cuvs_server: built flat index %u/%u (%lld vecs, %zu MB on disk)\n",
             cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, vec_bytes / (1024*1024));

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * 3P: handle_build_ivfpq — build an IVF-PQ index from the corpus in shm.
 *
 * Follows the same mmap-corpus / write_tids_atomic / serialize / registry
 * pattern as handle_build's unsharded path, but produces .tids + .ivfpq
 * instead of .tids + .cagra. No shard/hnsw/delta support in initial release.
 * ---------------------------------------------------------------- */
static void
handle_build_ivfpq(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;
    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);   /* IVF-PQ always uses shm tier */

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        send_error(client_fd, "IVF-PQ build needs at least 1 vector");
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

    const float    *vecs    = (const float *)mem;
    const uint64_t *tids_in = (const uint64_t *)((const char *)mem + vec_bytes);

    uint32_t pq_dim  = cmd->pq_dim  ? cmd->pq_dim  : (uint32_t)((cmd->dim + 1) / 2);
    uint32_t pq_bits = cmd->pq_bits ? cmd->pq_bits : 8;
    /* IVF-PQ VRAM ~ n_vecs * pq_dim * pq_bits/8 (PQ codes only, no raw vecs) */
    size_t needed = (size_t)cmd->n_vecs * pq_dim * (pq_bits / 8 + 1);

    pthread_mutex_lock(&g_index_mutex);

    int target_gpu = pick_gpu_for_index(needed);
    if (target_gpu < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        strncpy(hdr.error, "IVF-PQ index too large for any GPU VRAM budget", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }
    if (ensure_vram(needed, target_gpu) < 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_OOM_FALLBACK;
        snprintf(hdr.error, sizeof(hdr.error),
                 "VRAM exhausted on GPU %d after eviction", target_gpu);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    LOG_DEBUG("[handle_build_ivfpq] building n_vecs=%lld dim=%u n_lists=%u pq_bits=%u pq_dim=%u gpu=%d\n",
              (long long)cmd->n_vecs, cmd->dim, cmd->n_lists, pq_bits, pq_dim, target_gpu);

    CuvsIvfPqIndex new_handle = NULL;
    int build_rc = cuvs_ivfpq_build(vecs, cmd->n_vecs, (int)cmd->dim, cmd->metric,
                                    cmd->n_lists, pq_bits, pq_dim,
                                    target_gpu, &new_handle);
    if (build_rc != 0 || !new_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_ivfpq_build failed");
        return;
    }

    uint64_t *new_tids = malloc(tid_bytes);
    if (!new_tids)
    {
        cuvs_ivfpq_free(new_handle, target_gpu);
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "malloc tids failed");
        return;
    }
    memcpy(new_tids, tids_in, tid_bytes);
    munmap(mem, total);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;
    mkdir(save_dir, 0700);

    char tids_final[512], tids_tmp[576];
    char ivfpq_final[512], ivfpq_tmp[576];
    tids_file_path(tids_final, sizeof(tids_final), save_dir, cmd->db_oid, cmd->index_oid);
    ivfpq_file_path(ivfpq_final, sizeof(ivfpq_final), save_dir, cmd->db_oid, cmd->index_oid);
    snprintf(tids_tmp,  sizeof(tids_tmp),  "%s.tmp", tids_final);
    snprintf(ivfpq_tmp, sizeof(ivfpq_tmp), "%s.tmp", ivfpq_final);

    if (write_tids_atomic(tids_tmp, cmd->n_vecs, cmd->dim, cmd->metric, new_tids) != 0)
        goto persist_fail;

    if (cuvs_ivfpq_serialize(new_handle, ivfpq_tmp, target_gpu) != 0)
    {
        LOG_ERROR("[handle_build_ivfpq] cuvs_ivfpq_serialize FAILED\n");
        goto persist_fail;
    }

    if (rename(tids_tmp, tids_final) != 0)
        goto persist_fail;
    if (rename(ivfpq_tmp, ivfpq_final) != 0)
    {
        unlink(tids_final);
        goto persist_fail;
    }

    {
        int dir_fd = open(save_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    /* Remove any stale CAGRA artifacts for this OID so load_index takes the IVF-PQ path. */
    {
        char cagra_path[512];
        index_file_path(cagra_path, sizeof(cagra_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(cagra_path);
        char stale_path[512];
        stale_file_path(stale_path, sizeof(stale_path), save_dir, cmd->db_oid, cmd->index_oid);
        unlink(stale_path);
    }

    /* Register (or replace) in the hot index registry. */
    {
        IndexEntry *existing = find_index(cmd->db_oid, cmd->index_oid);
        if (existing)
        {
            if (existing->handle)       cuvs_cagra_free(existing->handle, existing->gpu_device_id);
            if (existing->ivfpq_handle) cuvs_ivfpq_free(existing->ivfpq_handle, existing->gpu_device_id);
            free(existing->tids);
            free_delta_cache(existing);
            free_main_bf_cache(existing);
            if (existing->hnsw_idx) { cuvs_hnsw_free(existing->hnsw_idx); existing->hnsw_idx = NULL; }
            existing->handle         = NULL;
            existing->ivfpq_handle   = new_handle;
            existing->ivfpq_n_vecs   = cmd->n_vecs;
            existing->ivfpq_vram_bytes = needed;
            existing->tids           = new_tids;
            existing->dim            = cmd->dim;
            existing->metric         = cmd->metric;
            existing->n_vecs         = cmd->n_vecs;
            existing->vram_bytes     = needed;
            existing->gpu_device_id  = (uint32_t)target_gpu;
            existing->last_search    = time(NULL);
            existing->valid          = 1;
            existing->stale          = 0;
            existing->stale_since    = 0;
            existing->last_search_mode = 5; /* ivfpq */
            reset_entry_stats(existing);
            existing->n_extended = 0;
            free(existing->rev_tids);     existing->rev_tids     = NULL;
            free(existing->rev_item_ids); existing->rev_item_ids = NULL;
            build_rev_tid_map(existing);
        }
        else
        {
            if (g_n_indexes >= g_max_indexes)
                evict_lru(target_gpu);
            if (g_n_indexes >= g_max_indexes)
            {
                /* Soft cap: artifacts durable on disk, defer to reload-on-demand
                 * (load_index reloads IVF-PQ). Don't roll back; reply OK. */
                LOG_WARN("[handle_build_ivfpq] registry full; %u/%u persisted, will reload on demand\n",
                         cmd->db_oid, cmd->index_oid);
                cuvs_ivfpq_free(new_handle, target_gpu);
                free(new_tids);
                pthread_mutex_unlock(&g_index_mutex);
                {
                    CuvsReplyHeader hdr_ok = {0};
                    hdr_ok.status = CUVS_STATUS_OK;
                    send_all(client_fd, &hdr_ok, sizeof(hdr_ok));
                }
                return;
            }
            IndexEntry *e = &g_indexes[g_n_indexes++];
            e->db_oid          = cmd->db_oid;
            e->index_oid       = cmd->index_oid;
            e->dim             = cmd->dim;
            e->metric          = cmd->metric;
            e->n_vecs          = cmd->n_vecs;
            e->handle          = NULL;
            e->ivfpq_handle    = new_handle;
            e->ivfpq_n_vecs    = cmd->n_vecs;
            e->ivfpq_vram_bytes = needed;
            e->tids            = new_tids;
            e->vram_bytes      = needed;
            e->last_search     = time(NULL);
            e->valid           = 1;
            e->stale           = 0;
            e->stale_since     = 0;
            e->shard_count     = 0;
            e->shards          = NULL;
            e->gpu_device_id   = (uint32_t)target_gpu;
            e->delta_idx = NULL; e->delta_tids = NULL; e->n_delta = 0;
            e->delta_generation = 0; e->delta_mtime = 0; e->delta_vram_bytes = 0;
            e->delta_vecs_host = NULL; e->delta_n_cached = 0;
            e->main_bf_idx = NULL; e->main_bf_n = 0; e->main_bf_vram_bytes = 0;
            e->main_bf_mtime = 0; e->main_bf_generation = 0; e->bf_precision = 0;
            e->hnsw_idx        = NULL;
            e->warmup_state    = WARMUP_HOT;
            e->last_search_mode = 5; /* ivfpq */
            reset_entry_stats(e);
            build_rev_tid_map(e);
        }
    }

    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("pg_cuvs_server: built IVF-PQ index %u/%u (%lld vecs, %zu MB VRAM)\n",
             cmd->db_oid, cmd->index_oid, (long long)cmd->n_vecs, needed / (1024*1024));

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
    return;

persist_fail:
    cuvs_ivfpq_free(new_handle, target_gpu);
    free(new_tids);
    unlink(tids_tmp);
    unlink(ivfpq_tmp);
    pthread_mutex_unlock(&g_index_mutex);
    send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED, "IVF-PQ disk persist failed");
}

/* ----------------------------------------------------------------
 * 3P: handle_search_ivfpq — search an IVF-PQ index.
 * ---------------------------------------------------------------- */
static void
handle_search_ivfpq(int client_fd, const CuvsCmdFrame *cmd)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e)
    {
        int load_rc = load_index(cmd->db_oid, cmd->index_oid);
        if (load_rc == 0)
            e = find_index(cmd->db_oid, cmd->index_oid);
    }

    if (!e || !e->ivfpq_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = e ? CUVS_STATUS_NOT_FOUND : CUVS_STATUS_NOT_FOUND;
        snprintf(hdr.error, sizeof(hdr.error), "IVF-PQ index %u/%u not found",
                 cmd->db_oid, cmd->index_oid);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    if (cmd->metric != e->metric)
    {
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_METRIC_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "index built with metric %u but queried with metric %u; REINDEX required",
                 e->metric, cmd->metric);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

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

    if (cmd->dim != e->dim)
    {
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_DIM_MISMATCH;
        snprintf(hdr.error, sizeof(hdr.error),
                 "query dim %u does not match index dim %u", cmd->dim, e->dim);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int k = (int)cmd->k;
    if (k <= 0) k = 1;

    CuvsSearchResult *raw = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsSearchResult));
    CuvsResult       *results = malloc((size_t)(k > 0 ? k : 1) * sizeof(CuvsResult));
    if (!raw || !results)
    {
        free(raw); free(results);
        munmap(query, vec_bytes);
        pthread_mutex_unlock(&g_index_mutex);
        send_error(client_fd, "malloc failed");
        return;
    }

    uint32_t n_probes = cmd->n_probes ? cmd->n_probes : 64;
    int sret = cuvs_ivfpq_search(e->ivfpq_handle, query, (int)cmd->dim, k,
                                  n_probes, raw, (int)e->gpu_device_id);
    munmap(query, vec_bytes);

    int n_valid = 0;
    if (sret == 0)
    {
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

    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t)(
        (t1.tv_sec - t0.tv_sec) * 1000000 +
        (t1.tv_nsec - t0.tv_nsec) / 1000);

    e->last_search      = time(NULL);
    e->last_search_mode = 5; /* ivfpq */
    record_search_stat(e, sret == 0 ? CUVS_STATUS_OK : CUVS_STATUS_ERROR, latency_us, NULL);
    e->last_requested_k = cmd->k;
    e->last_returned_k  = (uint32_t)n_valid;

    pthread_mutex_unlock(&g_index_mutex);

    free(raw);

    if (sret != 0)
    {
        free(results);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_ERROR;
        strncpy(hdr.error, "IVF-PQ search failed", sizeof(hdr.error) - 1);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

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
 * 3Q: handle_extend — extend a loaded CAGRA index in-place with new vectors.
 *
 * shm layout: same as BUILD — [float32 vecs: n_vecs×dim][uint64_t tids: n_vecs].
 * After cuvs_cagra_extend, VRAM graph includes the new vectors; no disk write
 * (extend_sync=false default). Rev_tids rebuilt so 3O prefilter stays correct.
 * ---------------------------------------------------------------- */
static void
handle_extend(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;

    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);

    if (cmd->n_vecs < 1 || cmd->dim == 0)
    {
        send_error(client_fd, "EXTEND needs at least 1 vector");
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

    const float    *new_vecs = (const float *)mem;
    const uint64_t *new_tids_src = (const uint64_t *)((const char *)mem + vec_bytes);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e || !e->handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        CuvsReplyHeader hdr = {0};
        hdr.status = CUVS_STATUS_NOT_FOUND;
        snprintf(hdr.error, sizeof(hdr.error), "CAGRA index %u/%u not loaded",
                 cmd->db_oid, cmd->index_oid);
        send_all(client_fd, &hdr, sizeof(hdr));
        return;
    }

    int64_t  old_n = e->n_vecs;
    int64_t  n_new = cmd->n_vecs;

    /* Pre-flight VRAM budget check.
     * Cannot call ensure_vram here: evict_lru shifts g_indexes[], invalidating
     * this IndexEntry *e pointer.  A budget-only check is sufficient — the
     * backend falls through to delta on BUILD_FAILED (pg_cuvs.c:3029). */
    size_t new_vram   = estimate_vram_bytes(old_n + n_new, (int)e->dim);
    size_t delta_vram = (new_vram > e->vram_bytes) ? new_vram - e->vram_bytes : 0;
    int    dev        = (int)e->gpu_device_id;
    if (delta_vram > 0 &&
        g_max_vram_per_gpu[dev] > 0 &&
        total_vram_used(dev) + delta_vram > g_max_vram_per_gpu[dev])
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "not enough VRAM for extend");
        return;
    }

    /* Grow TIDs array before the GPU call; failure here is cheap to handle. */
    uint64_t *grown = realloc(e->tids, (size_t)(old_n + n_new) * sizeof(uint64_t));
    if (!grown)
    {
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "realloc tids failed");
        return;
    }
    e->tids = grown;
    memcpy(e->tids + old_n, new_tids_src, (size_t)n_new * sizeof(uint64_t));

    int rc = cuvs_cagra_extend(e->handle, new_vecs, n_new, (int)cmd->dim,
                               cmd->max_chunk_size, (int)e->gpu_device_id);
    if (rc != 0)
    {
        /* Rollback TIDs to old_n: shrink the buffer so the invariant
         * e->tids[0..n_vecs-1] == valid holds again. realloc to a smaller
         * size is guaranteed to succeed on all POSIX malloc implementations;
         * on failure (defensive) the oversized buffer is still safe because
         * n_vecs was not updated. */
        if (old_n > 0)
        {
            uint64_t *shrunk = realloc(e->tids, (size_t)old_n * sizeof(uint64_t));
            if (shrunk)
                e->tids = shrunk;
        }
        pthread_mutex_unlock(&g_index_mutex);
        munmap(mem, total);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED, "cuvs_cagra_extend failed");
        return;
    }

    e->n_vecs     += n_new;
    e->n_extended += n_new;
    e->vram_bytes  = estimate_vram_bytes(e->n_vecs, (int)e->dim);

    /* Rebuild rev_tids to include new item_ids (needed for 3O prefilter). */
    free(e->rev_tids);     e->rev_tids     = NULL;
    free(e->rev_item_ids); e->rev_item_ids = NULL;
    build_rev_tid_map(e);

    pthread_mutex_unlock(&g_index_mutex);
    munmap(mem, total);

    LOG_INFO("[handle_extend] %u/%u: added %lld vecs (total %lld, n_extended %lld)\n",
             cmd->db_oid, cmd->index_oid,
             (long long)n_new, (long long)e->n_vecs, (long long)e->n_extended);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* Comparator for qsort/bsearch on raw uint64_t arrays. */
static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ----------------------------------------------------------------
 * 3Q: handle_compact — remove tombstoned vectors via cagra::merge.
 *
 * No extra shm payload; daemon reads .tombstone sidecar from save_dir.
 * After merge: new handle + compacted tids + rebuilt rev_tids + save_index.
 * ---------------------------------------------------------------- */
static void
handle_compact(int client_fd, const CuvsCmdFrame *cmd)
{
    char index_dir[256] = {0};
    int  passed_fd = -1;

    if (cuvs_fd_recv(client_fd, index_dir, sizeof(index_dir), &passed_fd) < 0)
    {
        send_error(client_fd, "recv index_dir failed");
        return;
    }
    if (passed_fd >= 0) close(passed_fd);

    const char *save_dir = (index_dir[0] != '\0') ? index_dir : g_index_dir;

    /* Read tombstone file before taking the mutex (disk I/O only). */
    char tombstone_path[512];
    snprintf(tombstone_path, sizeof(tombstone_path), "%s/%u_%u.tombstone",
             save_dir, cmd->db_oid, cmd->index_oid);

    FILE *tf = fopen(tombstone_path, "rb");
    if (!tf)
    {
        /* No tombstone: nothing to compact. Reply OK. */
        CuvsReplyHeader ok = {0};
        ok.status = CUVS_STATUS_OK;
        send_all(client_fd, &ok, sizeof(ok));
        return;
    }

    CuvsTombstoneHeader thdr;
    if (fread(&thdr, sizeof(thdr), 1, tf) != 1 ||
        thdr.magic != CUVS_TOMBSTONE_MAGIC || thdr.n_entries <= 0)
    {
        fclose(tf);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "invalid tombstone file");
        return;
    }

    int64_t  n_dead   = thdr.n_entries;
    uint64_t *dead_tids = malloc((size_t)n_dead * sizeof(uint64_t));
    if (!dead_tids)
    {
        fclose(tf);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "malloc dead_tids failed");
        return;
    }

    for (int64_t i = 0; i < n_dead; i++)
    {
        CuvsTombstoneRecord rec;
        if (fread(&rec, sizeof(rec), 1, tf) != 1)
        {
            fclose(tf);
            free(dead_tids);
            send_error_code(client_fd, CUVS_STATUS_ERROR, "tombstone read failed");
            return;
        }
        dead_tids[i] = rec.tid;
    }
    fclose(tf);
    qsort(dead_tids, (size_t)n_dead, sizeof(uint64_t), cmp_u64);

    pthread_mutex_lock(&g_index_mutex);

    IndexEntry *e = find_index(cmd->db_oid, cmd->index_oid);
    if (!e || !e->handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(dead_tids);
        send_error_code(client_fd, CUVS_STATUS_NOT_FOUND,
                        "CAGRA index not loaded for compact");
        return;
    }

    /* Build keep_bits: bit[i]=1 → keep vector i (not tombstoned). */
    int64_t   n_vecs   = e->n_vecs;
    int64_t   n_words  = (n_vecs + 31) / 32;
    uint32_t *keep_bits = calloc((size_t)n_words, sizeof(uint32_t));
    if (!keep_bits)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(dead_tids);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "calloc keep_bits failed");
        return;
    }

    int64_t dead_count = 0;
    for (int64_t i = 0; i < n_vecs; i++)
    {
        if (!bsearch(&e->tids[i], dead_tids, (size_t)n_dead,
                     sizeof(uint64_t), cmp_u64))
        {
            keep_bits[i / 32] |= (1u << (i % 32));
        }
        else
        {
            dead_count++;
        }
    }
    free(dead_tids);

    if (dead_count == 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(keep_bits);
        unlink(tombstone_path);
        CuvsReplyHeader ok = {0};
        ok.status = CUVS_STATUS_OK;
        send_all(client_fd, &ok, sizeof(ok));
        return;
    }

    LOG_INFO("[handle_compact] %u/%u: removing %lld of %lld vectors\n",
             cmd->db_oid, cmd->index_oid, (long long)dead_count, (long long)n_vecs);

    int64_t   new_n    = n_vecs - dead_count;

    /* Build compacted TIDs BEFORE the GPU call — both operations need keep_bits.
     * Free keep_bits only after both consumers are done. */
    uint64_t *new_tids = malloc((size_t)new_n * sizeof(uint64_t));
    if (!new_tids)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(keep_bits);
        send_error_code(client_fd, CUVS_STATUS_ERROR, "malloc new_tids failed");
        return;
    }
    {
        int64_t j = 0;
        for (int64_t i = 0; i < n_vecs; i++)
            if (keep_bits[i / 32] & (1u << (i % 32)))
                new_tids[j++] = e->tids[i];
    }

    CuvsCagraIndex new_handle = cuvs_cagra_compact(
        e->handle, keep_bits, n_vecs, e->metric, (int)e->gpu_device_id);
    free(keep_bits);

    if (!new_handle)
    {
        pthread_mutex_unlock(&g_index_mutex);
        free(new_tids);
        send_error_code(client_fd, CUVS_STATUS_BUILD_FAILED,
                        "cuvs_cagra_compact failed");
        return;
    }

    /* Swap handle + tids. */
    CuvsCagraIndex old_handle = e->handle;
    e->handle = new_handle;
    free(e->tids);
    e->tids = new_tids;
    e->n_vecs      = new_n;
    e->n_extended       = 0;
    e->compact_count++;
    e->last_compact_at  = time(NULL);
    e->vram_bytes  = estimate_vram_bytes(new_n, (int)e->dim);

    cuvs_cagra_free(old_handle, (int)e->gpu_device_id);

    /* Rebuild rev_tids from scratch. */
    free(e->rev_tids);     e->rev_tids     = NULL;
    free(e->rev_item_ids); e->rev_item_ids = NULL;
    build_rev_tid_map(e);

    /* Persist to disk. */
    if (save_index(e) != 0)
    {
        pthread_mutex_unlock(&g_index_mutex);
        send_error_code(client_fd, CUVS_STATUS_PERSIST_FAILED,
                        "save_index failed after compact");
        return;
    }

    unlink(tombstone_path);

    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("[handle_compact] %u/%u: compact OK, %lld vecs remaining\n",
             cmd->db_oid, cmd->index_oid, (long long)new_n);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * handle_set_vram_budget — runtime VRAM budget override (test/admin).
 *
 * cmd->n_vecs = new budget in bytes; 0 = unlimited.
 * No extra payload — just update g_max_vram_per_gpu for all GPUs.
 * ---------------------------------------------------------------- */
static void
handle_set_vram_budget(int client_fd, const CuvsCmdFrame *cmd)
{
    size_t new_budget = (size_t)(uint64_t)cmd->n_vecs;  /* 0 = unlimited */

    pthread_mutex_lock(&g_index_mutex);
    g_max_vram_bytes = new_budget;
    for (int d = 0; d < CUVS_MAX_GPUS; d++)
        g_max_vram_per_gpu[d] = new_budget;
    pthread_mutex_unlock(&g_index_mutex);

    LOG_INFO("[set_vram_budget] budget set to %zu bytes\n", new_budget);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* ----------------------------------------------------------------
 * handle_eat_vram / handle_free_vram — test helpers.
 *
 * EAT:  cmd->n_vecs = leave_bytes, cmd->dim = device_id
 * FREE: cmd->dim = device_id
 * ---------------------------------------------------------------- */
static void
handle_eat_vram(int client_fd, const CuvsCmdFrame *cmd)
{
    int64_t leave_bytes = cmd->n_vecs;
    int     dev         = ((int)cmd->dim >= 0 && (int)cmd->dim < CUVS_MAX_GPUS)
                          ? (int)cmd->dim : 0;

    int rc = cuvs_eat_vram(leave_bytes, dev);

    CuvsReplyHeader ok = {0};
    ok.status = (rc == 0) ? CUVS_STATUS_OK : CUVS_STATUS_ERROR;
    send_all(client_fd, &ok, sizeof(ok));
}

static void
handle_free_vram(int client_fd, const CuvsCmdFrame *cmd)
{
    int dev = ((int)cmd->dim >= 0 && (int)cmd->dim < CUVS_MAX_GPUS)
              ? (int)cmd->dim : 0;

    cuvs_free_vram(dev);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* handle_inject_extend_oom — test helper: arm/disarm synthetic OOM in cuvs_cagra_extend.
 * cmd->dim == 1 arms; == 0 disarms. */
static void
handle_inject_extend_oom(int client_fd, const CuvsCmdFrame *cmd)
{
    cuvs_set_inject_extend_oom((int)cmd->dim);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
}

/* handle_inject_build_oom — test helper (ADR-070 Bug #3): arm synthetic OOM for
 * the next cmd->dim cuvs_cagra_build calls, so a test can exercise the daemon's
 * evict-and-retry path deterministically. cmd->dim == 0 disarms. */
static void
handle_inject_build_oom(int client_fd, const CuvsCmdFrame *cmd)
{
    cuvs_set_inject_build_oom((int)cmd->dim);

    CuvsReplyHeader ok = {0};
    ok.status = CUVS_STATUS_OK;
    send_all(client_fd, &ok, sizeof(ok));
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
        case CUVS_OP_SEARCH_BATCH:
            handle_search_batch(client_fd, &cmd);
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
        case CUVS_OP_SHARD_STATS:
            handle_shard_stats(client_fd, &cmd);
            break;
        case CUVS_OP_DROP_INDEX:
            handle_drop(client_fd, &cmd);
            break;
        case CUVS_OP_EXPORT_ADJACENCY:
            handle_export_adjacency(client_fd, &cmd);
            break;
        case CUVS_OP_EXPORT_HNSW_SHM:
            handle_export_hnsw_shm(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD_IVFPQ:
            handle_build_ivfpq(client_fd, &cmd);
            break;
        case CUVS_OP_BUILD_FLAT:
            handle_build_flat(client_fd, &cmd);   /* ADR-073 */
            break;
        case CUVS_OP_SEARCH_IVFPQ:
            handle_search_ivfpq(client_fd, &cmd);
            break;
        case CUVS_OP_EXTEND:
            handle_extend(client_fd, &cmd);
            break;
        case CUVS_OP_COMPACT:
            handle_compact(client_fd, &cmd);
            break;
        case CUVS_OP_SET_VRAM_BUDGET:
            handle_set_vram_budget(client_fd, &cmd);
            break;
        case CUVS_OP_EAT_VRAM:
            handle_eat_vram(client_fd, &cmd);
            break;
        case CUVS_OP_FREE_VRAM:
            handle_free_vram(client_fd, &cmd);
            break;
        case CUVS_OP_INJECT_EXTEND_OOM:
            handle_inject_extend_oom(client_fd, &cmd);
            break;
        case CUVS_OP_INJECT_BUILD_OOM:
            handle_inject_build_oom(client_fd, &cmd);
            break;
        case CUVS_OP_SEARCH_STREAM_BF:
            handle_search_stream_bf(client_fd, &cmd);
            break;
        case CUVS_OP_SEARCH_BF_TRANSIENT:
            handle_search_bf_transient(client_fd, &cmd);
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
/* Phase 3L-9: mark every queued BF request done with `status` and wake its
 * producer. Caller holds g_bf_mtx. */
static void
bf_batch_fail_all_locked(int status)
{
    for (int i = 0; i < g_bf_queue_n; i++)
    {
        g_bf_queue[i]->status = status;
        g_bf_queue[i]->done   = 1;
    }
    g_bf_queue_n = 0;
    pthread_cond_broadcast(&g_bf_done_cond);
}

/* Phase 3L-9: run one coalesced group (requests in `batch` with gid[i]==g, all
 * sharing a (db,index,precision,dim) key) as a single cuvs_bf_search_batch
 * dispatch. Acquires g_index_mutex for the GPU work (eviction-safe, mirrors the
 * immediate path); the caller must NOT hold g_bf_mtx here. Writes each member's
 * out/n_out/status (but not `done` — the caller sets that after all groups). */
static void
bf_batch_run_group(CuvsBfRequest **batch, const int *gid, int n, int g)
{
    int idx[CUVS_BF_BATCH_MAX], Q = 0;
    for (int i = 0; i < n; i++)
        if (gid[i] == g)
            idx[Q++] = i;
    if (Q == 0)
        return;

    CuvsBfRequest *r0   = batch[idx[0]];
    int            dim  = (int) r0->key.dim;
    int            maxk = 0;
    for (int i = 0; i < Q; i++)
        if (batch[idx[i]]->k > maxk)
            maxk = batch[idx[i]]->k;

    pthread_mutex_lock(&g_index_mutex);
    IndexEntry *e = find_index(r0->key.db_oid, r0->key.index_oid);
    if (!e && load_index(r0->key.db_oid, r0->key.index_oid) == 0)
        e = find_index(r0->key.db_oid, r0->key.index_oid);

    int status = CUVS_STATUS_OK;
    if (!e)
        status = CUVS_STATUS_NOT_FOUND;
    else if (e->stale)
        status = CUVS_STATUS_STALE;
    else
    {
        refresh_main_bf_cache(e, r0->key.precision);
        if (!e->main_bf_idx)
            status = CUVS_STATUS_NO_VECTORS;
    }
    if (status != CUVS_STATUS_OK)
    {
        if (e)
            record_search_stat(e, status, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = status; batch[idx[i]]->n_out = 0; }
        return;
    }

    int K = (int) ((int64_t) maxk < e->n_vecs ? (int64_t) maxk : e->n_vecs);
    float            *queries = malloc((size_t) Q * (size_t) dim * sizeof(float));
    CuvsSearchResult *raw     = malloc((size_t) Q * (size_t) K * sizeof(CuvsSearchResult));
    if (!queries || !raw)
    {
        free(queries); free(raw);
        pthread_mutex_unlock(&g_index_mutex);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }
    for (int i = 0; i < Q; i++)
        memcpy(queries + (size_t) i * dim, batch[idx[i]]->query, (size_t) dim * sizeof(float));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ret = cuvs_bf_search_batch(e->main_bf_idx, queries, Q, dim, K, raw, delta_gpu_of(e));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint32_t latency_us = (uint32_t) ((t1.tv_sec - t0.tv_sec) * 1000000 +
                                      (t1.tv_nsec - t0.tv_nsec) / 1000);

    if (ret != 0)
    {
        record_search_stat(e, CUVS_STATUS_OOM_FALLBACK, 0, NULL);
        pthread_mutex_unlock(&g_index_mutex);
        free(queries); free(raw);
        for (int i = 0; i < Q; i++) { batch[idx[i]]->status = CUVS_STATUS_OOM_FALLBACK; batch[idx[i]]->n_out = 0; }
        return;
    }

    for (int i = 0; i < Q; i++)
    {
        CuvsBfRequest *r  = batch[idx[i]];
        int            ki = (r->k < K) ? r->k : K;
        int            nv = 0;
        for (int j = 0; j < ki; j++)
        {
            int64_t id = raw[(size_t) i * K + j].item_id;
            if (id < 0 || id >= e->n_vecs) continue;
            r->out[nv].tid      = e->tids[id];
            r->out[nv].distance = raw[(size_t) i * K + j].distance;
            nv++;
        }
        r->n_out  = nv;
        r->status = CUVS_STATUS_OK;
        record_search_stat(e, CUVS_STATUS_OK, latency_us, NULL);   /* per request */
    }
    e->bf_batch_count++;            /* one coalesced GPU dispatch served Q requests */
    e->last_search_mode = 3;        /* gpu_bf */
    e->last_requested_k = (uint32_t) r0->k;
    e->last_returned_k  = (uint32_t) batch[idx[0]]->n_out;
    pthread_mutex_unlock(&g_index_mutex);
    free(queries);
    free(raw);
}

/* Phase 3L-9: process the currently queued BF requests. Caller holds g_bf_mtx;
 * returns holding it. Snapshots + clears the queue (so producers can fill the
 * next batch), releases g_bf_mtx, groups by key, runs one cuvs_bf_search_batch
 * per group, then re-locks and wakes every producer in this batch. */
static void
bf_batch_process_locked(void)
{
    int n = g_bf_queue_n;
    if (n == 0)
        return;

    CuvsBfRequest *batch[CUVS_BF_BATCH_MAX];
    for (int i = 0; i < n; i++)
        batch[i] = g_bf_queue[i];
    g_bf_queue_n = 0;
    pthread_mutex_unlock(&g_bf_mtx);

    CuvsBfKey keys[CUVS_BF_BATCH_MAX];
    int       gid[CUVS_BF_BATCH_MAX];
    int       ng = 0;
    for (int i = 0; i < n; i++)
        keys[i] = batch[i]->key;
    cuvs_bf_batch_group(keys, n, gid, &ng);

    for (int g = 0; g < ng; g++)
        bf_batch_run_group(batch, gid, n, g);

    pthread_mutex_lock(&g_bf_mtx);
    for (int i = 0; i < n; i++)
        batch[i]->done = 1;
    pthread_cond_broadcast(&g_bf_done_cond);
}

/* Phase 3L-9: the single BF micro-batch consumer thread. Idle (1s poll) until a
 * request is enqueued or shutdown. On the first queued request it waits that
 * request's bf_batch_wait_us window (lock released, so more accumulate) then
 * coalesces everything queued into one batch. */
static void *
bf_batch_worker_thread(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&g_bf_mtx);
    while (!g_shutdown)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;                 /* 1s poll so shutdown stays responsive */
        while (g_bf_queue_n == 0 && !g_shutdown)
            if (pthread_cond_timedwait(&g_bf_cond, &g_bf_mtx, &ts) == ETIMEDOUT)
                break;
        if (g_shutdown)
            break;
        if (g_bf_queue_n == 0)
            continue;

        /* Accumulation window: let concurrent requests pile up before the GPU
         * dispatch. Released lock during the sleep so producers can enqueue. */
        uint32_t window = g_bf_queue[0]->wait_us;
        if (window > 10000) window = 10000;   /* GUC max; defensive */
        if (window > 0)
        {
            /* window us -> ns; window<=10000 keeps tv_nsec well under 1e9. */
            struct timespec ws = { 0, (long) window * 1000 };
            pthread_mutex_unlock(&g_bf_mtx);
            nanosleep(&ws, NULL);
            pthread_mutex_lock(&g_bf_mtx);
        }
        bf_batch_process_locked();
    }
    /* Shutdown: fail any still-queued requests so producers never hang. */
    bf_batch_fail_all_locked(CUVS_STATUS_UNAVAILABLE);
    pthread_mutex_unlock(&g_bf_mtx);
    return NULL;
}

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

    /* Phase 3L-9: wake the BF batch worker (g_shutdown is already set) so it
     * fails any queued requests and exits, then join it. Always spawned. */
    if (g_bf_worker_started)
    {
        pthread_mutex_lock(&g_bf_mtx);
        pthread_cond_broadcast(&g_bf_cond);
        pthread_mutex_unlock(&g_bf_mtx);
        pthread_join(g_bf_worker_tid, NULL);
        LOG_INFO("bf_batch: worker thread joined\n");
    }

    /* Phase 3D: signal warmup workers to exit and wait for them. */
    if (g_snapshot_uri[0] != '\0')
    {
        pthread_mutex_lock(&g_warmup_mutex);
        pthread_cond_broadcast(&g_warmup_cond);
        pthread_mutex_unlock(&g_warmup_mutex);

        for (int i = 0; i < g_warmup_nthreads; i++)
            pthread_join(g_warmup_tids[i], NULL);
        LOG_INFO("warmup: all worker threads joined\n");
    }

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
 * ADR-075 Phase 1: probe hardware constants once at boot and write the GLOBAL
 * cuvs_hw_profile (CRC) + cuvs_daemon_identity (text) sidecars atomically. The
 * planner reads these cheaply; failure is non-fatal (planner falls back to
 * compiled DEFAULTs). NOT consumed by any cost decision yet (Phase 2).
 * ---------------------------------------------------------------- */
/* ADR-075 Phase 2: host-side coefficient probes (no CUDA). Both best-effort and
 * return -1 on failure so the caller leaves the field at DEFAULT with its bit
 * clear (→ planner falls back to the legacy cost path for that coefficient). */

/* Local AF_UNIX round-trip latency floor (microseconds) — the IPC cost a
 * backend<->daemon request pays before any work. */
static double
probe_ipc_rtt_us(void)
{
    int    sv[2];
    char   b = 0;
    double best = 1e30;
    int    i;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return -1.0;
    /* warm */
    if (write(sv[0], &b, 1) == 1 && read(sv[1], &b, 1) == 1 &&
        write(sv[1], &b, 1) == 1 && read(sv[0], &b, 1) == 1) { /* ok */ }
    for (i = 0; i < 50; i++)
    {
        struct timespec t0, t1;
        double us;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (write(sv[0], &b, 1) != 1 || read(sv[1], &b, 1) != 1 ||
            write(sv[1], &b, 1) != 1 || read(sv[0], &b, 1) != 1) { best = 1e30; break; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
        if (us > 0.0 && us < best) best = us;
    }
    close(sv[0]); close(sv[1]);
    return (best < 1e29) ? best : -1.0;
}

/* CPU L2-distance throughput, (vectors*dim) per microsecond — the anchor that
 * converts GPU microseconds into PG cost units. Clamped to a plausible band;
 * outside it returns -1 (probe distrusted → DEFAULT/legacy). */
static double
probe_cpu_dist_tput(void)
{
    const int64_t   n = 100000; const int dim = 128;
    float          *corpus = (float *) malloc((size_t) n * dim * sizeof(float));
    float           q[128];
    struct timespec t0, t1;
    volatile double sink = 0.0;
    double          us, tput;
    int64_t         i; int j;

    if (!corpus)
        return -1.0;
    for (i = 0; i < n * dim; i++)
        corpus[i] = (float) ((uint64_t) i * 2654435761u % 1000) / 1000.0f;
    for (j = 0; j < dim; j++) q[j] = 0.1f;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < n; i++)
    {
        const float *v = corpus + i * dim;
        double s = 0.0;
        for (j = 0; j < dim; j++) { double d = (double) v[j] - q[j]; s += d * d; }
        sink += s;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void) sink;
    free(corpus);

    us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    if (us <= 0.0)
        return -1.0;
    tput = (double) n * dim / us;            /* (vec*dim)/us */
    if (tput < 1.0 || tput > 1.0e6)          /* implausible → distrust */
        return -1.0;
    return tput;
}

static void
write_hw_profile(void)
{
    int           dev = (g_n_allowed_gpus > 0) ? g_allowed_gpus[0] : 0;
    CuvsHwProfile p;
    char          final[512], tmp[576];
    FILE         *f;

    mkdir(g_index_dir, 0700);   /* may not exist yet if no index built */

    memset(&p, 0, sizeof(p));
    if (dev >= 0 && dev < g_n_gpus)
    {
        strncpy(p.gpu_name, g_gpus[dev].name, sizeof(p.gpu_name) - 1);
        p.total_vram_bytes = (int64_t) g_gpus[dev].total_vram_bytes;
    }
    p.n_gpus      = (uint32_t) g_n_gpus;
    p.measured_at = (int64_t) time(NULL);
    /* Conservative DEFAULTs (shared with the planner fallback); probes overwrite. */
    p.link_bw_bpus    = CUVS_HWP_DEFAULT_LINK_BW;
    p.hbm_bw_bpus     = CUVS_HWP_DEFAULT_HBM_BW;
    p.gpu_bf_tput     = CUVS_HWP_DEFAULT_BF_TPUT;
    p.ipc_rtt_us      = CUVS_HWP_DEFAULT_IPC_RTT;
    p.cpu_dist_tput   = CUVS_HWP_DEFAULT_CPU_DIST;   /* v2; probed in Stage 3 */
    p.gpu_cagra_lat_us = CUVS_HWP_DEFAULT_CAGRA_LAT; /* v2; probed in Stage 3 */
    p.probe_status    = 0;

    cuvs_probe_hw(dev, &p.link_bw_bpus, &p.hbm_bw_bpus, &p.gpu_bf_tput,
                  &p.gpu_cagra_lat_us, &p.probe_status);

    /* Host-side coefficients (no CUDA): IPC RTT floor + CPU distance throughput. */
    {
        double rtt  = probe_ipc_rtt_us();
        double cput = probe_cpu_dist_tput();
        if (rtt  > 0.0) { p.ipc_rtt_us    = rtt;  p.probe_status |= CUVS_HWPROBE_IPC_RTT; }
        if (cput > 0.0) { p.cpu_dist_tput = cput; p.probe_status |= CUVS_HWPROBE_CPU_DIST; }
    }

    /* atomic profile write */
    snprintf(final, sizeof(final), "%s/cuvs_hw_profile", g_index_dir);
    snprintf(tmp,   sizeof(tmp),   "%s.tmp", final);
    f = fopen(tmp, "wb");
    if (f)
    {
        int ok = (cuvs_hw_profile_write(f, &p) == 0);
        if (ok && fflush(f) != 0) ok = 0;
        if (ok && fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, final) != 0) ok = 0;
        if (!ok) { unlink(tmp); LOG_WARN("pg_cuvs_server: hw_profile write failed errno=%d\n", errno); }
    }
    else
        LOG_WARN("pg_cuvs_server: hw_profile fopen(%s) failed errno=%d\n", tmp, errno);

    /* atomic daemon-identity write (text: gpu_name / pid / boot epoch) */
    snprintf(final, sizeof(final), "%s/cuvs_daemon_identity", g_index_dir);
    snprintf(tmp,   sizeof(tmp),   "%s.tmp", final);
    f = fopen(tmp, "w");
    if (f)
    {
        int ok = (fprintf(f, "%s\n%d\n%lld\n", p.gpu_name, (int) getpid(),
                          (long long) p.measured_at) > 0);
        if (ok && fflush(f) != 0) ok = 0;
        if (ok && fsync(fileno(f)) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, final) != 0) ok = 0;
        if (!ok) unlink(tmp);
    }

    {
        int dir_fd = open(g_index_dir, O_RDONLY);
        if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    }

    LOG_INFO("pg_cuvs_server: hw_profile gpu='%s' link_bw=%.0f hbm_bw=%.0f bf_tput=%.0f "
             "cpu_dist=%.0f cagra_lat=%.1f ipc_rtt=%.1f probe_status=0x%x\n",
             p.gpu_name, p.link_bw_bpus, p.hbm_bw_bpus, p.gpu_bf_tput,
             p.cpu_dist_tput, p.gpu_cagra_lat_us, p.ipc_rtt_us, p.probe_status);
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
        else if (strcmp(argv[i], "--max-indexes") == 0 && i+1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n >= 1)
                g_max_indexes = n;   /* registry working-set capacity */
        }
        else if (strcmp(argv[i], "--snapshot-uri") == 0 && i+1 < argc)
            strncpy(g_snapshot_uri, argv[++i], sizeof(g_snapshot_uri) - 1);
        else if (strcmp(argv[i], "--cluster-id") == 0 && i+1 < argc)
            strncpy(g_cluster_id, argv[++i], sizeof(g_cluster_id) - 1);
        else if (strcmp(argv[i], "--gcs-key-file") == 0 && i+1 < argc)
            strncpy(g_gcs_key_file, argv[++i], sizeof(g_gcs_key_file) - 1);
        else if (strcmp(argv[i], "--warmup-threads") == 0 && i+1 < argc)
        {
            int n = atoi(argv[++i]);
            if (n >= 1 && n <= 8)
                g_warmup_nthreads = n;
        }
        else if (strcmp(argv[i], "--gpu-devices") == 0 && i+1 < argc)
        {
            char buf[256];
            strncpy(buf, argv[++i], sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ",");
            while (tok && g_n_allowed_gpus < CUVS_MAX_GPUS)
            {
                g_allowed_gpus[g_n_allowed_gpus++] = atoi(tok);
                tok = strtok(NULL, ",");
            }
        }
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

    /* 3S: ignore SIGPIPE so a client that disconnects mid-request (e.g. a backend
     * aborting a search on statement_timeout / query cancel) makes the reply
     * write() fail with EPIPE — the per-connection thread cleans up — instead of
     * killing the whole daemon (and every backend's GPU index). */
    signal(SIGPIPE, SIG_IGN);

    /* Phase 3C: initialize libcurl once for the process lifetime. Must be called
     * before any curl_easy_* or download/upload operations. */
    if (g_snapshot_uri[0] != '\0')
        curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Phase 3E: GPU detection */
    g_n_gpus = cuvs_detect_gpus(g_gpus, CUVS_MAX_GPUS);
    if (g_n_gpus == 0)
    {
        LOG_ERROR("pg_cuvs_server: no CUDA GPUs detected\n");
        return 1;
    }
    /* Validate and deduplicate --gpu-devices against detected GPUs */
    for (int i = 0; i < g_n_allowed_gpus; i++)
    {
        if (g_allowed_gpus[i] >= g_n_gpus)
        {
            LOG_ERROR("pg_cuvs_server: --gpu-devices references GPU %d but only %d detected\n",
                      g_allowed_gpus[i], g_n_gpus);
            return 1;
        }
        for (int j = i + 1; j < g_n_allowed_gpus; j++)
        {
            if (g_allowed_gpus[i] == g_allowed_gpus[j])
            {
                for (int k = j; k < g_n_allowed_gpus - 1; k++)
                    g_allowed_gpus[k] = g_allowed_gpus[k + 1];
                g_n_allowed_gpus--;
                j--;
            }
        }
    }

    /* Allocate the index registry now that --max-indexes is known (calloc =
     * zero-initialized, matching the old static arrays). Done before
     * startup_load_indexes() and any registry use; never realloc'd afterward. */
    g_indexes      = calloc((size_t)g_max_indexes, sizeof(IndexEntry));
    g_cold_indexes = calloc((size_t)g_max_indexes, sizeof(ColdIndexEntry));
    if (!g_indexes || !g_cold_indexes)
    {
        LOG_ERROR("pg_cuvs_server: failed to allocate index registry (--max-indexes %d)\n",
                  g_max_indexes);
        return 1;
    }
    LOG_INFO("index registry capacity: %d (--max-indexes)\n", g_max_indexes);

    /* ADR-075 Phase 1: probe hardware + write the global cost-model profile. */
    write_hw_profile();

    /* Initialize per-device VRAM budgets.
     *
     * ADR-065: when --max-vram-mb is omitted, default to a conservative fraction
     * of each device's total VRAM rather than "unlimited" — an external operator
     * who doesn't set a budget should NOT be able to OOM the device. The budget
     * is enforced against the daemon's own per-index accounting (total_vram_used),
     * which is reliable (it knows what it allocated); the headroom fraction
     * reserves room for the untracked cuVS workspace + CUDA context. (We do NOT
     * trust raw cudaMemGetInfo for the budget — cuVS's async mempool caches freed
     * memory so the "free" figure understates real availability.) Explicit
     * runtime-unlimited is still available via pg_cuvs_set_vram_budget(0). */
    {
        int n = n_usable_gpus();
        for (int i = 0; i < n; i++)
        {
            int dev = usable_gpu(i);
            if (g_max_vram_bytes > 0)
                g_max_vram_per_gpu[dev] = g_max_vram_bytes;          /* explicit --max-vram-mb */
            else
                g_max_vram_per_gpu[dev] =
                    (size_t)(g_gpus[dev].total_vram_bytes * CUVS_DEFAULT_VRAM_FRACTION);
            LOG_INFO("GPU %d (%s): %.0f MB total, budget %.0f MB (%s)\n",
                     dev, g_gpus[dev].name,
                     g_gpus[dev].total_vram_bytes / (1024.0 * 1024),
                     g_max_vram_per_gpu[dev] / (1024.0 * 1024),
                     g_max_vram_bytes ? "explicit --max-vram-mb"
                                      : "default fraction of total");
        }
    }

    /* Load persisted indexes */
    startup_load_indexes();

    /* Per-device GPU warm-up so the first client query doesn't pay the
     * one-time CUDA context / RMM / cuBLAS / kernel init (hundreds of ms). */
    {
        int n = n_usable_gpus();
        for (int gi = 0; gi < n; gi++)
        {
            int dev = usable_gpu(gi);
            struct timespec w0, w1;
            clock_gettime(CLOCK_MONOTONIC, &w0);
            cuvs_warmup_device(dev);
            clock_gettime(CLOCK_MONOTONIC, &w1);
            double ms = (w1.tv_sec - w0.tv_sec) * 1000.0 +
                        (w1.tv_nsec - w0.tv_nsec) / 1e6;
            LOG_INFO("GPU %d warm-up done in %.0f ms\n", dev, ms);
        }
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

    /* Phase 3D: spawn warmup worker threads. They process the queue populated
     * by startup_load_indexes() Pass 2 and by on-demand cache miss triggers.
     * Workers download from GCS and load into VRAM in background while the
     * daemon is already accepting queries. */
    if (g_snapshot_uri[0] != '\0')
    {
        for (int i = 0; i < g_warmup_nthreads; i++)
            pthread_create(&g_warmup_tids[i], NULL, warmup_worker_thread, NULL);
        LOG_INFO("warmup: spawned %d worker threads\n", g_warmup_nthreads);
    }

    /* Phase 3L-9: spawn the single BF micro-batch consumer. Always running but
     * inert until a request with bf_batch_wait_us>0 is enqueued. */
    if (pthread_create(&g_bf_worker_tid, NULL, bf_batch_worker_thread, NULL) == 0)
        g_bf_worker_started = 1;
    else
        LOG_ERROR("pg_cuvs_server: failed to spawn BF batch worker; "
                  "brute_force micro-batching disabled (immediate dispatch only)\n");

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
