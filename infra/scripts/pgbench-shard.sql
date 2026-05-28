-- pgbench custom script: concurrent queries against a single sharded CAGRA
-- index (Phase 3F). Each transaction fans out to every shard (one per GPU) and
-- merges a global top-k, so under concurrency the per-shard search counters in
-- pg_stat_gpu_shards rise on every GPU — proving real concurrent multi-GPU
-- dispatch, not sequential smoke.
--
-- Setup (single non-partitioned table, sharded across GPUs):
--   SET cuvs.shard_count = 2;
--   CREATE INDEX mg_cagra ON mg_items USING cagra (v vector_l2_ops);
-- Run (as the postgres role against the test db):
--   pgbench -n -f infra/scripts/pgbench-shard.sql -j 4 -c 8 -T 10 mg
-- Verify both GPUs were hit:
--   SELECT shard_id, gpu_device_id, search_count
--     FROM pg_stat_gpu_shards WHERE index_name = 'mg_cagra' ORDER BY shard_id;
\set qid random(1, 4000)
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.k = 10;
SET enable_seqscan = off;
SELECT id FROM mg_items ORDER BY v <-> (SELECT v FROM mg_items WHERE id = :qid) LIMIT 10;
