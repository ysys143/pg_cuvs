-- filter_comparison.sql — D-wedge spike (ADR-063): compare Option B (Function API)
--   vs Option A (Custom Scan hook) for GPU-filtered brute-force kNN.
--
-- Tests:
--   (1) Correctness: cuvs_filtered_knn() returns rows only from the target tenant.
--   (2) pgvector compatibility: Option A Custom Scan hook produces the same
--       tenant-confinement as Option B without explicit SQL changes.
--   (3) Degrade: NULL filter_tids falls back to unfiltered BF (returns k rows).
--
-- REQUIRES: pg_cuvs_server running with BF (brute_force) index mode.
--   Build each CAGRA index with cuvs.search_mode=brute_force to ensure the
--   .vectors sidecar is generated (BF requires it).

\set ON_ERROR_STOP off
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ---- Schema ----
DROP TABLE IF EXISTS fc CASCADE;
CREATE TABLE fc (
    tenant_id  int     NOT NULL,
    row_id     bigint  NOT NULL,
    v          vector(8)
);

-- 4 tenants x 200 rows, dim=8, distinct per-row vectors.
SELECT setseed(0.42);
INSERT INTO fc
SELECT t, t * 1000 + g,
       array_agg(round((random() * 0.9 + 0.05)::numeric, 4) ORDER BY d)::real[]::vector(8)
FROM generate_series(1, 4) t,
     generate_series(1, 200) g,
     generate_series(1, 8) d
GROUP BY t, g;

SET cuvs.search_mode = brute_force;
SET cuvs.k = 20;
SET max_parallel_workers_per_gather = 0;

CREATE INDEX fc_cagra ON fc USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: Option B — cuvs_filtered_knn function API
--
-- Build the tenant-1 TID set explicitly and call the function.
-- ----------------------------------------------------------------

-- Encode tenant-1 TIDs as bigint (block<<16 | off).
-- Results must all belong to tenant_id=1.

SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
    SELECT fc.tenant_id
    FROM cuvs_filtered_knn(
        'fc_cagra'::regclass,
        '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
        ARRAY(
            SELECT (((ctid::text::point)[0])::bigint << 16)
                 | (((ctid::text::point)[1])::bigint)
            FROM fc WHERE tenant_id = 1
            ORDER BY 1
        ),
        10
    ) f
    JOIN fc ON fc.ctid = f.ctid
) s;

-- NULL filter_tids → unfiltered, expect 10 rows.
SELECT count(*) AS n_unfiltered
FROM cuvs_filtered_knn(
    'fc_cagra'::regclass,
    '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
    NULL::bigint[],
    10
) f;

-- ----------------------------------------------------------------
-- Test 1b: Option B — tid[] type-safe overload
--
-- Same correctness check using the tid[] overload; no manual encoding.
-- ----------------------------------------------------------------

SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
    SELECT fc.tenant_id
    FROM cuvs_filtered_knn(
        'fc_cagra'::regclass,
        '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
        ARRAY(SELECT ctid FROM fc WHERE tenant_id = 1),
        10
    ) f
    JOIN fc ON fc.ctid = f.ctid
) s;

-- NULL tid[] → unfiltered, expect 10 rows.
SELECT count(*) AS n_unfiltered
FROM cuvs_filtered_knn(
    'fc_cagra'::regclass,
    '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
    NULL::tid[],
    10
) f;

-- ----------------------------------------------------------------
-- Test 2: Option A — Custom Scan hook
--
-- Transparent SQL: WHERE tenant_id=1 ORDER BY v <-> q LIMIT 10.
-- All returned rows must be from tenant_id=1.
-- ----------------------------------------------------------------

SET cuvs.filtered_knn_hook = on;
SET enable_seqscan = off;

-- Verify plan shows CuvsFilteredScan node.
EXPLAIN (COSTS OFF)
SELECT row_id FROM fc
WHERE tenant_id = 1
ORDER BY v <-> '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8)
LIMIT 10;

-- Verify results are tenant-confined.
SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
    SELECT fc.tenant_id
    FROM fc
    WHERE tenant_id = 1
    ORDER BY v <-> '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8)
    LIMIT 10
) s;

-- ----------------------------------------------------------------
-- Test 3: Regression — hook off restores normal plan shape
-- ----------------------------------------------------------------

SET cuvs.filtered_knn_hook = off;
SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT row_id FROM fc
WHERE tenant_id = 1
ORDER BY v <-> '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8)
LIMIT 10;

-- ----------------------------------------------------------------
-- Test 4: 3O pre-filter path (GPU BITSET) — forced via GUC
--
-- threshold=1.0 means selectivity < 1.0 is always true, so every
-- filtered call takes the 3O path regardless of filter size.
-- Correctness criterion: same as D-wedge — all results in tenant 1.
-- ----------------------------------------------------------------

SET cuvs.filtered_knn_hook = off;
SET cuvs.filter_auto_threshold = 1.0;

SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
    SELECT fc.tenant_id
    FROM cuvs_filtered_knn(
        'fc_cagra'::regclass,
        '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
        ARRAY(SELECT ctid FROM fc WHERE tenant_id = 1),
        10
    ) f
    JOIN fc ON fc.ctid = f.ctid
) s;

-- Force D-wedge by setting threshold=0.0; same correctness expected.
SET cuvs.filter_auto_threshold = 0.0;

SELECT count(*) AS n, count(*) FILTER (WHERE tenant_id <> 1) AS wrong_tenant
FROM (
    SELECT fc.tenant_id
    FROM cuvs_filtered_knn(
        'fc_cagra'::regclass,
        '[0.5,0.3,0.7,0.2,0.8,0.4,0.6,0.1]'::vector(8),
        ARRAY(SELECT ctid FROM fc WHERE tenant_id = 1),
        10
    ) f
    JOIN fc ON fc.ctid = f.ctid
) s;

RESET cuvs.filter_auto_threshold;

-- ----------------------------------------------------------------
-- Cleanup
-- ----------------------------------------------------------------

SET cuvs.filtered_knn_hook = off;
SET enable_seqscan = on;
DROP TABLE fc CASCADE;
