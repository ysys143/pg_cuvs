# Playbook: 데몬 재시작 및 인덱스 복구

`pg_cuvs_server`를 재시작한 뒤 persisted `.cagra`/`.tids` 쌍이 자동으로
reload되고 heap rebuild 없이 검색이 정상 동작하는지 확인한다.

---

## 1. 증상 (Symptoms)

- `systemctl restart pg-cuvs-server` 후 검색 쿼리가 `WARNING: pg_cuvs_server unreachable`
  또는 CPU fallback으로 빠진다.
- 재시작 후 journal에 `loaded index` 메시지가 없다.
- 재시작 후 첫 검색이 기대값을 반환하지 않는다.
- daemon이 비정상 종료(OOM killer, SIGKILL)된 뒤 소켓 파일이 남아 있다.

---

## 2. 진단

```bash
sudo systemctl status pg-cuvs-server
```

**기대 출력:**
```
● pg-cuvs-server.service - pg_cuvs GPU index server
   Loaded: loaded (...)
   Active: active (running) since ...
```
**→ 정상:** Active: active (running)  
**→ 이상 시:** `failed` 또는 `activating` → journalctl로 원인 확인

---

```bash
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

**기대 출력:**
```
pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)
```
**→ 정상:** `loaded index` 메시지 존재 → Step 3 (검증)으로  
**→ 이상 시:** `loaded index` 없음 → 아래 분기 확인

---

```bash
ls -la /tmp/.s.pg_cuvs
```

**기대 출력:**
```
srwxrwxrwx 1 postgres postgres 0 ... /tmp/.s.pg_cuvs
```
**→ 정상:** 소켓 파일 존재하고 서비스가 running  
**→ 이상 시:** 파일 존재하는데 서비스가 `failed` → stale 소켓 → 원인 C로

---

```bash
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

**기대 출력:**
```
5000, 40960
```
**→ 정상:** free가 인덱스 로드에 충분한 여유  
**→ 이상 시:** free가 매우 낮음 → 원인 B로

---

## 3. 원인 분기 (Cause branches)

### A. .cagra/.tids 파일이 없거나 쌍이 불완전함
`startup_load_indexes`는 `.cagra` 파일을 기준으로 스캔하고 대응하는 `.tids`를 함께 검증한다.
어느 한쪽이 없으면 해당 인덱스를 건너뛴다.
-> persistence-corruption-recovery.md 참조.

### B. VRAM 부족으로 load 스킵
journal에 `VRAM budget exceeded loading` 또는 `insufficient VRAM loading` 메시지가 있다.
-> vram-oom-fallback.md 참조.

### C. stale 소켓 파일로 bind 실패
이전 daemon이 SIGKILL로 죽어 소켓을 정리하지 못한 경우.
systemd unit이 `ExecStartPre`로 소켓을 제거하지 않으면 `bind: address already in use`.
daemon 코드는 `main()` 진입 시 `unlink(g_socket_path)`를 수행하므로 정상 종료 경로에서는
발생하지 않는다. SIGKILL 후 수동 정리가 필요할 수 있다.
→ 복구 Step 2A로

### D. index_dir 경로 불일치
`postgresql.conf`의 `cuvs.index_dir`와 daemon의 `--index-dir` 인수가 다르면
PG는 다른 경로를 가리키고 daemon은 다른 경로에서 파일을 찾는다.
→ 복구 Step 2B로

### E. 소켓 권한 문제
systemd unit의 `ExecStartPost`에서 `chmod 666 /tmp/.s.pg_cuvs`가 실행되기 전에
PG backend가 connect를 시도하면 `EACCES`로 `UNAVAILABLE`이 된다.
`sleep 1`이 포함되어 있으나 부하가 높으면 race가 발생할 수 있다.

---

## 4. Step-by-step 복구

### Step 1 — 표준 재시작

```bash
sudo systemctl restart pg-cuvs-server
```

**기대 출력:**
```
(에러 없이 즉시 완료)
```
**→ 성공:** Step 2로  
**→ 실패:** `Job for pg-cuvs-server.service failed` → journalctl 확인

