-- metrics.sql — pg_cuvs_build_hnsw with L2, cosine, inner-product metrics.
--
-- Coverage:
--   1. vector_l2_ops   (L2 distance, default)
--   2. vector_cosine_ops (cosine distance)
--   3. vector_ip_ops   (inner product / negative dot product)
--   Each metric: CAGRA build → pg_cuvs_build_hnsw → correct search results.
--
-- NOTE: Vectors are unit-normalized so L2 and cosine rankings coincide;
-- this lets us verify metric plumbing without needing separate expected results.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- 16 unit-normalized vectors in 4d.  Query ≈ [0.707, 0.707, 0, 0]:
-- nearest = id=9 ([1,0,0,0]→cos=0.707), id=10 ([0,1,0,0]→cos=0.707), etc.
CREATE TABLE met_test (id bigint, embedding vector(4));
INSERT INTO met_test VALUES
    (1, '[1,0,0,0]'),    (2, '[0,1,0,0]'),
    (3, '[0,0,1,0]'),    (4, '[0,0,0,1]'),
    (5, '[0.707,0.707,0,0]'), (6, '[0.707,0,0.707,0]'),
    (7, '[0.707,0,0,0.707]'), (8, '[0,0.707,0.707,0]'),
    (9, '[0,0.707,0,0.707]'), (10,'[0,0,0.707,0.707]'),
    (11,'[0.577,0.577,0.577,0]'), (12,'[0.577,0.577,0,0.577]'),
    (13,'[0.577,0,0.577,0.577]'), (14,'[0,0.577,0.577,0.577]'),
    (15,'[0.5,0.5,0.5,0.5]'),     (16,'[0.5,0.5,-0.5,-0.5]');

-- ── L2 metric ────────────────────────────────────────────────────
CREATE INDEX met_cagra_l2 ON met_test USING cagra (embedding vector_l2_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('met_cagra_l2'::regclass) IS NOT NULL AS l2_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
-- Query ≈ [0.707, 0.707, 0, 0]; top-1 should be id=5 (exact match).
SELECT id FROM met_test
ORDER BY embedding <-> '[0.707,0.707,0,0]'::vector LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

DO $$ DECLARE r RECORD;
BEGIN FOR r IN SELECT indexname FROM pg_indexes WHERE tablename='met_test' AND indexname LIKE 'pg_cuvs_hnsw%'
LOOP EXECUTE 'DROP INDEX ' || r.indexname; END LOOP; END $$;
DROP INDEX met_cagra_l2;

-- ── Cosine metric ────────────────────────────────────────────────
CREATE INDEX met_cagra_cos ON met_test USING cagra (embedding vector_cosine_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('met_cagra_cos'::regclass) IS NOT NULL AS cosine_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM met_test
ORDER BY embedding <=> '[0.707,0.707,0,0]'::vector LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

DO $$ DECLARE r RECORD;
BEGIN FOR r IN SELECT indexname FROM pg_indexes WHERE tablename='met_test' AND indexname LIKE 'pg_cuvs_hnsw%'
LOOP EXECUTE 'DROP INDEX ' || r.indexname; END LOOP; END $$;
DROP INDEX met_cagra_cos;

-- ── Inner product metric ─────────────────────────────────────────
CREATE INDEX met_cagra_ip ON met_test USING cagra (embedding vector_ip_ops);

SET client_min_messages = 'warning';
SELECT pg_cuvs_build_hnsw('met_cagra_ip'::regclass) IS NOT NULL AS ip_ok;
SET client_min_messages = 'notice';

SET enable_cuvs = off; SET enable_seqscan = off;
-- Negative inner product: highest similarity = most positive dot product.
SELECT id FROM met_test
ORDER BY embedding <#> '[0.707,0.707,0,0]'::vector LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- Cleanup.
DROP TABLE met_test;
DROP EXTENSION pg_cuvs;
