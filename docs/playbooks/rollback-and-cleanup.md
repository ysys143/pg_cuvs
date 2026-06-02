# Playbook: Rollback 및 Cleanup

잘못된 배포를 되돌리거나, 확장을 제거하거나, artifact를 안전하게 정리하는 절차.

index artifact(`.cagra`/`.tids`)는 derived data다 — WAL 대상이 아니고 원본 heap 데이터가
PostgreSQL에 존재하므로 언제든 `REINDEX`로 재생성할 수 있다. 삭제해도 데이터 손실 없음.

---

## 1. 증상 (Symptoms)

- 새 버전 배포 후 pg_cuvs.so 로드 실패, IPC 프로토콜 불일치, 또는 CREATE INDEX 동작 이상.
- 구버전으로 되돌려야 하는 상황.
- 개발/테스트 환경을 완전히 초기화해야 하는 상황.
- `cuvs.index_dir` 경로를 변경하거나 artifact를 다른 위치로 이동해야 하는 상황.

---

## 2. 진단

### Step 1 — 현재 설치 버전 및 상태 확인

```bash
# .so 및 바이너리 파일 확인
ls -la $(pg_config --pkglibdir)/pg_cuvs.so
ls -la $(pg_config --bindir)/pg_cuvs_server
```

**기대 출력:**
```
-rwxr-xr-x 1 root root 2.3M ... /usr/lib/postgresql/16/lib/pg_cuvs.so
-rwxr-xr-x 1 root root 8.1M ... /usr/lib/postgresql/16/bin/pg_cuvs_server
```
**-> .so 없음 (`No such file`):** 이미 제거됨 또는 설치 실패  
**-> .so 있음:** Step 1-2로

```bash
# Step 1-2: extension 버전 확인
psql -d postgres -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_cuvs';"
```

**기대 출력:**
```
 extversion
------------
 1.0
```
**-> 버전 확인:** Step 1-3으로  
**-> 0 rows:** extension이 catalog에 없음 (artifact만 남아 있을 수 있음)

```bash
# Step 1-3: daemon 상태 및 artifact 확인
sudo systemctl is-active pg-cuvs-server
ls -lh /tmp/cuvs_indexes/
psql -d postgres -c "SHOW cuvs.index_dir;"
psql -d postgres -c "SHOW shared_preload_libraries;"
```

**기대 출력:**
```
active
total 57M
-rw-r--r-- ... 16384_16392.cagra
-rw-r--r-- ... 16384_16392.tids
 cuvs.index_dir
----------------
 /tmp/cuvs_indexes
 shared_preload_libraries
--------------------------
 pg_cuvs
```
**-> 증상 파악 완료:** 원인 분기(섹션 3)로

---

## 3. 원인 분기 (Cause branches)

### A. .so와 pg_cuvs_server 버전 불일치
IPC 프레임 구조(CuvsCmdFrame, CuvsReplyHeader)가 바뀐 경우 구버전 daemon과 신버전 .so,
또는 그 반대 조합에서 통신 오류가 발생한다.
증상: `ERROR: pg_cuvs: BUILD failed (status 1)` 또는 검색 응답이 쓰레기값.
-> 복구 Step 4 (바이너리 교체)로

### B. 신버전 .so가 구버전 artifact를 읽지 못함
`.tids` 포맷 변경(magic/version 변경) 시 구버전 artifact는 `validation failed`로 skip된다.
-> 복구 Step 1 (daemon 중지) -> Step 3 (artifact 정리) -> Step 6 (재배포) 순으로

### C. postgresql.conf에 잘못된 설정이 남음
rollback 후 `shared_preload_libraries`에 pg_cuvs가 남아 있으면 postmaster 재시작 시
구버전 또는 제거된 .so를 로드하려다 실패할 수 있다.
-> 복구 Step 5 (shared_preload_libraries 제거)로

---

## 4. Step-by-step 복구

### Step 1 — daemon 중지

**선행 조건:** 없음. 가장 먼저 실행한다.

```bash
# SIGTERM으로 정상 종료 — 메모리 상주 인덱스를 disk에 serialize
sudo systemctl stop pg-cuvs-server
```

**기대 출력:**
```
(출력 없음, 즉시 복귀)
```

```bash
sudo journalctl -u pg-cuvs-server --no-pager | tail -5
```

**기대 출력:**
```
pg_cuvs: sigterm: N indexes saved
pg_cuvs: shutdown complete
```
**-> "shutdown complete" 확인:** Step 2로  
**-> daemon이 이미 inactive:** 문제 없음, Step 2로

