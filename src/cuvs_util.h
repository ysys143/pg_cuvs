#pragma once

/*
 * cuvs_util.h — shared, dependency-free helpers for pg_cuvs.
 *
 * This header is included by the PostgreSQL extension (pg_cuvs.c), the
 * standalone GPU daemon (pg_cuvs_server.c), the IPC client (cuvs_ipc.c),
 * and a standalone unit-test binary. It MUST NOT pull in PostgreSQL,
 * CUDA, or cuVS headers — only the C standard headers below.
 */

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

/* ----------------------------------------------------------------
 * Leveled logging macros (PG-free; stderr only).
 *
 * ERROR/WARN/INFO are UNCONDITIONAL — they always print. Only DEBUG is
 * gated behind PG_CUVS_DEBUG (hot-path trace only). Each macro prepends a
 * level tag to the caller's printf format string. Callers must pass a
 * string literal as the first argument (the format), e.g.
 *     LOG_ERROR("save_index FAILED for %u/%u\n", db, idx);
 *
 * The "[TAG] " fmt concatenation requires a string-literal format and the
 * ##__VA_ARGS__ GNU extension (available under both -std=gnu11 for the
 * daemon and gcc for the PGXS .so build).
 * ---------------------------------------------------------------- */
#ifndef PG_CUVS_DEBUG
#define PG_CUVS_DEBUG 0
#endif

