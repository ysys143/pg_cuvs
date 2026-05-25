# Playbook: CREATE INDEX 실패 진단

`CREATE INDEX USING cagra`가 `ERROR`로 실패하는 경우의 원인 분기와 복구 절차.

DDL durability 계약 (ADR-016): `CREATE INDEX`가 성공하려면 daemon 연결, VRAM CAGRA 빌드,
`.cagra` 직렬화, `.tids` persistence, atomic rename, fsync가 모두 성공해야 한다.
어느 하나라도 실패하면 `ereport(ERROR)`로 DDL이 실패하고 PostgreSQL catalog는 rollback된다.
이는 의도된 동작이다 — catalog에 인덱스가 존재하지만 artifact가 없는 상태를 방지한다.

---

## 1. 증상 (Symptoms)

```
ERROR:  pg_cuvs: BUILD failed (status N); CREATE INDEX aborted to preserve catalog durability
HINT:   ...
```

또는:

```
ERROR:  pg_cuvs: BUILD failed (status 4); CREATE INDEX aborted to preserve catalog durability
HINT:   pg_cuvs_server is not reachable. Start it and retry CREATE INDEX, ...
```

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# daemon 상태 확인
sudo systemctl status pg-cuvs-server
sudo systemctl is-active pg-cuvs-server

# daemon journal (최근 100줄)
sudo journalctl -u pg-cuvs-server -n 100 --no-pager

# 소켓 존재 여부
ls -la /tmp/.s.pg_cuvs

# index_dir 디스크 여유 공간
df -h /tmp/cuvs_indexes/

# VRAM 여유
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

```sql
-- PostgreSQL에서 GUC 확인
SHOW cuvs.socket_path;
SHOW cuvs.index_dir;
SHOW enable_cuvs;
```

---

## 3. 원인 분기 (Cause branches)

IPC status code별 원인과 힌트 메시지:

| Status code | 값 | 의미 | 힌트 |
|---|---|---|---|
| `CUVS_STATUS_UNAVAILABLE` | 4 | daemon이 내려갔거나 소켓이 없음 | daemon을 시작하고 재시도 |
| `CUVS_STATUS_OOM_FALLBACK` | 2 | VRAM 부족 — build 단계에서 ensure_vram 실패 | VRAM 확보 후 재시도, 또는 HNSW 사용 |
| `CUVS_STATUS_BUILD_FAILED` | 5 | GPU CAGRA 빌드 실패 (cuvs_cagra_build 반환 NULL) | journal에서 CUDA 오류 확인 |
| `CUVS_STATUS_PERSIST_FAILED` | 6 | 빌드 성공, 디스크 직렬화 실패 또는 registry full | 디스크 공간/권한 확인, registry 정원(64) 초과 확인 |
| `CUVS_STATUS_ERROR` | 1 | 기타 daemon 오류 (shm 실패 등) | journal 확인 |

### A. UNAVAILABLE (status=4): daemon 미구동
```bash
sudo systemctl start pg-cuvs-server
sleep 2
sudo systemctl is-active pg-cuvs-server
ls -la /tmp/.s.pg_cuvs
```

### B. OOM_FALLBACK (status=2): VRAM 부족
```bash
# 현재 VRAM 점유 확인
nvidia-smi
sudo journalctl -u pg-cuvs-server --no-pager | grep 'VRAM\|evict\|MB'
```
VRAM 확보 방법:
- 사용하지 않는 cagra 인덱스를 DROP INDEX한 뒤 `REINDEX` (LRU eviction 트리거).
- daemon 재시작 (`--max-vram-mb` 값 검토).
- 인덱스 수를 줄이거나 `cuvs.max_vram_mb` GUC를 낮춰 eviction을 더 일찍 트리거.

### C. BUILD_FAILED (status=5): GPU 빌드 오류
```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'cuvs_cagra_build|FAILED|CUDA|error'
```
- CUDA out-of-memory during build: OOM_FALLBACK과 동일 복구 절차.
- cuVS API 오류: libcuvs 버전과 빌드 시 헤더 버전 일치 여부 확인.

### D. PERSIST_FAILED (status=6): 디스크 직렬화 실패
```bash
# 디스크 공간
df -h $(psql -t -c "SHOW cuvs.index_dir;" | tr -d ' ')

# 권한 (daemon user가 쓸 수 있어야 함)
ls -la /tmp/cuvs_indexes/

# tmp 파일 잔재 확인 (원자적 rename 실패 흔적)
ls /tmp/cuvs_indexes/*.tmp 2>/dev/null

# registry full 여부: journal에서 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep 'registry full'
```

registry full(최대 64개):
```sql
-- 사용하지 않는 cagra 인덱스 조회
SELECT indexname, tablename FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%'
ORDER BY tablename;
-- 불필요한 인덱스 DROP INDEX 후 재시도
```

---

## 4. 복구 절차 (Recovery steps)

DDL이 실패하면 catalog는 이미 rollback된 상태다. 추가 cleanup이 필요 없다.
원인을 해결한 뒤 `CREATE INDEX USING cagra`를 재시도한다.

### UNAVAILABLE 복구

```bash
sudo systemctl start pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 20 --no-pager
# "listening on /tmp/.s.pg_cuvs" 확인
```

```sql
-- 재시도
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
```

### PERSIST_FAILED: 디스크 공간 부족

```bash
# 오래된 tmp 파일 정리 (daemon이 정리 못한 경우)
rm -f /tmp/cuvs_indexes/*.tmp

# 공간 확보 후 재시도
```

### enable_cuvs=off로 CPU 경로 우회 (긴급)

GPU 가속이 당장 필요하지 않은 경우:

```sql
SET enable_cuvs = off;
CREATE INDEX hnsw_idx ON items USING hnsw (embedding vector_l2_ops);
-- GPU 준비 후 DROP INDEX hnsw_idx; CREATE INDEX USING cagra; 로 전환
```

---

## 5. 검증 명령 (Verification commands)

```bash
# CREATE INDEX 성공 후 artifact가 디스크에 있는지 확인
ls -lh /tmp/cuvs_indexes/
# <db_oid>_<index_oid>.cagra + <db_oid>_<index_oid>.tids 쌍이 있어야 함

# daemon journal에 빌드 성공 메시지 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -10
# "built index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)" 확인
```

```sql
-- 검색 동작 확인
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE: pg_cuvs: cagra scan ... 확인
SET cuvs.debug = off;
```

---

## 6. Escalation 기준 (When to escalate)

- status=5 (BUILD_FAILED)이고 journal에 CUDA 오류 없이 `cuvs_cagra_build returned NULL`만 있으면:
  cuVS 버전과 빌드 시 헤더 버전 불일치 가능성. `make gpu-build` 재실행 검토.
- status=6 (PERSIST_FAILED)이고 디스크 공간과 권한이 모두 정상인데 계속 실패하면:
  `cuvs_cagra_serialize` 내부 오류. journal의 정확한 errno와 경로를 포함해 에스컬레이션.
- 동일 status가 반복되고 원인이 불명확하면: `cuvs.debug = on` + daemon 로그 전체를 수집해 에스컬레이션.
