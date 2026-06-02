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

## 2. 진단

오류 메시지의 `status=N` 값을 읽고 아래 순서로 확인한다.

### 2-1. status 코드 확인

```
ERROR:  pg_cuvs: BUILD failed (status N); ...
```

| status N | 원인 | 이동 |
|---|---|---|
| 4 | daemon 미구동 | Step 2A로 |
| 2 | VRAM 부족 | Step 2B로 |
| 5 | GPU CAGRA 빌드 실패 | Step 2C로 |
| 6 | 디스크 직렬화 실패 | Step 2D로 |
| 1 | 기타 daemon 오류 | journal 확인 후 Step 2A로 |

---

### Step 2A — daemon 상태 확인 (status=4)

psql 안에서 나가지 않고 확인하려면 `\!` 메타 커맨드를 사용한다:

```sql
-- psql 안에서 바로 실행
\! sudo systemctl is-active pg-cuvs-server
```

또는 bash에서:

```bash
sudo systemctl is-active pg-cuvs-server
```

**기대 출력:**
```
active
```
**-> 정상 (active):** Step 2A-2로  
**-> 이상 (inactive / failed):** daemon 미구동 -> 복구 Step 1로

```bash
# Step 2A-2: 소켓 존재 여부
ls -la /tmp/.s.pg_cuvs
```

**기대 출력:**
```
srwxrwxrwx 1 postgres postgres 0 ... /tmp/.s.pg_cuvs
```
**-> 정상:** 소켓 있음 -> GUC 확인 (SHOW cuvs.socket_path;) 후 재시도  
**-> 이상 (No such file):** 소켓 없음 -> 복구 Step 1로

---

### Step 2B — VRAM 여유 확인 (status=2)

```bash
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

**기대 출력 (단위: MiB):**
```
8192, 24576
```
**-> 정상 (free > 1000):** VRAM 충분 -> journal에서 eviction 실패 여부 확인  
**-> 이상 (free < 500):** VRAM 부족 -> 복구 Step 2로

```bash
# eviction 관련 로그 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'VRAM|evict|MB'
```

**기대 출력:**
```
pg_cuvs: evicted index 16384/16392 (freed 512 MB)
```
**-> eviction 없이 OOM:** 복구 Step 2로  
**-> eviction 반복 실패:** daemon 재시작 검토

---

### Step 2C — GPU 빌드 오류 확인 (status=5)

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'cuvs_cagra_build|FAILED|CUDA|error'
```

**기대 출력 (오류 예시):**
```
pg_cuvs: cuvs_cagra_build returned NULL (CUDA out of memory)
```
**-> CUDA out of memory:** OOM_FALLBACK과 동일 -> 복구 Step 2로  
**-> cuVS API 오류 (`cuvs_cagra_build returned NULL` 외 메시지):** libcuvs 버전 불일치 -> 복구 Step 3로  
**-> 오류 없음:** daemon journal 전체 확인 후 에스컬레이션

---

### Step 2D — 디스크/registry 확인 (status=6)

```bash
# 디스크 공간
df -h $(psql -t -c "SHOW cuvs.index_dir;" | tr -d ' ')
```

**기대 출력:**
```
Filesystem  Size  Used Avail Use%
tmpfs        16G  2.1G   14G  14% /tmp
```
**-> 정상 (Avail > 1G):** Step 2D-2로  
**-> 이상 (Use% > 95%):** 디스크 꽉 참 -> 복구 Step 4로

```bash
# Step 2D-2: tmp 파일 잔재 (원자적 rename 실패 흔적)
ls /tmp/cuvs_indexes/*.tmp 2>/dev/null
```

**기대 출력:**
```
(출력 없음)
```
**-> .tmp 파일 있음:** 이전 빌드가 rename 전에 중단됨 -> `rm -f /tmp/cuvs_indexes/*.tmp` 후 재시도  
**-> 출력 없음:** Step 2D-3으로

```bash
# Step 2D-3: registry full 여부
sudo journalctl -u pg-cuvs-server --no-pager | grep 'registry full'
```

**기대 출력 (registry full 시):**
```
pg_cuvs: registry full (64/64 slots occupied), cannot add new index
```
**-> registry full:** 복구 Step 5로  
**-> 출력 없음:** 디스크 권한 확인 (`ls -la /tmp/cuvs_indexes/`) 후 에스컬레이션

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

각 원인의 복구 이동:
- UNAVAILABLE (status=4) -> 복구 Step 1로
- OOM_FALLBACK (status=2) -> 복구 Step 2로
- BUILD_FAILED (status=5) libcuvs 불일치 -> 복구 Step 3로
- PERSIST_FAILED 디스크 공간 -> 복구 Step 4로
- PERSIST_FAILED registry full -> 복구 Step 5로

---

## 4. Step-by-step 복구

### Step 1 — daemon 시작 (UNAVAILABLE 복구)

psql 안에서 나가지 않고 바로 실행:

```sql
\! sudo systemctl start pg-cuvs-server
\! sudo journalctl -u pg-cuvs-server -n 20 --no-pager
```

또는 bash에서:

```bash
sudo systemctl start pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 20 --no-pager
```

**기대 출력:**
```
pg_cuvs: listening on /tmp/.s.pg_cuvs
pg_cuvs: startup_load_indexes: loaded N indexes
```
**-> 성공 ("listening on" 확인):** Step 5 (검증)로  
**-> 실패 (start failed, dependency error):** `journalctl -xe` 확인 후 에스컬레이션

---