#define LOG_ERROR(fmt, ...) \
    do { fprintf(stderr, "[ERROR] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_WARN(fmt, ...) \
    do { fprintf(stderr, "[WARN] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_INFO(fmt, ...) \
    do { fprintf(stderr, "[INFO] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_DEBUG(fmt, ...) \
    do { if (PG_CUVS_DEBUG) fprintf(stderr, "[DEBUG] " fmt, ##__VA_ARGS__); } while (0)

/* ----------------------------------------------------------------
 * TID encode/decode (heap TID <-> uint64 block<<16|offset)
 * ---------------------------------------------------------------- */
static inline uint64_t
cuvs_tid_encode(uint32_t block, uint16_t offset)
{
    return ((uint64_t)block << 16) | (uint64_t)offset;
}

static inline void
cuvs_tid_decode(uint64_t tid, uint32_t *block, uint16_t *offset)
{
    *block  = (uint32_t)(tid >> 16);
    *offset = (uint16_t)(tid & 0xFFFF);
}

/* ----------------------------------------------------------------
 * Index filename parsing: "<db_oid>_<index_oid>.cagra"
 * Returns 0 on success, -1 otherwise.
 * ---------------------------------------------------------------- */
int cuvs_parse_index_filename(const char *name, uint32_t *db_oid, uint32_t *index_oid);

/* ----------------------------------------------------------------
 * IPC status code -> human-readable string (see cuvs_ipc.h CUVS_STATUS_*)
 * ---------------------------------------------------------------- */
const char *cuvs_status_str(int status);

/* Opclass name -> CUVS_METRIC_* (cuvs_ipc.h); -1 if unrecognized. */
int cuvs_metric_from_opclass_name(const char *name);

/* ----------------------------------------------------------------
 * Latency histogram (per-index, daemon-side). Log2-spaced fixed buckets:
 * bucket 0 = {0 us}; bucket k (k>=1) covers [2^(k-1), 2^k) us. 32 buckets
 * span up to ~2^31 us (~35 min), which comfortably bounds any GPU search.
 * Percentiles are approximate (bucket upper edge) — adequate for
 * monitoring, not for precise SLA accounting. Pure, PG/GPU-free.
 * ---------------------------------------------------------------- */
#define CUVS_LAT_BUCKETS 32

/* Bucket index for a latency in microseconds (clamped to [0, CUVS_LAT_BUCKETS-1]). */
uint32_t cuvs_lat_bucket_index(uint32_t us);

/* q-quantile (q in [0,1]) over a CUVS_LAT_BUCKETS-wide histogram, returned as
 * the containing bucket's upper-edge latency in us. Returns 0 for an empty
 * histogram. nbuckets lets the unit test pass smaller arrays. */
uint32_t cuvs_lat_percentile(const uint32_t *buckets, int nbuckets, double q);

/* ----------------------------------------------------------------
 * Versioned, checksummed .tids on-disk sidecar format.
 *
 * Layout: [CuvsTidsHeader (32 bytes)] [n_vecs * uint64_t TID body].
 * LITTLE-ENDIAN ONLY: the daemon is x86-64 and the Makefile sets
 * RAFT_SYSTEM_LITTLE_ENDIAN=1; no byte-swap is performed. Legacy
 * headerless .tids files are intentionally rejected by the magic check
 * (pre-1.0, no shipped users -> reREINDEX such indexes).
 * ---------------------------------------------------------------- */
#define CUVS_TIDS_MAGIC    0x53444954u   /* 'TIDS' little-endian */
#define CUVS_TIDS_VERSION  1u
#define CUVS_TIDS_MAX_VECS 1000000000LL  /* sanity cap on n_vecs */

typedef struct CuvsTidsHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_vecs;
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;   /* crc32 over n_vecs*8 bytes of TID body */
    uint32_t reserved;     /* must be 0 */
} CuvsTidsHeader;          /* 32 bytes, LE-only, x86-64 daemon */

/* Standard table-based CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320). */
uint32_t cuvs_crc32(const void *data, size_t len);

/* Streaming CRC-32 for data that does not fit in one buffer (e.g. a large
 * .cagra artifact read in chunks). Usage:
 *     uint32_t s = cuvs_crc32_stream_begin();
 *     s = cuvs_crc32_stream_update(s, buf, n);   // repeat per chunk
 *     uint32_t crc = cuvs_crc32_stream_end(s);
 * cuvs_crc32(d,n) == end(update(begin(), d, n)). */
uint32_t cuvs_crc32_stream_begin(void);
uint32_t cuvs_crc32_stream_update(uint32_t state, const void *data, size_t len);
uint32_t cuvs_crc32_stream_end(uint32_t state);

/* Write header + TID body to an open FILE*. Returns 0 on success, -1 on a
 * short/failed fwrite. Caller is responsible for fflush/fsync/rename. */
int cuvs_tids_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                    const uint64_t *tids);

/* Read + validate a .tids file from an open FILE*. On success returns 0,
 * fills *hdr_out, and allocates *tids_out via malloc (caller frees).
 * Validates magic, version, n_vecs range (0 < n_vecs <= CUVS_TIDS_MAX_VECS),
 * full body read, and body crc32. On any failure returns -1 and frees only
 * what it allocated (leaving *tids_out NULL). */
int cuvs_tids_read(FILE *f, CuvsTidsHeader *hdr_out, uint64_t **tids_out);

/* ----------------------------------------------------------------
 * Versioned, checksummed .vectors sidecar (Phase 3L brute-force mode).
 *
 * Holds the raw row-major vector matrix so the daemon can build a resident
 * GPU brute-force index (CuvsBfIndex) for exact search, independent of the
 * CAGRA graph. Mirrors the .tids format: same header shape plus a crc32 over
 * the body, little-endian only. base_tids_crc32 ties this sidecar to its
 * build's .tids body_crc32 (generation token, like .delta/.shards): a REINDEX
 * rewrites the .tids and changes that crc, so a torn build (new .tids, stale
 * .vectors) is detected at load and brute_force fails closed until REINDEX.
 *
 * Layout: [CuvsVectorsHeader (36 bytes)] [n_vecs * dim * float32 body].
 * LITTLE-ENDIAN ONLY, x86-64 daemon.
 * ---------------------------------------------------------------- */
#define CUVS_VECTORS_MAGIC   0x53434556u   /* 'VECS' little-endian */
#define CUVS_VECTORS_VERSION 1u
#define CUVS_VECTORS_MAX_DIM 65535u        /* sanity cap on dim (corrupt header) */

typedef struct CuvsVectorsHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_vecs;
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;      /* crc32 over n_vecs*dim*4 bytes of float body */
    uint32_t base_tids_crc32; /* sibling .tids body_crc32 @ build (generation token) */
    uint32_t reserved;        /* must be 0 */
} CuvsVectorsHeader;          /* 36 bytes, LE-only, x86-64 daemon */

/* Write header + float body to an open FILE*. base_tids_crc32 is the build's
 * .tids body_crc32 (generation token). Returns 0 on success, -1 on a
 * short/failed fwrite. Caller is responsible for fflush/fsync/rename. */
int cuvs_vectors_write(FILE *f, int64_t n_vecs, uint32_t dim, uint32_t metric,
                       uint32_t base_tids_crc32, const float *vecs);

/* ADR-059: write the .vectors sidecar from N partitions (part_vecs[i] is
 * [n_each[i]][dim], concatenated in order) without a contiguous host copy.
 * Body crc32 is streamed over the partitions. Output is byte-identical to
 * cuvs_vectors_write over the concatenated corpus. Returns 0 / -1. */
int cuvs_vectors_write_multi(FILE *f, const int64_t *n_each, int n_parts,
                             uint32_t dim, uint32_t metric,
                             uint32_t base_tids_crc32, const float *const *part_vecs);

/* Read + validate a .vectors file from an open FILE*. On success returns 0,
 * fills *hdr_out, and allocates *vecs_out via malloc (caller frees). Validates
 * magic, version, n_vecs range (0 < n_vecs <= CUVS_TIDS_MAX_VECS), dim range
 * (0 < dim <= CUVS_VECTORS_MAX_DIM), reserved==0, full body read, and body
 * crc32. On any failure returns -1 and leaves *vecs_out NULL. */
int cuvs_vectors_read(FILE *f, CuvsVectorsHeader *hdr_out, float **vecs_out);

/* ----------------------------------------------------------------
 * Versioned, checksummed hardware profile (ADR-075 cost-model v2, Phase 1).
 *
 * GLOBAL (one per deployment, not per-index) sidecar `<index_dir>/cuvs_hw_profile`
 * written once by the daemon at boot after GPU detection. Holds measured PHYSICAL
 * constants (bandwidths/latencies) that the cost model will consume per deployment
 * so coefficients are not baked to one machine. Self-contained: all data is in the
 * struct (no separate body), so body_crc32 covers all bytes AFTER it (probe_status
 * onward). LITTLE-ENDIAN ONLY, x86-64 daemon. The planner reads + validates this
 * cheaply at plan time (no CUDA/IPC); any mismatch -> caller uses compiled DEFAULTs.
 * ---------------------------------------------------------------- */
#define CUVS_HWPROFILE_MAGIC   0x46505748u   /* 'HWPF' little-endian */
/* v1: link_bw/hbm_bw/gpu_bf_tput/ipc_rtt (136 bytes).
 * v2: + cpu_dist_tput + gpu_cagra_lat_us (152 bytes) for the physical cost model
 *     (ADR-075 Phase 2). The reader accepts v1 (older daemon) and zero-fills the
 *     v2 tail with DEFAULTs + clear bits, so the planner falls back to legacy. */
#define CUVS_HWPROFILE_VERSION 2u

/* probe_status bits: set = field measured on this hardware; clear = compiled DEFAULT. */
#define CUVS_HWPROBE_LINK_BW   0x1u
#define CUVS_HWPROBE_HBM_BW    0x2u
#define CUVS_HWPROBE_BF_TPUT   0x4u
#define CUVS_HWPROBE_IPC_RTT   0x8u
#define CUVS_HWPROBE_CPU_DIST  0x10u  /* v2: CPU L2-distance throughput measured */
#define CUVS_HWPROBE_CAGRA_LAT 0x20u  /* v2: CAGRA per-query graph-search latency measured */

typedef struct CuvsHwProfile {
    uint32_t magic;
    uint32_t version;
    uint32_t body_crc32;       /* crc32 over all bytes AFTER this field (probe_status..end) */
    /* --- crc-covered body starts here --- */
    uint32_t probe_status;     /* CUVS_HWPROBE_* bits */
    char     gpu_name[64];     /* primary GPU name (CuvsGpuDeviceInfo.name); env/identity tag */
    uint32_t n_gpus;
    uint32_t reserved;         /* must be 0 */
    int64_t  total_vram_bytes; /* primary GPU total VRAM */
    int64_t  measured_at;      /* epoch seconds at probe time */
    double   link_bw_bpus;     /* CPU<->GPU H2D bandwidth, bytes per microsecond */
    double   hbm_bw_bpus;      /* GPU device memory bandwidth, bytes per microsecond */
    double   gpu_bf_tput;      /* brute-force throughput, (vectors*dim) per microsecond */
    double   ipc_rtt_us;       /* loopback IPC round-trip, microseconds */
    /* --- v2 tail (ADR-075 Phase 2); v1 readers/files treat these as DEFAULT --- */
    double   cpu_dist_tput;    /* CPU L2-distance throughput, (vectors*dim) per microsecond */
    double   gpu_cagra_lat_us; /* CAGRA per-query graph-search latency floor, microseconds */
} CuvsHwProfile;               /* fixed-size POD, naturally aligned, LE-only */

/* Byte sizes of each on-disk version (for the version-conditional read). */
#define CUVS_HWPROFILE_SZ_V1   (offsetof(CuvsHwProfile, cpu_dist_tput))
#define CUVS_HWPROFILE_SZ_V2   (sizeof(CuvsHwProfile))

/* Stamp magic/version + body_crc32 on *p (body fields already filled) and fwrite
 * it. Returns 0 / -1. Caller does fflush/fsync/rename. */
int cuvs_hw_profile_write(FILE *f, CuvsHwProfile *p);

/* Read + validate a cuvs_hw_profile from an open FILE*. Validates magic, version,
 * reserved==0, and body crc32. On success returns 0 and fills *out; on any
 * failure returns -1 (caller falls back to compiled DEFAULT coefficients). */
int cuvs_hw_profile_read(FILE *f, CuvsHwProfile *out);

/* Conservative DEFAULT coefficients used when no measured profile is present.
 * Shared by the daemon (pre-probe seed) and the planner (fallback) so 'default'
 * behavior is consistent. bytes-per-microsecond unless noted. */
#define CUVS_HWP_DEFAULT_LINK_BW   12000.0      /* ~12 GB/s (PCIe floor) */
#define CUVS_HWP_DEFAULT_HBM_BW    1000000.0    /* ~1 TB/s floor */
#define CUVS_HWP_DEFAULT_BF_TPUT   1000.0       /* (vectors*dim) per microsecond */
#define CUVS_HWP_DEFAULT_IPC_RTT   500.0        /* microseconds */
#define CUVS_HWP_DEFAULT_CPU_DIST  200.0        /* CPU (vectors*dim) per microsecond */
#define CUVS_HWP_DEFAULT_CAGRA_LAT 1000.0       /* CAGRA graph-search latency, microseconds */

/* ----------------------------------------------------------------
 * Versioned, checksummed .shards manifest (Phase 3F multi-GPU sharding).
 *
 * Marks a logical CAGRA index as split into N standalone CAGRA shard
 * artifacts (`<db>_<idx>.s%03u.cagra`), each a contiguous build-order range
 * of the global `.tids`. The manifest is the commit marker: a build renames
 * all shard `.cagra` + global `.tids` first, then renames `.shards` last.
 * Reload refuses a logical sharded index unless the manifest validates AND
 * its base_tids_crc32 matches the current `.tids` body_crc32 (generation).
 *
 * Layout: [CuvsShardsHeader (40 bytes)] [shard_count * CuvsShardRecord (40)].
 * LITTLE-ENDIAN ONLY, like .tids. The record body has its own crc32 in the
 * header. cuvs_shards_read also enforces two semantic invariants so a
 * structurally-valid-but-incoherent manifest fails closed: shard offsets are
 * contiguous from 0 and the per-shard n_vecs sum to the header n_vecs.
 * ---------------------------------------------------------------- */
#define CUVS_SHARDS_MAGIC   0x53524853u   /* 'SHRS' little-endian */
#define CUVS_SHARDS_VERSION 1u
#define CUVS_SHARDS_MAX     256           /* sanity cap on shard_count */

typedef struct CuvsShardsHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t shard_count;
    uint32_t base_tids_crc32;  /* global .tids body_crc32 — generation token */
    int64_t  n_vecs;           /* total vectors across all shards (== .tids n_vecs) */
    uint32_t dim;
    uint32_t metric;
    uint32_t body_crc32;       /* crc32 over shard_count * CuvsShardRecord */
    uint32_t reserved;         /* must be 0 */
} CuvsShardsHeader;            /* 40 bytes, LE-only */

