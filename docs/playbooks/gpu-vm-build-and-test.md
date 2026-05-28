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

## 0. 전제 조건 — `.env.gpu` 설정

모든 `make gpu-*` 래퍼와 직접 명령은 아래 변수들을 사용한다.  
`make`는 Makefile의 `-include .env.gpu` + `export`로 자동 로드하지만,  
**직접 명령을 칠 때는 반드시 먼저 `source .env.gpu`를 실행해야 한다.**

### `.env.gpu` 파일 만들기 (최초 1회)

프로젝트 루트에 `.env.gpu` 파일을 생성한다 (`.gitignore` 대상 — 커밋하지 않는다):

```bash
# pg_cuvs 프로젝트 루트에서
cat > .env.gpu << 'EOF'
GCP_VM=ubuntu@<외부IP>          # VM 외부 IP (stop/start 시 바뀜 — gpu-vm-lifecycle.md 참조)
GCP_INSTANCE=pg-cuvs-dev        # gcloud 인스턴스 이름
GCP_ZONE=us-central1-b          # 인스턴스가 있는 zone
GCP_PROJECT=gpu-experiment-wdl-2026
CONDA_ENV=rapids                 # VM의 conda 환경 이름
CUDA_ARCH=sm_80                  # A100 = sm_80, A10 = sm_86
EOF
```

> `GCP_VM`의 IP는 VM을 stop/start 할 때마다 바뀐다(ephemeral IP).  
> IP 확인: `gcloud compute instances describe $GCP_INSTANCE --zone $GCP_ZONE --project $GCP_PROJECT --format='get(networkInterfaces[0].accessConfigs[0].natIP)'`

### SSH 접속 설정 (최초 1회)

`make gpu-*` 래퍼들은 내부적으로 `ssh $GCP_VM ...` 으로 VM에 접속한다.  
처음 사용하는 경우 SSH 키를 GCP에 등록해야 한다.

```bash
# gcloud로 SSH 키 자동 등록 + 접속 테스트
gcloud compute ssh $GCP_INSTANCE --zone $GCP_ZONE --project $GCP_PROJECT

# 성공하면 VM 셸이 열린다. exit로 빠져나온다.
# 이후 일반 ssh도 동작:
ssh $GCP_VM "echo connected"
```

**기대 출력:**
```
connected
```
**-> `Permission denied (publickey)`:** SSH 키가 등록되지 않음 — `gcloud compute ssh` 로 키 등록 먼저  
**-> `Connection refused` / `No route to host`:** VM이 꺼져 있음 — Step 0으로  
**-> `Host key verification failed`:** VM IP가 바뀜 — `ssh-keygen -R <old_IP>` 후 재시도

### 직접 명령 실행 전 환경 변수 로드

```bash
cd ~/Documents/GitHub/pg_cuvs
source .env.gpu

# 설정 확인
echo "VM=$GCP_VM  INSTANCE=$GCP_INSTANCE  ZONE=$GCP_ZONE  CONDA=$CONDA_ENV"
```

**기대 출력:**
```
VM=ubuntu@35.224.x.x  INSTANCE=pg-cuvs-dev  ZONE=us-central1-b  CONDA=rapids
```
**→ 빈 값 있음:** `.env.gpu` 파일이 없거나 경로가 다름 — 위의 파일 생성 단계 수행  
**→ `GCP_VM`의 IP가 틀림:** VM stop/start 후 IP 변경 → `gpu-vm-lifecycle.md` "IP 변경 후"로

---

## 2. 진단

### VM 상태 확인

VM이 꺼진 경우 먼저 기동한다.

```bash
# make vm-start 가 실제로 하는 것 (Makefile:166):
gcloud compute instances start $GCP_INSTANCE --zone $GCP_ZONE
# SSH가 응답할 때까지 3초 간격으로 폴링
until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no $GCP_VM true 2>/dev/null; do sleep 3; done
echo "VM ready: $GCP_VM"

# 또는 래퍼:
make vm-start
```

