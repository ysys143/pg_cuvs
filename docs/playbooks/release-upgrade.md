# Playbook: 릴리스 및 업그레이드 (release-upgrade)

pg_cuvs 설치/재설치, 데몬 재시작 순서, artifact magic 호환성 확인 절차.
현재 default_version은 **0.1.0** (단일 버전, 미릴리스).
향후 버전 bump 시 `ALTER EXTENSION pg_cuvs UPDATE` 경로를 포함한다.

**현재 상태 한정**: 0.1.0 단일 버전이므로 실제 cross-version upgrade 경로는
**TBD: 첫 릴리스 후 cross-version upgrade 검증 필요**. 이 playbook은
신규 설치/재설치 및 코드 변경 후 재배포 절차를 주로 다룬다.

---

## 1. 증상 (Symptoms)

- 소스 변경 후 `make install`만 했는데 데몬 기능(GCS snapshot, HNSW 사이드카
  직렬화 등)이 반영되지 않는다.
- 재설치 후 검색 쿼리가 `WARNING: pg_cuvs_server unreachable`로 CPU fallback한다.
- 데몬 재시작 없이 `.so` 교체 후 기존 backend 세션이 이전 코드로 동작한다.
- 업그레이드 후 `loaded index` 메시지 없이 데몬이 뜨거나, artifact magic 오류가
  journal에 나온다.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# 설치된 확장 버전
psql -d postgres -c "SELECT name, default_version, installed_version
                     FROM pg_available_extensions WHERE name='pg_cuvs';"
```

**기대 출력:**
```
  name   | default_version | installed_version
---------+-----------------+-------------------
 pg_cuvs | 0.1.0           | 0.1.0
```
**→ `installed_version IS NULL`:** `CREATE EXTENSION pg_cuvs`가 아직 실행되지 않음  
**→ `installed_version != default_version`:** `ALTER EXTENSION pg_cuvs UPDATE` 필요
  (향후 버전 bump 시 — 현재는 둘 다 0.1.0이라 발생하지 않음)

```bash
# 설치된 바이너리/라이브러리 타임스탬프 (재설치가 실제로 적용됐는지 확인)
ls -la $(pg_config --pkglibdir)/pg_cuvs.so
ls -la $(which pg_cuvs_server 2>/dev/null || echo /usr/local/bin/pg_cuvs_server)
```

```bash
# 데몬 현재 상태 및 바이너리 경로
sudo systemctl status pg-cuvs-server --no-pager
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

```bash
# artifact magic/version (reload 실패 시 확인)
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "magic|version|artifact|tids|cagra"
```

---

## 3. 원인 분기 (Cause branches)

### A. `make install`만 실행 — 데몬 바이너리 미갱신
`make install`은 PostgreSQL 확장 `.so`(pg_cuvs.so)와 `.control`/SQL 파일만 설치한다.
`pg_cuvs_server` 데몬 바이너리는 **`make install-server`**로 별도 설치한다.
GCS 업로드/다운로드, HNSW 사이드카 직렬화(`from_cagra()`), sharded snapshot 등
모든 데몬 기능은 이 바이너리에 있다.
→ 복구 Step 1 (install + install-server).

### B. `.so` 교체 후 기존 백엔드 세션이 이전 코드 사용
PostgreSQL 백엔드는 `shared_preload_libraries`로 로드된 `.so`를 프로세스 시작 시
한 번만 dlopen한다. `make install` 후 새 쿼리 세션에도 이전 `.so`가 쓰이려면
PostgreSQL을 재시작(`pg_ctl restart`)해야 한다. 데몬은 별도 프로세스이므로 데몬도
재시작해야 새 바이너리가 적용된다.
→ 복구 Step 2 (올바른 재시작 순서).

### C. artifact magic/version 불일치
각 artifact는 magic 바이트와 version 필드를 파일 헤더에 기록한다:
- `.tids`: magic `'TIDS'` (4바이트) + version 필드
- `.cagra`/`.shards`: 자체 magic 바이트

