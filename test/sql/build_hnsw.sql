-- build_hnsw.sql — pg_cuvs_build_hnsw: all modes, recall, return type.
--
-- Coverage:
--   1. Function registered.
--   2. mode='nsw'     — flat NSW, correct top-2.
--   3. mode='hnsw'    — heuristic hierarchy, correct top-2.
--   4. mode='hnswlib' — from_cagra() via /dev/shm, correct top-2.
--   5. Return type: returned regclass is usable (rename, search).
--   6. Two independent builds on same table work independently.
--
-- REQUIRES: pg_cuvs_server running with GPU, cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

SELECT proname FROM pg_proc WHERE proname = 'pg_cuvs_build_hnsw';

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Setup: 20-vector 4-dim table with deterministic nearest neighbors.
-- Query [1, 0.5, 0, 0]: top-2 = id=20 ([0.9,0.1,0,0]) then id=1 ([1,0,0,0]).
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
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 2;
RESET enable_cuvs; RESET enable_seqscan;

-- Drop auto-named index before next build.
DO $$ DECLARE r RECORD;
BEGIN
  FOR r IN SELECT indexname FROM pg_indexes WHERE tablename='bh_test' AND indexname LIKE 'pg_cuvs_hnsw%'
  LOOP EXECUTE 'DROP INDEX IF EXISTS ' || r.indexname; END LOOP;
END $$;

-- ── mode='hnsw' (heuristic hierarchy) ───────────────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass, 'hnsw') IS NOT NULL AS heuristic_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 2;
RESET enable_cuvs; RESET enable_seqscan;

DO $$ DECLARE r RECORD;
BEGIN
  FOR r IN SELECT indexname FROM pg_indexes WHERE tablename='bh_test' AND indexname LIKE 'pg_cuvs_hnsw%'
  LOOP EXECUTE 'DROP INDEX IF EXISTS ' || r.indexname; END LOOP;
END $$;

-- ── mode='hnswlib' (from_cagra via /dev/shm) ────────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass, 'hnswlib') IS NOT NULL AS hnswlib_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 2;
RESET enable_cuvs; RESET enable_seqscan;

-- ── Return type: returned regclass can be renamed ────────────────
DO $$ DECLARE hnsw_oid regclass; new_name text;
BEGIN
  SET client_min_messages TO warning;
  SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) INTO hnsw_oid;
  RESET client_min_messages;
  -- Rename the index using the returned OID.
  new_name := 'bh_hnsw_renamed';
  EXECUTE 'ALTER INDEX ' || hnsw_oid::text || ' RENAME TO ' || new_name;
  RAISE NOTICE 'renamed to %', new_name;
END $$;

-- Named index is searchable.
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM bh_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 2;
RESET enable_cuvs; RESET enable_seqscan;

-- ── Two independent builds coexist on same table ────────────────
SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) IS NOT NULL AS build_a;
SELECT pg_cuvs_build_hnsw('bh_cagra'::regclass) IS NOT NULL AS build_b;
SET client_min_messages = 'notice';

SELECT count(*) AS hnsw_index_count
FROM pg_indexes
WHERE tablename = 'bh_test'
  AND indexdef LIKE '%hnsw%'
  AND indexname LIKE 'pg_cuvs_hnsw%';

-- Cleanup.
DROP TABLE bh_test;
DROP EXTENSION pg_cuvs;