**기대 출력:**
```
VM ready: ubuntu@35.224.x.x
```
**-> 정상:** 진단 2로  
**-> timeout 무한 반복:** IP가 변경됐을 가능성 → `gpu-vm-lifecycle.md` "IP 변경 후" Step으로

---

### conda 환경 확인

```bash
ssh $GCP_VM "source ~/miniforge3/bin/activate ${CONDA_ENV} && \
  echo CONDA_PREFIX=\$CONDA_PREFIX && \
  ls \$CONDA_PREFIX/lib/libcuvs.so && \
  echo OK"
```

**기대 출력:**
```
CONDA_PREFIX=/home/ubuntu/miniforge3/envs/rapids
/home/ubuntu/miniforge3/envs/rapids/lib/libcuvs.so
OK
```
**-> 정상:** 진단 3으로  
**-> `CONDA_PREFIX=` (빈 값):** 원인 A  
**-> `No such file or directory` (libcuvs.so 없음):** 원인 A

---

### rpath 확인

```bash
ssh $GCP_VM "source ~/miniforge3/bin/activate ${CONDA_ENV} && \
  objdump -x \$(pg_config --pkglibdir)/pg_cuvs.so | grep RPATH"
```

**기대 출력:**
```
  RPATH                /home/ubuntu/miniforge3/envs/rapids/lib
```
**-> 정상:** 진단 4로  
**-> 출력 없음:** 원인 B → Step 1(sync)부터 전체 재빌드 필요

---

### 빌드 로그 확인

빌드 실패 시 VM의 로그를 확인한다.

```bash
ssh $GCP_VM "tail -50 /tmp/pg_cuvs_build.log"
```

**기대 출력:**
```
gcc ... -o pg_cuvs.so
```
**-> `#include <cuvs/neighbors/cagra.hpp>` not found:** 원인 A  
**-> `-lcuvs` 링크 실패:** 원인 A  
**-> `libcuvs.so: cannot open shared object file` (런타임):** 원인 B

---

## 3. 원인 분기 (Cause branches)

### A. conda 환경이 활성화되지 않음
`CONDA_PREFIX`가 비어 있어 `CUVS_INCLUDE`/`CUVS_LIB`이 해결되지 않는다.
증상: `#include <cuvs/neighbors/cagra.hpp> not found` 또는 `-lcuvs` 링크 실패.
-> 복구 Step 1(sync)부터 전체 빌드 순서 수행

### B. rpath가 없어 postmaster가 libcuvs.so를 못 찾음
`shared_preload_libraries = 'pg_cuvs'` 재시작 시 `libcuvs.so: cannot open shared object file`.
`SHLIB_LINK`의 `-Wl,-rpath,$(CUVS_LIB)` 누락이 원인.
-> 복구 Step 1부터 전체 재빌드 수행

### C. conda env lib 전체를 ldconfig에 등록한 경우 (심각)
VM이 부팅 불능 또는 `/usr/bin/python3` 같은 시스템 바이너리가 동작하지 않는다.
`libstdc++.so.6` + `libgcc_s.so.1` 두 파일만 `/usr/local/lib`에 심볼릭 링크해야 한다.
복구: `design/` 디렉터리의 troubleshooting.md "ldconfig 사고" 항목 참조.
-> Escalation 항목으로

### D. pg_cuvs_server 바이너리가 오래됨
`gpu-server` 단계를 건너뛰어 새 IPC 프로토콜과 구버전 daemon이 불일치한다.
-> 복구 Step 4로

### E. shared_preload_libraries 미설정
`make gpu-postinstall`이 아직 실행되지 않아 첫 쿼리 planning이 ~95ms 소요된다 (ADR-018).
-> 복구 Step 5로

---

## 4. Step-by-step 복구

### Step 0 (선택) — VM 기동

VM이 TERMINATED 상태일 때만 실행한다.

```bash
# make vm-start 가 실제로 하는 것 (Makefile:166):
gcloud compute instances start $GCP_INSTANCE --zone $GCP_ZONE
until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no $GCP_VM true 2>/dev/null; do sleep 3; done
echo "VM ready: $GCP_VM"

# 또는 래퍼:
make vm-start
```

