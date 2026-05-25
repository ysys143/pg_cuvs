# Playbook: JIT Threshold Sweep

[WARN] 이 playbook은 large-dataset-benchmark.md 실행 결과 `EXPLAIN (ANALYZE)` 출력에
`JIT:` 섹션이 나타나고 vector-search p95/p99 latency에 스파이크가 관측된 경우에만 실행한다.

측정 없이 `jit = off`를 전역 적용하거나 임의의 `jit_above_cost`를 설정하지 않는다.
이는 프로젝트 정책이다 (PLAN.md L163-168, ADR-018).

---

## 1. 증상 (Symptoms)

- `EXPLAIN (ANALYZE)` 출력에 `JIT:` 섹션이 있고 `Timing: yes`.
- vector-search p95 또는 p99 latency가 p50의 3배 이상이거나 run-to-run variance가 크다.
- `large-dataset-benchmark.md`의 측정 데이터가 이미 수집된 상태다.

다음 증상만으로는 이 playbook을 실행하지 않는다:

- `JIT:` 섹션이 있어도 latency가 안정적인 경우.
- 소규모 데이터셋(10K)에서만 JIT가 발생하는 경우.

---

## 2. 확인 명령 (Diagnostic commands)

```sql
-- 현재 JIT 관련 설정 확인
SHOW jit;
SHOW jit_above_cost;
SHOW jit_optimize_above_cost;

-- EXPLAIN에서 JIT 섹션 확인
EXPLAIN (ANALYZE, VERBOSE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
-- "JIT:" 섹션 존재 여부 및 "Timing: yes/no" 확인
```

```bash
# 현재 postgresql.conf에서 JIT 관련 설정 확인
ssh $GCP_VM "grep -E '^jit' \$(psql -t -c 'SHOW config_file' | tr -d ' ') 2>/dev/null || echo 'no jit settings'"
```

---

## 3. 원인 분기 (Cause branches)

JIT이 트리거되는 구조적 원인:

- `shared_preload_libraries = 'pg_cuvs'` 설정 후 `cagra` cost model의 total cost가
  `jit_above_cost` 기본값(100000)을 초과하는 경우 JIT이 활성화된다.
- 현재 cost model: `startup_cost=1000 + per_tuple_cost * rows`.
  rows가 수십만 이상이면 합산 cost가 기본 `jit_above_cost`를 넘을 수 있다.
- JIT은 GPU kernel 실행 자체가 아니라 PG executor 표현식 평가에 적용된다.
  vector-search path에서 JIT compilation overhead가 GPU search 이득을 상쇄할 수 있다.

---

## 4. 복구 절차 (Recovery steps)

### 4-1. baseline 측정 (현재 기본값)

현재 `jit_above_cost` 기본값에서 p50/p95/p99를 기록한다.
large-dataset-benchmark.md §4-3의 latency 측정 결과를 기준으로 사용한다.

### 4-2. threshold 후보 sweep

후보 값: 기본값(100000), 1e6, 1e7, 1e8.
각 값에서 p95/p99가 baseline 대비 스파이크 없이 안정적인지 확인한다.

```sql
-- 세션 레벨에서 후보 값 적용 (postgresql.conf 변경 없이 테스트)
SET jit_above_cost = 1000000;   -- 1e6
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
-- JIT: 섹션 사라졌는지 확인, Planning/Execution time 기록
```

```sql
SET jit_above_cost = 10000000;  -- 1e7
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

```sql
SET jit_above_cost = 100000000; -- 1e8
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

### 4-3. 결과 해석 및 값 선택

- vector-search p95/p99가 스파이크 없는 후보 중 가장 낮은 값을 선택한다.
- mixed analytical workload가 있는 경우: 분석 쿼리에 대해서도 동일 threshold를
  적용했을 때 JIT 이득이 유지되는지 별도 측정한다.
  vector-search 이득과 분석 쿼리 이득이 충돌하면 전역 설정 대신
  `SET LOCAL jit_above_cost` 또는 별도 connection pool 분리를 검토한다.

### 4-4. postgresql.conf 적용 (결정 후에만)

```bash
# threshold 값이 결정된 경우에만 적용
# 예: 1e7로 결정된 경우
ssh $GCP_VM "sudo -u postgres psql -c \
  \"ALTER SYSTEM SET jit_above_cost = 10000000;\""
ssh $GCP_VM "sudo -u postgres psql -c \"SELECT pg_reload_conf();\""

# 확인
ssh $GCP_VM "psql -d postgres -c 'SHOW jit_above_cost;'"
```

[WARN] `jit = off` 전역 적용은 허용되지 않는다. analytical workload의 JIT 이득을
측정 없이 제거하는 것이기 때문이다. sweep 결과가 1e8에서도 스파이크가 사라지지 않는다면
에스컬레이션한다.

---

## 5. 검증 명령 (Verification commands)

```sql
-- threshold 적용 후 JIT 섹션 소멸 확인
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
-- "JIT:" 섹션이 없거나 "Timing: no"여야 함

-- p50/p95/p99 재측정 (baseline 대비 개선 또는 동등 확인)
```

```bash
# 설정이 재시작 후에도 유지되는지 확인
ssh $GCP_VM "sudo systemctl restart postgresql"
ssh $GCP_VM "psql -d postgres -c 'SHOW jit_above_cost;'"
```

---

## 6. Escalation 기준 (When to escalate)

- 1e8에서도 `JIT:` 섹션이 사라지지 않고 p99 스파이크가 계속되면:
  cost model 자체가 의도치 않게 매우 높은 값을 반환하고 있을 가능성.
  `cuvsamcostestimate` 반환값을 `EXPLAIN (VERBOSE)`로 확인하고
  cost model 버그로 에스컬레이션한다.
- mixed workload에서 vector-search와 분석 쿼리의 최적 threshold가 충돌하면:
  connection pool 분리 또는 workload별 `jit_above_cost` 오버라이드를 검토.
  이 결정은 이 playbook의 범위를 벗어난다.
