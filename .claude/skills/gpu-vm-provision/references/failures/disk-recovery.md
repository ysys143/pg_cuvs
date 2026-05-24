# Disk Recovery — 망가진 VM의 부팅 디스크 수리

VM이 SSH 불가 + startup-script로도 복구 불가일 때. ldconfig 사고가 가장 흔한 트리거지만 다른 부팅 손상에도 동일 절차.

## 의사 결정 — 정말 복구해야 하나

| 상황 | 권장 |
|------|------|
| 망가진 VM에서 10분 미만 작업 | `terraform destroy && apply` (~15분, 더 단순) |
| conda install + 빌드 완료 상태 | 디스크 레스큐 (~10분, 작업 보존) |
| `~/pg_cuvs`에 미커밋 변경 있음 | 디스크 레스큐 필수 |
| 어떤 상태인지 모름 | 시리얼 콘솔 로그 먼저 보고 결정 |

```bash
gcloud compute instances get-serial-port-output <INSTANCE> \
  --zone <ZONE> --project <PROJECT> 2>&1 | tail -40
```

## 절차

### 1. 망가진 VM 정지

```bash
gcloud compute instances stop <BROKEN_VM> --zone <ZONE>
# 정지에 5-10분 걸릴 수 있음 (sshd/dbus 죽은 상태라 graceful shutdown 못 함)
```

### 2. 부팅 디스크 분리

```bash
gcloud compute instances detach-disk <BROKEN_VM> --disk <BROKEN_VM> --zone <ZONE>
```

GCE 관례: VM 이름과 boot disk 이름이 동일.

### 3. 레스큐 VM 생성

작고 빠른 일반 Ubuntu VM. 만약 프로젝트의 기본 컴퓨트 서비스 계정이 없으면 `--no-service-account --no-scopes` 필요:

```bash
gcloud compute instances create rescue-vm --zone <ZONE> \
  --machine-type=e2-small \
  --image-family=ubuntu-2204-lts --image-project=ubuntu-os-cloud \
  --metadata=ssh-keys="ubuntu:$(cat ~/.ssh/id_rsa.pub)" \
  --no-service-account --no-scopes
```

### 4. 망가진 디스크를 secondary로 attach

```bash
gcloud compute instances attach-disk rescue-vm --disk <BROKEN_VM> --zone <ZONE>

RESCUE_IP=$(gcloud compute instances describe rescue-vm --zone <ZONE> \
  --format="value(networkInterfaces[0].accessConfigs[0].natIP)")
ssh-keygen -R $RESCUE_IP 2>/dev/null
```

### 5. SSH → mount → repair

```bash
until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no ubuntu@$RESCUE_IP true 2>/dev/null; do sleep 3; done

ssh ubuntu@$RESCUE_IP "
lsblk
# /dev/sdb1 = 망가진 디스크의 root 파티션
# (sda = 레스큐 VM 자신, sdb = secondary)

sudo mkdir -p /mnt/broken
sudo mount /dev/sdb1 /mnt/broken

# ldconfig 사고 케이스:
sudo rm -f /mnt/broken/etc/ld.so.conf.d/cuvs.conf
sudo rm -f /mnt/broken/etc/ld.so.cache   # 캐시도 함께!

# chroot로 ldconfig 재실행 (캐시 재빌드)
sudo mount --bind /dev /mnt/broken/dev
sudo mount --bind /proc /mnt/broken/proc
sudo mount --bind /sys /mnt/broken/sys
sudo chroot /mnt/broken ldconfig

# 검증: 시스템 lib을 가리키는지
sudo chroot /mnt/broken ldconfig -p | grep -E 'libdbus|libssl' | head -5
# /lib/x86_64-linux-gnu/...  나와야 함 (conda 경로 아님)

# 정리
sudo umount /mnt/broken/sys /mnt/broken/proc /mnt/broken/dev
sudo umount /mnt/broken
"
```

**왜 ldconfig 재실행이 필요한가**: `/etc/ld.so.cache`는 바이너리 캐시 파일이다. `.conf` 파일을 지워도 캐시에는 stale 엔트리(conda 경로)가 남아있어 reboot 후에도 같은 문제 재발. 캐시 파일을 삭제하고 ldconfig를 재실행해서 새 캐시를 빌드해야 함.

**chroot가 필요한 이유**: 망가진 디스크의 `/lib`, `/etc/ld.so.conf` 등을 기준으로 ldconfig를 돌려야 그 디스크의 cache가 올바르게 빌드됨. 레스큐 VM 호스트에서 그냥 ldconfig 돌리면 호스트의 cache가 빌드될 뿐.

### 6. 디스크 떼고 레스큐 VM 삭제

```bash
gcloud compute instances detach-disk rescue-vm --disk <BROKEN_VM> --zone <ZONE>
gcloud compute instances delete rescue-vm --zone <ZONE> --quiet
```

### 7. 원 VM에 boot으로 재attach + 시작

```bash
gcloud compute instances attach-disk <BROKEN_VM> --disk <BROKEN_VM> --zone <ZONE> --boot

# 임시 startup-script 메타데이터 주입했었다면 제거
gcloud compute instances remove-metadata <BROKEN_VM> --zone <ZONE> --keys startup-script 2>/dev/null

gcloud compute instances start <BROKEN_VM> --zone <ZONE>
```

### 8. 검증

```bash
NEW_IP=$(gcloud compute instances describe <BROKEN_VM> --zone <ZONE> \
  --format="value(networkInterfaces[0].accessConfigs[0].natIP)")

ssh-keygen -R $NEW_IP
until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no ubuntu@$NEW_IP true 2>/dev/null; do sleep 5; done

ssh ubuntu@$NEW_IP "
ls /etc/ld.so.conf.d/cuvs.conf 2>&1   # 'No such file' 나와야 함
sudo systemctl is-active postgresql    # 'active'
sudo systemctl is-active sshd          # 'active'
"
```

## 외부 IP 변경 처리

GCE의 ephemeral IP는 정지/시작 시 바뀐다. `.env.gpu`의 `GCP_VM` 갱신:

```bash
NEW_IP=$(gcloud compute instances describe <BROKEN_VM> --zone <ZONE> \
  --format="value(networkInterfaces[0].accessConfigs[0].natIP)")

cat > /Users/jaesolshin/Documents/GitHub/pg_cuvs/.env.gpu << EOF
GCP_VM=ubuntu@$NEW_IP
GCP_INSTANCE=<BROKEN_VM>
GCP_ZONE=<ZONE>
GCP_PROJECT=<PROJECT>
CONDA_ENV=cuvs_dev
CUDA_ARCH=sm_80
EOF
```

## 영구 IP 옵션

IP 변경이 잦아 운영에 부담되면 GCE의 static external IP 예약:
```bash
gcloud compute addresses create pg-cuvs-static --region <REGION>
# Terraform main.tf의 access_config에 nat_ip 인자 추가
```
사용하지 않는 시간에도 월 ~$5 과금됨.
