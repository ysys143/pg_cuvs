# conda: libcuvs-dev 패키지 없음

## 증상

```
PackagesNotFoundInChannelsError: The following packages are not available from current channels:
  - libcuvs-dev=24.12
```

## 원인

24.x 시대에는 `libcuvs`(런타임)와 `libcuvs-dev`(헤더) 두 패키지가 분리됐다. 25.x부터 통합되어 헤더가 `libcuvs`에 포함된다. `libcuvs-dev` 패키지는 더 이상 존재하지 않는다.

## 해결

설치 명령에서 `libcuvs-dev` 제거. `libcuvs`만 명시:

```bash
~/miniforge3/bin/conda create -n cuvs_dev -y \
    -c rapidsai -c conda-forge -c nvidia \
    libcuvs=26.04 \
    cuda-toolkit=12.* \
    python=3.11
```

## 가용 버전 확인

```bash
ssh ubuntu@<IP> "source ~/miniforge3/bin/activate && \
    conda search -c rapidsai -c conda-forge -c nvidia 'libcuvs' 2>&1 | tail -10"
```

대표 버전(2026년 5월 기준):
- 25.10.00 (LTS, pgstrom-results에서 검증)
- 25.12.00
- 26.02.00
- 26.04.00 (현 권장, 단 cagra::serialize SIGSEGV 이슈 있음)

## 헤더 위치 (참고)

설치 후 헤더와 라이브러리 위치:
- `$CONDA_PREFIX/include/cuvs/neighbors/cagra.hpp`
- `$CONDA_PREFIX/include/cuvs/neighbors/brute_force.hpp`
- `$CONDA_PREFIX/include/rapids/cuda/std/__mdspan/...` ← libcudacxx
- `$CONDA_PREFIX/lib/libcuvs.so`
- `$CONDA_PREFIX/lib/libcuvs_c.so`
- `$CONDA_PREFIX/lib/librmm.so` (CUDA memory pool)
