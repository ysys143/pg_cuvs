# cuvs::neighbors::cagra::serialize → SIGSEGV (cuVS 26.04)

## 증상

데몬이 CAGRA build를 끝낸 직후 SIGSEGV로 죽음.

저널:
```
[handle_build] cuvs_cagra_build OK
[handle_build] storing tids and IndexEntry...
[handle_build] cuvs_cagra_serialize(/tmp/cuvs_indexes/<db>_<idx>.cagra)...
pg-cuvs-server.service: Main process exited, code=dumped, status=11/SEGV
pg-cuvs-server.service: Failed with result 'core-dump'.
```

파일 시스템:
```
/tmp/cuvs_indexes/<db>_<idx>.cagra    # 0 byte 파일 남음
```

## 원인 (추정)

cuVS 26.04 (어쩌면 25.x도)의 `cuvs::neighbors::cagra::serialize` 호출 시 segfault.

- try/catch도 안 잡힘 → 시그널 핸들러 레벨에서 죽음 (스택 손상 또는 nullptr deref)
- 작은 데이터(n=4~8)에서도 재현
- 가설:
  1. raft::device_resources 인스턴스를 함수 안에서 새로 만든 게 문제일 수 있음 (build에 쓴 res와 다른 res)
  2. `include_dataset` 인자 default가 변경됐을 가능성
  3. 26.04 자체 버그 (RAPIDS issue tracker 확인 필요)

## Phase 1 데모용 우회 (현재 적용)

`pg_cuvs_server.c` handle_build에서 serialize 호출을 비활성화:

```c
/* TODO Phase 1.5: cuvs::cagra::serialize SIGSEGVs in 26.04 — investigate */
fprintf(stderr, "[handle_build] skipping serialize (Phase 1 demo)\n");
(void)idx_path;
/* int ser_rc = cuvs_cagra_serialize(handle, idx_path);  -- 비활성 */
```

영향:
- SIGTERM persistence 동작 안 함
- 데몬 재시작 시 인덱스 사라짐 → 매번 CREATE INDEX 다시
- SPEC.md PERSIST-02, PERSIST-03 미충족
- 빌드/검색 자체는 정상

## 진짜 해결 후보 (미검증)

### 후보 1: cuVS C API로 변경

C++ 템플릿 코드 대신 C API `cuvsCagraSerialize` 사용. C API는 ABI 안정성이 더 보장됨.

```cpp
// 현재 (문제)
cuvs::neighbors::cagra::serialize(res, std::string(path), impl->idx, true);

// 후보
#include <cuvs/neighbors/cagra.h>  // C 헤더
cuvsCagraSerialize(<args>);
```

C API 시그니처는 cuvs/neighbors/cagra.h 참조.

### 후보 2: 25.10 다운그레이드

pgstrom-results 프로젝트에서 25.10 사용 시 serialize 정상 동작 확인.

```bash
conda create -n cuvs_dev_25 -y \
    -c rapidsai -c conda-forge -c nvidia \
    libcuvs=25.10 cuda-toolkit=12.* python=3.11
```

`.env.gpu`의 `CONDA_ENV=cuvs_dev_25`로 변경 후 재빌드. Makefile rpath가 conda env path를 박아두므로 .so도 재빌드 필요.

### 후보 3: include_dataset 명시

26.04 시그니처는:
```cpp
void serialize(raft::resources const& handle,
               const std::string& filename,
               const index<T, IdxT>& index,
               bool include_dataset = true);
```

`include_dataset=false`로 시도해 볼 수 있음 (데이터셋 직렬화가 문제일 가능성):
```cpp
cuvs::neighbors::cagra::serialize(res, std::string(path), impl->idx, false);
```

## 디버깅 정보 수집 (Phase 1.5에서 할 일)

GitHub issue 신고 또는 패치 만들 때 필요한 정보:
1. cuVS 정확한 버전: `conda list libcuvs`
2. coredump 분석: `coredumpctl list` → `coredumpctl info <PID>`
3. 스택 트레이스: gdb로 core 파일 열어서 `bt full`
4. 최소 재현 케이스: cuvs_wrapper.cu 단독 테스트 프로그램

## 영향 받는 다른 기능

`cuvs_cagra_deserialize`도 함께 미테스트. serialize가 0-byte 파일을 만들기 때문에 deserialize 검증 자체가 불가. 25.10으로 다운그레이드해서 serialize/deserialize 둘 다 확인 필요.
