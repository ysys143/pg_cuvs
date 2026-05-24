# ldconfig 사고 — VM 자체가 부팅 불가

가장 치명적인 함정. 한 번에 VM을 무력화시킴.

## 증상

VM 재부팅 후 SSH 불가 (`Connection reset by peer` 또는 timeout).

시리얼 콘솔 (`gcloud compute instances get-serial-port-output`):
```
sshd[XXXX]: OpenSSL version mismatch. Built against 30000020, you have 30600020
```
또는:
```
dbus-daemon[XXXX]: /home/ubuntu/miniforge3/envs/cuvs_dev/lib/libdbus-1.so.3:
                   version `LIBDBUS_PRIVATE_1.12.20' not found
systemd[1]: dbus.service: Failed with result 'exit-code'.
systemd[1]: dbus.service: Start request repeated too quickly.
systemd[1]: Failed to start D-Bus System Message Bus.
```

dbus가 죽으면 google-guest-agent도 못 뜨므로 startup-script로도 복구 불가. 완전 lockout.

## 원인 — 절대 하지 말 것

이 명령 한 번이면 끝난다:
```bash
# 절대 금지
echo "$CONDA_PREFIX/lib" | sudo tee /etc/ld.so.conf.d/cuvs.conf
sudo ldconfig
```

conda env의 lib 디렉터리에는 cuvs.so 외에도:
- `libssl.so.3` (OpenSSL 3.6+) — 시스템은 OpenSSL 3.0
- `libdbus-1.so.3` (새 ABI) — 시스템 dbus는 다른 버전
- `libcrypto.so.3`, `libz.so.1`, ... 기타 시스템 라이브러리 자체 복사본 다수

ldconfig 등록 시 이 모두가 시스템 라이브러리보다 우선됨. sshd/dbus/postgresql 등 시스템 데몬이 시작할 때 conda 버전의 .so를 잡아 ABI 충돌, 시작 실패.

## 예방

**원칙**: conda lib 디렉터리 전체를 시스템에 노출시키지 말 것.

올바른 방법 (둘 중 하나):

### 방법 A: -Wl,-rpath (가장 깔끔)
extension .so 빌드 시 rpath를 박는다. 이미 Makefile에 있음:
```makefile
SHLIB_LINK = -L$(CUVS_LIB) -lcuvs -lcudart -lstdc++ -Wl,-rpath,$(CUVS_LIB)
```
시스템은 영향 없음. 우리 .so만 conda 경로를 안다.

### 방법 B: 파일 단위 심볼릭 링크
libstdc++ 같은 특정 lib만 시스템 경로에 노출:
```bash
sudo ln -sf $CONDA_PREFIX/lib/libstdc++.so.6 /usr/local/lib/libstdc++.so.6
sudo ln -sf $CONDA_PREFIX/lib/libgcc_s.so.1 /usr/local/lib/libgcc_s.so.1
sudo ldconfig
```
디렉터리 등록은 절대 X. 파일 단위만.

## 복구 — 디스크 레스큐

이미 망가졌으면 [disk-recovery.md](disk-recovery.md)로. 요약:
1. VM stop → boot disk detach
2. rescue VM 생성 (e2-small), 망가진 디스크 attach as secondary
3. mount → `/etc/ld.so.conf.d/cuvs.conf` + `/etc/ld.so.cache` 둘 다 삭제
4. chroot로 ldconfig 재실행 (cache 재빌드)
5. detach → boot으로 원 VM에 재attach → start

**핵심**: `.conf` 파일만 지우면 안 됨. `/etc/ld.so.cache`는 바이너리 캐시라 reboot 후에도 stale 엔트리 유지. cache까지 재빌드해야 부팅됨.

## 대안: VM 새로 만들기

망가진 VM에서 작업한 게 얼마 없으면 (10분 미만) 그냥 새로 만드는 게 빠르다:
```bash
gcloud compute instances delete <BROKEN_VM> --zone <ZONE> --quiet
gcloud compute disks delete <BROKEN_VM> --zone <ZONE> --quiet
terraform apply -auto-approve  # 깨끗하게 다시
```

긴 conda 설치 + 미커밋 코드 등이 있으면 디스크 레스큐로 보존.

## 학습한 것

1. conda env는 "자기 완결적인 시스템"이라고 생각해야 함. 시스템과 절대 섞지 말 것.
2. dbus가 시작 안 되면 google-guest-agent도 안 뜨므로 GCE의 startup-script/metadata 우회 메커니즘도 무효.
3. `/etc/ld.so.cache`는 바이너리. 텍스트 .conf 파일만 지운다고 캐시가 무효화되지 않음.