데몬이 artifact를 reload할 때 magic/version이 현재 코드와 맞지 않으면 해당
인덱스를 건너뛴다(journal에 magic 불일치 메시지). 이 경우 REINDEX로 현재 버전
artifact를 새로 만들어야 한다.

> 현재(0.1.0)는 단일 버전이므로 소스 변경 없이 같은 코드베이스로 재빌드하면
> magic은 동일하다. magic이 바뀌는 경우는 format-breaking 변경이 있는 새 버전에서
> 발생하므로 **첫 릴리스 후 cross-version 시나리오 검증 필요(TBD)**.
→ 복구 Step 3 (REINDEX로 artifact 재생성).

### D. 향후: `ALTER EXTENSION pg_cuvs UPDATE` (버전 bump 시)
`pg_cuvs--0.1.0--0.2.0.sql` 같은 migration script가 있을 때 아래 명령으로 카탈로그를
업데이트한다. 현재(0.1.0 단일 버전)에서는 적용할 migration이 없다.
→ TBD: 첫 릴리스 후 migration script 작성 및 검증 필요.

---

## 4. 복구 절차 (Recovery steps)

### Step 1 — 재설치 (`.so` + 데몬 바이너리)

```bash
# 소스 디렉토리에서 실행
make install          # pg_cuvs.so + .control + SQL 파일
make install-server   # pg_cuvs_server 바이너리
```

**기대 출력:**
```
/bin/install ... pg_cuvs.so
/bin/install ... pg_cuvs_server
```
**→ `make install` 성공 + `make install-server` 성공:** Step 2로  
**→ `make install-server` 명령 없음:** Makefile 버전 확인(Phase 3I 이후 추가됨)

VM 원격 배포 시:
```bash
make sync              # 소스 VM 동기화
make gpu-build         # VM에서 빌드
make gpu-install       # make install 실행
make gpu-server        # pg_cuvs_server 빌드
make gpu-install       # (또는 별도 gpu-server-install 타겟 — Makefile 확인)
```

---

### Step 2 — 올바른 재시작 순서

**순서가 중요하다**: 데몬을 먼저 재시작해 소켓을 준비한 뒤 PostgreSQL을 재시작한다.
PostgreSQL이 `shared_preload_libraries`를 로드할 때 데몬이 이미 listening이면
startup hook에서 연결을 시도해 `pg_stat_gpu_search` 초기화가 성공한다.

```bash
# 1. 데몬 재시작 (새 pg_cuvs_server 바이너리 적용)
sudo systemctl restart pg-cuvs-server

# 2. 데몬이 listening 중인지 확인
sudo journalctl -u pg-cuvs-server --no-pager -n 20 | grep "listening"
# 기대: pg_cuvs_server: listening on /tmp/.s.pg_cuvs

# 3. PostgreSQL 재시작 (.so 교체 반영, 기존 세션 종료)
sudo systemctl restart postgresql
# 또는: pg_ctl restart -D $PGDATA
```

**기대 출력 (데몬 로그):**
```
[INFO] pg_cuvs_server: GPU 0 (NVIDIA A100-SXM4-40GB): 40448 MB total, budget N MB
[INFO] pg_cuvs_server: loaded index <db>/<idx> (N vecs, N MB VRAM)
[INFO] pg_cuvs_server: listening on /tmp/.s.pg_cuvs
```
**→ `loaded index` 없음:** artifact 없거나 magic 불일치 → Step 3  
**→ `no CUDA GPUs detected`:** `gpu-vm-lifecycle.md` (driver mismatch) 참조

---

### Step 3 — artifact magic 불일치 시 REINDEX

```bash
# journal에서 magic/version 불일치 인덱스 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "magic|invalid|version mismatch"
```

해당 인덱스 이름을 찾아:

```sql
-- 현재 버전 artifact로 재생성
REINDEX INDEX <idx>;
```

**기대 출력:**
```
REINDEX
```

```bash
# 재빌드 후 artifact 타임스탬프 갱신 확인
ls -lh /tmp/cuvs_indexes/ | grep -E "\.cagra|\.tids"
```

