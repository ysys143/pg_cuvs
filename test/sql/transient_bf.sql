-- transient_bf.sql — B (ADR-073): transient no-index GPU exact brute-force CustomScan.
--
-- Comprehensive coverage of the B path. Routing decisions are asserted as
-- booleans via plan_has_b() (robust to EXPLAIN formatting, identical across the
-- Tier-1 CPU shim and Tier-2 A100). Recall is checked on SORTED top-k distance
-- lists vs the seqscan ground truth (exact ties never cause spurious mismatch).
--
-- Sections:
--   A. Routing: off→seqscan; on→CuvsTransientBF; shape guards (no LIMIT / OFFSET /
--      GROUP BY / join / index-present) must NOT fire B; bare ORDER BY top-k only.
--   B. Execution + projection variants: bare column (Result node, ps_ProjInfo=NULL
--      regression — used to segfault), SELECT *, computed expr, EXPLAIN ANALYZE.
--   C. Metrics: L2 / cosine / inner-product all fire B and stay exact (recall=1.0).
--   D. Param binding: <-> $1 prepared routes to B, exact (no approx downgrade).
--   E. Filter-first: WHERE consumed natively, exact.
--   F. Edge cases: NULL vector rows skipped; <-> NULL query → empty; k > N.
--   G. Fail-closed: corpus over the VRAM budget → clean ERROR, daemon survives.
--
-- REQUIRES: pg_cuvs_server running; cuvs.index_dir writable.

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- true iff the plan for q contains the transient-BF CustomScan node.
CREATE FUNCTION plan_has_b(q text) RETURNS boolean AS $$
DECLARE line text; found bool := false;
BEGIN
  FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF line LIKE '%CuvsTransientBF%' THEN found := true; END IF;
  END LOOP;
  RETURN found;
END$$ LANGUAGE plpgsql;

-- Deterministic 200-vector, 8-dim corpus (same generator as flat_smoke.sql).
-- Intentionally NO vector index — B is the no-index path. `tenant` exercises WHERE;
-- `grp` exercises the GROUP BY guard.
CREATE TABLE tbf_test (id int, tenant int, grp int, embedding vector(8));
INSERT INTO tbf_test
SELECT g, (g % 4), (g % 3),
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

-- ============================================================ A. Routing
-- (A1) DEFAULT (gpu_bruteforce = off): plan unchanged — Seq Scan, no B.
EXPLAIN (COSTS OFF) SELECT id FROM tbf_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;
SELECT plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS off_fires_b;

SET cuvs.gpu_bruteforce = on;
SET enable_seqscan = off;

-- (A2) on: canonical plan is Limit → Sort → Result → Custom Scan (CuvsTransientBF).
EXPLAIN (COSTS OFF) SELECT id FROM tbf_test
    ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;

