-- cagra_streaming.sql — Phase 3Q: CAGRA Streaming Updates (EXTEND + COMPACT)
--
-- Tests:
--   1. INSERT after index build: new vector visible in search (EXTEND path).
--   2. pg_cuvs_compact() on a clean index: no-op, returns without error.
--   3. DELETE + VACUUM + pg_cuvs_compact(): dead vector removed from results.
--   4. Compact with no tombstone file: idempotent, returns OK.
--   5. VACUUM alone triggers compact (amvacuumcleanup path).
--   6. INSERT via EXTEND: delta_rows stays 0 (no delta fallback).
--   7. EXTEND updates vram_bytes to non-zero.
--   8. UPDATE e2e: new vector findable, compact succeeds.
--
-- Determinism: base cluster sits near origin (dist ~0.01); the inserted vector
-- sits at [10,0,0,0] — unambiguously the nearest neighbor to that query point.
-- After compact, the far point must not appear in top-1 for that query.
-- CAGRA is approximate but at this distance ratio (1000x) ordering is exact.
--
-- Requires a running pg_cuvs_server with a loaded CAGRA index.

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: small base cluster near origin, all within dist 0.05
-- ----------------------------------------------------------------
CREATE TABLE cs (id bigint, embedding vector(4));
INSERT INTO cs SELECT g, ('['||(g*0.01)||',0,0,0]')::vector
FROM generate_series(1, 100) g;
CREATE INDEX cs_cagra ON cs USING cagra (embedding vector_l2_ops);

-- ----------------------------------------------------------------
-- Test 1: INSERT a far vector — it must appear as top-1 near [10,0,0,0]
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (1001, '[10,0,0,0]');

SET enable_seqscan = off;
-- Expect: id=1001 (the just-inserted far vector)
SELECT id FROM cs ORDER BY embedding <-> '[10,0,0,0]'::vector LIMIT 1;

-- ----------------------------------------------------------------
-- Test 2: pg_cuvs_compact() on a clean index (no tombstones) — must not error
-- ----------------------------------------------------------------
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- ----------------------------------------------------------------
-- Test 3: DELETE the far vector, VACUUM, compact, verify it's gone
-- ----------------------------------------------------------------
DELETE FROM cs WHERE id = 1001;
VACUUM cs;
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- After compact: top-1 near [10,0,0,0] must come from the base cluster (id <= 100)
SELECT id <= 100 AS from_base_cluster
FROM cs ORDER BY embedding <-> '[10,0,0,0]'::vector LIMIT 1;

-- ----------------------------------------------------------------
-- Test 4: second compact on already-clean index — idempotent
-- ----------------------------------------------------------------
SELECT pg_cuvs_compact('cs_cagra'::regclass);

-- ----------------------------------------------------------------
-- Test 5: VACUUM alone triggers compact (amvacuumcleanup path)
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (2001, '[20,0,0,0]');
DELETE FROM cs WHERE id = 2001;
VACUUM cs;
-- After amvacuumcleanup fires COMPACT: top-1 near [20,0,0,0] must be from base cluster
SELECT id <= 100 AS from_base_cluster
FROM cs ORDER BY embedding <-> '[20,0,0,0]'::vector LIMIT 1;

-- ----------------------------------------------------------------
-- Scenario 6: INSERT -> EXTEND 성공 시 delta fallback 없음
-- daemon이 정상일 때 EXTEND가 delta_append를 건너뛰므로
-- pg_stat_gpu_search.delta_rows는 0을 유지해야 한다.
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (3001, '[30,0,0,0]');
SELECT delta_rows = 0 AS no_delta_fallback
FROM pg_stat_gpu_search
WHERE index_oid = 'cs_cagra'::regclass;
DELETE FROM cs WHERE id = 3001;

-- ----------------------------------------------------------------
-- Scenario 7: EXTEND 후 vram_bytes 갱신
-- handle_extend는 e->vram_bytes = estimate_vram_bytes(n_vecs, dim)으로
-- 갱신하므로 INSERT 후 vram_bytes > 0이어야 한다.
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (4001, '[40,0,0,0]');
SELECT vram_bytes > 0 AS vram_positive
FROM pg_stat_gpu_search
WHERE index_oid = 'cs_cagra'::regclass;
DELETE FROM cs WHERE id = 4001;

-- ----------------------------------------------------------------
-- Scenario 8: UPDATE e2e — 새 벡터 검색 가능, compact 정상
-- UPDATE는 DELETE(tombstone) + INSERT(EXTEND)로 변환된다.
-- compact 후 새 벡터([50,1,0,0])가 top-1으로 나와야 한다.
-- ----------------------------------------------------------------
INSERT INTO cs VALUES (5001, '[50,0,0,0]');
UPDATE cs SET embedding = '[50,1,0,0]' WHERE id = 5001;
VACUUM cs;
SELECT pg_cuvs_compact('cs_cagra'::regclass);
SELECT id = 5001 AS updated_vec_found
FROM cs ORDER BY embedding <-> '[50,1,0,0]'::vector LIMIT 1;
DELETE FROM cs WHERE id = 5001;

RESET enable_seqscan;

DROP TABLE cs;
DROP EXTENSION pg_cuvs;