---

### Step 2 — extension 제거 (PostgreSQL catalog에서)

**선행 조건:** Step 1 완료 (daemon 중지) 불필요. PostgreSQL이 살아 있으면 실행 가능.

```sql
-- cagra 인덱스 목록 확인 (extension DROP 전 선행 필요)
SELECT indexname, tablename FROM pg_indexes WHERE indexdef LIKE '%USING cagra%';
```

**기대 출력:**
```
 indexname  | tablename
------------+-----------
 cagra_idx  | items
```

```sql
-- 인덱스마다 DROP
DROP INDEX cagra_idx;
-- 또는
DROP INDEX CONCURRENTLY cagra_idx;

-- extension 제거
DROP EXTENSION pg_cuvs;
DROP EXTENSION vector;      -- pg_cuvs가 vector 타입을 사용하는 경우만
```

**기대 출력:**
```
DROP INDEX
DROP EXTENSION
DROP EXTENSION
```
**-> 성공:** Step 3으로  
**-> "ERROR: cannot drop index ... because other objects depend on it":** 의존 객체(뷰, 함수 등)를 먼저 제거 후 재시도  
**-> "ERROR: extension does not exist":** 이미 제거됨, Step 3으로

---

### Step 3 — artifact 정리

**선행 조건:** Step 1 (daemon 중지) 완료 후 실행. daemon 실행 중에는 파일이 잠길 수 있다.

```bash
# index_dir의 artifact 전체 제거
rm -rf /tmp/cuvs_indexes/
```

**기대 출력:**
```
(출력 없음)
```

```bash
# 소켓 파일 제거
rm -f /tmp/.s.pg_cuvs
```

**기대 출력:**
```
(출력 없음)
```
**-> 성공:** Step 4로  
**-> "Operation not permitted":** daemon이 아직 실행 중 -> Step 1로 돌아가 daemon 중지 후 재시도

artifact를 삭제하지 않고 다른 경로로 이동하는 경우:

```bash
# 이동 (인덱스 재생성 없이 경로만 변경)
mv /tmp/cuvs_indexes /var/lib/postgresql/16/main/cuvs_indexes
# systemd unit의 --index-dir 와 postgresql.conf의 cuvs.index_dir 를 새 경로로 수정
```

---

### Step 4 — .so 및 바이너리 교체 (이전 버전으로)

**선행 조건:** Step 1 (daemon 중지) 완료. git에서 대상 버전으로 체크아웃 완료.

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
src/pg_cuvs.c
...
sent N bytes  received M bytes
```
**-> 성공:** Step 4-2로  
**-> "Connection refused" / "Host key verification failed":** GCP_VM 환경변수 및 SSH 키 확인

```bash
# Step 4-2: VM에서 재빌드 (Makefile:L188)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && make 2>&1 | tee /tmp/pg_cuvs_build.log"
```

**기대 출력 (마지막 줄):**
```
build complete
```
**-> 빌드 성공:** Step 4-3으로  
**-> 빌드 실패 (CUDA 컴파일 오류):** `cat /tmp/pg_cuvs_build.log` 확인 후 에스컬레이션

```bash
# Step 4-3: VM에서 설치 (Makefile:L193)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && sudo -E make install"
```

**기대 출력:**
```
/bin/install -c pg_cuvs.so $(pkglibdir)/pg_cuvs.so
/bin/install -c pg_cuvs_server $(bindir)/pg_cuvs_server
```

```bash
# Step 4-4: daemon 바이너리도 교체 (Makefile:L203)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && make server && sudo make install-server"
```

**기대 출력:**
```
/bin/install -c pg_cuvs_server $(bindir)/pg_cuvs_server
```
**-> 성공:** 다음 단계는 목적에 따라:
  - 완전 제거가 목적이면 Step 5로
  - 재배포가 목적이면 Step 6으로  
**-> "permission denied":** `sudo -E` 환경 또는 sudoers 확인

---

### Step 5 — shared_preload_libraries에서 제거 (extension 완전 제거 시)

**선행 조건:** Step 2 (extension DROP) 완료.

```sql
-- pg_cuvs를 shared_preload_libraries에서 제거
ALTER SYSTEM SET shared_preload_libraries = '';
-- 또는 다른 preload 항목이 있으면 해당 항목만 남기고 제거
-- 예: ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements';
```

**기대 출력:**
```
ALTER SYSTEM
```

```bash
# PostgreSQL 재시작
sudo systemctl restart postgresql
```

**기대 출력:**
```
(출력 없음)
```

```bash
psql -d postgres -c "SHOW shared_preload_libraries;"
```

**기대 출력:**
```
 shared_preload_libraries
