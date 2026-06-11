# CI 전략 — 2-tier (CPU-reference shim + on-demand GPU)

> 스펙. 결정 근거는 [DECISIONS.md ADR-067](DECISIONS.md)로 승격됨 — **구현·검증 완료**(PR #46–48/#50). 관련: ROADMAP "CI GPU 전략", [ADR-065](DECISIONS.md)(VRAM budget — mempool).

## 1. 목표와 모델

GPU CI는 무료 옵션이 없다(GitHub hosted 러너에 GPU 없음 → self-hosted = 유료 VM). 그러나 이번 세션의 false-done 버그(3O rev map 미빌드, manifest version 스탬프, base_generation=0)는 **하나도 GPU 커널 버그가 아니라 glue**(IPC 직렬화, 데몬 라우팅, fail-closed, mode 라벨링, manifest 계약)에서 났다. 실제로 무는 버그 클래스는 GPU 없이 잡힌다.

| Tier | 머신 | 트리거 | 검증 대상 |
|------|------|--------|-----------|
| **1 — CPU-reference shim** | GitHub hosted `ubuntu-latest` (public repo = 무료 무제한) | 매 PR 자동 | plumbing·IPC 계약·fail-closed·mode 라벨링·정확성·VRAM 회계 로직 |
| **2 — 실 A100 installcheck** | 사용자 GPU VM (self-hosted) | **사용자 on-demand**(`workflow_dispatch` + `/gpu-test` 코멘트/`gpu-ci` 라벨) | GPU 커널 correctness·approximate recall·실 VRAM 거동·latency |

release는 트리거가 아니라 머지 정책으로 얹는다("최근 Tier 2 GREEN 있는지 확인").

## 2. shim 경계 — `src/cuvs_wrapper.h` 단일 헤더

데몬이 cuVS/CUDA를 호출하는 모든 지점이 이 헤더 뒤에 있다. shim = 이 헤더의 **모든 선언 심볼을 CPU 구현으로 제공하는 대체 TU** `src/cuvs_wrapper_shim_cpu.c`. 데몬·백엔드 나머지 코드는 한 줄도 안 바뀐다.

빌드 선택: `make PGCUVS_CPU_SHIM=1` → `cuvs_wrapper.c`(실 cuVS) 대신 `cuvs_wrapper_shim_cpu.c` 링크.

### 2.1 CUDA-툴킷-free 빌드 (무료 성립 조건)

shim 빌드가 CUDA 툴킷 설치를 요구하면 hosted 러너 무료 이점이 깨진다. 따라서:

- shim 빌드 시 `cuvs_wrapper.h`가 끌어오는 CUDA 타입(`cudaStream_t` 등)을 **shim 전용 미니 헤더가 stub**: `typedef void *cudaStream_t;` 수준.
- opaque 핸들(`CuvsCagraIndex`/`CuvsBfIndex`/`CuvsIvfPqIndex`/`CuvsHnswIndex`)은 shim에서 host 구조체 포인터(`{float *vecs; int64_t n; int dim; uint32_t metric;}`)로 실체화.
- 결과: shim 경로는 cuVS/CUDA 헤더·라이브러리 의존 0 → 순수 CPU 빌드.

## 3. shim 의미 (카테고리별)

### A. build / free — host 사본 보관
- `cuvs_bf_build`, `cuvs_cagra_build`, `cuvs_ivfpq_build` → 입력 벡터를 host로 복사 보관, 핸들 반환. **그래프/PQ 파라미터 무시**.
- `cuvs_cagra_build_multi` → partial들 연접 후 보관(병렬 빌드 경로 검증).
- `*_free` → host 사본 해제.

### B. search — CPU exact kNN (이게 ground truth)
- `cuvs_brute_force_search`, `cuvs_bf_search`, `cuvs_cagra_search`, `cuvs_ivfpq_search` → metric별(L2/IP/cosine) 전수 거리계산 → top-k. **CAGRA/IVF-PQ도 exact 반환(recall=1.0)**.
- `cuvs_bf_search_filtered`, `cuvs_cagra_search_filtered` → 동일 + host BITSET 적용(3O 경로).
- `cuvs_cagra_search_batch`, `cuvs_bf_search_batch` → exact를 Q회 루프.

### C. streaming / compaction
- `cuvs_cagra_extend` → host 사본에 append.
- `cuvs_cagra_compact` → tombstone bitvector로 필터해 host 사본 재구성.
- `cuvs_set_inject_extend_oom` → inject 플래그 유지(OOM 주입 테스트 그대로 동작).

### D. serialize / deserialize — round-trip 필수(재시작 테스트)
- `cuvs_cagra_serialize`/`_deserialize`, `cuvs_ivfpq_*`, `cuvs_hnsw_*` → host 사본을 단순 포맷으로 파일 write/read. 재시작 reload·load 경로가 진짜 돌아야 함(load 경로의 rev map 빌드 검증).
- `cuvs_hnsw_search` → exact kNN.
- `cuvs_cagra_extract_adjacency` → 결정적 합성 인접리스트(3I export 경로 형태 검증; 값은 무의미해도 포맷·크기 계약 유지).

### E. VRAM / device 회계 — **결정적 fake (이번 세션 작업의 직접 수혜)**
- `cuvs_detect_gpus` → fake 디바이스 1개 보고. `cuvs_gpu_available` → 1(데몬이 GPU 경로 타게). 환경변수로 0 강제 가능(CPU fallback 경로 테스트).
- `cuvs_vram_free_bytes`/`_on` → **shim 내부 가상 VRAM 카운터** 반환(env로 초기 예산 설정).
- `cuvs_eat_vram`/`cuvs_free_vram` → 그 카운터를 가감 → **evict_lru·VRAM budget 강제·OOM을 실 GPU 없이 결정적으로 재현**.
- `cuvs_warmup`/`_device` → no-op.

> E 덕분에 **VRAM budget 강제·OOM 후 재사용**을 Tier 1에서 결정적으로 돌릴 수 있다. budget 강제는 ADR-065 해소대로 **데몬 자기-회계**(`total_vram_used` = Σ per-index `vram_bytes`, 기본 총량 90% cap)라 device 조회가 아예 불필요 — shim은 `cuvs_detect_gpus`로 **fake 총량만** 주면 cap 산술이 CPU에서 진짜 돈다. 단 ADR-065 경고 유지: 실 mempool headroom(explicit-unlimited 경로의 `cudaMemPoolGetAttribute` 보정)은 Tier 2에서만 검증된다.

## 4. 잡는 것 / 못 잡는 것 (정직)

**Tier 1이 잡는다**:
- IPC struct 직렬화 drift(= CI Linux oracle의 padding UB 클래스)
- `search_mode` 라벨링 — **3O false-done을 잡았을 것**
- fail-closed(corrupt SHA256 / relfilenode / version / base_generation reject)
- rev map 빌드(build·load 양 경로), delta merge, manifest 계약, sidecar read
- recall — shim이 exact라 ground truth 그 자체
- VRAM 회계 로직(evict/budget/OOM)의 제어흐름

**Tier 1이 못 잡는다(→ Tier 2 전속)**:
- 실 cuVS/CUDA 통합(API 오용, dtype, stream sync)
- **approximate recall 회귀**(CAGRA는 근사, shim은 exact → 그래프 품질 저하 안 보임)
- 실 VRAM/mempool 거동(ADR-065)
- latency·멀티GPU 실배치

## 5. 워크플로 2종

### `.github/workflows/ci.yml` (Tier 1, 자동)
- on: `push`, `pull_request`
- runner: `ubuntu-latest`
- steps: PG + pgvector 설치 → `make PGCUVS_CPU_SHIM=1` → 데몬 기동 → `make installcheck` + `make test-unit` + isolation
- 기존 `.sql` 스위트 대부분 그대로 재사용(BF recall@10=1.0은 shim과 동일, CAGRA recall assert는 `>=`라 exact가 통과, mode assert는 데몬 로직이 진짜 도니 통과)

### `.github/workflows/gpu.yml` (Tier 2, on-demand) — 구현 모델 (정정)
- on: **`workflow_dispatch`만** (Actions UI "Run workflow" 버튼 = 쓰기권한자만). 코멘트/라벨
  자동 트리거는 fork-PR가 VM에서 임의 코드를 돌릴 위험이라 **비채택**(UI 버튼이 권한 게이트 내장).
- 3-job 구조: **start-vm**(hosted, WIF 키리스 인증 → `gcloud instances start`) → **gpu-test**
  (`[self-hosted, gpu, a100]` — VM 위 부팅-시작 러너가 `ubuntu`+conda로 실 `make installcheck`) →
  **stop-vm**(hosted, `if: always()` → `gcloud instances stop`, 비용 누수 방지).
- 보안: GCP 권한은 GitHub에 키로 두지 않고 **GCP의 WIF 신뢰정책**(repo 스코프)에 둠. self-hosted
  러너는 트리거된 런 동안 VM이 켜진 시점만 온라인 → 상시 공격표면 없음. `environment: gpu` +
  required reviewers로 VM 기동 전 사람 승인 게이트. 셋업: `docs/ci-gpu-setup.md`.
- 비용은 버튼 누를 때만 발생. (초안의 `runner: [self-hosted, gpu]` 단일-잡 모델은 VM이 대부분
  꺼져 있고 IP가 바뀌는 비용통제 VM과 맞지 않아 위 hosted-제어 모델로 개정.)

## 6. 정직성 라벨(필수)

green CI badge가 "GPU 검증됨"으로 읽히면 그 자체가 CI의 false-done이다. README/badge에 명시:

> CI(Tier 1)는 CPU reference로 plumbing·계약·정확성을 검증한다. GPU 커널 correctness·approximate recall·실 VRAM 거동은 on-demand A100 런(Tier 2)에서 검증된다.

## 7. 후속

- 본 스펙을 **ADR-067**로 승격(결정 근거 기록).
- emulator 기반 GCS 회귀(ADR-066 비고의 "후속")를 Tier 1에 편입 가능 — fake GCS 엔드포인트로 objstore round-trip을 hosted 러너에서.
- shim 자체의 미니 검증(shim이 exact를 정확히 내는지)은 `make test-unit`에 1케이스로 충분.
