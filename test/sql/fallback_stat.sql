-- fallback_stat.sql — pg_stat_gpu_fallback observability (repo 공개 전 운영 하드닝).
--
-- A GPU index "falls back" to CPU when cuvsamcostestimate gates it off at plan
-- time (the planner then picks seqscan/pgvector). That decision never reaches
-- the daemon, so pg_stat_gpu_search cannot show it. The backend records per-index
-- fallback counts + reason in shared memory, surfaced by pg_stat_gpu_fallback.
--
-- Tests:
--   1. a GPU-served query records NO fallback (baseline 0 rows for the index),
--   2. forcing a fallback (enable_cuvs=off gates the path) records it with
--      fallback_count>=1 and last_reason='disabled'.
--
-- REQUIRES: pg_cuvs_server running; pg_cuvs in shared_preload_libraries (shmem).

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

DROP TABLE IF EXISTS fb CASCADE;
CREATE TABLE fb (id int, v vector(8));
SELECT setseed(0.23);
INSERT INTO fb
SELECT g, array_agg(round((random())::numeric, 5) ORDER BY d)::real[]::vector(8)
FROM generate_series(1, 200) g, generate_series(1, 8) d
GROUP BY g;

SET cuvs.search_mode = cagra;
SET cuvs.k = 5;
SET max_parallel_workers_per_gather = 0;
CREATE INDEX fb_cagra ON fb USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: a GPU-served query records no fallback for this index.
-- ----------------------------------------------------------------
SELECT count(*) FROM (
    SELECT id FROM fb
    ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8)
    LIMIT 5
) s;

SELECT count(*) AS baseline_fallback_rows
FROM pg_stat_gpu_fallback WHERE index_oid = 'fb_cagra'::regclass;

-- ----------------------------------------------------------------
-- Test 2: force a fallback (enable_cuvs=off gates the GPU path at plan time),
-- then assert it was recorded with the right reason.
-- ----------------------------------------------------------------
SET enable_cuvs = off;
SELECT count(*) FROM (
    SELECT id FROM fb
    ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector(8)
    LIMIT 5
) s;
RESET enable_cuvs;

SELECT fallback_count >= 1 AS recorded, last_reason
FROM pg_stat_gpu_fallback WHERE index_oid = 'fb_cagra'::regclass;

DROP TABLE fb CASCADE;
