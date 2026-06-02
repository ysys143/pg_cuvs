-- build_hnsw.sql — pg_cuvs_build_hnsw: all modes, recall, return type.
--
-- Coverage:
--   1. Function registered.
--   2. mode='nsw'     — flat NSW, correct top-1.
--   3. mode='hnsw'    — heuristic hierarchy, correct top-1.
--   4. mode='hnswlib' — from_cagra() via /dev/shm, correct top-1.
--   5. Return type: returned regclass is renameable and still searchable.
--   6. Two independent builds on same CAGRA produce unique index names.
--
-- REQUIRES: pg_cuvs_server running with GPU, cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

SELECT proname FROM pg_proc WHERE proname = 'pg_cuvs_build_hnsw';

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Setup: 20-vector 4-dim table.
-- Query [1, 0, 0, 0]: unambiguous top-1 = id=1 (exact match).
-- Query [1, 0.5, 0, 0]: unambiguous top-1 = id=20 (dist≈0.41 vs id=1 dist=0.5).
CREATE TABLE bh_test (id bigint, embedding vector(4));
INSERT INTO bh_test VALUES
    (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
    (5,'[0.1,0,0,0]'),(6,'[0,0.1,0,0]'),(7,'[0,0,0.1,0]'),(8,'[0,0,0,0.1]'),
    (9,'[0.5,0.5,0,0]'),(10,'[0,0.5,0.5,0]'),(11,'[0,0,0.5,0.5]'),
    (12,'[0.5,0,0.5,0]'),(13,'[0.5,0,0,0.5]'),(14,'[0,0.5,0,0.5]'),
    (15,'[0.3,0.3,0.3,0]'),(16,'[0.3,0.3,0,0.3]'),(17,'[0.3,0,0.3,0.3]'),
    (18,'[0,0.3,0.3,0.3]'),(19,'[0.25,0.25,0.25,0.25]'),(20,'[0.9,0.1,0,0]');

CREATE INDEX bh_cagra ON bh_test USING cagra (embedding vector_l2_ops);

-- ── mode='nsw' (default, flat NSW) ───────────────────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) IS NOT NULL AS nsw_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── mode='hnsw' (heuristic hierarchy) ───────────────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass, 'hnsw') IS NOT NULL AS heuristic_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── mode='hnswlib' (from_cagra via /dev/shm) ────────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass, 'hnswlib') IS NOT NULL AS hnswlib_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── Return type: returned regclass can be renamed ────────────────
-- Uses WARNING to suppress variable-OID NOTICEs from pg_cuvs_build_hnsw.
SET client_min_messages = 'warning';
DO $$ DECLARE hnsw_oid regclass;
BEGIN
    SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) INTO hnsw_oid;
    EXECUTE 'ALTER INDEX ' || hnsw_oid::text || ' RENAME TO bh_hnsw_renamed';
END $$;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
-- bh_hnsw_renamed index is used for search.
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── Two independent builds produce unique names ──────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) IS NOT NULL AS build_a;
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) IS NOT NULL AS build_b;
SET client_min_messages = 'notice';

-- Should be ≥ 2 pg_cuvs_hnsw* indexes (from all builds + renamed).
SELECT count(*) >= 2 AS multiple_hnsw
FROM pg_indexes
WHERE tablename = 'bh_test' AND indexname LIKE '%hnsw%';

-- Cleanup.
DROP TABLE bh_test CASCADE;
DROP EXTENSION pg_cuvs CASCADE;
