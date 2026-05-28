# Playbook: GPU VM 생애주기 및 복구 (provisioning / stop-start / recovery)

pg_cuvs의 GPU VM(개발용 `pg-cuvs-dev` 1×A100, 멀티 GPU 수용 `pg-cuvs-dev-mgpu`
2×A100)을 start/stop/reset 할 때 발생하는 운영 함정과 복구 절차. **둘 다 GCP
project `gpu-experiment-wdl-2026`에 있다** — gcloud 명령에 항상
`--project gpu-experiment-wdl-2026`를 붙인다(기본 project가 다름).

---

## 1. 증상 (Symptoms)

- VM stop/start 후 `make sync`/`ssh`가 **이전 IP로 접속 실패**(timeout/no route).
- VM stop/start 후 데몬이 `[ERROR] pg_cuvs_server: no CUDA GPUs detected`로 죽는다.
- `nvidia-smi`가 `Failed to initialize NVML: Driver/library version mismatch`.
- 재시작 후 `/tmp/cuvs_indexes`가 비어 있다(인덱스 artifact가 사라짐).
- GCS snapshot이 "동작 안 함"(업로드/다운로드 실패) — VM에 service account 없음.

---

## 2. 진단

### 현재 외부 IP 확인

stop/start 시 ephemeral IP가 바뀐다. `.env.gpu`의 `GCP_VM`이 구 IP를 가리키는지 확인한다.

```bash
PROJ=gpu-experiment-wdl-2026

# 현재 VM에 할당된 외부 IP
gcloud compute instances describe pg-cuvs-dev \
  --zone=us-central1-b \
  --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)'
```

**기대 출력:**
```
35.224.130.40
```
**-> 출력된 IP == `.env.gpu`의 `GCP_VM` 뒤 IP:** 다음 진단으로  
**-> IP가 다름:** 원인 A -> Step 1로

---

### VM 상태 및 service account 확인

```bash
PROJ=gpu-experiment-wdl-2026

gcloud compute instances describe pg-cuvs-dev \
  --zone=us-central1-b \
  --project=$PROJ \
  --format='value(status, serviceAccounts[0].email)'
```

**기대 출력:**
```
RUNNING	gpu-exp-may@gpu-experiment-wdl-2026.iam.gserviceaccount.com
```
**-> `RUNNING  ` (SA 이메일 없음):** 원인 D -> Step 4로  
**-> `TERMINATED`:** VM 기동 필요 — `gcloud compute instances start pg-cuvs-dev --zone=us-central1-b --project=$PROJ`  
**-> SA 이메일 정상:** 다음 진단으로

---

### GPU 드라이버 정상 여부 확인

```bash
# IP를 먼저 확인한 뒤 대입
NEWIP=$(gcloud compute instances describe pg-cuvs-dev \
  --zone=us-central1-b \
  --project=gpu-experiment-wdl-2026 \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')

ssh ubuntu@$NEWIP 'nvidia-smi | head -3'
```

**기대 출력:**
```
+-----------------------------------------------------------------------------+
| NVIDIA-SMI 525.x.x    Driver Version: 525.x.x    CUDA Version: 12.x        |
|-------------------------------+----------------------+----------------------+
```
**-> 정상 (Driver Version == CUDA Version 일치):** 다음 진단으로  
**-> `Failed to initialize NVML: Driver/library version mismatch`:** 원인 B -> Step 2로

---

### 데몬 GPU 감지 로그 확인

```bash
ssh ubuntu@$NEWIP \
  'sudo journalctl -u pg-cuvs-server --no-pager -n 20 | grep -iE "GPU|CUDA"'
```

**기대 출력:**
```
GPU 0 (NVIDIA A100-SXM4-40GB): 40465 MB total
listening on /tmp/.s.pg_cuvs
```
**-> `no CUDA GPUs detected`:** 원인 B -> Step 2로  
**-> 로그 없음 (서비스 미시작):** `sudo systemctl start pg-cuvs-server`

---

## 3. 원인 분기 (Cause branches)

### A. Ephemeral 외부 IP 변경
`pg-cuvs-dev`/`pg-cuvs-dev-mgpu`는 정적 IP가 아니다. stop/start 시 외부 IP가
바뀐다(예: 이번 세션 `35.224.130.40` -> `104.197.150.30`). `.env.gpu`의 `GCP_VM`은
이전 IP를 가리키므로 `make sync`/`gpu-*`가 죽은 호스트로 붙는다.
-> 복구 Step 1로

### B. NVIDIA driver/library version mismatch (stop/start의 주 함정)
stop/start 후 userspace NVML 라이브러리와 로드된 커널 모듈 버전이 어긋나
`nvidia-smi`가 NVML 초기화에 실패하고, 데몬은 `cuvs_detect_gpus`에서 0개를 보고
`no CUDA GPUs detected`로 종료한다. **reboot(reset)으로 일치하는 커널 모듈을 다시
로드**하면 해소된다.
-> 복구 Step 2로

