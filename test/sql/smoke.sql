-- smoke.sql — Phase 1 load and registration test.
-- Verifies extension install, AM, operator classes, GUCs, and functions.
-- Does NOT require pg_cuvs_server to be running.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- Access method registered?
SELECT amname FROM pg_am WHERE amname = 'cagra';

-- Operator classes registered?
SELECT opcname FROM pg_opclass o
JOIN pg_am a ON a.oid = o.opcmethod
WHERE a.amname = 'cagra'
ORDER BY opcname;

-- GUCs registered?
SHOW enable_cuvs;
SHOW cuvs.socket_path;
SHOW cuvs.circuit_breaker_threshold;

-- pg_cuvs_reset_circuit function registered?
SELECT proname FROM pg_proc WHERE proname = 'pg_cuvs_reset_circuit';

-- CREATE INDEX USING cagra on a small table.
-- With no daemon running, the index is built (ambuild issues WARNING)
-- but the table is accessible via HNSW fallback.
CREATE TABLE smoke_items (id bigint, embedding vector(4));
INSERT INTO smoke_items VALUES
    (1, '[1,0,0,0]'), (2, '[0,1,0,0]'),
    (3, '[0,0,1,0]'), (4, '[0,0,0,1]');

CREATE INDEX cagra_idx ON smoke_items
    USING cagra (embedding vector_l2_ops);

-- Index entry exists in pg_index?
SELECT indexrelid::regclass FROM pg_index
WHERE indrelid = 'smoke_items'::regclass
  AND indexrelid::regclass::text = 'cagra_idx';

-- CPU fallback trigger 2: enable_cuvs = off routes to pgvector
CREATE INDEX hnsw_idx ON smoke_items
    USING hnsw (embedding vector_l2_ops);

SET enable_cuvs = off;
SELECT id FROM smoke_items
ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 2;
SET enable_cuvs = on;

-- Cleanup
DROP TABLE smoke_items;
DROP EXTENSION pg_cuvs;