typedef struct CuvsShardRecord {
    uint32_t shard_id;
    uint32_t gpu_device_id;    /* GPU assigned at build (advisory; reload re-places) */
    int64_t  tid_offset;       /* global TID start offset of this shard's [start,end) */
    int64_t  n_vecs;           /* vectors in this shard */
    uint32_t dim;
    uint32_t metric;
    uint32_t artifact_crc32;   /* crc32 of the shard's .cagra file bytes */
    uint32_t reserved;         /* must be 0 */
} CuvsShardRecord;             /* 40 bytes, LE-only */

/* Write manifest header + record body to an open FILE*. Sets magic/version/
 * body_crc32/reserved internally from (shard_count, n_vecs, dim, metric,
 * base_tids_crc32, recs). Returns 0 on success, -1 on a short/failed fwrite.
 * Caller fflush/fsync/renames. */
int cuvs_shards_write(FILE *f, uint32_t shard_count, int64_t n_vecs,
                      uint32_t dim, uint32_t metric, uint32_t base_tids_crc32,
                      const CuvsShardRecord *recs);

/* Read + validate a .shards manifest. On success returns 0, fills *hdr_out,
 * and allocates *recs_out via malloc (caller frees). Validates magic, version,
 * reserved==0, shard_count in [1,CUVS_SHARDS_MAX], n_vecs/dim ranges, full
 * body read, body crc32, AND the semantic invariants (contiguous offsets from
 * 0, per-shard n_vecs sum == header n_vecs, each record reserved==0). On any
 * failure returns -1 and leaves *recs_out NULL. */
