-- drop_subxact.sql — ADR-060: subtransaction-aware DROP collection.
-- A DROP collected by object_access_hook must follow its subtransaction's fate:
--   ROLLBACK TO savepoint  -> the index is still live, GPU artifacts must survive
--   RELEASE then top COMMIT -> the DROP is real, must fire (no over-suppression)
-- Probe: a row in pg_stat_gpu_search means the daemon still holds the index
-- (resident); 0 rows means it was dropped (artifacts freed + unlinked).
-- REQUIRES: pg_cuvs_server running with GPU; cuvs.index_dir writable.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

CREATE TABLE ds_test (id bigint, v vector(16));
INSERT INTO ds_test
SELECT id, array_agg(random() ORDER BY d)::real[]::vector(16)
FROM generate_series(1, 2000) id, generate_series(1, 16) d
GROUP BY id;
CREATE INDEX ds_idx ON ds_test USING cagra (v vector_l2_ops);

SELECT count(*) AS baseline_resident
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx';

-- 1. ROLLBACK TO savepoint: the DROP is undone -> index survives on GPU.
BEGIN; SAVEPOINT s1; DROP INDEX ds_idx; ROLLBACK TO s1; COMMIT;
SELECT count(*) AS catalog_after_rollback FROM pg_class WHERE relname = 'ds_idx';
SELECT count(*) AS resident_after_rollback
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx';

-- 2. Nested: DROP in inner subxact, RELEASE inner, ROLLBACK outer -> survives
--    (the drop is reparented to the outer subxact, then discarded when it aborts).
BEGIN; SAVEPOINT s1; SAVEPOINT s2; DROP INDEX ds_idx; RELEASE s2; ROLLBACK TO s1; COMMIT;
SELECT count(*) AS resident_after_nested
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx';

-- 3. RELEASE savepoint then top COMMIT: the DROP is real -> fires.
BEGIN; SAVEPOINT s1; DROP INDEX ds_idx; RELEASE s1; COMMIT;
SELECT count(*) AS catalog_after_release FROM pg_class WHERE relname = 'ds_idx';
SELECT count(*) AS resident_after_release
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx';

-- 4. Plain top-level DROP fires (regression control).
CREATE INDEX ds_idx2 ON ds_test USING cagra (v vector_l2_ops);
SELECT count(*) AS rebuilt_resident
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx2';
DROP INDEX ds_idx2;
SELECT count(*) AS resident_after_plain_drop
FROM pg_stat_gpu_search WHERE index_name = 'ds_idx2';

DROP TABLE ds_test CASCADE;
