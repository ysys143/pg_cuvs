# Playbook: 대규모 데이터셋 벤치마크

Phase 1.5 운영 기준선 고정을 위한 벤치마크 절차.
JIT threshold 결정, VRAM 예산 설정, fallback 정책 검증의 근거 데이터를 수집한다.
PLAN.md §5 참조.

---

## 1. 증상 (Symptoms)

이 playbook은 장애 대응이 아니라 측정 절차다.
다음 상황에서 실행한다:

- Phase 1.5 완료 기준으로 1M/1536d 이상 벤치마크가 필요할 때.
- `EXPLAIN (ANALYZE)`에 `JIT:` 섹션이 나타나 latency variance가 의심될 때.
- VRAM 예산(`cuvs.max_vram_mb`) 또는 `--max-vram-mb` 값을 결정해야 할 때.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# GPU 메모리 현황
ssh $GCP_VM "nvidia-smi --query-gpu=memory.total,memory.used,memory.free \
  --format=csv,noheader,nounits"

# daemon VRAM 추정 (registry 기준)
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index\|built index'"

# PG 백엔드별 CUDA context 생성 여부 확인
# pg_cuvs는 백엔드에서 CUDA context를 만들지 않아야 한다 (ADR-002).
# nvidia-smi에서 pg_cuvs_server 프로세스만 GPU를 점유해야 한다.
ssh $GCP_VM "nvidia-smi"
```

---

## 3. 원인 분기 (Cause branches)

이 playbook에서의 "원인 분기"는 측정 결과 해석이다.

| 관측 | 의미 | 다음 단계 |
|------|------|-----------|
| `EXPLAIN`에 `JIT:` 섹션 없음 | 정상. threshold 변경 불필요 | 완료 |
| `JIT:` 섹션 있고 p95/p99 안정 | JIT이 도움이 되거나 무해 | threshold sweep 생략 가능 |
| `JIT:` 섹션 있고 p95/p99 스파이크 | JIT이 vector-search latency를 해침 | jit-threshold-sweep.md 실행 |
| `nvidia-smi`에 PG 백엔드가 CUDA context 소유 | 아키텍처 위반 (ADR-002) | 즉시 에스컬레이션 |
| fallback count > 0 (OOM_FALLBACK) | VRAM 부족 | `--max-vram-mb` 조정 또는 인덱스 수 감소 |

---

## 4. 복구 절차 (Recovery steps)

### 4-1. 데이터 생성

```sql
-- Small: 10K x dim=384
CREATE TABLE bench_10k_384 (id bigint, v vector(384));
INSERT INTO bench_10k_384
  SELECT i, (SELECT array_agg(random())::vector(384) FROM generate_series(1,384))
  FROM generate_series(1, 10000) i;

-- Medium: 1M x dim=384  (시간이 걸림 — VM에서 실행 권장)
CREATE TABLE bench_1m_384 (id bigint, v vector(384));
INSERT INTO bench_1m_384
  SELECT i, (SELECT array_agg(random())::vector(384) FROM generate_series(1,384))
  FROM generate_series(1, 1000000) i;
```

dim=768, dim=1536 테이블은 같은 패턴으로 생성한다.

### 4-2. 인덱스 빌드 시간 측정

```sql
-- backend RSS 측정 전 pg_stat_bgwriter 리셋
SELECT pg_stat_reset();

\timing on
CREATE INDEX cagra_10k_384 ON bench_10k_384 USING cagra (v vector_l2_ops);
-- \timing 출력 기록 -> "CREATE INDEX build time"

-- daemon 로그에서 VRAM 확인
-- journalctl -u pg-cuvs-server -n 5 --no-pager
```

### 4-3. 검색 latency 측정 (k=10/100/1000)

```sql
-- cold planning (새 세션에서)
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
-- "Planning Time", "Execution Time", "JIT:" 섹션 여부 기록

-- warm planning (같은 세션, 두 번째 실행)
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

p50/p95/p99는 루프로 측정한다:

```bash
ssh $GCP_VM "psql -d postgres -c \"
  SELECT percentile_cont(ARRAY[0.5,0.95,0.99]) WITHIN GROUP (ORDER BY elapsed_ms)
  FROM (
    SELECT (extract(epoch from clock_timestamp()) - extract(epoch from statement_timestamp())) * 1000 AS elapsed_ms
    FROM generate_series(1,100) i,
         LATERAL (SELECT 1 FROM bench_1m_384 ORDER BY v <-> '[0.1,0.2,0.3]'::vector(384) LIMIT 10) q
  ) t;
\""
```

[INFO] 이 쿼리 템플릿은 pg_cuvs_last_search_latency_us()보다 정확도가 낮다. Phase 2에서 `pg_stat_gpu_search`가 추가되면 해당 뷰를 사용한다.

### 4-4. 아티팩트 크기 및 reload 시간 측정

```bash
# .cagra + .tids 파일 크기
ssh $GCP_VM "ls -lh /tmp/cuvs_indexes/"

# daemon 재시작 후 reload 시간
ssh $GCP_VM "time sudo systemctl restart pg-cuvs-server"
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index'"
```

### 4-5. fallback count 확인

```sql
-- fallback 발생 시 pg_cuvs_server journal에 WARNING이 남는다.
-- 현재 Phase 1.5에서는 pg_stat_gpu_search가 없으므로 journal로 확인한다.
```

```bash
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server --no-pager | grep -i 'fallback\|OOM\|evict'"
```

---

## 5. 검증 명령 (Verification commands)

```bash
# daemon이 단독으로 GPU를 점유하는지 확인 (PG 백엔드는 0 MB여야 함)
ssh $GCP_VM "nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
  --format=csv,noheader"

# 검색 결과 정확도 (id=1이 최근접이어야 함)
ssh $GCP_VM "psql -d postgres -c \
  \"SELECT id FROM bench_10k_384 ORDER BY v <-> (SELECT v FROM bench_10k_384 WHERE id=1) LIMIT 1;\""
```

---

## 6. Escalation 기준 (When to escalate)

- `nvidia-smi`에서 PG 백엔드 프로세스가 CUDA context를 소유하면: 즉시 에스컬레이션. ADR-002 위반.
- 1M 벤치마크에서 `cuvs_ambuild()` 중 backend OOM (`pg_cuvs: out of memory accumulating index vectors`): Phase 2 streaming handoff 전 임시 대응으로 `work_mem` 증가는 무의미 (malloc 사용). 데이터를 분할하거나 PLAN.md Phase 2 §5 참조.
- `EXPLAIN (ANALYZE)`에서 `JIT:` 섹션이 나타나고 p95 latency가 기준치의 2배 이상이면: jit-threshold-sweep.md 실행.
