# Playbook: VRAM OOM 및 CPU Fallback

VRAM 고갈로 인한 build-time OOM, search-time CPU fallback, circuit breaker 동작에 대한
진단 및 복구 절차. ADR-010, ADR-011 참조.

---

## 1. 증상 (Symptoms)

**빌드 시 (CREATE INDEX):**
```
ERROR:  pg_cuvs: BUILD failed (status 2); CREATE INDEX aborted to preserve catalog durability
HINT:   GPU VRAM exhausted. Free VRAM (drop other cagra indexes or restart pg_cuvs_server) and retry, ...
```

**검색 시 (SELECT):**
```
WARNING:  pg_cuvs: VRAM exhausted, falling back to CPU
WARNING:  pg_cuvs: pg_cuvs_server unreachable, falling back to CPU
WARNING:  pg_cuvs: GPU search failed (status N), falling back to CPU
```

검색 시 fallback은 결과를 반환하지만 GPU가 아닌 CPU path(pgvector 또는 seqscan)로 처리된다.
circuit breaker가 트리거된 뒤에는 `WARNING` 없이 자동으로 CPU로 라우팅된다.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# VRAM 현황
nvidia-smi --query-gpu=memory.total,memory.used,memory.free --format=csv,noheader,nounits

# daemon이 보유한 인덱스 목록과 VRAM 추정치
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'loaded index|built index|evict|VRAM'

# daemon의 --max-vram-mb 설정 확인
sudo systemctl cat pg-cuvs-server | grep ExecStart

# circuit breaker 상태 (process-local, 세션별 상태)
# Phase 1.5에서는 pg_stat_gpu_search가 없으므로 cuvs.debug로 간접 확인한다
```

```sql
-- circuit breaker 상태 간접 확인
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE가 없고 결과만 나오면 GPU path가 아님 (fallback 또는 circuit open)
SET cuvs.debug = off;

-- circuit breaker threshold 확인
SHOW cuvs.circuit_breaker_threshold;
-- 기본값: 3 (연속 3회 GPU 오류 시 해당 인덱스 circuit open)
```

---

## 3. 원인 분기 (Cause branches)

### A. Build-time OOM: ensure_vram 실패 (status=2, OOM_FALLBACK)

`handle_build`에서 `ensure_vram(needed)`가 -1을 반환한다.
LRU eviction을 수행했지만 `--max-vram-mb` 예산 또는 물리 VRAM 한계를 넘었다.

3계층 OOM 정책 (ADR-010):
```
Layer 1 — preflight: total_vram_used + needed > max_vram_bytes
Layer 2 — LRU eviction: 가장 오래된 인덱스를 persist 후 VRAM 해제, 반복
Layer 3 — fallback: eviction 후에도 부족하면 OOM_FALLBACK 반환
```

build-time OOM은 DDL 실패로 처리된다 (query path와 달리 fallback success 없음).

### B. Search-time OOM: handle_search에서 cuvs_cagra_search 실패

`cuvs_cagra_search` 반환 != 0 시 `CUVS_STATUS_OOM_FALLBACK`을 반환한다.
`gettuple`이 `WARNING: VRAM exhausted, falling back to CPU`를 내고 false를 반환한다.
executor는 다음 index path(HNSW 또는 seqscan)로 자동 전환한다.

### C. Circuit breaker 트리거

`CUVS_STATUS_UNAVAILABLE`을 제외한 GPU 오류가 `cuvs.circuit_breaker_threshold`(기본 3)회
연속 발생하면 해당 인덱스에 대해 circuit breaker가 열린다.
이후 해당 인덱스로의 모든 쿼리는 `WARNING` 없이 자동으로 CPU로 라우팅된다 (process-local).

### D. --max-vram-mb가 너무 낮게 설정됨

물리 VRAM이 충분한데도 OOM이 발생하면 `--max-vram-mb` 값이 지나치게 보수적인 것이다.
`nvidia-smi`의 `memory.total`을 기준으로 적절한 값을 설정한다.

---

## 4. 복구 절차 (Recovery steps)

### Build-time OOM 복구

```bash
# 1. 현재 VRAM 점유량 파악
nvidia-smi