int cuvs_shards_read(FILE *f, CuvsShardsHeader *hdr_out, CuvsShardRecord **recs_out);

/* Decide how many shards a logical CAGRA index needs (Phase 3G auto count).
 * Pure arithmetic over the daemon's runtime inputs; no CUDA. Returns 1 for
 * unsharded (fits one GPU / unknown budget / too small), 2..max_shards for a
 * split, or 0 when it cannot fit even maximally sharded (caller fails closed).
 * `needed` bytes mirror estimate_vram_bytes(): n_vecs*(dim*4 + 64). */
int cuvs_auto_shard_count(int64_t n_vecs, int dim, size_t per_gpu_budget_bytes,
                          int n_gpus, int max_shards);

/* ----------------------------------------------------------------
 * Versioned .delta pending-insert sidecar (Phase 3A).
 *
 * Layout: [CuvsDeltaHeader (32 bytes)] [n_rows * record], each record being
 * { uint64_t tid; float vec[dim]; } (fixed width = 8 + dim*4 bytes). Holds
 * vectors inserted/updated since the base CAGRA build so a query can merge GPU
 * base candidates with CPU-exact delta candidates without a rebuild.
 *
 * LITTLE-ENDIAN ONLY, like .tids. Corruption (e.g. a truncated file) is caught
 * by a file-size check: the body must be exactly n_rows*record_bytes. There is
 * deliberately no whole-body CRC — appends must stay O(1), and a CRC recomputed
 * over the growing body on every insert would be O(n^2). base_tids_crc32 ties a
 * delta to its base build's .tids body_crc32; a REINDEX rewrites the base and
 * changes that CRC, so a leftover delta is detected as a generation mismatch.
 * ---------------------------------------------------------------- */
