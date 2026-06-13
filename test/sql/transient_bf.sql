-- transient_bf.sql — B (ADR-073): transient no-index GPU exact brute-force CustomScan.
--
-- Coverage (the design's Gherkin success criteria):
--   1. cuvs.gpu_bruteforce = off (DEFAULT) → plan is unchanged (Seq Scan, no
--      Custom Scan): B never fires unless opted in.
--   2. cuvs.gpu_bruteforce = on + no vector index → the planner uses the
--      transient GPU-BF CustomScan (CuvsTransientBF) under a Sort (Sort-accept,
--      no pathkeys claimed), and results are exact (recall@k = 1.0 vs seqscan).
--   3. A parameterized query (<-> $1) still routes to CuvsTransientBF and stays
--      exact — NOT silently downgraded (exec-time param binding).
--   4. A WHERE filter is consumed natively (filter-first) and stays exact.
--   5. corpus > VRAM admission bound → the query fails closed with a clear error
--      (no daemon crash / OOM).
--
-- Like brute_force.sql / flat_smoke.sql, recall is checked on SORTED top-k
-- distance lists so exact ties never cause a spurious mismatch. Under the Tier-1
-- CPU shim brute-force is exact, so recall=1.0 holds without a GPU.
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- Deterministic 200-vector, 8-dim corpus (same generator as flat_smoke.sql).
-- Intentionally NO vector index — B is the no-index path.
CREATE TABLE tbf_test (id int, tenant int, embedding vector(8));
INSERT INTO tbf_test
SELECT g,
       (g % 4),
       format('[%s,%s,%s,%s,%s,%s,%s,%s]',
              (g * 0.013)::numeric(12,6),
              (g * g * 0.0007)::numeric(12,6),
              sin(g * 0.10)::numeric(12,6),
              cos(g * 0.17)::numeric(12,6),
              ((g % 13) * 0.05)::numeric(12,6),
              ((g % 7) * 0.08)::numeric(12,6),
              sin(g * 0.30)::numeric(12,6),
              cos(g * 0.23)::numeric(12,6))::vector
FROM generate_series(1, 200) g;

-- (1) DEFAULT (cuvs.gpu_bruteforce = off): plan unchanged — Seq Scan, no B.
EXPLAIN (COSTS OFF) SELECT id FROM tbf_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;

-- Ground truth: pure CPU seqscan (B off).
CREATE TEMP TABLE gt AS
    SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
    FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;

-- (2) cuvs.gpu_bruteforce = on: the planner uses the transient BF CustomScan.
SET cuvs.gpu_bruteforce = on;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id FROM tbf_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;

-- (2) recall@10 = 1.0 vs the seqscan ground truth.
CREATE TEMP TABLE br AS
    SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
    FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SELECT (SELECT array_agg(d ORDER BY d) FROM gt)
     = (SELECT array_agg(d ORDER BY d) FROM br) AS transient_bf_recall_ok;

-- (3) parameterized (<-> $1): routes to CuvsTransientBF and stays exact.
PREPARE tq(vector) AS
    SELECT id FROM tbf_test ORDER BY embedding <-> $1 LIMIT 3;
EXPLAIN (COSTS OFF) EXECUTE tq('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]');
EXECUTE tq('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]');
DEALLOCATE tq;

-- (4) WHERE filter consumed natively, still exact vs the filtered seqscan.
SET enable_seqscan = on;
SET cuvs.gpu_bruteforce = off;
CREATE TEMP TABLE gtf AS
    SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
    FROM tbf_test WHERE tenant = 2
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;
SET cuvs.gpu_bruteforce = on;
SET enable_seqscan = off;
CREATE TEMP TABLE brf AS
    SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
    FROM tbf_test WHERE tenant = 2
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;
SELECT (SELECT array_agg(d ORDER BY d) FROM gtf)
     = (SELECT array_agg(d ORDER BY d) FROM brf) AS transient_bf_filtered_ok;

-- (5) corpus > VRAM admission bound → fail closed with a clear error (no crash).
-- A 1-byte daemon budget rejects any corpus; the backend raises ERROR (status 2),
-- it does not silently downgrade or crash the daemon.
SELECT pg_cuvs_set_vram_budget(1);
\set ON_ERROR_STOP off
SELECT id FROM tbf_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 3;
\set ON_ERROR_STOP on
SELECT pg_cuvs_set_vram_budget(0);   -- restore unlimited for later tests

-- The daemon is still alive after the fail-closed rejection: a normal B query works.
SELECT count(*) > 0 AS daemon_alive FROM (
    SELECT id FROM tbf_test
        ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 3) s;

RESET enable_seqscan;
RESET cuvs.gpu_bruteforce;
DROP TABLE tbf_test;
