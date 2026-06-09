-- extend_cuda_oom.sql — physical CUDA OOM 시 EXTEND→delta fallback 검증
--
-- 목적: pg_cuvs_eat_vram()으로 VRAM을 거의 소진한 상태에서 cuvsCagraExtend가
-- rmm::out_of_memory를 던지고 _pr.poison() 경로를 거쳐 BUILD_FAILED를 반환하는
-- 물리적 OOM 경로를 테스트한다.  이는 budget-check 경로(extend_vram_fallback.sql)와
-- 달리 실제 CUDA 레이어의 예외 처리를 검증한다.
--
-- 설계:
--   1. dim=128, 5000벡터로 CAGRA 빌드 → 데몬 VRAM 캐시 워밍
--   2. pg_cuvs_set_vram_budget(0)  → budget check 비활성화 (0 = unlimited)
--   3. pg_cuvs_eat_vram(1MB)       → GPU VRAM 잔여 ~1MB (cuVS workspace 부족)
--   4. INSERT id=9999 [1.5,...,1.5] → EXTEND OOM → delta fallback
--   5. extend_count = 0  (OOM이 차단했음)
--   6. 검색: id=9999이 delta에 있으므로 가장 가까운 이웃 = 9999
--   7. pg_cuvs_free_vram() → VRAM 복원
--   8. REINDEX → delta 흡수, delta_rows = 0

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

-- ----------------------------------------------------------------
-- Setup: dim=128, 5000벡터 빌드
-- 각 벡터 = [(g%100)/100.0, ...] — 값 범위 [0.0, 0.99]
-- ----------------------------------------------------------------
CREATE TABLE co (id bigint, v vector(128));
INSERT INTO co
    SELECT g,
           array_fill((g % 100)::real / 100.0, ARRAY[128])::vector
    FROM generate_series(1, 5000) g;
CREATE INDEX co_cagra ON co USING cagra (v vector_l2_ops);

-- ----------------------------------------------------------------
-- 캐시 워밍: 빌드 직후 한 번 검색해 인덱스가 VRAM 캐시에 로드되도록 함.
-- (VRAM을 먹은 후 extend 시 인덱스 load 시도가 없도록)
-- ----------------------------------------------------------------
SELECT count(*) > 0 AS index_loaded
FROM co
ORDER BY v <-> array_fill(0.5::real, ARRAY[128])::vector
LIMIT 1;

-- ----------------------------------------------------------------
-- Test: budget check 비활성화 후 VRAM 소진 → 물리적 CUDA OOM 유발
-- ----------------------------------------------------------------
SELECT pg_cuvs_set_vram_budget(0);
SELECT pg_cuvs_eat_vram(1024 * 1024);  -- 1MB만 남김

-- id=9999 은 기존 데이터셋과 완전히 다른 위치 (값=1.5, 기존 최대=0.99)
-- EXTEND → cuvsCagraExtend → rmm::out_of_memory → _pr.poison() → BUILD_FAILED
-- → backend cuvs_aminsert delta fallback
INSERT INTO co VALUES (9999, array_fill(1.5::real, ARRAY[128])::vector);

-- extend_count = 0: OOM이 extend를 차단했음을 확인
SELECT extend_count = 0 AS cuda_oom_blocked_extend
FROM pg_stat_gpu_search
WHERE index_oid = 'co_cagra'::regclass;

-- ----------------------------------------------------------------
-- VRAM 복원 후 CAGRA+delta 검색 정합성 검증
-- id=9999 ([1.5,...])가 delta에 있으므로 쿼리 [1.5,...] 의 nearest = 9999
-- ----------------------------------------------------------------
SELECT pg_cuvs_free_vram();

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