### Step 2 — VRAM 확보 (OOM_FALLBACK 복구)

```sql
-- 사용하지 않는 cagra 인덱스 확인
SELECT indexname, tablename FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%'
ORDER BY tablename;
```

**기대 출력:**
```
  indexname  | tablename
-------------+-----------
 cagra_idx   | items
 cagra_idx2  | logs
```

```sql
-- 불필요한 인덱스 DROP (LRU eviction 트리거)
DROP INDEX cagra_idx2;
```

**기대 출력:**
```
DROP INDEX
```
**-> 성공:** nvidia-smi로 VRAM 여유 재확인 후 CREATE INDEX 재시도  
**-> DROP 후에도 VRAM 부족:** daemon 재시작 (`sudo systemctl restart pg-cuvs-server`) 후 재시도

```sql
-- 긴급 우회: CPU(HNSW) 경로 사용
SET enable_cuvs = off;
CREATE INDEX hnsw_idx ON items USING hnsw (embedding vector_l2_ops);
-- GPU 준비 후: DROP INDEX hnsw_idx; CREATE INDEX USING cagra;
```

---

### Step 3 — 바이너리 재빌드 (BUILD_FAILED/버전 불일치 복구)

```bash
# 로컬에서 VM으로 소스 동기화 (Makefile:L179)
rsync -avz --delete \
    --exclude '.git' --exclude 'src/*.o' --exclude 'src/*.bc' \
    --exclude '*.so' --exclude '.env.gpu' \
    ./ $(GCP_VM):~/pg_cuvs/
```

**기대 출력:**
```
sending incremental file list
...
sent N bytes  received M bytes
```

```bash
# VM에서 재빌드 (Makefile:L188)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && make 2>&1 | tee /tmp/pg_cuvs_build.log"
```

**기대 출력 (마지막 줄):**
```
build complete
```
**-> 빌드 성공:** Step 3-2로  
**-> 빌드 실패 (CUDA 컴파일 오류):** `/tmp/pg_cuvs_build.log` 확인 후 에스컬레이션

```bash
# Step 3-2: VM에서 설치 (Makefile:L193)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && sudo -E make install"
```

**기대 출력:**
```
/bin/install -c pg_cuvs.so $(pkglibdir)/pg_cuvs.so
```
**-> 성공:** CREATE INDEX 재시도 후 Step 5 (검증)로  
**-> 실패 (permission denied):** `sudo -E` 환경 확인

---

### Step 4 — 디스크 공간 확보 (PERSIST_FAILED 복구)

```bash
# .tmp 잔재 제거 (daemon이 정리 못한 경우)
rm -f /tmp/cuvs_indexes/*.tmp
```

**기대 출력:**
```
(출력 없음)
```

```bash
# 디스크 공간 재확인
df -h /tmp/cuvs_indexes/
```

**기대 출력:**
```
Avail 이 1G 이상이어야 함
```
**-> 공간 확보:** CREATE INDEX 재시도 후 Step 5 (검증)로  
**-> 공간 부족 지속:** `/tmp` 전체 정리 또는 `cuvs.index_dir`을 다른 볼륨으로 변경

---

### Step 5 — registry 슬롯 확보 (PERSIST_FAILED / registry full 복구)

```sql
-- 인덱스 수 확인 (64개 초과 시 registry full)
SELECT count(*) FROM pg_indexes WHERE indexdef LIKE '%USING cagra%';
```

**기대 출력:**
```
 count
-------
    64
```

```sql
-- 불필요한 인덱스 DROP
DROP INDEX <index_name>;
```

**기대 출력:**
```
DROP INDEX
```
**-> 성공:** CREATE INDEX 재시도 후 Step 5 (검증)로  
**-> DROP 후에도 registry full:** daemon 재시작 후 재시도 (`sudo systemctl restart pg-cuvs-server`)

---

## 5. 검증 체크리스트

CREATE INDEX 성공 후 아래를 순서대로 확인한다.

```bash
# artifact 쌍 확인
ls -lh /tmp/cuvs_indexes/
```

**기대 출력:**
```
-rw-r--r-- 1 postgres postgres  45M ... 16384_16392.cagra
-rw-r--r-- 1 postgres postgres  12K ... 16384_16392.tids
```
- [ ] `.cagra` + `.tids` 쌍이 존재한다

```bash
# daemon journal에서 빌드 성공 메시지 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -10
```

**기대 출력:**
```
pg_cuvs: built index 16384/16392 (N vecs, M MB VRAM)
```
- [ ] `built index <db_oid>/<index_oid>` 메시지가 있다

```sql
-- 검색 동작 확인 (GPU path 사용 여부)
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력:**
```
NOTICE:  pg_cuvs: cagra scan ...
```
- [ ] `NOTICE: pg_cuvs: cagra scan` 메시지가 있다 (GPU path 사용 중)

---

## 6. Escalation 기준 (When to escalate)

- status=5 (BUILD_FAILED)이고 journal에 CUDA 오류 없이 `cuvs_cagra_build returned NULL`만 있으면:
  cuVS 버전과 빌드 시 헤더 버전 불일치 가능성. `make gpu-build` 재실행 검토.
- status=6 (PERSIST_FAILED)이고 디스크 공간과 권한이 모두 정상인데 계속 실패하면:
  `cuvs_cagra_serialize` 내부 오류. journal의 정확한 errno와 경로를 포함해 에스컬레이션.
- 동일 status가 반복되고 원인이 불명확하면: `cuvs.debug = on` + daemon 로그 전체를 수집해 에스컬레이션.
