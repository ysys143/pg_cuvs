-- cpu_hnsw_fallback.sql — Phase 3I-1: CPU HNSW serving via cuVS from_cagra().
--
-- Verifies that after CREATE INDEX USING cagra:
--   1. cuvs.cpu_hnsw_fallback GUC is registered (defaults off).
--   2. SET cuvs.cpu_hnsw_fallback=on routes queries through the CPU HNSW sidecar.
--   3. Results match the GPU CAGRA path (deterministic test vectors).
--   4. pg_stat_gpu_search.search_mode = 'cpu_hnsw' after a cpu_hnsw query.
--
-- REQUIRES: pg_cuvs_server running with GPU, cuvs.index_dir writable.
-- NOTE: 20 vectors used so n_vecs >= 16 (HNSW serialization threshold).
--       enable_seqscan=off forces planner to use the CAGRA index.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- GUC registered and defaults to off?
SHOW cuvs.cpu_hnsw_fallback;

-- Setup: 20-vector 4-dim table.
SET cuvs.index_dir = '/tmp/cuvs_indexes';
CREATE TABLE hnsw_test (id bigint, embedding vector(4));
INSERT INTO hnsw_test VALUES
    (1,  '[1,0,0,0]'),
    (2,  '[0,1,0,0]'),
    (3,  '[0,0,1,0]'),
    (4,  '[0,0,0,1]'),
    (5,  '[0.1,0,0,0]'),
    (6,  '[0,0.1,0,0]'),
    (7,  '[0,0,0.1,0]'),
    (8,  '[0,0,0,0.1]'),
    (9,  '[0.5,0.5,0,0]'),
    (10, '[0,0.5,0.5,0]'),
    (11, '[0,0,0.5,0.5]'),
    (12, '[0.5,0,0.5,0]'),
    (13, '[0.5,0,0,0.5]'),
    (14, '[0,0.5,0,0.5]'),
    (15, '[0.3,0.3,0.3,0]'),
    (16, '[0.3,0.3,0,0.3]'),
    (17, '[0.3,0,0.3,0.3]'),
    (18, '[0,0.3,0.3,0.3]'),
    (19, '[0.25,0.25,0.25,0.25]'),
    (20, '[0.9,0.1,0,0]');

-- Enable cpu_hnsw_fallback during build so the daemon serializes the .hnsw
-- sidecar alongside the CAGRA index (required for CPU HNSW search path).
SET cuvs.cpu_hnsw_fallback = on;
CREATE INDEX cagra_hnsw_idx ON hnsw_test
    USING cagra (embedding vector_l2_ops);
SET cuvs.cpu_hnsw_fallback = off;

-- Force planner to use CAGRA index (20 rows -> seqscan is cheaper by default).
SET enable_seqscan = off;

-- GPU baseline: verify CAGRA is used and returns results.
SELECT id FROM hnsw_test
ORDER BY embedding <-> '[1,0.5,0,0]'::vector
LIMIT 2;

-- Enable CPU HNSW mode.
SET cuvs.cpu_hnsw_fallback = on;

-- CPU HNSW search: same top-2 expected.
SELECT id FROM hnsw_test
ORDER BY embedding <-> '[1,0.5,0,0]'::vector
LIMIT 2;

-- search_mode must be 'cpu_hnsw' after a cpu_hnsw_fallback query.
SELECT search_mode
FROM pg_stat_gpu_search
WHERE index_name = 'cagra_hnsw_idx';

-- Cleanup.
RESET enable_seqscan;
SET cuvs.cpu_hnsw_fallback = off;
DROP TABLE hnsw_test;
DROP EXTENSION pg_cuvs;
