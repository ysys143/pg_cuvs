-- multigpu-partition-recipe.sql
-- Phase 3E-2: Partitioned-table recipe for multi-GPU CAGRA
--
-- Pattern: hash-partition a vector table so each partition gets its own CAGRA
-- index. The daemon places each index on a different GPU via VRAM-aware
-- assignment (Phase 3E-1). PG's partitioned-index scan + Append merge
-- produces the correct global top-k across all GPUs.
--
-- Usage:
--   psql -d <db> -f multigpu-partition-recipe.sql
--
-- Prerequisites:
--   - pg_cuvs + vector extensions installed
--   - pg_cuvs_server running with N GPUs detected (check daemon log for "GPU N warm-up done")

-- ============================================================================
-- 1. Setup: extensions
-- ============================================================================
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- ============================================================================
-- 2. Partitioned table (hash on id, N partitions = N GPUs)
--    Adjust MODULUS to match your GPU count.
-- ============================================================================
\set N_PARTITIONS 2
\set DIM 128
\set N_ROWS 100000

DROP TABLE IF EXISTS mgpu_vectors CASCADE;

CREATE TABLE mgpu_vectors (
    id        bigint NOT NULL,
    embedding vector(:DIM)
) PARTITION BY HASH (id);

-- Create partitions. On a 2-GPU system, partition 0 -> GPU 0, partition 1 -> GPU 1.
CREATE TABLE mgpu_vectors_p0 PARTITION OF mgpu_vectors
    FOR VALUES WITH (MODULUS :N_PARTITIONS, REMAINDER 0);
CREATE TABLE mgpu_vectors_p1 PARTITION OF mgpu_vectors
    FOR VALUES WITH (MODULUS :N_PARTITIONS, REMAINDER 1);

-- ============================================================================
-- 3. Populate with random vectors
-- ============================================================================
INSERT INTO mgpu_vectors
SELECT g,
       ('[' || string_agg((random())::text, ',') || ']')::vector(:DIM)
FROM generate_series(1, :N_ROWS) g,
     generate_series(1, :DIM) d
GROUP BY g;

-- Verify partition distribution
SELECT tableoid::regclass AS partition, count(*) AS rows
FROM mgpu_vectors GROUP BY 1 ORDER BY 1;

-- ============================================================================
-- 4. Build per-partition CAGRA indexes
--    The daemon's pick_gpu_for_index() spreads these across GPUs by VRAM headroom.
-- ============================================================================
CREATE INDEX mgpu_p0_cagra ON mgpu_vectors_p0 USING cagra (embedding vector_l2_ops);
CREATE INDEX mgpu_p1_cagra ON mgpu_vectors_p1 USING cagra (embedding vector_l2_ops);

-- Verify GPU placement in stats
SELECT index_name, gpu_device_id, resident, n_vecs, warmup_state
FROM pg_stat_gpu_search
WHERE index_name LIKE 'mgpu_%'
ORDER BY index_name;

-- ============================================================================
-- 5. Correctness: global top-k query across partitions
--    PG scans each partition index, each returns local top-k, then merges.
-- ============================================================================
\set K 10
\set QUERY_ID 42

-- Reference: brute-force CPU ground truth (pgvector seqscan)
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id, embedding <-> (SELECT embedding FROM mgpu_vectors WHERE id = :QUERY_ID) AS dist
FROM mgpu_vectors
ORDER BY dist LIMIT :K;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- GPU path: CAGRA via partitioned index
SELECT id, embedding <-> (SELECT embedding FROM mgpu_vectors WHERE id = :QUERY_ID) AS dist
FROM mgpu_vectors
ORDER BY dist LIMIT :K;

-- Correctness check: both should return the same ids (order may vary on ties).
-- Run the GPU query separately with seqscan off to force the CAGRA path.
SET enable_seqscan = off;
SELECT array_agg(id ORDER BY id) AS gpu_ids FROM (
    SELECT id FROM mgpu_vectors
    ORDER BY embedding <-> (SELECT embedding FROM mgpu_vectors WHERE id = :QUERY_ID)
    LIMIT :K
) s;
RESET enable_seqscan;

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT array_agg(id ORDER BY id) AS cpu_ids FROM (
    SELECT id FROM mgpu_vectors
    ORDER BY embedding <-> (SELECT embedding FROM mgpu_vectors WHERE id = :QUERY_ID)
    LIMIT :K
) s;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- ============================================================================
-- 6. EXPLAIN to verify partition pruning + multi-index scan
-- ============================================================================
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT id FROM mgpu_vectors
ORDER BY embedding <-> (SELECT embedding FROM mgpu_vectors WHERE id = :QUERY_ID)
LIMIT :K;
RESET enable_seqscan;

-- ============================================================================
-- 7. Stats: verify per-GPU distribution
-- ============================================================================
SELECT gpu_device_id, resident_count, vram_used_mb, vram_budget_mb
FROM pg_stat_gpu_cache;

-- ============================================================================
-- 8. Cleanup
-- ============================================================================
-- DROP TABLE mgpu_vectors CASCADE;