#define CUVS_DELTA_MAGIC   0x544c4544u   /* 'DELT' little-endian */
#define CUVS_DELTA_VERSION 1u

typedef struct CuvsDeltaHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_rows;
    uint32_t dim;
    uint32_t metric;
    uint32_t base_tids_crc32;  /* .tids body_crc32 at delta creation (generation) */
    uint32_t reserved;         /* must be 0 */
} CuvsDeltaHeader;             /* 32 bytes, LE-only, x86-64 daemon */

/* Bytes per delta record for a given dim: TID (uint64) + dim float32s. */
static inline size_t
cuvs_delta_record_bytes(uint32_t dim)
{
    return sizeof(uint64_t) + (size_t) dim * sizeof(float);
}

/* Initialize a fresh (empty) delta header. */
void cuvs_delta_header_init(CuvsDeltaHeader *h, uint32_t dim, uint32_t metric,
                            uint32_t base_tids_crc32);

/* Validate a delta header against the actual body byte count (file size minus
 * sizeof(CuvsDeltaHeader)). Checks magic/version/reserved/dim, n_rows range,
 * and that body_bytes == n_rows * record_bytes exactly. Returns 0 if valid,
 * -1 otherwise. Pure (no I/O). */
int cuvs_delta_validate(const CuvsDeltaHeader *h, int64_t body_bytes);

/* Read + validate just the delta header from an open FILE* (does not read the
 * body). Returns 0 and fills *out on magic/version/reserved success; -1 on a
 * short read or bad fields. The body-size check is the caller's job (it has
 * the file size) via cuvs_delta_validate. */
