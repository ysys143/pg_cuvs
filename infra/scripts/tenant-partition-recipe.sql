-- tenant-partition-recipe.sql
-- Online large-scale pattern: partition-pruned per-tenant CAGRA, VRAM as LRU cache.
--
-- Pattern: LIST/RANGE-partition a vector table on a QUERY-ALIGNED key (e.g. tenant_id).
-- A query that filters on the partition key (`WHERE tenant_id = X`) is pruned by the
-- PostgreSQL planner to a SINGLE child partition's CAGRA index — only that partition's
-- (small) index is searched. The daemon keeps hot partitions VRAM-resident and evicts
-- cold ones to disk (LRU), reloading on access. So the total corpus across all tenants
-- can far exceed VRAM while each query touches only one small, resident index.
--
-- This is the OPPOSITE goal of multigpu-partition-recipe.sql, which hash-partitions on
-- `id` to SPREAD partitions across GPUs (no pruning — every query fans out to all
-- partitions). Hash-on-id cannot prune a similarity query; use a query-aligned key here.
--
-- Best fit: multi-tenant SaaS RAG (per-tenant data small, every query filters tenant,
-- high QPS, low latency = query-cost-dominated). See design/STRATEGY_NOTES.md §G,
-- design/DECISIONS.md ADR-061.
--
-- Usage:
--   psql -d <db> -f tenant-partition-recipe.sql
-- Prerequisites:
--   - pg_cuvs + vector extensions installed; pg_cuvs_server running with a GPU.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

\set DIM 128
\set ROWS_PER_TENANT 5000

-- ============================================================================
-- 1. Partition by a query-aligned key (tenant_id). LIST shown; RANGE works too
--    (e.g. time buckets). Add partitions as tenants onboard.
-- ============================================================================
DROP TABLE IF EXISTS docs CASCADE;
CREATE TABLE docs (
    tenant_id int    NOT NULL,
    id        bigint NOT NULL,
    embedding vector(:DIM)
) PARTITION BY LIST (tenant_id);

CREATE TABLE docs_t1 PARTITION OF docs FOR VALUES IN (1);
CREATE TABLE docs_t2 PARTITION OF docs FOR VALUES IN (2);
CREATE TABLE docs_t3 PARTITION OF docs FOR VALUES IN (3);

-- ============================================================================
-- 2. Populate (random vectors; replace with real embeddings)
-- ============================================================================
INSERT INTO docs
SELECT t, t*1000000 + g,
       ('[' || string_agg((random())::text, ',') || ']')::vector(:DIM)
FROM generate_series(1,3) t,
     generate_series(1, :ROWS_PER_TENANT) g,
     generate_series(1, :DIM) d
GROUP BY t, g;

-- ============================================================================
-- 3. One CAGRA index PER partition. The daemon places + LRU-caches each.
-- ============================================================================
CREATE INDEX docs_t1_cagra ON docs_t1 USING cagra (embedding vector_l2_ops);
CREATE INDEX docs_t2_cagra ON docs_t2 USING cagra (embedding vector_l2_ops);
CREATE INDEX docs_t3_cagra ON docs_t3 USING cagra (embedding vector_l2_ops);

-- ============================================================================
-- 4. The online query: ALWAYS filter on the partition key. The planner prunes
--    to one child index -> only that small index is searched (VRAM-resident).
-- ============================================================================
SET enable_seqscan = off;

-- Verify pruning: expect a single `Index Scan using docs_t2_cagra`, NO Append.
EXPLAIN (COSTS OFF)
SELECT id FROM docs
WHERE tenant_id = 2
ORDER BY embedding <-> (SELECT embedding FROM docs WHERE id = 2000001)
LIMIT 10;

-- The actual top-k (confined to tenant 2):
SELECT id FROM docs
WHERE tenant_id = 2
ORDER BY embedding <-> (SELECT embedding FROM docs WHERE id = 2000001)
LIMIT 10;

RESET enable_seqscan;

-- ============================================================================
-- 5. Observe the VRAM LRU cache: hot partitions resident, cold evicted to disk.
--    With many tenants and a VRAM budget, only hot tenants stay resident; a cold
--    tenant's first query reloads its index from disk (cache-miss latency).
-- ============================================================================
SELECT index_name, resident, n_vecs FROM pg_stat_gpu_search
WHERE index_name LIKE 'docs_%' ORDER BY index_name;
SELECT gpu_device_id, resident_count, vram_used_mb, vram_budget_mb FROM pg_stat_gpu_cache;

-- Notes:
--   - Cross-tenant (no partition-key filter) queries fan out to ALL partitions
--     (Merge Append) and lose the pruning benefit — avoid for online serving.
--   - A single oversized tenant (> VRAM) still needs IVF-PQ/sharding/streaming.
--   - Daemon resident+cold index tracking is capped (MAX_INDEXES); see ADR-061 for
--     the many-tenants caveat.

-- Cleanup:
-- DROP TABLE docs CASCADE;
