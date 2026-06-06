-- release_hardening.sql — Wave 1 build-time advisories (ADR-043 / OBJSTORE-03).
--
-- Coverage (deterministic, daemon-up):
--   1. TOAST-able high-dim vector column (default EXTERNAL storage) → NOTICE on CAGRA build.
--   2. PLAIN storage, same high dim → NO NOTICE.
--   3. Small-dim toastable column → NO NOTICE (rows stay inline, no detoast cost).
--
-- NOT exercised here (conditional, verified manually — see docs/best-practices.md):
--   - index_dir WARNING: fires only when resolved index_dir is under $PGDATA;
--     the suite uses /tmp/cuvs_indexes (outside PGDATA), so no warning.
--   - pgvector version guard: WARNs only when pgvector is outside 0.5.x–0.8.x.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- 1. High-dim, default (EXTERNAL) storage → expect TOAST NOTICE at build.
CREATE TABLE rh_ext (id bigint, embedding vector(2000));
INSERT INTO rh_ext
SELECT g, (SELECT '[' || string_agg(((g+i)%10)::text, ',') || ']'
           FROM generate_series(1, 2000) i)::vector
FROM generate_series(1, 8) g;
CREATE INDEX rh_ext_cagra ON rh_ext USING cagra (embedding vector_l2_ops);
DROP TABLE rh_ext;

-- 2. High-dim, PLAIN storage → expect NO NOTICE.
CREATE TABLE rh_plain (id bigint, embedding vector(2000));
ALTER TABLE rh_plain ALTER COLUMN embedding SET STORAGE PLAIN;
INSERT INTO rh_plain
SELECT g, (SELECT '[' || string_agg(((g+i)%10)::text, ',') || ']'
           FROM generate_series(1, 2000) i)::vector
FROM generate_series(1, 8) g;
CREATE INDEX rh_plain_cagra ON rh_plain USING cagra (embedding vector_l2_ops);
DROP TABLE rh_plain;

-- 3. Small-dim toastable column → NO NOTICE (stays inline).
CREATE TABLE rh_small (id bigint, embedding vector(4));
INSERT INTO rh_small VALUES (1, '[1,0,0,0]'), (2, '[0,1,0,0]');
CREATE INDEX rh_small_cagra ON rh_small USING cagra (embedding vector_l2_ops);
DROP TABLE rh_small;