--------------------------

```
**-> pg_cuvs 없음:** 완전 제거 완료 -> Step 5 (검증)로  
**-> pg_cuvs 여전히 있음:** `postgresql.conf` 파일을 직접 편집 (`pg_ctl show`로 경로 확인)  
**-> postmaster 재시작 "FATAL: could not load library":** 제거하지 못한 `.so` 참조 잔재 -> postgresql.conf 직접 편집 후 재시작

---

### Step 6 — 재배포 및 재인덱싱

**선행 조건:** Step 3 (artifact 정리) 및 Step 4 (바이너리 교체) 완료.

```bash
# 로컬에서 VM으로 최신 소스 동기화 (Makefile:L179)
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
# VM에서 빌드 (Makefile:L188)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && make 2>&1 | tee /tmp/pg_cuvs_build.log"
```

**기대 출력:**
```
build complete
```
**-> 빌드 실패:** `/tmp/pg_cuvs_build.log` 확인 후 에스컬레이션

```bash
# 설치 (Makefile:L193)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && sudo -E make install"
```

**기대 출력:**
```
/bin/install -c pg_cuvs.so $(pkglibdir)/pg_cuvs.so
```

```bash
# daemon 바이너리 교체 + 서비스 등록 (Makefile:L203)
ssh -tt $(GCP_VM) "cd ~/pg_cuvs && source ~/miniforge3/bin/activate $(CONDA_ENV) && make server && sudo make install-server"
```

**기대 출력:**
```
/bin/install -c pg_cuvs_server $(bindir)/pg_cuvs_server
```

```bash
# postinstall: shared_preload_libraries 재설정 포함 (Makefile:L212)
CONDA_ENV=$(CONDA_ENV) ssh $(GCP_VM) "CONDA_ENV=$(CONDA_ENV) bash -s" < infra/scripts/postinstall.sh
```

**기대 출력:**
```
postinstall: shared_preload_libraries configured
postinstall: complete
```
**-> 성공:** SQL로 extension 재설치 및 인덱스 재생성

```sql
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION pg_cuvs;

-- 인덱스 재생성 (heap 데이터는 그대로)
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
```

**기대 출력:**
```
CREATE EXTENSION
CREATE EXTENSION
CREATE INDEX
```
**-> 성공:** Step 5 (검증)로  
**-> "ERROR: pg_cuvs: BUILD failed (status 4)":** daemon 미구동 -> `sudo systemctl start pg-cuvs-server` 후 재시도

---

## 5. 검증 체크리스트

```bash
# extension 제거 여부 확인 (완전 제거 시)
psql -d postgres -c "SELECT extname FROM pg_extension WHERE extname = 'pg_cuvs';"
```

**기대 출력:**
```
 extname
---------
(0 rows)
```
- [ ] extension이 catalog에 없다

```bash
# artifact 제거 여부 확인
ls /tmp/cuvs_indexes/ 2>/dev/null || echo "directory gone or empty"
```

**기대 출력:**
```
directory gone or empty
```
- [ ] artifact 디렉토리가 비어 있거나 없다

```bash
# 재설치 후 smoke test
psql -d postgres -c "SELECT amname FROM pg_am WHERE amname = 'cagra';"
sudo systemctl is-active pg-cuvs-server
```

**기대 출력:**
```
 amname
--------
 cagra
(1 row)

active
```
- [ ] `cagra` access method가 등록되어 있다
- [ ] daemon이 active 상태다

```sql
-- 재인덱싱 후 검색 동작 확인
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

- `DROP EXTENSION pg_cuvs`가 `ERROR: extension "pg_cuvs" does not exist`가 아닌
  다른 오류로 실패하면: `pg_depend`에 잔여 의존이 있는 것. `DROP INDEX`로 의존 객체를
  먼저 제거한 뒤 재시도.
- `shared_preload_libraries` 변경 후 postmaster 재시작이 `FATAL: could not load library`로
  실패하면: 제거하지 못한 `.so` 참조가 남아 있는 것. postgresql.conf 또는
  `pg_ctl show`로 설정 파일 경로를 확인하고 직접 편집한다.
- artifact 삭제 후 `REINDEX`가 `ERROR: pg_cuvs: BUILD failed (status 4)`로 실패하면:
  daemon이 아직 기동되지 않은 것. daemon을 먼저 시작하고 재시도.
