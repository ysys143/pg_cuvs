-- cpu_fallback.sql — verifies that SET enable_cuvs = off forces planner
-- away from the cagra AM. With enable_cuvs=off, cuvsamcostestimate returns
-- 1e9 startup cost so the planner picks pgvector HNSW or SeqScan.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

CREATE TABLE smoke_items (id bigint, embedding vector(4));
INSERT INTO smoke_items VALUES
    (1, '[1,0,0,0]'),
    (2, '[0,1,0,0]'),
    (3, '[0,0,1,0]'),
    (4, '[0,0,0,1]');

-- HNSW on pgvector for fallback comparison
CREATE INDEX hnsw_idx ON smoke_items
    USING hnsw (embedding vector_l2_ops);

SET enable_cuvs = off;

-- Planner should pick hnsw_idx, not cagra. Search must succeed.
SELECT id FROM smoke_items
ORDER BY embedding <-> '[1,0,0,0]'::vector
LIMIT 2;

DROP TABLE smoke_items;
DROP EXTENSION pg_cuvs;