---

### Step 2 — journal에서 loaded index 확인

```bash
sudo journalctl -u pg-cuvs-server -n 50 --no-pager
```

**기대 출력:**
```
pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)
```
**→ 성공:** `loaded index` 있음 → Step 3 (검증)으로  
**→ 실패:** `loaded index` 없고 stale socket 의심 → Step 2A로  
**→ 실패:** `loaded index` 없고 index_dir 불일치 의심 → Step 2B로  
**→ 실패:** `loaded index` 없고 journal에 VRAM 관련 메시지 → Step 2C로

---

### Step 2A — stale 소켓 수동 정리 후 시작

```bash
ls /tmp/.s.pg_cuvs
```

**기대 출력:**
```
/tmp/.s.pg_cuvs
```

파일이 존재하면:

```bash
sudo rm -f /tmp/.s.pg_cuvs
sudo systemctl start pg-cuvs-server
```

**기대 출력:**
```
(에러 없이 완료)
```
**→ 성공:** Step 2로 돌아가 journal 재확인  
**→ 실패:** 다른 오류 메시지 → Step 2B 또는 journalctl 전체 확인

---

### Step 2B — index_dir 경로 동기화 확인

```bash
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

**기대 출력:**
```
ExecStart=/usr/local/bin/pg_cuvs_server --index-dir /tmp/cuvs_indexes ...
```

```bash
psql -d postgres -c "SHOW cuvs.index_dir;"
```

**기대 출력:**
```
 cuvs.index_dir
----------------
 /tmp/cuvs_indexes
(1 row)
```
**→ 두 값이 일치:** 경로가 원인이 아님 → 다른 분기 확인  
**→ 두 값이 불일치:** systemd unit의 `--index-dir` 또는 `postgresql.conf`의 `cuvs.index_dir`를 일치시킨 뒤 해당 서비스 재시작 → Step 2로 돌아가 journal 재확인

---

### Step 2C — VRAM 부족 확인

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -i 'VRAM\|skip'
```

**기대 출력:**
```
pg_cuvs_server: VRAM budget exceeded loading index ..., skip
```
**→ VRAM 관련 메시지 있음:** vram-oom-fallback.md 참조  
**→ 없음:** 다른 원인 → journalctl 전체 확인 후 Escalation 기준 참조

---

### Step 3 — 검증

```sql
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력:**
```
NOTICE:  pg_cuvs: cagra scan index_oid=XXXXX gpu
 id
----
  1
(1 row)
```
**→ 성공:** NOTICE에 `cagra scan ... gpu` 포함 → 복구 완료  
**→ 실패:** NOTICE 없거나 fallback 메시지 → Step 2 재확인

---

## 5. 검증 체크리스트

- [ ] `sudo systemctl is-active pg-cuvs-server` → 기대 출력: `active`
- [ ] `sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index'` → 기대 출력: `pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)` 1줄 이상
- [ ] `SET cuvs.debug = on; SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;` → 기대 출력: `NOTICE: pg_cuvs: cagra scan index_oid=XXXXX gpu`
- [ ] 이전과 동일한 결과 반환 (heap rebuild 없이 기존 cagra index 사용)

---

## 6. Escalation 기준 (When to escalate)

- 재시작 후 journal에 `loaded index` 메시지가 전혀 없고 `.cagra` 파일도 존재하면:
  persistence-corruption-recovery.md로 이동.
- daemon이 시작 직후 exit하면 (`systemctl is-active` = failed):
  journal 전체를 `sudo journalctl -u pg-cuvs-server --no-pager`로 확인.
  `socket`/`bind`/`CUDA` 오류 여부 확인 후 에스컬레이션.
- 재시작 10회 이상 자동 재시작(`Restart=on-failure`)이 반복되면:
  systemd restart loop를 `sudo systemctl stop pg-cuvs-server`로 멈추고
  원인을 journal에서 분석한 뒤 수동으로 재시작한다.