### C. `/tmp` 휘발
`/tmp/cuvs_indexes`는 stop/start/reset 시 비워진다. 로컬 artifact가 사라지므로
인덱스는 (GCS snapshot이 있으면) warmup으로 다시 받거나 REINDEX가 필요하다.
-> 복구 Step 3으로

### D. Service account 부재 -> GCS 인증 불가
`pg-cuvs-dev`는 기본적으로 SA가 부착돼 있지 않았다. 데몬의 GCS 클라이언트는 instance
metadata에서 토큰을 못 받고 `cuvs.gcs_key_file`도 비어 있으면 GCS 업로드/다운로드가
전부 실패한다(-> `gcs-snapshot-ops.md`).
-> 복구 Step 4로

### E. 멀티 GPU VM은 비용 발생 + 별도 setup
`pg-cuvs-dev-mgpu`(a2-highgpu-2g, ~$7.35/hr)는 평소 TERMINATED. start 후 sync/build/
install/postinstall + systemd unit 재구성이 필요하고, **사용 후 반드시 stop**.
-> 복구 Step 5로

---

## 4. Step-by-step 복구

### Step 1 — IP 변경 후 `.env.gpu` 갱신

```bash
PROJ=gpu-experiment-wdl-2026

# 현재 VM에 할당된 외부 IP 조회
NEWIP=$(gcloud compute instances describe pg-cuvs-dev \
  --zone=us-central1-b \
  --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')

echo "new IP = $NEWIP"
```

**기대 출력:**
```
new IP = 104.197.150.30
```
**-> 성공:** `.env.gpu`의 `GCP_VM=ubuntu@<old>` 를 `ubuntu@$NEWIP` 로 수정 후 다음 작업 계속  
**-> 일회성으로만 사용:** make 명령에 override → `make sync GCP_VM=ubuntu@$NEWIP`  
**-> IP 조회 실패 (`ERROR: (gcloud.compute.instances.describe)`):** `--project` 플래그 누락 확인

---

### Step 2 — Driver mismatch 해소 (reset)

stop/start이 아닌 **reset**으로 재부팅해 커널 모듈을 다시 로드한다.

```bash
gcloud compute instances reset pg-cuvs-dev \
  --zone=us-central1-b \
  --project=gpu-experiment-wdl-2026
```

**기대 출력:**
```
Updated [https://www.googleapis.com/compute/v1/projects/...].
```

부팅 완료 대기 (~45초) 후 드라이버 정상 여부 확인:

```bash
# IP는 reset 후에도 동일하게 유지됨 (stop/start과 달리 변경 없음)
ssh ubuntu@$NEWIP 'nvidia-smi | head -3'
```

**기대 출력:**
```
| NVIDIA-SMI 525.x.x    Driver Version: 525.x.x    CUDA Version: 12.x        |
```
**-> 성공 (Driver Version 일치):** Step 1로 돌아가 IP 확인 후 빌드/테스트 재개  
**-> 여전히 mismatch:** 드라이버 패키지 버전 불일치 — Escalation으로

> stop/start 대신 가능하면 **reset/reboot**을 쓴다. 이미 mismatch가 났으면 reset이
> 정답. mismatch가 반복되면 드라이버 패키지(예: `nvidia-dkms`) 재설치 필요.

---

### Step 3 — `/tmp/cuvs_indexes` 재구성

reset/stop-start 후 `/tmp`가 비워진 경우.

GCS snapshot이 있으면 warmup으로 복구된다 — 데몬이 시작할 때 자동으로 다운로드한다.
GCS snapshot이 없으면 REINDEX 필요:

```bash
ssh ubuntu@$NEWIP "psql -d postgres -c 'REINDEX INDEX <index_name>;'"
```

**-> 성공:** 데몬 로그에서 `built index ... vecs` 확인  
**-> GCS 다운로드 실패:** 원인 D -> Step 4로

---

### Step 4 — GCS용 SA 부착

SA 변경은 VM **stop** 상태에서만 가능하다. 변경 후 start 시 driver mismatch가 날 수 있으므로 reset을 이어서 수행한다.

```bash
PROJ=gpu-experiment-wdl-2026
Z=us-central1-b

# 1. VM 정지
gcloud compute instances stop pg-cuvs-dev --zone=$Z --project=$PROJ
```

**기대 출력:**
```
Updated [https://www.googleapis.com/compute/v1/projects/...].
```

```bash
# 2. SA 부착
gcloud compute instances set-service-account pg-cuvs-dev \
  --zone=$Z \
  --project=$PROJ \
  --service-account=gpu-exp-may@gpu-experiment-wdl-2026.iam.gserviceaccount.com \
  --scopes=https://www.googleapis.com/auth/cloud-platform
```

**기대 출력:**
```
Updated [https://www.googleapis.com/compute/v1/projects/...].
```

```bash
# 3. VM 기동
gcloud compute instances start pg-cuvs-dev --zone=$Z --project=$PROJ
```

```bash
# 4. SA가 정상 부착됐는지 확인
NEWIP=$(gcloud compute instances describe pg-cuvs-dev --zone=$Z --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')

ssh ubuntu@$NEWIP \
  'curl -s -H "Metadata-Flavor: Google" \
  http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email'
```

