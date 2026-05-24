---
name: pg-cuvs-dev
description: >
  pg_cuvs GPU 개발 환경 전문 스킬. GCP L4 GPU VM 프로비저닝(Terraform),
  .env.gpu 설정, Makefile gpu-* 워크플로우, CUDA/cuVS/PG16 환경 함정 대응을 다룬다.
  "VM 올려줘", "gpu-build 안 돼", "cuVS 설치", "conda 환경", "nvcc 에러",
  "make gpu-", ".env.gpu", "infra 프로비저닝", "GCP L4", "CUDA 경로" 키워드가
  나오면 이 스킬을 사용하라.
---

# pg_cuvs GPU 개발 환경 가이드

GCP L4 GPU VM 기반 개발 환경. 로컬 Mac은 코드 편집만, 빌드/테스트는 VM에서 실행 (ADR-004).

## 전체 흐름

```
로컬 Mac (코드 편집)
  │  make sync
  ▼
GCP L4 VM (ubuntu@<IP>:~/pg_cuvs/)
  │  source ~/miniforge3/bin/activate cuvs_dev
  │  make  →  sudo make install  →  make installcheck
  ▼
PostgreSQL 16 (VM 로컬)
  │  CREATE EXTENSION pg_cuvs
  ▼
GPU (NVIDIA L4, sm_89, 24GB VRAM)
```

---

## 1. VM 최초 프로비저닝 (Terraform)

### 사전 조건
```bash
terraform --version   # >= 1.5
gcloud auth application-default login
```

### 프로비저닝
```bash
cd infra/terraform
cp terraform.tfvars.example terraform.tfvars
# terraform.tfvars 편집: project_id, zone 등

terraform init
terraform apply

# 완료 후 .env.gpu 스니펫 출력됨 → 복사해서 .env.gpu에 붙여넣기
terraform output env_gpu_snippet
```

### .env.gpu 설정
```bash
# .env.gpu (gitignored)
GCP_VM=ubuntu@<external_ip>
GCP_INSTANCE=pg-cuvs-dev
GCP_ZONE=asia-northeast3-b
GCP_PROJECT=your-project-id
CONDA_ENV=cuvs_dev
CUDA_ARCH=sm_89   # L4=sm_89, A100=sm_80, H100=sm_90
```

startup script 완료 확인 (약 10~15분 소요):
```bash
ssh ubuntu@<ip> "tail -20 /var/log/pg_cuvs_setup.log"
# 마지막 줄: "=== pg_cuvs GPU env setup complete ==="
```

---

## 2. 일상 워크플로우

| 명령 | 용도 |
|------|------|
| `make vm-start` | VM 시작 (정지 상태에서) |
| `make vm-stop` | VM 정지 (비용 절약) |
| `make sync` | 로컬 → VM rsync (.o/.so/.env.gpu 제외) |
| `make gpu-build` | VM에서 make (nvcc + PGXS) |
| `make gpu-install` | VM에서 sudo make install |
| `make gpu-test` | VM에서 make installcheck |
| `make gpu-shell` | VM SSH 대화형 세션 |
| `make gpu-cycle` | sync → build → install → test 전체 |

---

## 3. 자주 겪는 함정

### A. nvcc가 libcuvs 헤더를 못 찾음
**증상**: `fatal error: cuvs/neighbors/cagra.h: No such file or directory`
**원인**: conda 환경이 활성화되지 않은 상태에서 빌드
**해결**: Makefile의 `gpu-build` 타깃이 `source ~/miniforge3/bin/activate $(CONDA_ENV)` 실행 확인
```bash
ssh $(GCP_VM) "source ~/miniforge3/bin/activate cuvs_dev && echo \$CONDA_PREFIX"
# /home/ubuntu/miniforge3/envs/cuvs_dev 출력되면 정상
```

### B. PGXS가 PostgreSQL 헤더를 못 찾음
**증상**: `pg_config: command not found` 또는 `make: pg_config not found`
**원인**: `/usr/bin/pg_config`가 PATH에 없거나 PG16이 아닌 다른 버전
**해결**:
```bash
ssh $(GCP_VM) "which pg_config && pg_config --version"
# /usr/bin/pg_config, PostgreSQL 16.x 출력되면 정상
```

### C. nvcc와 PG 헤더의 float4 충돌
**증상**: `error: redefinition of 'float4'`
**원인**: .cu 파일에서 PG 헤더를 직접 include한 경우
**해결**: ADR-001 참조. `.c` 파일과 `.cu` 파일을 분리, `cuvs_wrapper.h`의 `extern "C"` 인터페이스만 공유