# 2. 불필요한 cagra 인덱스 제거 (LRU eviction 트리거)
```

```sql
-- 사용하지 않는 cagra 인덱스 목록
SELECT indexname, tablename FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%';

-- 불필요한 인덱스 DROP (daemon에서 해당 항목이 evict되고 VRAM 해제됨)
DROP INDEX <unused_index_name>;
```

```bash
# 3. --max-vram-mb 조정 (물리 VRAM의 80% 목표 예시)
# /etc/systemd/system/pg-cuvs-server.service 의 ExecStart 수정
sudo systemctl edit pg-cuvs-server --force
# ExecStart 행의 --max-vram-mb 값 변경

sudo systemctl daemon-reload
sudo systemctl restart pg-cuvs-server
```

```sql
-- 4. CREATE INDEX 재시도
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
```

### Search-time Fallback: 일시적 VRAM 부족

daemon의 LRU eviction이 VRAM을 확보하면 다음 검색부터 자동으로 GPU path로 복귀한다.
즉각적인 GPU 강제가 필요하면:

```bash
# daemon 재시작 (VRAM 초기화 후 startup_load_indexes 재실행)
sudo systemctl restart pg-cuvs-server
```

### Circuit breaker 해제

circuit breaker는 process-local이다 — backend 프로세스마다 상태가 다를 수 있다.

```sql
-- circuit breaker 해제 (인덱스 OID로 지정)
SELECT pg_cuvs_reset_circuit(<index_oid>);
-- 예: SELECT pg_cuvs_reset_circuit('cagra_idx'::regclass::oid);

-- 또는 인덱스 OID 조회 후 지정
SELECT oid, relname FROM pg_class WHERE relname = 'cagra_idx';
SELECT pg_cuvs_reset_circuit(16392);
```

circuit breaker가 반복적으로 트리거된다면 근본 원인(VRAM 부족, daemon 불안정)을 먼저 해결한다.

### GPU path 완전 비활성화 (비상)

```sql
-- 이 세션에서만 CPU 강제 사용
SET enable_cuvs = off;
```

`enable_cuvs = off`는 `PGC_USERSET`이므로 세션 단위로 적용된다.
전역 적용이 필요하면 `ALTER SYSTEM SET enable_cuvs = off`를 사용하되, 이는 임시 조치다.

---

## 5. 검증 명령 (Verification commands)

```bash
# VRAM 여유 확인
nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits

# eviction 이후 daemon journal 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -20
# "evict_lru" 또는 "saved index" 메시지 확인
```

```sql
-- GPU path 복귀 확인
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE: pg_cuvs: cagra scan ... 확인
SET cuvs.debug = off;

-- circuit breaker 해제 후 GPU latency 확인
SELECT pg_cuvs_last_search_latency_us();
-- NULL이면 아직 GPU search가 실행되지 않음
```

---

## 6. Escalation 기준 (When to escalate)

- `--max-vram-mb`를 물리 VRAM의 50% 이하로 낮춰도 build-time OOM이 반복되면:
  `cuvs_ambuild()`가 backend memory에 전체 corpus를 모으는 과정에서 메모리 압박이
  커지고 있을 수 있다. PLAN.md Phase 2 §5(streaming handoff)로 에스컬레이션.
- circuit breaker가 threshold=1 설정에서도 매 쿼리 트리거되면:
  GPU search 자체가 반복적으로 실패하는 것. daemon journal의 `OOM_FALLBACK` 이유 확인 후
  에스컬레이션.
- `pg_cuvs_reset_circuit()`이 없는 pg_cuvs 버전에서는 backend 프로세스를 종료
  (idle 연결 kill)하면 process-local circuit breaker가 초기화된다.