**기대 출력:**
```
gpu-exp-may@gpu-experiment-wdl-2026.iam.gserviceaccount.com
```
**-> 성공:** GCS 접근 가능 — `gcs-snapshot-ops.md` 참조  
**-> start 후 `nvidia-smi` mismatch:** Step 2(reset)로  
**-> metadata endpoint에서 SA 이메일 미반환:** SA 부착이 적용되지 않음 — set-service-account 명령 재확인

---

### Step 5 — 멀티 GPU VM 시작 / setup / 종료

**비용 주의: a2-highgpu-2g ~$7.35/hr. 작업 후 반드시 stop.**

```bash
PROJ=gpu-experiment-wdl-2026

# 1. VM 기동
gcloud compute instances start pg-cuvs-dev-mgpu \
  --zone=us-central1-f \
  --project=$PROJ
```

**기대 출력:**
```
Updated [https://www.googleapis.com/compute/v1/projects/...].
```

```bash
# 2. 새 IP 확인 (start 시 IP 변경됨)
MGIP=$(gcloud compute instances describe pg-cuvs-dev-mgpu \
  --zone=us-central1-f \
  --project=$PROJ \
  --format='get(networkInterfaces[0].accessConfigs[0].natIP)')

echo "mgpu IP = $MGIP"
```

**기대 출력:**
```
mgpu IP = 34.170.x.x
```

```bash
# 3. 소스 동기화 + 빌드 + 설치 (GCP_VM override 필수)
make sync gpu-build gpu-install gpu-server gpu-postinstall GCP_VM=ubuntu@$MGIP
```

**기대 출력:** `gpu-vm-build-and-test.md` 각 Step의 기대 출력과 동일

```bash
# 4. GPU 2개 모두 인식 확인
ssh ubuntu@$MGIP 'nvidia-smi --query-gpu=index,name --format=csv,noheader'
```

**기대 출력:**
```
0, NVIDIA A100-SXM4-40GB
1, NVIDIA A100-SXM4-40GB
```

```bash
# 5. 작업 완료 후 반드시 stop
gcloud compute instances stop pg-cuvs-dev-mgpu \
  --zone=us-central1-f \
  --project=$PROJ

# 종료 확인
gcloud compute instances describe pg-cuvs-dev-mgpu \
  --zone=us-central1-f \
  --project=$PROJ \
  --format='get(status)'
```

**기대 출력:**
```
TERMINATED
```
**-> `TERMINATED` 확인:** 완료. 사용 시간 × $7.35/hr 비용 기록  
**-> `RUNNING` 상태 지속:** 명령 재실행 후 재확인  
**-> GPU 1개만 보임 (`0, NVIDIA A100...` 한 줄):** zone capacity/quota 문제 — Escalation으로

---

## 5. 검증 체크리스트

- [ ] GPU 정상 (드라이버/라이브러리 버전 일치)

```bash
ssh ubuntu@$NEWIP 'nvidia-smi --query-gpu=index,name --format=csv,noheader'
```
**기대 출력:** `0, NVIDIA A100-SXM4-40GB` (mgpu면 0/1 두 줄)

- [ ] 데몬이 GPU를 잡고 listening

```bash
ssh ubuntu@$NEWIP \
  'sudo journalctl -u pg-cuvs-server --no-pager -n 30 | grep -iE "GPU|listening"'
```
**기대 출력:** `GPU 0 (... A100 ...): 40465 MB total` + `listening on /tmp/.s.pg_cuvs`

- [ ] SA 부착 시 instance metadata 토큰 발급 확인

```bash
ssh ubuntu@$NEWIP \
  'curl -s -H "Metadata-Flavor: Google" \
  http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email'
```
**기대 출력:** `gpu-exp-may@gpu-experiment-wdl-2026.iam.gserviceaccount.com`

- [ ] mgpu VM 종료 확인

```bash
gcloud compute instances describe pg-cuvs-dev-mgpu \
  --zone=us-central1-f \
  --project=gpu-experiment-wdl-2026 \
  --format='get(status)'
```
**기대 출력:** `TERMINATED`

---

## 6. Escalation 기준 (When to escalate)

- `reset` 후에도 `nvidia-smi`가 mismatch면: 드라이버 패키지 버전 불일치(부분
  업그레이드). `apt list --installed | grep nvidia` 확인 후 드라이버/DKMS 재설치.
- mgpu VM이 start 후 GPU 0개만 보이면: zone capacity/quota 문제일 수 있음 — 다른
  zone 또는 시간 재시도.
- `.env.gpu` IP를 고정하고 싶으면: 정적 외부 IP를 예약해 VM에 연결(운영 정책 결정 필요).
- 비용: mgpu VM을 stop 안 하고 방치한 흔적이 보이면 즉시 stop + 사용 시간 보고.

관련: `gpu-vm-build-and-test.md`(빌드/테스트), `gcs-snapshot-ops.md`(SA/GCS),
`daemon-restart-recovery.md`(데몬 reload).
