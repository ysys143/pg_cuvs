# Troubleshooting — Failure Mode Index

증상으로 빠르게 찾기. 각 항목은 한 줄 요약이며 detail은 depth-2 파일로 분리.

## Provisioning 단계

| 증상 | depth-2 |
|------|---------|
| `terraform apply` 시 STOCKOUT, sub-state:STOCKOUT | [failures/stockout.md](failures/stockout.md) |
| conda: `PackagesNotFoundInChannelsError: libcuvs-dev` | [failures/cuvs-package-not-found.md](failures/cuvs-package-not-found.md) |

## Build 단계 (nvcc/PGXS)

| 증상 | depth-2 |
|------|---------|
| `fatal error: cuda/std/__mdspan/concepts.h: No such file` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| `RAFT_SYSTEM_LITTLE_ENDIAN is undefined` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| `raft::device_resources(device_resources &&) is a deleted function` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| `cuvs/neighbors/cagra_serialize.hpp: No such file` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| `make: *** No rule to make target 'src/cuvs_wrapper.bc'` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| `implicit declaration of function 'clock_gettime'` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| Format `%ld` vs `int64_t` 경고 | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |
| Linker: `undefined reference to _ZTVN3rmm9bad_allocE` | [failures/cuvs-build-errors.md](failures/cuvs-build-errors.md) |

## Runtime — Extension Load

| 증상 | depth-2 |
|------|---------|
| `libstdc++.so.6: version GLIBCXX_3.4.31 not found` | [failures/libstdcpp-version.md](failures/libstdcpp-version.md) |
| `function vector_cosine_distance(vector, vector) does not exist` | [failures/runtime-gotchas.md](failures/runtime-gotchas.md) |
| `required extension "vector" is not installed` | [failures/runtime-gotchas.md](failures/runtime-gotchas.md) |

## Runtime — Daemon / IPC

| 증상 | depth-2 |
|------|---------|
| systemd `status 209/STDOUT, Permission denied` | [failures/runtime-gotchas.md](failures/runtime-gotchas.md) |
| Daemon: `shm_open FAILED errno=13 (Permission denied)` | [failures/runtime-gotchas.md](failures/runtime-gotchas.md) |
| Client: `daemon returned status 1` but daemon log shows success | [failures/runtime-gotchas.md](failures/runtime-gotchas.md) |
| Daemon: `code=dumped, status=11/SEGV` after `cuvs_cagra_serialize` | [failures/cuvs-serialize-sigsegv.md](failures/cuvs-serialize-sigsegv.md) |

## VM 자체가 망가짐 (catastrophic)

| 증상 | depth-2 |
|------|---------|
| 시리얼 콘솔: `OpenSSL version mismatch ... 30600020` | [failures/ldconfig-disaster.md](failures/ldconfig-disaster.md) |
| 시리얼 콘솔: `libdbus-1.so.3: LIBDBUS_PRIVATE_1.12.20 not found` | [failures/ldconfig-disaster.md](failures/ldconfig-disaster.md) |
| systemd 부팅 실패, dbus.service fail-loop | [failures/ldconfig-disaster.md](failures/ldconfig-disaster.md) |
| 디스크 손상/부팅 무한 멈춤 (위 ldconfig와 무관) | [failures/disk-recovery.md](failures/disk-recovery.md) |

## API Quirks (오류는 아니지만 알아야 할 것)

cuVS 24.x → 26.x 마이그레이션 정보는 [cuvs-26x-quirks.md](cuvs-26x-quirks.md)로.
