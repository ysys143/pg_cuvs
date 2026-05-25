# Playbook: GPU VM 빌드 및 테스트

GPU VM에서 pg_cuvs를 처음 빌드하거나 재빌드할 때 사용한다.
정상 순서: `sync` -> `gpu-build` -> `gpu-install` -> `gpu-server` -> `gpu-postinstall` -> `gpu-test`.

---

## 1. 증상 (Symptoms)

- 로컬 코드 변경 후 VM에서 이전 바이너리가 실행된다.
- `make gpu-build`가 컴파일 오류로 실패한다.
- `make gpu-test`의 `installcheck`가 실패한다.
- `CREATE EXTENSION pg_cuvs`가 `could not load library` 오류를 낸다.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# VM 상태 확인
make vm-start          # VM이 꺼져 있을 때

# 빌드 로그 확인 (VM에서)
ssh $GCP_VM "tail -50 /tmp/pg_cuvs_build.log"

# 설치된 .so 확인
ssh $GCP_VM "ls -la \$(pg_config --pkglibdir)/pg_cuvs.so"

# conda 환경 확인
ssh $GCP_VM "source ~/miniforge3/bin/activate ${CONDA_ENV} && echo \$CONDA_PREFIX && ls \$CONDA_PREFIX/lib/libcuvs.so"

# rpath 확인
ssh $GCP_VM "source ~/miniforge3/bin/activate ${CONDA_ENV} && \
  objdump -x \$(pg_config --pkglibdir)/pg_cuvs.so | grep RPATH"
```

---

## 3. 원인 분기 (Cause branches)

### A. conda 환경이 활성화되지 않음
`CONDA_PREFIX`가 비어 있어 `CUVS_INCLUDE`/`CUVS_LIB`이 해결되지 않는다.
증상: `#include <cuvs/neighbors/cagra.hpp> not found` 또는 `-lcuvs` 링크 실패.

### B. rpath가 없어 postmaster가 libcuvs.so를 못 찾음
`shared_preload_libraries = 'pg_cuvs'` 재시작 시 `libcuvs.so: cannot open shared object file`.
`SHLIB_LINK`의 `-Wl,-rpath,$(CUVS_LIB)` 누락이 원인.

### C. conda env lib 전체를 ldconfig에 등록한 경우 (심각)
VM이 부팅 불능 또는 `/usr/bin/python3` 같은 시스템 바이너리가 동작하지 않는다.
`libstdc++.so.6` + `libgcc_s.so.1` 두 파일만 `/usr/local/lib`에 심볼릭 링크해야 한다.
복구: `design/` 디렉터리의 troubleshooting.md "ldconfig 사고" 항목 참조.

### D. pg_cuvs_server 바이너리가 오래됨
`gpu-server` 단계를 건너뛰어 새 IPC 프로토콜과 구버전 daemon이 불일치한다.

### E. shared_preload_libraries 미설정
`make gpu-postinstall`이 아직 실행되지 않아 첫 쿼리 planning이 ~95ms 소요된다 (ADR-018).

---

## 4. 복구 절차 (Recovery steps)

### 표준 전체 빌드 순서

```bash
# 1. 로컬 -> VM 동기화
make sync

# 2. VM에서 빌드 (.o 파일은 rsync 제외 대상이므로 VM에서 클린 빌드)
make gpu-build

# 3. PG 확장 설치 (sudo make install)
make gpu-install

# 4. pg_cuvs_server 바이너리 빌드 + 설치
make gpu-server

# 5. post-install 설정 (shared_preload_libraries, libstdc++ 심링크, PG restart)
make gpu-postinstall

# 6. 회귀 테스트
make gpu-test
```

### conda 환경 문제 수동 확인

```bash
ssh -tt $GCP_VM "source ~/miniforge3/bin/activate ${CONDA_ENV} && \
  echo CONDA_PREFIX=\$CONDA_PREFIX && \
  ls \$CONDA_PREFIX/lib/libcuvs.so && \
  echo OK"
```

`CONDA_ENV` 변수는 로컬의 `.env.gpu`에 정의되어 있어야 한다.

### shared_preload_libraries 수동 확인 / 재적용

```bash
# postinstall 재실행 (idempotent)
make gpu-postinstall

# 확인
ssh $GCP_VM "psql -d postgres -c 'SHOW shared_preload_libraries;'"
# 출력에 pg_cuvs 포함 여부 확인
```

---

## 5. 검증 명령 (Verification commands)

```bash
# installcheck (smoke + cpu_fallback 회귀 테스트)
make gpu-test

# E2E 수동 smoke test
ssh $GCP_VM "psql -d postgres" << 'EOF'
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SELECT amname FROM pg_am WHERE amname = 'cagra';
CREATE TABLE _smoke (id int, v vector(4));
INSERT INTO _smoke VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]');
CREATE INDEX ON _smoke USING cagra (v vector_l2_ops);
SELECT id FROM _smoke ORDER BY v <-> '[1,0,0,0]'::vector LIMIT 1;
DROP TABLE _smoke;
EOF

# daemon 로그 확인
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server -n 20 --no-pager"
# "built index ... vecs" 메시지 확인
```

---

## 6. Escalation 기준 (When to escalate)

- `make gpu-postinstall` 후에도 `libcuvs.so: cannot open` 오류가 계속되면: rpath 누락 여부를 `objdump -x`로 확인한 뒤 Makefile의 `SHLIB_LINK` 점검.
- conda env lib을 ldconfig에 등록했다가 VM이 불안정해진 경우: VM 스냅샷이 없다면 `terraform destroy && terraform apply`로 VM 재생성이 가장 빠르다.
- `make gpu-test`의 regression diff가 IPC 관련 오류를 보이면: daemon이 실행 중인지 먼저 확인 (`systemctl is-active pg-cuvs-server`).
