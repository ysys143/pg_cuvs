---
name: gpu-vm-provision
description: >
  GCP GPU VM 프로비저닝 전문 스킬 (pg_cuvs 프로젝트 전용). Terraform으로 A100/L4/H100
  인스턴스를 생성하고, CUDA + conda + libcuvs + PostgreSQL + pgvector 환경을 구성하며,
  pg_cuvs 확장 빌드를 검증하는 전체 워크플로우를 다룬다.

  트리거 키워드: "VM 프로비저닝", "VM 올려줘", "GCP GPU VM 생성", "terraform apply",
  "libcuvs 설치", "cuVS 환경", "GPU dev 환경 셋업", "VM 새로 만들자", "fresh provisioning",
  "STOCKOUT", "다른 zone으로", "VM 망가짐", "VM 복구", "디스크 복구", "ldconfig 문제",
  "libstdc++ GLIBCXX", "data dir reset" 등이 나오면 사용.

  포함 시나리오: (1) 처음부터 새 VM 생성, (2) 기존 VM 망가졌을 때 디스크 레스큐로 복구,
  (3) 빌드/install 시 발생하는 conda+CUDA+PG 통합 함정 대응, (4) cuVS 25.x/26.x API 변경 대응.
---

# gpu-vm-provision

pg_cuvs 개발용 GPU VM의 프로비저닝/복구/검증을 다룬다. 시행착오를 통해 검증된 패턴만 포함.

## 아키텍처 한 줄

```
Terraform (GCP A100/L4) → startup script (CUDA driver) →
conda env (libcuvs 26.x) → PG16 + pgvector →
make gpu-cycle (pg_cuvs.so 빌드) → systemd (pg_cuvs_server 데몬)
```

## 워크플로우 선택

| 상황 | 다음 단계 |
|------|----------|
| 처음 VM 만들기 | `references/quick-start.md`로 |
| VM이 망가져 SSH 불가 | `references/disk-recovery.md`로 |
| 빌드/install 실패 | `references/troubleshooting.md`로 |
| cuVS API 변경/SIGSEGV | `references/cuvs-26x-quirks.md`로 |

각 파일은 한 시나리오에 집중. 한꺼번에 다 읽지 말 것.

## 핵심 결정 사항 (변경 금지)

이 결정들은 시행착오로 검증됐다. 변경하려면 reference 파일의 "왜 이렇게 하나" 설명 먼저 읽기.

1. **OS**: Ubuntu 22.04 LTS (jammy). PGDG repo가 22.04용 PG16 안정 제공.
2. **CUDA 설치 경로**: NVIDIA cuda-keyring → `cuda-toolkit-12-4 cuda-drivers` apt 패키지.
3. **libcuvs 설치**: conda env `cuvs_dev`에 `libcuvs=26.04`. **`libcuvs-dev` 패키지는 존재하지 않음** (헤더가 `libcuvs`에 포함).
4. **ldconfig**: conda env의 lib 디렉터리 전체를 `/etc/ld.so.conf.d/`에 등록 **금지**. 시스템 OpenSSL/dbus와 ABI 충돌해서 VM 부팅 불가가 됨. 자세한 건 troubleshooting.md의 "ldconfig 사고" 항목.
5. **libstdc++ 해결**: conda libstdc++.so.6 + libgcc_s.so.1만 `/usr/local/lib/`에 심볼릭 링크. 디렉터리 전체 X.
6. **systemd 데몬 로그**: `StandardOutput=append:/tmp/...` **금지** (status 209/STDOUT). systemd 기본 journal 사용.
7. **CUDA_ARCH**: A100=sm_80, L4=sm_89, H100/H200=sm_90. Terraform `accelerator_type`과 항상 일치시킬 것.

## 첫 부팅 후 즉시 확인할 것

VM이 살아있고 startup script가 완료됐는지:

```bash
ssh ubuntu@<IP> "tail -3 /var/log/pg_cuvs_setup.log"
# 마지막 줄에 "=== pg_cuvs GPU env setup complete ===" 가 있어야 함
```

없으면 startup script 실패. `cat /var/log/pg_cuvs_setup.log`로 어느 단계인지 확인.

## 비용

| 인스턴스 | 시간당 |
|---------|-------|
| g2-standard-4 (L4) | ~$0.85 |
| a2-highgpu-1g (A100-40GB) | ~$3.69 |
| a3-highgpu-1g (H100) | ~$5.95 |
| 정지 시 (디스크만) | ~$0.02 |

작업 후 반드시 `make vm-stop` 또는 `terraform destroy`.