int cuvs_delta_read_header(FILE *f, CuvsDeltaHeader *out);

/* ----------------------------------------------------------------
 * Versioned .tombstone sidecar (Phase 3A-4).
 *
 * Layout: [CuvsTombstoneHeader (32 bytes)] [n_entries * CuvsTombstoneRecord].
 * Records dead TIDs from DELETE/UPDATE-old so base CAGRA results can be
 * filtered before merge. The backend does snapshot-aware filtering using
 * delete_xid (the daemon has no MVCC knowledge). Tied to a base build via
 * base_tids_crc32, like .delta.
 * ---------------------------------------------------------------- */
#define CUVS_TOMBSTONE_MAGIC   0x424D4F54u  /* 'TOMB' little-endian */
#define CUVS_TOMBSTONE_VERSION 1u

typedef struct CuvsTombstoneHeader {
    uint32_t magic;
    uint32_t version;
    int64_t  n_entries;
    uint32_t base_tids_crc32;
    uint32_t reserved;          /* must be 0 */
    uint64_t _pad0;             /* pad to 32 bytes */
} CuvsTombstoneHeader;          /* 32 bytes */

typedef struct CuvsTombstoneRecord {
    uint64_t tid;               /* heap TID of the dead tuple */
    uint64_t delete_xid;        /* xact ID that deleted/updated this tuple */
} CuvsTombstoneRecord;          /* 16 bytes */

void cuvs_tombstone_header_init(CuvsTombstoneHeader *h, uint32_t base_tids_crc32);
int  cuvs_tombstone_validate(const CuvsTombstoneHeader *h, int64_t body_bytes);
int  cuvs_tombstone_read_header(FILE *f, CuvsTombstoneHeader *out);

#ifdef CUVS_TEST_HOOKS
/* Test-only fault injection: returns 1 if env var `name` is set, else 0.
 * Compiled in ONLY under CUVS_TEST_HOOKS; absent from production builds. */
int cuvs_fault(const char *name);
#endif

/* ----------------------------------------------------------------
 * Circuit breaker state (per index, process-local). Moved here from
 * cuvs_ipc.h so it can be linked by daemon + extension + tests.
 * ---------------------------------------------------------------- */
#define CUVS_MAX_TRACKED_INDEXES 64

typedef struct CuvsCircuitBreaker {
    uint32_t index_oid;
    int      consecutive_errors;
    int      open;              /* 1 = tripped, routing to CPU */
} CuvsCircuitBreaker;

/* Defined in cuvs_util.c */
extern CuvsCircuitBreaker cuvs_circuit_breakers[CUVS_MAX_TRACKED_INDEXES];
extern int                cuvs_n_circuit_breakers;

void cuvs_circuit_record_error(uint32_t index_oid, int threshold);
void cuvs_circuit_record_success(uint32_t index_oid); /* reset consecutive_errors */
void cuvs_circuit_reset(uint32_t index_oid);          /* also clears open flag */
int  cuvs_circuit_is_open(uint32_t index_oid);
void cuvs_circuit_reset_all(void);                    /* zeroes all breaker state */

/* ----------------------------------------------------------------
 * Phase 3L-9: brute-force micro-batch grouping (pure, daemon + test).
 *
 * The daemon's BF batch worker collects concurrent brute_force requests and
 * coalesces those targeting the SAME (db_oid, index_oid, precision, dim) into a
 * single cuvs_bf_search_batch dispatch. cuvs_bf_batch_group is the pure
 * indexing core (no threads, no CUDA): it assigns each request a group id so
 * requests with an identical key share a group. Group ids are dense and
 * assigned in first-seen order. O(n^2) but n is bounded by the batch cap.
 * ---------------------------------------------------------------- */
typedef struct CuvsBfKey {
    uint32_t db_oid;
    uint32_t index_oid;
    uint32_t precision;   /* 0=float32, 1=float16 */
    uint32_t dim;
} CuvsBfKey;

/* group_id_out[i] = group of request i (requests with equal keys share it);
 * *n_groups_out = number of distinct keys. group_id_out must hold n ints. */
void cuvs_bf_batch_group(const CuvsBfKey *keys, int n,
                         int *group_id_out, int *n_groups_out);
