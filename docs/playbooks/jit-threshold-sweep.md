# Playbook: JIT Threshold Sweep

> [!WARNING]
> large-dataset-benchmark.md 결과에서 `JIT:` 섹션 + p95 스파이크가 확인된 경우에만 실행.
> 측정 없이 `jit = off`를 전역 적용하거나 임의의 `jit_above_cost`를 설정하지 않는다.
> 이는 프로젝트 정책이다 (PLAN.md L163-168, ADR-018).

---

## 1. 증상 (Symptoms)

- `EXPLAIN (ANALYZE)` 출력에 `JIT:` 섹션이 있고 `Timing: yes`.
- vector-search p95 또는 p99 latency가 p50의 3배 이상이거나 run-to-run variance가 크다.
- `large-dataset-benchmark.md`의 측정 데이터가 이미 수집된 상태다.

다음 증상만으로는 이 playbook을 실행하지 않는다:

- `JIT:` 섹션이 있어도 latency가 안정적인 경우.
- 소규모 데이터셋(10K)에서만 JIT가 발생하는 경우.

---

## 2. 진단

```sql
SHOW jit;
SHOW jit_above_cost;
SHOW jit_optimize_above_cost;
```

**기대 출력:**
```
 jit
-----
 on
(1 row)

 jit_above_cost
----------------
 100000
(1 row)

 jit_optimize_above_cost
-------------------------
 500000
(1 row)
```
**→ 정상:** 기본값 확인  
**→ 이상 시:** `jit = off` → 이 playbook 불필요 (JIT이 이미 비활성화됨)

---

```sql
EXPLAIN (ANALYZE, VERBOSE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**기대 출력 (JIT 활성화됨):**
```
...
JIT:
  Functions: 3
  Options: Inlining true, Optimization true, Expressions true, Deforming true
  Timing: Generation 1.234 ms, Inlining 5.678 ms, Optimization 12.345 ms, Emission 8.901 ms, Total 28.158 ms
