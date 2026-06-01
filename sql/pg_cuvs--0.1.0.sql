-- pg_cuvs--0.1.0.sql
-- Loaded by CREATE EXTENSION pg_cuvs; (requires pgvector)
\echo Use "CREATE EXTENSION pg_cuvs" to load this file. \quit

-- ----------------------------------------------------------------
-- Index Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION cuvsamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'cuvsamhandler'
LANGUAGE C;

CREATE ACCESS METHOD cagra
TYPE INDEX
HANDLER cuvsamhandler;

COMMENT ON ACCESS METHOD cagra IS
  'GPU-accelerated CAGRA index for pgvector vector type (pg_cuvs)';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the cagra AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING cagra AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

-- ----------------------------------------------------------------
-- pg_cuvs_reset_circuit(index_name text)
-- Re-enables GPU routing after circuit breaker trips (FALLBACK-04).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_reset_circuit(index_oid regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_reset_circuit'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_reset_circuit(regclass) IS
  'Reset circuit breaker for a cagra index to re-enable GPU routing. '
  'Use after repeated GPU errors have been resolved. '
  'Example: SELECT pg_cuvs_reset_circuit(''my_schema.cagra_idx''::regclass);';

-- ----------------------------------------------------------------
-- Last-search stats (process-local). NULL if no scan in this session.
-- For EXPLAIN VERBOSE integration set cuvs.debug = on so the same
-- stats appear inline via NOTICE.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_last_search_latency_us()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_latency_us'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_n_results()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_n_results'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_k()
RETURNS integer
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_k'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_index()
RETURNS oid
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_index'
LANGUAGE C;

CREATE FUNCTION pg_cuvs_last_search_metric()
RETURNS text
AS '$libdir/pg_cuvs', 'pg_cuvs_last_search_metric'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_last_search_latency_us() IS
  'Daemon-reported wall-clock latency in microseconds for the most '
  'recent successful cagra index scan in this backend. NULL if none.';

-- ----------------------------------------------------------------
-- pg_stat_gpu_search — server-wide per-index GPU search statistics.
-- Source of truth is the sidecar daemon (cross-backend, but reset on
-- index rebuild/reload). Returns zero rows when the daemon is down so
-- monitoring stays queryable. Column order must match the SRF in
-- src/pg_cuvs.c (pg_cuvs_gpu_search_stats).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gpu_search_stats(
    OUT database_oid    oid,
    OUT index_oid       oid,
    OUT index_name      text,
    OUT dim             integer,
    OUT metric          text,
    OUT n_vecs          bigint,
    OUT vram_bytes      bigint,
    OUT resident        boolean,
    OUT search_count    bigint,
    OUT error_count     bigint,
    OUT avg_latency_us  double precision,
    OUT p50_latency_us  integer,
    OUT p95_latency_us  integer,
    OUT p99_latency_us  integer,
    OUT last_status     text,
    OUT last_error      text,
    OUT last_search_at  timestamptz,
    OUT requested_k     integer,
    OUT returned_k      integer,
    OUT stale           boolean,
    OUT stale_since     timestamptz,
    OUT delta_rows         bigint,
    OUT delta_generation   bigint,
    OUT delta_vram_bytes   bigint,
    OUT delta_merged_count bigint,
    OUT delta_search_mode  text,
    OUT warmup_state       text,
    OUT last_warmup_at     timestamptz,
    OUT warmup_duration_ms integer,
    OUT download_count     bigint,
    OUT cache_miss_count   bigint,
    OUT gpu_device_id      integer,
    OUT shard_count        integer,
    OUT search_mode        text
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_search_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_search_stats() IS
  'Per-index GPU search statistics from the pg_cuvs sidecar daemon for the '
  'current database. Backs the pg_stat_gpu_search view. Empty when the '
  'daemon is unavailable.';

CREATE VIEW pg_stat_gpu_search AS
  SELECT * FROM pg_cuvs_gpu_search_stats();

COMMENT ON VIEW pg_stat_gpu_search IS
  'GPU CAGRA per-index search stats: counts, fallbacks/errors, and '
  'approximate p50/p95/p99 latency. Counters reset on index rebuild or '
  'daemon restart; empty while the daemon is down.';

-- ----------------------------------------------------------------
-- pg_stat_gpu_cache — daemon-global VRAM tiered-cache counters.
-- One row normally; empty while the daemon is down. Column order must match
-- the SRF in src/pg_cuvs.c (pg_cuvs_gpu_cache_stats).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gpu_cache_stats(
    OUT gpu_device_id    integer,
    OUT hits             bigint,
    OUT misses           bigint,
    OUT evictions        bigint,
    OUT reloads          bigint,
    OUT persist_failures bigint,
    OUT resident_count   integer,
    OUT vram_used_mb     bigint,
    OUT vram_budget_mb   bigint
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_cache_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_cache_stats() IS
  'Daemon-global VRAM tiered-cache counters (hits/misses/evictions/reloads). '
  'Backs the pg_stat_gpu_cache view. Empty when the daemon is unavailable.';

CREATE VIEW pg_stat_gpu_cache AS
  SELECT * FROM pg_cuvs_gpu_cache_stats();

COMMENT ON VIEW pg_stat_gpu_cache IS
  'GPU VRAM cache: cumulative hit/miss/eviction/reload counters, resident '
  'index count, and VRAM used vs budget (MB). Counters are daemon-lifetime; '
  'empty while the daemon is down.';

-- ----------------------------------------------------------------
-- pg_stat_gpu_shards — per-shard placement and stats for sharded CAGRA
-- indexes (Phase 3F, cuvs.shard_count >= 2). One row per shard; empty for
-- unsharded indexes or while the daemon is down. Column order must match the
-- SRF in src/pg_cuvs.c (pg_cuvs_gpu_shard_stats).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gpu_shard_stats(
    OUT database_oid    oid,
    OUT index_oid       oid,
    OUT index_name      text,
    OUT shard_id        integer,
    OUT gpu_device_id   integer,
    OUT n_vecs          bigint,
    OUT tid_offset      bigint,
    OUT vram_used_mb    bigint,
    OUT search_count    bigint,
    OUT error_count     bigint,
    OUT resident        boolean,
    OUT last_status     text
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_shard_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_shard_stats() IS
  'Per-shard placement and search stats for sharded CAGRA indexes in the '
  'current database. Backs the pg_stat_gpu_shards view. Empty for unsharded '
  'indexes or when the daemon is unavailable.';

CREATE VIEW pg_stat_gpu_shards AS
  SELECT * FROM pg_cuvs_gpu_shard_stats();

COMMENT ON VIEW pg_stat_gpu_shards IS
  'GPU CAGRA per-shard view (Phase 3F): which GPU each shard of a sharded '
  'logical index is resident on, its global TID range, VRAM, and per-shard '
  'search/error counts. Empty for unsharded indexes or while the daemon is down.';

-- ----------------------------------------------------------------
-- pg_cuvs_import_hnsw(cagra_oid regclass, hnsw_oid regclass)
-- Phase 3I-2: read .hnsw sidecar (hnswlib binary) written alongside a
-- CAGRA index and bulk-write a pgvector-compatible HNSW index into an
-- existing pgvector HNSW relation.  The target relation is truncated and
-- fully rebuilt from the sidecar; call this only on an offline index.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_import_hnsw(cagra_oid regclass, hnsw_oid regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_import_hnsw'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_import_hnsw(regclass, regclass) IS
  'Phase 3I-2: Import the hnswlib binary sidecar written alongside a CAGRA '
  'index into an existing pgvector HNSW index relation. '
  'OFFLINE OPERATION: acquires AccessExclusiveLock on the target HNSW index, '
  'blocking all concurrent queries against it until the transaction commits. '
  'Requires pgvector 0.5.0+. Validates AM type, dimension, and metric before '
  'truncating. Crash-safe via WAL full-page images (log_newpage_buffer). '
  'UNLOGGED target: if the target HNSW index is UNLOGGED, WAL is skipped for '
  'faster import (~2x); index is lost on crash and must be rebuilt. '
  'Example: SELECT pg_cuvs_import_hnsw(''my_cagra_idx''::regclass, '
  '''my_hnsw_idx''::regclass);';