**기대 출력:**
```
VM ready: ubuntu@35.224.x.x
```
**-> 성공:** Step 1로  
**-> SSH 폴링이 1분 이상 반복:** IP 변경 → `gpu-vm-lifecycle.md` "IP 변경 후"로  
**-> `ERROR: set GCP_INSTANCE in .env.gpu`:** `.env.gpu` 파일에 `GCP_INSTANCE` 설정 필요

---

### Step 1 — 로컬 소스 동기화

로컬 소스를 VM에 미러링한다. **소스는 로컬이 정본, 바이너리는 VM이 정본** 원칙에 따라
빌드 결과물은 제외한다.

```bash
# make sync 가 실제로 하는 것 (Makefile:179):
rsync -avz --delete \
    --exclude '.git' \       # git 메타데이터 — VM에 불필요
    --exclude 'src/*.o' \    # VM에서 nvcc로 컴파일된 오브젝트 — 로컬 것으로 덮으면 GPU 아키텍처 불일치
    --exclude 'src/*.bc' \   # LLVM 비트코드(PG JIT용) — 마찬가지
    --exclude '*.so' \       # 빌드 결과물 — sync 방향이 로컬→VM이므로 제외
    --exclude '.env.gpu' \   # GCP 자격증명 포함 — VM에 올리면 안 됨
    ./ $GCP_VM:~/pg_cuvs/
# --delete: 로컬에서 삭제한 파일은 VM에서도 삭제 (파일명 변경 시 VM에 구버전 좀비 방지)

# 또는 래퍼:
make sync
```

**기대 출력:**
```
sending incremental file list
...
sent N bytes  received M bytes  ...
```
**-> 성공:** Step 2로  
**-> `ssh: connect to host ... Connection refused`:** Step 0으로 (VM이 꺼짐)  
**-> `rsync: [sender] change_dir "/..." failed`:** 로컬 경로 확인

---

### Step 2 — GPU VM에서 빌드

conda 환경 활성화 후 `make`를 실행하고 로그를 `/tmp/pg_cuvs_build.log`에 저장한다.

```bash
# make gpu-build 가 실제로 하는 것 (Makefile:188):
ssh -tt $GCP_VM "cd ~/pg_cuvs && \
    source ~/miniforge3/bin/activate $CONDA_ENV && \
    make 2>&1 | tee /tmp/pg_cuvs_build.log"

# 또는 래퍼:
make gpu-build
```

**기대 출력 (마지막 줄):**
```
gcc ... -shared -o pg_cuvs.so ...
```
**-> 성공:** Step 3으로  
**-> `cagra.hpp: No such file or directory`:** 원인 A — `$CONDA_ENV` 환경변수가 `.env.gpu`에 있는지 확인  
**-> `-lcuvs: not found`:** 원인 A — conda env 내 libcuvs.so 존재 여부 확인  
**-> 기타 컴파일 오류:** `ssh $GCP_VM "tail -50 /tmp/pg_cuvs_build.log"` 로 상세 확인

---

### Step 3 — PG 확장 설치

```bash
# make gpu-install 가 실제로 하는 것 (Makefile:193):
ssh -tt $GCP_VM "cd ~/pg_cuvs && \
    source ~/miniforge3/bin/activate $CONDA_ENV && \
    sudo -E make install"

# 또는 래퍼:
make gpu-install
```

**기대 출력:**
```
/usr/bin/install ... pg_cuvs.so '/usr/lib/postgresql/.../lib'
/usr/bin/install ... pg_cuvs.control '/usr/share/postgresql/.../extension/'
```
**-> 성공:** Step 4로  
**-> `sudo: make: command not found`:** `sudo -E`가 PATH를 승계하지 못함 — VM에서 `/etc/sudoers`의 `secure_path` 확인  
**-> `Permission denied`:** VM의 PG 설치 경로 권한 문제

---

### Step 4 — pg_cuvs_server 바이너리 빌드 + 설치

```bash
# make gpu-server 가 실제로 하는 것 (Makefile:203):
ssh -tt $GCP_VM "cd ~/pg_cuvs && \
    source ~/miniforge3/bin/activate $CONDA_ENV && \
    make server && sudo make install-server"

# 또는 래퍼:
make gpu-server
```