...
```
**기대 출력 (JIT 비활성화됨):**
```
(JIT: 섹션 없음)
```
**→ JIT: 섹션 없음:** 이 playbook 불필요  
**→ JIT: 있고 p95 안정:** 이 playbook 불필요  
**→ JIT: 있고 p95 스파이크:** Step 0으로

---

```bash
ssh $GCP_VM "grep -E '^jit' \$(psql -t -c 'SHOW config_file' | tr -d ' ') 2>/dev/null || echo 'no jit settings'"
```

**기대 출력:**
```
no jit settings
```
또는 현재 적용된 jit 관련 설정 행  
**→ 참고:** postgresql.conf에 jit 관련 설정이 없으면 기본값이 적용 중임

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

## 4. Step-by-step 복구

### Step 0 — 전제 확인

```sql
EXPLAIN (ANALYZE, VERBOSE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**→ JIT: 섹션 없음:** 이 playbook 불필요 — 중단  
**→ JIT: 있고 p95 안정:** 이 playbook 불필요 — 중단  
**→ JIT: 있고 p95 스파이크 확인됨:** Step 1으로

---

### Step 1 — baseline 기록

현재 `jit_above_cost` 기본값에서 p50/p95/p99를 기록한다.
large-dataset-benchmark.md §4-3의 latency 측정 결과를 기준으로 사용한다.

```sql
SHOW jit_above_cost;
```

**기대 출력:**
```
 jit_above_cost
----------------
 100000
(1 row)
```

현재 Planning/Execution time을 기록한다:

```sql
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**기대 출력:**
```
Planning Time: X.XXX ms
Execution Time: X.XXX ms
```

baseline p50 / p95 / p99 값을 메모한 뒤 Step 2로

---

### Step 2 — jit_above_cost = 1e6 테스트

```sql
SET jit_above_cost = 1000000;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**기대 출력 (JIT 제거됨):**
```
Planning Time: X.XXX ms
Execution Time: X.XXX ms
(JIT: 섹션 없음)
```
**기대 출력 (JIT 여전히 있음):**
```
JIT:
  ...
```

Planning/Execution time과 JIT: 섹션 유무를 기록한다.  
**→ JIT 사라지고 p95 스파이크 없음:** 이 값이 후보 → Step 5로  
**→ JIT 여전히 있음 또는 p95 스파이크 지속:** Step 3으로

---

### Step 3 — jit_above_cost = 1e7 테스트

```sql
SET jit_above_cost = 10000000;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**기대 출력 (JIT 제거됨):**
```
Planning Time: X.XXX ms
Execution Time: X.XXX ms
(JIT: 섹션 없음)
```

Planning/Execution time과 JIT: 섹션 유무를 기록한다.  
**→ JIT 사라지고 p95 스파이크 없음:** 이 값이 후보 → Step 5로  
**→ JIT 여전히 있음 또는 p95 스파이크 지속:** Step 4로

---

### Step 4 — jit_above_cost = 1e8 테스트

```sql
SET jit_above_cost = 100000000;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM bench_1m_384
  ORDER BY v <-> '[0.1,0.2,...]'::vector(384)
  LIMIT 10;
```

**기대 출력 (JIT 제거됨):**
```
Planning Time: X.XXX ms
Execution Time: X.XXX ms
(JIT: 섹션 없음)
```

Planning/Execution time과 JIT: 섹션 유무를 기록한다.  
**→ JIT 사라지고 p95 스파이크 없음:** 이 값이 후보 → Step 5로  
**→ 1e8에서도 JIT 있거나 p95 스파이크 지속:** Escalation 기준 참조

**결과 해석:**
- vector-search p95/p99가 스파이크 없는 후보 중 가장 낮은 값을 선택한다.
- mixed analytical workload가 있는 경우: 분석 쿼리에 대해서도 동일 threshold를
  적용했을 때 JIT 이득이 유지되는지 별도 측정한다.
  vector-search 이득과 분석 쿼리 이득이 충돌하면 전역 설정 대신
  `SET LOCAL jit_above_cost` 또는 별도 connection pool 분리를 검토한다.

---

### Step 5 — postgresql.conf 적용

threshold 값이 결정된 경우에만 적용한다. 예: 1e7로 결정된 경우:

```bash
ssh $GCP_VM "sudo -u postgres psql -c \
  \"ALTER SYSTEM SET jit_above_cost = 10000000;\""
```

**기대 출력:**
```
ALTER SYSTEM
```

```bash
ssh $GCP_VM "sudo -u postgres psql -c \"SELECT pg_reload_conf();\""
```

**기대 출력:**
```
 pg_reload_conf
----------------
 t
(1 row)
```

적용 확인:

```bash
ssh $GCP_VM "psql -d postgres -c 'SHOW jit_above_cost;'"
```

**기대 출력:**
```
 jit_above_cost
----------------
 10000000
(1 row)
```
**→ 성공:** 검증 체크리스트로  
**→ 실패:** 값이 바뀌지 않음 → `ALTER SYSTEM` 후 `pg_reload_conf()` 재시도

> [!WARNING]
> `jit = off` 전역 적용은 허용되지 않는다. analytical workload의 JIT 이득을
> 측정 없이 제거하는 것이기 때문이다. sweep 결과가 1e8에서도 스파이크가 사라지지 않는다면
> 에스컬레이션한다.

---

## 5. 검증 체크리스트

- [ ] `EXPLAIN (ANALYZE, BUFFERS) SELECT id FROM bench_1m_384 ORDER BY v <-> '[0.1,0.2,...]'::vector(384) LIMIT 10;` → 기대 출력: `JIT:` 섹션 없거나 `Timing: no`
- [ ] p50/p95/p99 재측정 → 기대 출력: baseline 대비 p95 스파이크 없고 동등하거나 개선됨
- [ ] `SHOW jit_above_cost;` → 기대 출력: 결정된 threshold 값 (예: `10000000`)
- [ ] 재시작 후 설정 유지 확인:
  ```bash
  ssh $GCP_VM "sudo systemctl restart postgresql"
  ssh $GCP_VM "psql -d postgres -c 'SHOW jit_above_cost;'"
  ```
  → 기대 출력: 설정한 값 그대로 유지

---

## 6. Escalation 기준 (When to escalate)

- 1e8에서도 `JIT:` 섹션이 사라지지 않고 p99 스파이크가 계속되면:
  cost model 자체가 의도치 않게 매우 높은 값을 반환하고 있을 가능성.
  `cuvsamcostestimate` 반환값을 `EXPLAIN (VERBOSE)`로 확인하고
  cost model 버그로 에스컬레이션한다.
- mixed workload에서 vector-search와 분석 쿼리의 최적 threshold가 충돌하면:
  connection pool 분리 또는 workload별 `jit_above_cost` 오버라이드를 검토.
  이 결정은 이 playbook의 범위를 벗어난다.
