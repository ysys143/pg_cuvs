-- pg_cuvs_hnsw.sql — Phase 3K (ADR-038): CREATE INDEX ... USING pg_cuvs_hnsw DDL.
--
-- Coverage:
--   1. AM + operator classes registered by the 0.2.0 migration.
--   2. DDL build from a CAGRA source (default mode 'nsw').
--   3. Index is a first-class pg_indexes entry, served via pgvector HNSW path.
--   4. Explicit mode + WITH options.
--   5. REINDEX re-runs ambuild from the stored source (natural rebuild).
--   6. DROP INDEX behaves naturally.
--   7. Missing 'source' option -> clear ERROR.
--   8. Non-cagra source -> clear ERROR.
--
-- REQUIRES: pg_cuvs_server running with GPU, cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- AM + opclasses present (from the 0.1.0->0.2.0 migration).
SELECT amname FROM pg_am WHERE amname = 'pg_cuvs_hnsw';
SELECT opcname
FROM pg_opclass oc JOIN pg_am am ON am.oid = oc.opcmethod
WHERE am.amname = 'pg_cuvs_hnsw'
ORDER BY opcname;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Setup: 20-vector 4-dim table. Query [1,0.5,0,0] -> unambiguous top-1 = id=20.
CREATE TABLE ph_test (id bigint, embedding vector(4));
INSERT INTO ph_test VALUES
    (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
    (5,'[0.1,0,0,0]'),(6,'[0,0.1,0,0]'),(7,'[0,0,0.1,0]'),(8,'[0,0,0,0.1]'),
    (9,'[0.5,0.5,0,0]'),(10,'[0,0.5,0.5,0]'),(11,'[0,0,0.5,0.5]'),
    (12,'[0.5,0,0.5,0]'),(13,'[0.5,0,0,0.5]'),(14,'[0,0.5,0,0.5]'),
    (15,'[0.3,0.3,0.3,0]'),(16,'[0.3,0.3,0,0.3]'),(17,'[0.3,0,0.3,0.3]'),
    (18,'[0,0.3,0.3,0.3]'),(19,'[0.25,0.25,0.25,0.25]'),(20,'[0.9,0.1,0,0]');

-- Source CAGRA index (daemon-resident; ambuild reads its graph over IPC).
CREATE INDEX ph_cagra ON ph_test USING cagra (embedding vector_l2_ops);

-- ── DDL build (default mode 'nsw') ───────────────────────────────
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_cagra');
SET client_min_messages = 'notice';

-- Catalog visibility: a first-class pg_indexes entry using the pg_cuvs_hnsw AM.
SELECT i.indexname, a.amname
FROM pg_indexes i
JOIN pg_class c ON c.relname = i.indexname
JOIN pg_am a ON a.oid = c.relam
WHERE i.tablename = 'ph_test' AND i.indexname = 'ph_hnsw';

-- Served by pgvector HNSW path with GPU off. Top-1 must be id=20.
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── explicit mode = 'hnsw' ───────────────────────────────────────
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw2 ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_cagra', mode = 'hnsw');
SET client_min_messages = 'notice';
SELECT indexname FROM pg_indexes
WHERE tablename = 'ph_test' AND indexname = 'ph_hnsw2';

-- ── REINDEX re-runs ambuild from the stored 'source' relopt ──────
SET client_min_messages = 'warning';
REINDEX INDEX ph_hnsw;
SET client_min_messages = 'notice';
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- ── DROP INDEX behaves naturally ─────────────────────────────────
DROP INDEX ph_hnsw2;
SELECT count(*) FROM pg_indexes
WHERE tablename = 'ph_test' AND indexname = 'ph_hnsw2';

-- ── Error case (do not abort the script) ─────────────────────────
-- Omitting source is now valid (ephemeral build — see the source-less section
-- below), so the only remaining error is a source that is not a cagra index.
\set ON_ERROR_STOP off

-- Source given but is not a cagra index (ph_hnsw uses the pg_cuvs_hnsw AM).
CREATE INDEX ph_bad2 ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'ph_hnsw');

-- Source metric (ph_cagra is L2) must match this index's opclass (cosine):
-- fail-fast ERROR in ambuild before any daemon IPC.
CREATE INDEX ph_bad3 ON ph_test USING pg_cuvs_hnsw (embedding vector_cosine_ops)
    WITH (source = 'ph_cagra');

\set ON_ERROR_STOP on

-- ── Source-less mode (ADR-041): ephemeral CAGRA from heap, auto-dropped ──
-- No source CAGRA: ambuild builds a temporary CAGRA on the GPU, converts it
-- into this index's pages, then drops it. One self-contained DDL.
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw_solo ON ph_test USING pg_cuvs_hnsw (embedding vector_l2_ops);
SET client_min_messages = 'notice';

-- First-class catalog index using the pg_cuvs_hnsw AM, no source needed.
SELECT i.indexname, a.amname
FROM pg_indexes i
JOIN pg_class c ON c.relname = i.indexname
JOIN pg_am a ON a.oid = c.relam
WHERE i.tablename = 'ph_test' AND i.indexname = 'ph_hnsw_solo';

-- Served by pgvector HNSW (GPU off). Top-1 must be id=20.
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- REINDEX rebuilds from the heap with NO source CAGRA dependency.
SET client_min_messages = 'warning';
REINDEX INDEX ph_hnsw_solo;
SET client_min_messages = 'notice';
SET enable_cuvs = off; SET enable_seqscan = off;
SELECT id FROM ph_test ORDER BY embedding <-> '[1,0.5,0,0]' LIMIT 1;
RESET enable_cuvs; RESET enable_seqscan;

-- The ephemeral CAGRA must be dropped — no daemon registry entry lingers
-- under this index's OID (count must be 0).
SELECT count(*) AS leftover_ephemeral_cagra
FROM pg_stat_gpu_search s
JOIN pg_class c ON c.oid = s.index_oid
WHERE c.relname = 'ph_hnsw_solo';

DROP INDEX ph_hnsw_solo;

-- Source-less with a NON-L2 metric: the ephemeral CAGRA must be built with the
-- COSINE metric (not the old L2 fallback), otherwise fill_* raises a metric
-- mismatch and CREATE INDEX fails. Success here proves AM-agnostic metric.
SET client_min_messages = 'warning';
CREATE INDEX ph_hnsw_cos ON ph_test USING pg_cuvs_hnsw (embedding vector_cosine_ops);
SET client_min_messages = 'notice';
SELECT a.amname
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relname = 'ph_hnsw_cos';
DROP INDEX ph_hnsw_cos;

-- Cleanup.
DROP TABLE ph_test CASCADE;
DROP EXTENSION pg_cuvs CASCADE;
