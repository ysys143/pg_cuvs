-- build_params.sql — 3R: CAGRA build parameter reloptions
--   graph_degree / intermediate_graph_degree (ints) + build_algo (auto|ivf_pq|nn_descent).
-- Verifies: reloption parse + catalog durability, build with non-default params,
-- each build_algo, DDL-time validation (fail-closed), and default == current.
-- REQUIRES: pg_cuvs_server running with GPU; cuvs.index_dir writable.

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Distinct per-row vectors (cross join + GROUP BY -> random() per (id,d), not an
-- InitPlan-once degenerate set). 5000 rows so ivf_pq graph build has enough scale.
CREATE TABLE bp_test (id bigint, v vector(32));
INSERT INTO bp_test
SELECT id, array_agg(random() ORDER BY d)::real[]::vector(32)
FROM generate_series(1, 5000) id, generate_series(1, 32) d
GROUP BY id;

SET enable_seqscan = off;
SET cuvs.k = 5;

-- 1. Non-default int params: build + reloption catalog durability.
CREATE INDEX bp_idx ON bp_test USING cagra (v vector_l2_ops)
  WITH (graph_degree = 32, intermediate_graph_degree = 64);
SELECT unnest(reloptions) AS reloption FROM pg_class WHERE relname = 'bp_idx' ORDER BY 1;
SELECT count(*) AS n FROM (SELECT id FROM bp_test
  ORDER BY v <-> (SELECT v FROM bp_test WHERE id = 1) LIMIT 5) s;
DROP INDEX bp_idx;

-- 2. build_algo = nn_descent.
CREATE INDEX bp_nnd ON bp_test USING cagra (v vector_l2_ops) WITH (build_algo = 'nn_descent');
SELECT count(*) AS n FROM (SELECT id FROM bp_test
  ORDER BY v <-> (SELECT v FROM bp_test WHERE id = 1) LIMIT 5) s;
DROP INDEX bp_nnd;

-- 3. build_algo = ivf_pq.
CREATE INDEX bp_ivf ON bp_test USING cagra (v vector_l2_ops) WITH (build_algo = 'ivf_pq');
SELECT count(*) AS n FROM (SELECT id FROM bp_test
  ORDER BY v <-> (SELECT v FROM bp_test WHERE id = 1) LIMIT 5) s;
DROP INDEX bp_ivf;

-- 4. Validation: intermediate_graph_degree < graph_degree -> ERROR (fail-closed).
CREATE INDEX bp_bad ON bp_test USING cagra (v vector_l2_ops)
  WITH (graph_degree = 64, intermediate_graph_degree = 32);

-- 5. Invalid build_algo string -> ERROR at DDL (validator).
CREATE INDEX bp_bad2 ON bp_test USING cagra (v vector_l2_ops) WITH (build_algo = 'bogus');

-- 6. Default (no WITH) still builds + searches.
CREATE INDEX bp_def ON bp_test USING cagra (v vector_l2_ops);
SELECT count(*) AS n FROM (SELECT id FROM bp_test
  ORDER BY v <-> (SELECT v FROM bp_test WHERE id = 1) LIMIT 5) s;
DROP INDEX bp_def;

DROP TABLE bp_test CASCADE;
