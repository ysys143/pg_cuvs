-- extend_cuda_oom.sql — cuvsCagraExtend OOM 경로 검증 (_pr.poison() + delta fallback)
--
-- 목적: cuvsCagraExtend 내부의 예외 핸들러(_pr.poison() → BUILD_FAILED → delta fallback)를
-- 검증한다.  RMM pool이 freed VRAM을 내부적으로 캐싱하므로 외부 cudaMalloc 소진으로는
-- 물리적 OOM을 유발할 수 없다.  대신 pg_cuvs_inject_extend_oom(1)으로 bad_alloc을
-- 직접 주입해 동일한 예외 처리 경로를 확실하게 커버한다.
--
-- 설계:
--   1. dim=128, 5000벡터 CAGRA 빌드
--   2. pg_cuvs_set_vram_budget(0)   → budget check 비활성화
--   3. pg_cuvs_inject_extend_oom(1) → 다음 extend에서 bad_alloc 주입
--   4. INSERT id=9999                → extend throws → _pr.poison() → BUILD_FAILED
--                                     → backend delta fallback
--   5. extend_count = 0              → OOM이 extend를 막았음을 확인
--   6. pg_cuvs_inject_extend_oom(0) → 플래그 해제 (자동 해제됐지만 명시적 정리)
--   7. 검색: id=9999이 delta에 있으므로 nearest = 9999
--   8. REINDEX → delta 흡수, delta_rows = 0

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: dim=128, 5000벡터 빌드
-- ----------------------------------------------------------------
CREATE TABLE co (id bigint, v vector(128));
INSERT INTO co
    SELECT g,
           array_fill((g % 100)::real / 100.0, ARRAY[128])::vector
    FROM generate_series(1, 5000) g;
CREATE INDEX co_cagra ON co USING cagra (v vector_l2_ops);

-- 캐시 워밍
SELECT count(*) > 0 AS index_loaded
FROM (
    SELECT 1 FROM co
    ORDER BY v <-> array_fill(0.5::real, ARRAY[128])::vector
    LIMIT 1
) t;

-- ----------------------------------------------------------------
-- Test: budget check 비활성화 + extend OOM 주입
-- ----------------------------------------------------------------
SELECT pg_cuvs_set_vram_budget(0);
SELECT pg_cuvs_inject_extend_oom(1);

-- INSERT → cuvs_cagra_extend throws bad_alloc → _pr.poison() → BUILD_FAILED
-- → cuvs_aminsert falls back to delta
INSERT INTO co VALUES (9999, array_fill(1.5::real, ARRAY[128])::vector);

-- extend_count = 0: OOM이 extend를 차단했음을 확인
SELECT extend_count = 0 AS cuda_oom_blocked_extend
FROM pg_stat_gpu_search
WHERE index_oid = 'co_cagra'::regclass;

-- ----------------------------------------------------------------
-- Cleanup injection flag (already self-cleared on fire, but explicit)
-- ----------------------------------------------------------------
SELECT pg_cuvs_inject_extend_oom(0);

-- ----------------------------------------------------------------
-- CAGRA + delta 검색 정합성: id=9999([1.5,...])가 delta에 있어야 함
-- ----------------------------------------------------------------
SELECT id FROM co
ORDER BY v <-> array_fill(1.5::real, ARRAY[128])::vector
LIMIT 1;

-- ----------------------------------------------------------------
-- REINDEX: delta 흡수
-- ----------------------------------------------------------------
REINDEX INDEX CONCURRENTLY co_cagra;

SELECT delta_rows = 0   AS no_delta,
       extend_count = 0 AS extend_clean
FROM pg_stat_gpu_search
WHERE index_oid = 'co_cagra'::regclass;

-- ----------------------------------------------------------------
-- Teardown
-- ----------------------------------------------------------------
DROP TABLE co;