### D. postmaster가 libcuvs를 찾지 못함
**증상**: `CREATE EXTENSION pg_cuvs` 시 `ERROR: could not load library ... libcuvs.so`
**원인**: conda 환경의 lib 경로가 postmaster 실행 시 LD_LIBRARY_PATH에 없음
**해결**: `SHLIB_LINK`에 `-Wl,-rpath,$(CUVS_LIB)` 추가 (ADR-007 항목)
```makefile
SHLIB_LINK = -L$(CUVS_LIB) -lcuvs -lcudart -lstdc++ -Wl,-rpath,$(CUVS_LIB)
```

### E. gpu-cycle 중 make installcheck 실패
**증상**: `pg_regress: could not connect to the postmaster`
**원인**: PostgreSQL 서비스가 정지 상태이거나 pg_hba.conf trust 설정 누락
**해결**:
```bash
ssh $(GCP_VM) "sudo systemctl status postgresql"
ssh $(GCP_VM) "sudo systemctl start postgresql"
```

### F. sync 후 VM의 .o 파일이 로컬 빌드 결과물로 덮어써짐
**증상**: `make`는 성공하나 nvcc로 빌드된 .o가 아닌 로컬 gcc .o가 링크됨
**원인**: rsync --delete가 VM의 올바른 .o를 삭제
**해결**: Makefile의 sync 타깃 확인 — `--exclude 'src/*.o'` 포함됨

### G-1. 절대 하지 말 것: conda lib을 ldconfig에 등록
**증상**: VM 재부팅 후 sshd/dbus가 죽어서 VM 자체에 접근 불가
**원인**:
```bash
# 이렇게 하면 안 됨
echo "$CONDA_PREFIX/lib" | sudo tee /etc/ld.so.conf.d/cuvs.conf
sudo ldconfig
```
conda env의 lib 디렉터리에는 `libcuvs.so` 외에도 `libssl.so.3`, `libdbus-1.so.3` 같은 시스템 라이브러리가 들어있다. ldconfig에 등록하면 시스템 sshd/dbus가 conda의 (다른 버전) 라이브러리를 잡고 ABI 충돌로 죽음.

**올바른 방법**:
- `-Wl,-rpath,$(CUVS_LIB)` 만 사용 (현재 Makefile에 적용됨)
- postgres가 conda 경로를 traverse할 수 있게 `chmod o+x /home/<user>`만 풀어주면 충분
- 만약 시스템 등록이 꼭 필요하면 특정 .so 파일만 심볼릭 링크:
  ```bash
  sudo ln -s $CONDA_PREFIX/lib/libcuvs.so /usr/local/lib/
  sudo ldconfig
  ```

**복구 방법** (이미 망가졌을 때):
1. VM stop → boot disk detach
2. rescue VM 생성, 망가진 디스크 attach
3. mount + chroot로 ldconfig 캐시 재빌드:
   ```bash
   sudo rm /mnt/broken/etc/ld.so.conf.d/cuvs.conf
   sudo rm /mnt/broken/etc/ld.so.cache
   sudo mount --bind /dev /mnt/broken/dev  # 등
   sudo chroot /mnt/broken ldconfig
   ```
4. disk 분리 → 원래 VM에 boot으로 재attach → start

### G. startup script가 아직 실행 중인데 SSH 접속
**증상**: `sudo make install` 시 `dpkg: error: dpkg status database is locked`
**원인**: apt-get이 백그라운드에서 아직 실행 중
**해결**: `/var/log/pg_cuvs_setup.log` 마지막 줄이 "setup complete"인지 확인 후 진행

---

## 4. 비용 관리

| VM 상태 | 시간당 비용 (g2-standard-4, L4, asia-northeast3) |
|---------|------------------------------------------------|
| 실행 중 | ~$0.85 |
| 정지 | ~$0.003 (디스크만) |

- 작업 후 반드시 `make vm-stop`
- `preemptible = true`로 설정 시 ~70% 절감 (24h 강제 종료)
- 벤치마크 전용 세션은 preemptible 사용, 장기 개발은 non-preemptible

---

## 5. VM 재사용 (Terraform 재적용 없이)

VM이 이미 존재하면 Terraform 없이:
```bash
make vm-start           # 정지된 VM 시작
# 약 60초 후 SSH 가능
make sync               # 최신 코드 동기화
make gpu-build          # 빌드
```

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| `design/DECISIONS.md` ADR-004 | GCP VM 선택 이유 |
| `design/DECISIONS.md` ADR-001 | C/.cu 분리, float4 충돌 |
| `design/DECISIONS.md` ADR-007 | -Wl,-rpath 이슈 |
| `design/SPEC.md` | GEARS 요구사항 전체 |
| `infra/terraform/` | Terraform 설정 |
| `infra/terraform/scripts/install_gpu_env.sh` | VM 스타트업 스크립트 |