-- (A3) shape guards — B must NOT fire (bare LIMIT top-k only).
SELECT
  plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]''')                          AS no_limit,
  plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5 OFFSET 5')         AS with_offset,
  plan_has_b('SELECT grp FROM tbf_test GROUP BY grp ORDER BY min(embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'') LIMIT 5') AS with_groupby,
  plan_has_b('SELECT a.id FROM tbf_test a JOIN tbf_test b USING (tenant) ORDER BY a.embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS with_join;

-- (A4) the firing case is true.
SELECT plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS on_fires_b;

-- ============================================================ B. Exec + projection
-- (B1) recall@10 = 1.0 vs the seqscan ground truth (computed-expr projection).
SET cuvs.gpu_bruteforce = off; SET enable_seqscan = on;
CREATE TEMP TABLE gt AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SET cuvs.gpu_bruteforce = on; SET enable_seqscan = off;
CREATE TEMP TABLE br AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SELECT (SELECT array_agg(d ORDER BY d) FROM gt)
     = (SELECT array_agg(d ORDER BY d) FROM br) AS recall_l2_ok;

-- (B2) bare-column SELECT (Result node above CustomScan, ps_ProjInfo NULL —
-- ExecProject(NULL) used to segfault). Must execute and return the top-k ids.
SELECT id FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 3;

-- (B3) SELECT * (full-row projection) executes and returns k rows.
SELECT count(*) AS star_rows FROM (
  SELECT * FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 7) s;

-- (B4) EXPLAIN ANALYZE executes B (instrumented) without crashing, bare + full row.
-- Output discarded (timing is non-deterministic); survival is the assertion.
DO $$ BEGIN
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF) SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5';
  EXECUTE 'EXPLAIN (ANALYZE, TIMING OFF) SELECT *  FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5';
END $$;
SELECT 'explain_analyze_no_crash' AS b4;

-- ============================================================ C. Metrics
-- cosine (<=>) and inner product (<#>) fire B; metric resolved via pg_amop.
SELECT
  plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <=> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS cosine_fires_b,
  plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <#> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS ip_fires_b;

SET cuvs.gpu_bruteforce = off; SET enable_seqscan = on;
CREATE TEMP TABLE gtc AS
  SELECT round((embedding <=> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <=> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SET cuvs.gpu_bruteforce = on; SET enable_seqscan = off;
CREATE TEMP TABLE brc AS
  SELECT round((embedding <=> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <=> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SELECT (SELECT array_agg(d ORDER BY d) FROM gtc)
     = (SELECT array_agg(d ORDER BY d) FROM brc) AS recall_cosine_ok;

SET cuvs.gpu_bruteforce = off; SET enable_seqscan = on;
CREATE TEMP TABLE gti AS
  SELECT round((embedding <#> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <#> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SET cuvs.gpu_bruteforce = on; SET enable_seqscan = off;
CREATE TEMP TABLE bri AS
  SELECT round((embedding <#> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <#> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 10;
SELECT (SELECT array_agg(d ORDER BY d) FROM gti)
     = (SELECT array_agg(d ORDER BY d) FROM bri) AS recall_ip_ok;

-- ============================================================ D. Param binding
-- <-> $1 prepared routes to B and stays exact (exec-time bind; not downgraded).
PREPARE tq(vector) AS SELECT id FROM tbf_test ORDER BY embedding <-> $1 LIMIT 3;
SELECT plan_has_b('EXECUTE tq(''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'')') AS param_fires_b;
EXECUTE tq('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]');
DEALLOCATE tq;

-- ============================================================ E. Filter-first
-- WHERE consumed natively, exact vs the filtered seqscan.
SET cuvs.gpu_bruteforce = off; SET enable_seqscan = on;
CREATE TEMP TABLE gtf AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test WHERE tenant = 2 ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;
SET cuvs.gpu_bruteforce = on; SET enable_seqscan = off;
CREATE TEMP TABLE brf AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test WHERE tenant = 2 ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5;
SELECT (SELECT array_agg(d ORDER BY d) FROM gtf)
     = (SELECT array_agg(d ORDER BY d) FROM brf) AS recall_filtered_ok;

-- ============================================================ F. Edge cases
-- (F1) a NULL vector row is skipped from the corpus (not top-k, no crash).
INSERT INTO tbf_test VALUES (9001, 0, 0, NULL);
SELECT count(*) = 5 AS null_row_skipped FROM (
  SELECT id FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 5) s;
DELETE FROM tbf_test WHERE id = 9001;

-- (F2) a NULL query vector has no neighbors → empty result, no crash. A literal
-- `<-> NULL` is const-folded to seqscan by the planner (B never sees it), so we
-- force a generic plan and bind a NULL param: B fires and its qisnull branch
-- returns empty. The non-NULL param exercises the same generic plan normally.
SET plan_cache_mode = force_generic_plan;
PREPARE nq(vector) AS SELECT count(*)::int AS n FROM (
  SELECT id FROM tbf_test ORDER BY embedding <-> $1 LIMIT 5) s;
EXECUTE nq(NULL);
EXECUTE nq('[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]');
DEALLOCATE nq;
RESET plan_cache_mode;

-- (F3) k > N (LIMIT exceeds the 200-row corpus) returns all rows, still exact.
SET cuvs.gpu_bruteforce = off; SET enable_seqscan = on;
CREATE TEMP TABLE gtk AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 1000;
SET cuvs.gpu_bruteforce = on; SET enable_seqscan = off;
CREATE TEMP TABLE brk AS
  SELECT round((embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]')::numeric, 4) AS d
  FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 1000;
SELECT (SELECT count(*) FROM brk) = 200 AS k_over_n_returns_all,
       (SELECT array_agg(d ORDER BY d) FROM gtk)
     = (SELECT array_agg(d ORDER BY d) FROM brk) AS recall_k_over_n_ok;

-- ============================================================ A5. Index-present → A1, not B
-- "flat index present → A1; absent → B" — with a flat index, B must not fire.
SET client_min_messages = 'warning';
CREATE INDEX tbf_flat ON tbf_test USING flat (embedding vector_l2_ops);
SET client_min_messages = 'notice';
SELECT plan_has_b('SELECT id FROM tbf_test ORDER BY embedding <-> ''[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]'' LIMIT 5') AS index_present_fires_b;
DROP INDEX tbf_flat;

-- ============================================================ G. Fail-closed
-- corpus > VRAM admission bound → clean ERROR (status 2), daemon survives.
SELECT pg_cuvs_set_vram_budget(1);
\set ON_ERROR_STOP off
SELECT id FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 3;
\set ON_ERROR_STOP on
SELECT pg_cuvs_set_vram_budget(0);
SELECT count(*) > 0 AS daemon_alive FROM (
  SELECT id FROM tbf_test ORDER BY embedding <-> '[0.5,0.3,0.1,0.7,0.2,0.4,0.6,0.15]' LIMIT 3) s;

RESET enable_seqscan;
RESET cuvs.gpu_bruteforce;
DROP FUNCTION plan_has_b(text);
DROP TABLE tbf_test;
