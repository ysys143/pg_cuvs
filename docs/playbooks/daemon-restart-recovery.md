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

## 2. 확인 명령 (Diagnostic commands)

```bash
# 서비스 상태 확인
sudo systemctl status pg-cuvs-server

# 최근 journal (재시작 전후)
sudo journalctl -u pg-cuvs-server -n 50 --no-pager

# persisted artifact 목록
ls -lh /tmp/cuvs_indexes/
# 또는 cuvs.index_dir 경로가 다른 경우:
psql -d postgres -c "SHOW cuvs.index_dir;"

# 소켓 파일 존재 여부
ls -la /tmp/.s.pg_cuvs

# VRAM 여유 확인 (로드 실패 원인 중 하나)
nvidia-smi --query-gpu=memory.free,memory.total --format=csv,noheader,nounits
```

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

### D. index_dir 경로 불일치
`postgresql.conf`의 `cuvs.index_dir`와 daemon의 `--index-dir` 인수가 다르면
PG는 다른 경로를 가리키고 daemon은 다른 경로에서 파일을 찾는다.

### E. 소켓 권한 문제
systemd unit의 `ExecStartPost`에서 `chmod 666 /tmp/.s.pg_cuvs`가 실행되기 전에
PG backend가 connect를 시도하면 `EACCES`로 `UNAVAILABLE`이 된다.
`sleep 1`이 포함되어 있으나 부하가 높으면 race가 발생할 수 있다.

---

## 4. 복구 절차 (Recovery steps)

### 표준 재시작

```bash
sudo systemctl restart pg-cuvs-server

# 재시작 완료 대기 (loaded index 메시지 확인)
sudo journalctl -u pg-cuvs-server -f --no-pager &
JPID=$!
sleep 5
kill $JPID
```

### stale 소켓 수동 정리 후 재시작

```bash
sudo rm -f /tmp/.s.pg_cuvs
sudo systemctl start pg-cuvs-server
```

### index_dir 경로 동기화 확인

```bash
# daemon 실행 인수 확인
sudo systemctl cat pg-cuvs-server | grep ExecStart

# PG GUC 확인
psql -d postgres -c "SHOW cuvs.index_dir;"
```

두 값이 다르면 systemd unit의 `--index-dir` 인수 또는 `postgresql.conf`의
`cuvs.index_dir`를 일치시킨다. 수정 후 해당 서비스만 재시작하면 된다.

### VRAM 부족으로 일부 인덱스가 로드 안 된 경우

```bash
# journal에서 skip된 인덱스 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep 'skip\|VRAM'
```

skip된 인덱스에 대한 검색은 `CUVS_STATUS_NOT_FOUND`를 반환하고 CPU fallback한다.
VRAM 여유가 생기면 handle_search 내의 lazy load 경로(`load_index` in handle_search)가
동일 세션 내 다음 검색 시 재시도한다.

---

## 5. 검증 명령 (Verification commands)

```bash
# journal에서 reload 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep 'loaded index'
# 예상 출력: pg_cuvs_server: loaded index <db_oid>/<index_oid> (<n> vecs, <N> MB VRAM)

# 서비스 active 확인
sudo systemctl is-active pg-cuvs-server
# 출력: active
```

```sql
-- 재시작 후 heap rebuild 없이 검색이 정상 동작하는지 확인
-- (인덱스 생성 없이 기존 cagra index 사용)
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- 이전과 동일한 결과여야 함

-- GPU path가 사용되었는지 확인 (fallback 없이)
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE: pg_cuvs: cagra scan ... 메시지가 나와야 함
SET cuvs.debug = off;
```

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