**기대:** 현재 시각으로 갱신된 파일

---

### Step 4 — 향후 버전 bump 시 (TBD)

> **TBD: 첫 릴리스(0.1.0 → 0.2.0 등) 후 검증 필요.**
> 아래는 절차 템플릿이며 실제 migration script 없이는 실행하지 않는다.

```bash
# 1. 소스에 pg_cuvs--0.1.0--0.2.0.sql migration script가 있는지 확인
ls sql/pg_cuvs--*.sql

# 2. .control 파일의 default_version이 새 버전인지 확인
grep default_version pg_cuvs.control

# 3. 재설치
make install && make install-server

# 4. 데몬 재시작 (새 artifact format 지원)
sudo systemctl restart pg-cuvs-server

# 5. 카탈로그 업데이트
psql -d <dbname> -c "ALTER EXTENSION pg_cuvs UPDATE;"

# 6. PostgreSQL 재시작
sudo systemctl restart postgresql
```

---

## 5. 검증 명령 (Verification commands)

```bash
# 확장 버전 일치
psql -d postgres -c "SELECT installed_version FROM pg_available_extensions
                     WHERE name='pg_cuvs';"
# 기대: 0.1.0
```

```bash
# 데몬 active + loaded index
sudo systemctl is-active pg-cuvs-server
# 기대: active
sudo journalctl -u pg-cuvs-server --no-pager | grep "loaded index"
# 기대: loaded index <db>/<idx> (<N> vecs, <N> MB VRAM) — 1줄 이상
```

```sql
-- GPU 검색 경로 정상 동작
SET enable_cuvs=on; SET enable_seqscan=off;
EXPLAIN (ANALYZE, BUFFERS)
  SELECT id FROM <table> ORDER BY v <-> '[...]'::vector LIMIT 10;
-- 기대: "Custom Scan (CuVSScan)" 또는 NOTICE에 "cagra scan ... gpu"
```

```sql
-- pg_stat_gpu_search 에서 resident + 검색 카운터 증가
SELECT index_name, resident, search_count, last_status
  FROM pg_stat_gpu_search;
-- 기대: resident=t, last_status='ok'
```

- [ ] `installed_version = default_version = 0.1.0`
- [ ] `sudo systemctl is-active pg-cuvs-server` → `active`
- [ ] journalctl에 `loaded index` 1줄 이상
- [ ] `EXPLAIN` 결과에 GPU scan path 확인
- [ ] `pg_stat_gpu_search.resident = t`

> 검증 근거: `daemon-restart-recovery.md` (재시작 후 reload 검증, 소켓/index_dir
> 경로 동기화 확인); BENCHMARK_CROSSOVER.md §16.5 주의사항 (`make install-server`
> 별도 필요 — `make install`만으로는 `.so`만 갱신).

---

## 6. Escalation 기준 (When to escalate)

- `make install-server` 후에도 journal에서 `from_cagra()` / GCS 업로드 기능이 없는
  구버전 메시지: 데몬 바이너리 경로 확인(`which pg_cuvs_server`) — 이전 설치본이
  PATH 앞에 있을 수 있음.
- magic 불일치로 REINDEX를 해도 즉시 재발: 빌드한 `.so`와 `pg_cuvs_server`의
  소스 커밋이 다를 가능성(각각 별도 빌드됨) → 같은 소스 상태에서 `make` 전체 재빌드.
- cross-version 업그레이드 후 `ALTER EXTENSION` 실패(`could not open file pg_cuvs--0.1.0--N.sql`):
  migration script 미포함 → **TBD: migration script 작성 및 검증 필요**.
- PostgreSQL 재시작 후 `shared_preload_libraries`에서 pg_cuvs.so 로드 실패:
  `pg_config --pkglibdir` 경로가 설치 경로와 맞는지 확인.

관련: `daemon-restart-recovery.md`, `persistence-corruption-recovery.md`.  
설계 근거: pg_cuvs.control(default_version=0.1.0), Makefile install / install-server 타겟.