**기대 출력:**
```
gcc ... -o pg_cuvs_server ...
install ... pg_cuvs_server '/usr/local/bin/'
```
**-> 성공:** Step 5로  
**-> 컴파일 오류:** Step 2 실패와 동일하게 빌드 로그 확인  
**-> `install-server: No rule to make target`:** Makefile에 `install-server` 타겟 없음 — 코드 확인 필요

---

### Step 5 — post-install 설정

`shared_preload_libraries` 추가, `libstdc++` 심볼릭 링크, PG 재시작을 수행한다. idempotent.

```bash
# make gpu-postinstall 가 실제로 하는 것 (Makefile:212):
CONDA_ENV=$CONDA_ENV ssh $GCP_VM "CONDA_ENV=$CONDA_ENV bash -s" \
    < infra/scripts/postinstall.sh

# 또는 래퍼:
make gpu-postinstall
```

**기대 출력:**
```
shared_preload_libraries ... pg_cuvs
restarting postgresql ...
```
**-> 성공:** Step 6으로  
**-> `bash -s` 에서 스크립트 미실행:** `infra/scripts/postinstall.sh` 파일 존재 여부 확인  
**-> PG restart 실패:** `ssh $GCP_VM "sudo systemctl status postgresql"` 로 상세 확인

설정 확인:
```bash
ssh $GCP_VM "psql -d postgres -c 'SHOW shared_preload_libraries;'"
# pg_cuvs 포함 여부 확인
```

---

### Step 6 — 회귀 테스트

```bash
# make gpu-test 가 실제로 하는 것 (Makefile:198):
ssh -tt $GCP_VM "cd ~/pg_cuvs && \
    source ~/miniforge3/bin/activate $CONDA_ENV && \
    make installcheck"

# 또는 래퍼:
make gpu-test
```

**기대 출력:**
```
...
All N tests passed.
```
**-> 성공:** 빌드 완료  
**-> IPC 관련 오류 (`pg_cuvs_server` 응답 없음):** daemon 상태 확인 → `ssh $GCP_VM "sudo systemctl is-active pg-cuvs-server"` → 비활성이면 `sudo systemctl start pg-cuvs-server`  
**-> regression diff 존재:** 예상 출력과 실제 출력 비교 — `results/` vs `expected/` 확인

---

## 5. 검증 체크리스트

- [ ] `make gpu-test` — `All N tests passed.` 확인

```bash
make gpu-test
```

- [ ] 설치된 .so 확인

```bash
ssh $GCP_VM "ls -la \$(pg_config --pkglibdir)/pg_cuvs.so"
```
**기대 출력:** `-rwxr-xr-x 1 root root ... pg_cuvs.so`

- [ ] shared_preload_libraries 설정 확인

```bash
ssh $GCP_VM "psql -d postgres -c 'SHOW shared_preload_libraries;'"
```
**기대 출력:** `pg_cuvs` 포함

- [ ] E2E smoke test

```bash
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
```
**기대 출력:** `SELECT amname` -> `cagra`, 마지막 SELECT -> `1`

- [ ] daemon 로그 확인

```bash
ssh $GCP_VM "sudo journalctl -u pg-cuvs-server -n 20 --no-pager"
```
**기대 출력:** `built index ... vecs` 메시지 포함

---

## 6. Escalation 기준 (When to escalate)

- `make gpu-postinstall` 후에도 `libcuvs.so: cannot open` 오류가 계속되면: rpath 누락 여부를 `objdump -x`로 확인한 뒤 Makefile의 `SHLIB_LINK` 점검.
- conda env lib을 ldconfig에 등록했다가 VM이 불안정해진 경우: VM 스냅샷이 없다면 `terraform destroy && terraform apply`로 VM 재생성이 가장 빠르다.
- `make gpu-test`의 regression diff가 IPC 관련 오류를 보이면: daemon이 실행 중인지 먼저 확인 (`systemctl is-active pg-cuvs-server`).
