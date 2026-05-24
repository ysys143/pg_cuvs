# Quick Start — 처음부터 VM 프로비저닝

처음 시작하거나 깨끗하게 다시 시작할 때. 검증된 순서.

## 0. 사전 조건 확인

```bash
gcloud auth application-default print-access-token >/dev/null && echo "ADC OK"
terraform --version  # >= 1.5
which gcloud         # GCP CLI
ls ~/.ssh/id_rsa.pub # SSH 공개키
```

## 1. GCP 프로젝트 + GPU quota 확인

```bash
gcloud projects list                          # 적절한 프로젝트 선택
PROJECT=<your-gpu-project>
gcloud config set project $PROJECT

# A100/L4 quota 확인 (us-central1, asia-east1 등)
for region in us-central1 us-east4 asia-east1; do
  echo "=== $region ==="
  gcloud compute regions describe $region --format=json 2>/dev/null | \
    python3 -c "import sys,json; d=json.load(sys.stdin); [print(f\"  {q['metric']}: {q['usage']}/{q['limit']}\") for q in d.get('quotas',[]) if any(k in q['metric'] for k in ['A100','H100','L4'])]"
done
```

**선택 기준**:
- 검증: A100 = 충분, L4 = 비용 우선
- 한국과의 위치는 무관 (CI/배치 용도)
- A100 40GB quota는 일반적으로 기본 1개 제공

## 2. terraform.tfvars 작성

```bash
cd infra/terraform
cp terraform.tfvars.example terraform.tfvars
# 편집:
#   project_id       = "<your-project>"
#   region           = "us-central1"
#   zone             = "us-central1-b"   # us-central1-a STOCKOUT 시 b로
#   machine_type     = "a2-highgpu-1g"   # A100-40GB
#   accelerator_type = "nvidia-tesla-a100"
```

## 3. Apply

```bash
terraform init && terraform apply -auto-approve
# ~30초 안에 VM 생성됨

terraform output env_gpu_snippet > ../../.env.gpu
# .env.gpu에 GCP_VM, GCP_INSTANCE, CUDA_ARCH 등 자동 채워짐
```

**STOCKOUT 오류 시**: tfvars의 zone을 b/c/f로 변경 후 재시도.

## 4. Startup script 완료 대기 (~10-15분)

```bash
IP=$(terraform output -raw external_ip)
ssh-keygen -R $IP  # 이전 호스트키 제거

# SSH 가능해질 때까지 대기
until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no ubuntu@$IP true 2>/dev/null; do sleep 5; done

# 완료 확인 (로그 마지막 줄)
ssh ubuntu@$IP "sudo tail -3 /var/log/pg_cuvs_setup.log"
# "=== pg_cuvs GPU env setup complete ===" 확인
```

설치되는 것들:
- NVIDIA driver 535+ / CUDA toolkit 12.4
- Miniforge3 + cuvs_dev conda env (libcuvs 26.x + cuda-toolkit 12.x + python 3.11)
- PostgreSQL 16 (PGDG repo) + postgresql-server-dev-16
- pgvector 0.8.0 (소스 빌드 + install)

## 5. PG role + 권한 설정

```bash
ssh ubuntu@$IP "
sudo -u postgres createuser -s ubuntu
sudo -u postgres createdb ubuntu
sudo chmod o+x /home/ubuntu
sudo chmod o+x /home/ubuntu/miniforge3
sudo chmod o+x /home/ubuntu/miniforge3/envs
sudo chmod o+x /home/ubuntu/miniforge3/envs/cuvs_dev
sudo chmod o+x /home/ubuntu/miniforge3/envs/cuvs_dev/lib
"
```

postgres 유저가 conda env까지 traverse 가능해야 .so 로드 시 rpath가 동작함.

## 6. libstdc++ + libgcc_s 심볼릭 링크 (핵심)

```bash
ssh ubuntu@$IP "
sudo ln -sf /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libstdc++.so.6 /usr/local/lib/libstdc++.so.6
sudo ln -sf /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libgcc_s.so.1 /usr/local/lib/libgcc_s.so.1
sudo ldconfig
sudo systemctl restart postgresql
"
```

**중요**: 이 두 파일만 링크하라. 디렉터리 전체를 ldconfig에 등록하면 VM이 망가진다 (troubleshooting.md의 "ldconfig 사고" 참조).

## 7. 빌드 + 설치 + 검증

```bash
make sync         # 로컬 → VM rsync
make gpu-build    # nvcc + PGXS 빌드
make gpu-install  # sudo make install
make gpu-server   # 데몬 바이너리 빌드 + 설치

# 검증
ssh ubuntu@$IP "psql -d postgres -c 'CREATE EXTENSION IF NOT EXISTS vector; CREATE EXTENSION pg_cuvs; SELECT amname FROM pg_am WHERE amname=\\\$\\\$cagra\\\$\\\$;'"
# cagra 출력되면 OK
```

## 8. pg_cuvs_server systemd 서비스

```bash
ssh ubuntu@$IP "sudo tee /etc/systemd/system/pg-cuvs-server.service > /dev/null" << 'EOF'
[Unit]
Description=pg_cuvs GPU sidecar daemon
After=network.target

[Service]
Type=simple
User=ubuntu
Group=ubuntu
ExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server --socket /tmp/.s.pg_cuvs --index-dir /tmp/cuvs_indexes --max-vram-mb 20480
ExecStartPost=/bin/sh -c "sleep 1; chmod 666 /tmp/.s.pg_cuvs"
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

ssh ubuntu@$IP "
sudo mkdir -p /tmp/cuvs_indexes
sudo chown ubuntu:ubuntu /tmp/cuvs_indexes
sudo systemctl daemon-reload
sudo systemctl enable pg-cuvs-server
sudo systemctl start pg-cuvs-server
sleep 2
sudo systemctl is-active pg-cuvs-server
ls -la /tmp/.s.pg_cuvs
"
```

`Standard{Output,Error}=` 파일 리다이렉트 **금지**. systemd 기본 journal 사용. (`journalctl -u pg-cuvs-server`로 조회)

## 9. End-to-end 동작 확인

```bash
ssh ubuntu@$IP "psql -d postgres" << 'EOF'
SET cuvs.index_dir = '/tmp/cuvs_indexes';
CREATE TABLE items (id bigint, embedding vector(4));
INSERT INTO items VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]');
CREATE INDEX cagra_idx ON items USING cagra (embedding vector_l2_ops);
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 2;
EOF
# id 1이 첫 번째로 나와야 함

ssh ubuntu@$IP "sudo journalctl -u pg-cuvs-server -n 10 --no-pager"
# "built index ... vecs" 메시지 확인
```

## 10. 정지

작업 끝나면:
```bash
make vm-stop
# 또는 완전히 제거할 때
terraform destroy -auto-approve
```
