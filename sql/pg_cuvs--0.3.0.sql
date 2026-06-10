-- pg_cuvs--0.2.0.sql
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
-- pg_cuvs_hnsw access method (Phase 3K, ADR-038)
--
-- Exposes the GPU CAGRA -> pgvector-HNSW conversion as standard CREATE INDEX
-- DDL:  CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
--         WITH (source = 'my_cagra', mode = 'nsw');
-- The handler borrows pgvector hnsw's IndexAmRoutine for the read path and
-- overrides only ambuild (CAGRA->HNSW page conversion) and amoptions
-- (WITH (source, mode, ...)). See src/hnsw_export.c.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_hnsw_handler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'pg_cuvs_hnsw_handler'
LANGUAGE C;

CREATE ACCESS METHOD pg_cuvs_hnsw
TYPE INDEX
HANDLER pg_cuvs_hnsw_handler;

COMMENT ON ACCESS METHOD pg_cuvs_hnsw IS
  'GPU-built CAGRA -> pgvector HNSW index (pg_cuvs Phase 3K). '
  'CREATE INDEX ... USING pg_cuvs_hnsw (col vector_l2_ops) '
  'WITH (source = ''my_cagra'', mode = ''nsw'').';

-- Operator classes MUST mirror pgvector's hnsw opclass support functions
-- exactly, because the delegated scan resolves them via index_getprocinfo on
-- THIS opclass. Verified against pgvector 0.8.0:
--   l2     : proc 1 = vector_l2_squared_distance
--   ip     : proc 1 = vector_negative_inner_product
--   cosine : proc 1 = vector_negative_inner_product, proc 2 = vector_norm
-- (cosine proc 1 is the negative inner product, NOT cosine_distance — pgvector
--  normalizes at build time via proc 2 and ranks by inner product.)
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING pg_cuvs_hnsw AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING pg_cuvs_hnsw AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING pg_cuvs_hnsw AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector),
    FUNCTION 2 vector_norm(vector);

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
    OUT search_mode        text,
    OUT bf_batch_count     bigint,
    OUT extend_count       bigint,
    OUT compact_count      bigint,
    OUT last_compact_at    timestamptz
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
    OUT vram_budget_mb   bigint,
    OUT bf_vram_mb       bigint,
    OUT bf_precision     text
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
-- pg_stat_gpu_fallback — per-index CPU-fallback counters (backend shmem).
-- A GPU index "falls back" when cuvsamcostestimate gates it off at plan time
-- (the planner picks seqscan/pgvector); that decision never reaches the daemon,
-- so pg_stat_gpu_search cannot show it. Backed by the SRF in src/pg_cuvs.c.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gpu_fallback_stats(
    OUT index_oid        regclass,
    OUT fallback_count   bigint,
    OUT last_reason      text,
    OUT last_fallback_at timestamptz
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gpu_fallback_stats'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gpu_fallback_stats() IS
  'Per-index CPU-fallback counters for the current database. Backs the '
  'pg_stat_gpu_fallback view. Counts are a relative pressure signal (plan-time '
  'cost estimate may run more than once per query), not exact query counts.';

CREATE VIEW pg_stat_gpu_fallback AS
  SELECT * FROM pg_cuvs_gpu_fallback_stats();

COMMENT ON VIEW pg_stat_gpu_fallback IS
  'Per-index GPU->CPU fallback: how many times the planner dropped the GPU '
  'index path and why (last_reason: disabled/circuit_breaker/stale/delete_drift/'
  'daemon_down/no_artifact/delta_unusable/tombstone_unusable). Watch the trend '
  'against pg_stat_gpu_search.search_count to detect queries silently using CPU.';

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
-- pg_cuvs_build_hnsw(cagra_oid, mode) — GPU-accelerated HNSW creation.
-- Creates pgvector HNSW from CAGRA WITHOUT pgvector CPU build (285s).
-- Returns: regclass OID of the newly created HNSW index.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_build_hnsw(
    cagra_oid regclass,
    mode      text DEFAULT 'nsw'
)
RETURNS regclass
AS '$libdir/pg_cuvs', 'pg_cuvs_build_hnsw'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_build_hnsw(regclass, text) IS
  'Build pgvector HNSW from CAGRA index without pgvector CPU build. '
  'Uses INDEX_CREATE_SKIP_BUILD — the 285s CPU HNSW build is eliminated. '
  ''
  'Recommended modes: '
  '  ''nsw''     (default) Flat NSW via IPC. 117s, 2.4x vs native. '
  '             Empirically equal quality at ef>=40. '
  '  ''hnswlib'' from_cagra() hierarchy via /dev/shm. 139s, 2.0x. '
  '             Slight recall advantage at ef<20. '
  ''
  'Hidden modes (kept for research, not recommended): '
  '  ''hnsw''         Direct hierarchy w/ heuristic neighbor selection. '
  '                   Currently 144s with no quality gain over ''nsw''. '
  '                   Kept pending future improvement of level assignment. '
  '  ''hnswlib_file''  Disk-based sidecar. Superceded by ''hnswlib'' (shm). '
  ''
  'Returns OID of the new HNSW index (regclass). '
  'Example: SELECT pg_cuvs_build_hnsw(''my_cagra''::regclass);';


-- ----------------------------------------------------------------
-- Phase 3M (ADR-040): pg_cuvs_batch_search — Q queries in one IPC round-trip.
--
-- Sends Q query vectors to the daemon in a single request and returns up to
-- K = min(k, n_vecs) neighbors per query from one batched GPU dispatch. Honors
-- cuvs.search_mode (cagra / brute_force) and cuvs.bf_precision. Returns raw
-- ctids + the daemon distance with no internal MVCC filtering (same semantics
-- as the index scan, which sets xs_recheck) — JOIN on ctid for visible rows:
--
--   SELECT t.*, b.query_idx, b.distance
--   FROM pg_cuvs_batch_search('items', ARRAY[q1,q2]::vector[], 10) b
--   JOIN items t ON t.ctid = b.ctid
--   ORDER BY b.query_idx, b.distance;
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_batch_search(
    rel       regclass,
    queries   vector[],
    k         integer,
    OUT query_idx integer,
    OUT ctid      tid,
    OUT distance  real
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_batch_search'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_batch_search(regclass, vector[], integer) IS
  'Phase 3M: batch GPU k-NN. Q queries in one IPC round-trip / GPU dispatch. '
  'Returns (query_idx, ctid, distance); JOIN on ctid for MVCC-visible rows. '
  'Honors cuvs.search_mode and cuvs.bf_precision (brute_force needs .vectors).';


-- ----------------------------------------------------------------
-- pg_cuvs_gc_orphans(do_delete) — ADR-046 orphan artifact GC.
-- Reconciles the daemon's index_dir against the catalog (the daemon is a
-- standalone sidecar with no catalog access). Reports/removes artifacts left
-- by daemon-down DROP or DROP DATABASE that would otherwise be reloaded as
-- zombies on restart. do_delete=false (default) is a dry run.
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_gc_orphans(
    do_delete       boolean DEFAULT false,
    OUT db_oid      oid,
    OUT index_oid   oid,
    OUT reason      text,
    OUT action      text
)
RETURNS SETOF record
AS '$libdir/pg_cuvs', 'pg_cuvs_gc_orphans'
LANGUAGE C;

COMMENT ON FUNCTION pg_cuvs_gc_orphans(boolean) IS
  'ADR-046: garbage-collect orphan GPU index artifacts in index_dir whose OIDs '
  'are absent from the catalog (daemon-down DROP, DROP DATABASE). reason is one '
  'of missing_in_catalog / dead_database / unverifiable_other_db (other live DB '
  '— rerun there). do_delete=false (default) reports only; do_delete=true frees '
  'VRAM + unlinks via the daemon, or unlinks files directly when it is down.';

-- D-wedge spike (ADR-063): cuvs_filtered_knn — Option B Function API.
-- Executes an exact (brute-force) GPU kNN search restricted to a caller-supplied
-- TID set.  filter_tids is a sorted bigint[] of heap TIDs encoded as (block<<16|off);
-- pass NULL to run unfiltered BF.  Returns (ctid tid, distance float4) rows.
--
-- Usage example:
--   SELECT t.*
--   FROM cuvs_filtered_knn(
--         'items_embedding_idx'::regclass,
--         '[0.1, 0.2, ...]'::vector,
--         ARRAY(SELECT (ctid::text::point)[0]::bigint * 65536
--                    + (ctid::text::point)[1]::bigint
--               FROM items WHERE tenant_id = 5
--               ORDER BY 1),
--         10
--       ) f
--   JOIN items t ON t.ctid = f.ctid
--   ORDER BY f.distance;
CREATE FUNCTION cuvs_filtered_knn(
    index_rel   regclass,
    query       vector,
    filter_tids bigint[],
    k           integer
)
RETURNS TABLE (ctid tid, distance float4)
AS '$libdir/pg_cuvs', 'cuvs_filtered_knn'
LANGUAGE C STABLE;

COMMENT ON FUNCTION cuvs_filtered_knn(regclass, vector, bigint[], integer) IS
  'ADR-063 D-wedge spike (Option B): GPU BF kNN restricted to filter_tids. '
  'filter_tids is a sorted bigint[] of heap TIDs encoded as block<<16|off; '
  'NULL degrades to unfiltered BF.  Returns (ctid, distance) pairs.';

-- Type-safe tid[] overload: accepts ctid values directly, encodes internally.
-- Usage example:
--   SELECT t.*
--   FROM cuvs_filtered_knn(
--         'items_embedding_idx'::regclass,
--         '[0.1, 0.2, ...]'::vector,
--         ARRAY(SELECT ctid FROM items WHERE tenant_id = 5),
--         10
--       ) f
--   JOIN items t ON t.ctid = f.ctid
--   ORDER BY f.distance;
CREATE FUNCTION cuvs_filtered_knn(
    index_rel   regclass,
    query       vector,
    filter_tids tid[],
    k           integer
)
RETURNS TABLE (ctid tid, distance float4)
LANGUAGE sql STABLE AS $$
    SELECT * FROM cuvs_filtered_knn(
        index_rel,
        query,
        (SELECT array_agg(
                    (((t::text::point)[0])::bigint << 16) |
                     ((t::text::point)[1])::bigint
                ) FROM unnest(filter_tids) t),
        k
    );
$$;

COMMENT ON FUNCTION cuvs_filtered_knn(regclass, vector, tid[], integer) IS
  'Type-safe tid[] overload of cuvs_filtered_knn. Accepts ctid values directly; '
  'encodes as block<<16|off internally. NULL filter_tids degrades to unfiltered BF.';

-- ----------------------------------------------------------------
-- Phase 3P: IVF-PQ access method
-- ----------------------------------------------------------------
-- ----------------------------------------------------------------
-- IVF-PQ Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION ivfpqamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'ivfpqamhandler'
LANGUAGE C;

CREATE ACCESS METHOD ivfpq
TYPE INDEX
HANDLER ivfpqamhandler;

COMMENT ON ACCESS METHOD ivfpq IS
  'GPU IVF-PQ index for pgvector. 10-100x less VRAM than cagra via Product '
  'Quantization. Tuning: WITH (n_lists=1024, pq_bits=8, pq_dim=0). '
  'Search recall: SET cuvs.ivfpq_n_probes = 64 (higher = better recall).';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the ivfpq AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

-- ----------------------------------------------------------------
-- Phase 3Q: CAGRA streaming updates
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_compact(index_rel regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_compact'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_compact(regclass) IS
  'Compact a CAGRA index: remove tombstoned vectors via cuvsCagraMerge, '
  'rebuild the on-disk .cagra + .tids, and delete the .tombstone sidecar. '
  'Auto-triggered during VACUUM when cuvs.compact_delete_ratio is exceeded.';

CREATE FUNCTION pg_cuvs_set_vram_budget(budget_bytes bigint)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_set_vram_budget'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_set_vram_budget(bigint) IS
  'Override the per-GPU VRAM budget (bytes) for the running daemon. '
  '0 = unlimited. Intended for testing and capacity management; '
  'does not persist across daemon restarts.';

CREATE FUNCTION pg_cuvs_eat_vram(leave_bytes bigint)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_eat_vram'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_eat_vram(bigint) IS
  'Test helper: pre-allocate GPU VRAM via cudaMalloc so that only '
  'leave_bytes remain free.  Forces physical CUDA OOM on the next '
  'large GPU operation, bypassing the VRAM budget-check path. '
  'Device 0. Release with pg_cuvs_free_vram().';

CREATE FUNCTION pg_cuvs_free_vram()
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_free_vram'
LANGUAGE C VOLATILE;

COMMENT ON FUNCTION pg_cuvs_free_vram() IS
  'Release the VRAM held by pg_cuvs_eat_vram(). Device 0.';

CREATE FUNCTION pg_cuvs_inject_extend_oom(enable integer)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_inject_extend_oom'
LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION pg_cuvs_inject_extend_oom(integer) IS
  'Test-only: arm (1) or disarm (0) synthetic OOM injection in cuvs_cagra_extend. '
  'When armed, the next extend throws bad_alloc, exercising _pr.poison() → '
  'BUILD_FAILED → delta fallback. The flag self-clears on fire.';
