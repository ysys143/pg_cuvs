# pg_cuvs Crossover Benchmark — 설계

> 목적: **"언제 pg_cuvs를 써야 하는가"를 숫자로 증명한다.**
> pgvector / pgvectorscale / VectorChord 대비 pg_cuvs(CAGRA, GPU)가 이기는
> (N, dim, QPS, p95/p99, cost) 구간(**crossover**)을 찾고, 지는 구간도 정직하게 기록한다.
> 본 문서는 [[PROJECT_POSITIONING.md]]의 차별성 주장을 숫자로 뒷받침하기 위한 설계다.

---

## 데이터 방법론 요약 (필독)

현재까지의 모든 결과는 **합성 데이터**다. 실제 임베딩 데이터로 측정한 결과는 아직 없다.

| 벤치 섹션 | 데이터 종류 | 비고 |
|---|---|---|
| §11 Pilot (pgvector HNSW vs CAGRA) | synthetic clustered — 20 cluster, sigma=0.05, `numpy` 생성 | recall은 clustered 특성으로 실제보다 낙관적일 수 있음 |
| §12 50M competitive | synthetic clustered (동일 방식) | VectorChord/IVF 계열에 유리한 분포 |
| §13 Phase 3I HNSW import | synthetic random — `random.Random(42)`, pilot과 **다른** 생성기 | recall=1.0000은 이 분포의 특성 |
| §14 GPU resource/MIG | synthetic random, N=100K | 동작 검증 목적, 대표성 낮음 |

**실제 임베딩 데이터 (Cohere Wikipedia 1024d)** 하네스는 `infra/anbench/fetch_dataset.py`에
준비되어 있지만 아직 핵심 결과에 미반영이다.

### 합성 데이터 해석 시 주의

- **clustered synthetic에서 IVF 계열(VectorChord vchordrq) recall이 과도하게 높게 나온다.**
  실제 RAG 임베딩셋은 k-means로 설명되는 구조가 약해서 probes가 훨씬 높아야 동일 recall 달성.
- **recall=1.0000(§13)은 synthetic random의 특성.** 실 데이터에서는 distance concentration으로
  recall이 낮아지며, build speedup 수치와 달리 recall 수치는 재측정이 필요하다.
- **build speedup(§13: 2.6x–15.8x)은 데이터 독립적.** CAGRA build/HNSW import 시간은
  벡터 내용이 아닌 N·dim·M에 의존하므로 실 데이터에서도 유사한 speedup이 예상된다.
- **latency crossover(N≈50K)는 synthetic clustered 기준.** 실 데이터의 클러스터 구조·dim
  분포에 따라 crossover 좌표가 달라질 수 있다.

### Planned: Cohere Wikipedia 1024d real embedding benchmark

| 항목 | 계획 |
|---|---|
| 데이터 | Cohere `wikipedia-2023-11-embed-multilingual-v3` (1024d) |
| N | 1M, 10M |
| 비교 엔진 | pgvector HNSW, pg_cuvs CAGRA, VectorChord, pgvectorscale |
| 검증 목표 | crossover N, latency/QPS, §13 build speedup 재확인, recall on real dist |
| 하네스 | `infra/anbench/fetch_dataset.py` + `bench/bench_50m.sh` 확장 |
| 상태 | **미실행** — 우선순위 대기 중 |

---

## 0. 배경 / 동기

- Phase 3(3A~3G)는 기능적으로 완료. 남은 핵심은 **가치 입증 + 제품화**.
- [[PHASE_3B_SPIKE.md]] 결과: cuVS Vamana disk/PQFlash 경로 **NO-GO** (cuVS 26.04).
  따라서 현 시점 비교 대상은 **CAGRA(GPU, VRAM-resident)** 가 중심이고, NVMe cold
  tier는 benchmark/baseline 이후에 go/no-go를 다시 본다.
- positioning 문서는 "고QPS·대규모 빌드·multi-GPU"를 차별점으로 주장하지만 **숫자가
  없다.** crossover benchmark가 그 공백을 메운다.

## 1. 핵심 질문 / 가설

검증할 가설(반증 가능하게 서술):

- **H1 (latency)**: 중·대규모(N, dim)에서 고QPS일 때 pg_cuvs CAGRA의 p95/p99가
  pgvector HNSW보다 낮다.
- **H2 (build)**: 인덱스 빌드 시간은 pg_cuvs(GPU)가 CPU 대비 크게 우위다.
- **H3 (crossover 하단)**: 작은 N / 낮은 QPS에서는 IPC·daemon round-trip
  오버헤드 때문에 pgvector가 더 빠르고/싸다 → pg_cuvs가 **지는** 구간이 존재한다.
- **H4 (cost)**: GPU 시간당 단가가 높으므로 `$/sustained-QPS`는 특정 QPS 임계
  **이상에서만** pg_cuvs가 유리하다.

목표 산출물은 이 가설들의 참/거짓을 가르는 **crossover 좌표**(N×dim×QPS×cost).

## 2. 비교 대상 (baseline)

| 시스템 | 인덱스 | 연산 위치 | 비고 |
|---|---|---|---|
| pgvector | HNSW | CPU | 사실상 표준 baseline |
| pgvectorscale | StreamingDiskANN | CPU / NVMe | larger-than-RAM 경쟁자 |
| VectorChord | RaBitQ / IVF | CPU | 최신 경쟁자 |
| **pg_cuvs** | CAGRA (unsharded) | GPU | 본 프로젝트 |
| **pg_cuvs** | CAGRA (sharded, Phase 3F) | multi-GPU | 대규모 구간 |

> 버전 고정 필수: 각 시스템 버전·커밋·빌드 옵션을 결과 CSV에 함께 기록(재현성).

## 3. 측정 지표

| 범주 | 지표 | 비고 |
|---|---|---|
| 정확도 | recall@k (k=10, 100) | offline, exact brute-force GT 대비 |
| 지연 | p50 / p95 / p99, **avg_latency_us** | pg_cuvs `pg_stat_gpu_search.p50`은 **log2 버킷**이라 비교 부적합 → `avg_latency_us` 사용 |
| 처리량 | QPS (single client + N concurrent) | pgbench `-c/-j` sweep |
| 빌드 | build time, index size | VRAM / host RAM / disk 각각 |
| pg_cuvs 고유 | reload time, **GCS warmup time** | cold node 복원 비용 |
| 비용 | `$/1M queries`, `$/sustained-QPS` | 실제 인스턴스 시간당 단가 기반 |

## 4. 공정성 / 방법론 (가장 중요)

- **iso-recall 비교**: 각 시스템을 **동일 recall target**(예: 0.95, 0.99)에
  맞도록 파라미터를 튜닝한 뒤 latency/QPS/cost를 비교한다. (튜닝 파라미터:
  HNSW `m`/`ef_search`, CAGRA `graph_degree`/`itopk`/`search_width`,
  pgvectorscale `num_neighbors`/`search_list_size`, VectorChord 해당 노브.)
  사용한 파라미터는 전부 결과에 공개한다.
- **동일 하드웨어 축**: pg_cuvs는 GPU VM(A100, [[reference_gpu_vms]]), CPU baseline은
  같은 VM의 CPU 또는 동급 CPU 인스턴스. cost는 각 인스턴스의 실제 시간당 단가로 환산.
- **warm 상태 통일**: pg_cuvs는 daemon resident, pgvector/경쟁자는
  `shared_buffers` warm 후 측정. cold-start는 별도 항목으로 분리 측정.
- **동일 쿼리셋 + 동일 GT**: 같은 query.fbin, 같은 brute-force GT로 모든 시스템 채점.
- **반복/분산 보고**: 각 셀 N회 반복, p50/p95/p99와 분산을 함께 보고. 단일 측정 금지.

## 5. 데이터셋 / 파라미터 매트릭스

**Pilot (이번 단계)** — pgvector HNSW vs pg_cuvs CAGRA만:

| 축 | 값 |
|---|---|
| N | 100K, 1M |
| dim | 384, 1536 |
| k | 10, 100 |
| recall target | 0.95, 0.99 |
| concurrency | 1, 8, 32 clients |

**Full (다음 단계)**:

- N 10M+ 추가, pgvectorscale + VectorChord 추가.
- cost/QPS, build/reload/GCS warmup 포함.
- 데이터: 합성(random + clustered) + 실제 임베딩셋(예: SIFT1M/GIST1M 또는
  Cohere/OpenAI 임베딩 1M·10M) 병행 — 합성만으로는 분포 비현실성 위험.

## 6. Harness 설계

재사용:
- `infra/scripts/pgbench-shard.sql` — pg_cuvs KNN 부하 (`<=>`, `cuvs.probe_*`).
- `infra/scripts/benchmark.sh` — 실행 래퍼.
- `infra/scripts/integration-test.sh` — recall 검증 scenario 패턴.

신규(동일 인터페이스로 시스템별 분리):
- `bench/gen_dataset.py` — base/query 생성·로드 (fbin ↔ pgvector COPY).
- `bench/gt.py` — exact brute-force GT (numpy/GPU).
- `bench/run_{pgvector,pgcuvs,pgvectorscale,vectorchord}.sh` — 동일 입력/출력 규약.
- `bench/collect.py` — 결과 → 단일 CSV.
- `bench/plot.py` — crossover 표/플롯.

**결과 스키마(CSV)**: `system, version, index, N, dim, k, recall_target,
build_s, index_bytes_vram, index_bytes_host, qps, p50_us, p95_us, p99_us,
avg_latency_us, recall_at_k, cost_per_1M, hw, params_json`.

## 7. 산출물

- 본 문서(`design/BENCHMARK_CROSSOVER.md`).
- `bench/results/*.csv` + crossover 표 + 플롯.
- README용 **"when to use pg_cuvs"** 표 — positioning 문서 보강(어느 N·QPS·dim에서
  pg_cuvs를 쓰고, 어디서 pgvector/pgvectorscale를 쓰라고 정직하게 명시).

## 8. Cost model 연계

- benchmark 결과로 `src/pg_cuvs.c:689`의 `cuvsamcostestimate`(planner cost
  callback)를 **재보정**한다. 현재 공식은 상수 3개로만 구성:
  `CUVS_STARTUP_COST=1000.0` + `CUVS_K_COST=0.5 * cuvs.k` + `CUVS_ROWS_COST=1e-5 * rows`.
  반영할 인자: `shard_count`, parallel fanout, `shard_overfetch`, delta GPU cache,
  eviction/reload·warmup 상태(현재 startup 상수에 미반영).
- planner의 path 선택 crossover(작은 N→CPU/seqscan, 큰 N→GPU)가 **실측 crossover와
  일치**하는지 검증. GPU 경로를 끄는 **8개 gate**(`enable_cuvs`, circuit breaker,
  `.stale`, delete-drift, socket, artifact, delta/tombstone unusable)가
  `enable_seqscan=off` 강제 상황에서도 안전하게 동작하는지 확인.

## 9. 단계 / 실행 순서

1. **Pilot** (이번): pgvector HNSW vs pg_cuvs CAGRA, 100K/1M × dim 384/1536 ×
   k 10/100, iso-recall 0.95/0.99. → 1차 crossover 좌표 확인.
2. **Competitive baseline**: pgvectorscale + VectorChord 설치·튜닝, 같은 조건 합류.
3. **Full**: 10M+, cost/QPS, build/reload/GCS warmup.
4. **Cost model recalibration** (§8).
5. **3B cold-tier go/no-go** — full 결과로 NVMe tier 가치 재판단.

## 10. 리스크 / 한계

- 경쟁자(VectorChord/pgvectorscale) 설치·튜닝 난이도 → 버전·파라미터 전부 기록으로 완화.
- 공정 튜닝의 주관성 → **iso-recall + 파라미터 공개**로 방어.
- GPU 단가 변동 → cost는 **가정을 명시**(인스턴스/시간당 단가 표).
- 합성 데이터 비현실성 → 실제 임베딩셋 병행(§5).
- pg_cuvs는 WAL-logged mutable native index가 아님(write-path 한계) → 비교 시
  "정적/배치 인덱스" 전제임을 명기.

## 11. Pilot 결과 (2026-05-29) — pgvector HNSW vs pg_cuvs CAGRA

조건: `pg-cuvs-dev`(단일 A100-40GB), k=10, **clustered** 합성(20 cluster), queries=1000,
concurrency=8, iso-recall target=0.95(미달 시 sweep 최대값 채택), `maintenance_work_mem=2GB`
(HNSW build). 원시 데이터: `bench/results/pilot.csv`.

| N | dim | engine | build(s) | p50(us) | QPS(c=8) | recall@10 | param |
|---|---|---|---|---|---|---|---|
| 1,000 | 384 | HNSW | 0.25 | **224** | **24,513** | 0.988 | ef=10 |
| 1,000 | 384 | CAGRA | 0.14 | 871 | 1,864 | 1.000 | k=16 |
| 10,000 | 384 | HNSW | 2.25 | **865** | **7,862** | 0.985 | ef=80 |
| 10,000 | 384 | CAGRA | 0.33 | 1,146 | 1,270 | 0.995 | k=16 |
| 100,000 | 384 | HNSW | 25.5 | 8,232 | 876 | 0.932 | ef=320 |
| 100,000 | 384 | CAGRA | **2.77** | **1,228** | **1,206** | 0.982 | k=128 |
| 1,000,000 | 384 | HNSW | 366.2 | 13,700 | 432 | 0.928 | ef=320 |
| 1,000,000 | 384 | CAGRA | **28.3** | **1,196** | **1,197** | 0.978 | k=128 |
| 100,000 | 1536 | HNSW | 151.5 | 14,230 | 471 | 0.975 | ef=320 |
| 100,000 | 1536 | CAGRA | **7.48** | **1,605** | **916** | 0.956 | k=128 |
| 1,000,000 | 1536 | HNSW | 2,721 | 14,969 | 400 | 0.910 | ef=320 |
| 1,000,000 | 1536 | CAGRA | **75.3** | **1,702** | **893** | 0.995 | k=256 |

### 발견

- **CAGRA latency floor가 N에 거의 불변.** dim=384에서 871→1,146→1,228→1,196 us (1k→1M).
  dim=1536에서도 1,605→**1,702 us** (100k→1M). HNSW는 dim 384: 224→13,700 us,
  dim 1536: 14,230→**14,969 us** → **latency crossover ≈ N 10k–100k** (**H1·H3 확인**).
- **Build 우위가 N·dim 둘 다에서 계속 확대.** 9×(100k×384) → 13×(1M×384) → 20×(100k×1536)
  → **36×(1M×1536)**. HNSW build은 1M×1536에서 **2,721s(45분)** — `maintenance_work_mem=2GB`
  한계로 디스크 spill 모드, dim 1536에서 내부 구조가 RAM을 초과. CAGRA는 A100 40GB에 전체를
  올려 75s 완료 — **H2 확인, 스케일에서 격차 심화**.
- **Recall도 1M×1536에서 CAGRA 압승.** CAGRA 0.995 vs HNSW 0.910 (둘 다 sweep 상한). HNSW는
  1M×1536에서 0.95 target 미달 — 빌드 품질 저하(spill) + 거리 집중 이중 영향. 100k×1536의
  근소 역전(HNSW 0.975 vs CAGRA 0.956)은 1M에서 완전히 뒤집힌다.
- **dim↑ 일수록 GPU가 더 유리.** HNSW latency: dim 384@1M→13,700 vs dim 1536@1M→14,969 (9%
  증가). CAGRA: 1,196→1,702 (42% 증가). 절대치는 더 오르지만 격차는 CAGRA 쪽에 더 유리.
- **QPS: 1M×1536에서 CAGRA가 HNSW를 QPS로도 역전.** CAGRA 893 > HNSW 400. N·dim이 커져
  HNSW의 per-query 비용이 GPU daemon 오버헤드보다 높아지면 daemon 천장도 HNSW보다 낮지 않다.
  소규모(N≤10k)에선 여전히 HNSW QPS 압도적(최대 24,513), 대규모에선 역전.

### "when to use" 최종 결론 (L2, clustered, k=10, 단일 A100 기준)

```text
N <~ 10k                            -> pgvector HNSW (CPU)
                                       단일 쿼리 latency + QPS 모두 압도적
N >~ 100k  또는  dim >= 1536         -> pg_cuvs CAGRA (GPU)
                                       latency 8–12×, build 9–36×, recall도 우위
                                       N·dim이 클수록 격차 확대 (1M×1536: build 36×, latency 8.8×)
동시 QPS 목표 (소~중 N)              -> 단일 GPU daemon 천장(~1k QPS) 주의 → 멀티-GPU 필요
동시 QPS 목표 (대규모 N)             -> HNSW 자체가 병목, CAGRA가 QPS도 우위
```

### 한계 / 다음

단일 A100·clustered 합성·k=10. iso-recall은 근사(행별 `recall_at_k` 병기). 미측정:
**H4 cost($/QPS)**, N 10M+, 실제 임베딩셋, 경쟁자(pgvectorscale/VectorChord),
multi-GPU sharded CAGRA의 QPS 천장. → full / competitive baseline 단계.

---

## 12. 50M×384 Competitive Benchmark (2026-05-30) — 4-way 대결

조건: `pg-cuvs-dev-mgpu`(2×A100-SXM4-40GB, 170 GB RAM), N=50,000,000, dim=384, k=10,
clustered 합성, queries=1000. 원시 데이터: `bench/results/competitive.csv`.

| engine | index | build_s | QPS | p50_us | p95_us | recall@10 | params | index_bytes |
|---|---|---|---|---|---|---|---|---|
| diskann | diskann | **>18,000 (TIMEOUT)** | NA | NA | NA | TIMEOUT | nn=50, BUILD_MEM=2GB | NA |
| hnsw | hnsw | 21,879 | 546 | 12,985 | 21,321 | 미측정† | ef=320, BUILD_MEM=64GB | 97.7 GB |
| vchordrq | vchordrq | **5,784** | **152** | **49,101** | **61,032** | **0.9991** | lists=8192, probes=5 | 83.2 GB |
| cagra | cagra | **FAILED** | NA | NA | NA | FAILED | shard_count=4 | NA |

> † HNSW recall은 인덱스 재빌드 없이 측정하지 않음. build_s·latency는 유효. pilot 1M에서 동일 조건 recall ≈ 0.93.
> vchordrq: GT 버그 수정(2026-05-31) 후 재측정. probes=5로 recall 0.9991 달성 — 클러스터된 합성 데이터에서 IVF가 매우 효율적.

### 엔진별 결과

**DiskANN (pgvectorscale, BUILD_MEM=2GB)**
pgvectorscale 자사 공식 벤치마크 조건(2 GB cache) 재현 시도.
50M 벡터 중 ~470K (~0.9%) 적재 후 cache full → graph quality 저하 → 5h timeout.
이 조건에서 50M×384는 DiskANN으로 사실상 불가능.

**HNSW (pgvector, BUILD_MEM=64GB)**
64 GB RAM도 50M×384 HNSW 그래프(~97 GB)에 부족 → 30M 튜플 이후 disk spill.
21,879s(~6h) 완료. p50=13ms, QPS=546. pilot 1M 대비 latency 증가 미미.
recall 재측정 미완료 (인덱스 재빌드 6h 필요, 스킵).

**vchordrq (VectorChord, GT 수정 후 재측정)**
IVF k-means(8192 clusters, ~68분) + 인덱스 빌드(~29분) = 5,784s(~1.6h).
HNSW 대비 빌드 3.8× 빠름. search: probes=5로 recall 0.9991 달성.
p50=49ms, QPS=152. HNSW(p50=13ms, QPS=546) 대비 latency 3.8×, QPS 3.6× 느림.
클러스터된 데이터에서 IVF는 매우 적은 probes로 높은 recall 달성 가능 — 실제 분포에서는 probes가 훨씬 높아야 할 수 있음.

**CAGRA 2x/4x sharding (pg_cuvs)**
- shard_count=2 시도: 25M×384×4B=38.4 GB/shard > A100 GPU budget → pre-check 실패
- 서비스 설정 버그 발견: `--max-vram-mb 128` (128 MB 제한!) → 40000으로 수정
- shard_count=4 시도: corpus 로딩(76.8 GB, ~20분) 성공 → daemon 전 스레드 futex 교착.
  50분 후 완전 hung(0 CPU ticks). 강제 종료.
- 근본 제약: 50M×384×float32 = 73.24 GiB 원시 데이터. 2×A100 40GB = 80 GB VRAM.
  CAGRA 빌드 워크스페이스(그래프+중간 버퍼) 포함 시 실질적으로 VRAM 초과.

### 발견 및 함의

1. **50M×384는 현재 pg_cuvs의 single-node 한계점이다.**
   float32 원시 데이터 73 GiB가 2×40 GB VRAM에 간신히 들어가는 수준이라
   빌드 워크스페이스 포함 시 실패. A100-80GB × 2 = 160 GB or 더 많은 GPU 필요.

2. **HNSW는 50M에서도 latency를 유지하지만 빌드 비용이 크다.**
   1M pilot(p50=13.7ms)에서 50M(p50=13.0ms)으로 latency 거의 무변화.
   단 빌드 21,879s(6h), 97 GB 인덱스는 운영 부담.

3. **vchordrq는 클러스터 데이터에서 빌드가 빠르고 recall이 높다.**
   probes=5로 recall 0.9991. 단 latency는 HNSW 대비 3.8×, QPS 3.6× 낮음.
   주의: 클러스터된 합성 데이터의 결과이므로 실제 임베딩 데이터에서는 probes가 훨씬 높아야 함.

4. **DiskANN은 실제로 50M을 2 GB에서 처리하지 못한다.**
   pgvectorscale의 공식 조건에서도 실패. 더 큰 cache가 필요하거나
   1B+ 스케일 NVMe cold tier 시나리오에만 의미 있음.

### GT 불일치 버그 (수정 완료 — 2026-05-31)

`gt_faiss.py --regen` 내부 RNG 호출 순서가 `load_binary.py`의 배치별 생성과 달라
동일 seed에서 다른 벡터 집합이 생성됨 → 모든 recall=0.0000.

**수정 (2026-05-31, commit `6a74863`)**: `gt_faiss.py --regen`의 `GBATCH=1_000_000`을
`GBATCH=50_000`으로 변경하여 `load_binary.py --batch 50000` 기본값과 일치시킴.
배치 크기가 다르면 `rng.integers()` → `rng.standard_normal()` 간 state 전환이 달라져
noise 시퀀스가 달라지는 것이 원인. 수정 후 vchordrq recall 0.9991 확인.

### 다음 단계

- HNSW 50M recall 측정 (인덱스 재빌드 ~6h 필요, 현재 스킵)
- CAGRA: A100-80GB 또는 shard_count 최적화 재시도
- Cell C: 1M×384 pgvectorscale/VectorChord Pareto sweep
- 실제 임베딩 데이터셋 적용

---

## §13 — Phase 3I: CAGRA-to-HNSW GPU Build Accelerator

**날짜**: 2026-06-01  
**환경**: pg-cuvs-dev-mgpu (A100-40GB × 2, 170GB RAM), dim=384, k=10, L2

### 핵심 메시지

> **pgvector HNSW를 그대로 서치하되, 인덱스 빌드만 GPU로 가속한다.**

이 경로의 현실적인 가치는 온프렘/프라이빗 RAG 배포에서 특히 크다. 그런 환경에서는
embedding model serving, reranker, batch embedding 때문에 GPU 서버가 이미 존재하는 경우가
많고, 벡터 DB도 그 근처에 배치된다. pg_cuvs는 그 GPU 리소스 풀을 인덱스 build/rebuild
시간에만 빌려 쓰고, 온라인 검색은 익숙한 PostgreSQL/pgvector HNSW 경로로 유지한다.

즉 운영자에게 요구하는 변화는 "새 벡터 DB로 교체"가 아니라 "이미 있는 GPU로 느린 HNSW
빌드를 가속하고, 결과물은 pgvector가 계속 서빙"이다.

`pg_cuvs_import_hnsw(cagra_oid, hnsw_oid)`:
1. GPU로 CAGRA 빌드 (빠름)
2. cuVS `from_cagra()` → `.hnsw` sidecar (hnswlib 포맷)
3. pgvector HNSW 페이지 포맷으로 변환, WAL-safe 직접 기록
4. 이후 pgvector 서치 경로 그대로 사용 (`USING hnsw`로 쿼리)

### 측정 결과 (bench/results/hnsw_import_bench.csv, 2026-06-01)

**데이터**: `bench/test_3i_bench.py`의 `random.Random(42)` 기반 synthetic random 벡터.
pilot crossover(§11)의 clustered 합성 데이터와 **다른 생성기**다. 실제 임베딩 데이터 미측정.

| N | dim | CAGRA build | HNSW import | GPU total | pgvector native | speedup | p50 (us) | recall@10 |
|---|-----|-------------|-------------|-----------|-----------------|---------|----------|-----------|
| 10K | 384 | 0.34s | 0.17s | **0.5s** | 1.3s | **2.6x** | 740 | 1.0000¹ |
| 100K | 384 | 2.7s | 2.1s | **4.7s** | 74.7s | **15.8x** | 1239 | 1.0000¹ |
| 1M | 384 | 27.3s | 39.0s | **66.3s** | 918.3s | **13.9x** | 1648 | 1.0000¹ |

¹ **recall=1.0000은 synthetic random 데이터의 특성**이다. 균일 분포 데이터에서는 exact kNN이
  명확하고 HNSW/CAGRA 모두 쉽게 찾는다. 실제 고차원 임베딩(e.g. Cohere Wikipedia 1024d)에서는
  distance concentration 현상으로 recall이 낮아지며, pgvector HNSW native 대비 import HNSW의
  recall 동등성은 별도 검증이 필요하다 (구조상 동일 index이므로 동등해야 하지만 실측 미완료).

**build speedup은 데이터 독립적**: CAGRA build / HNSW import 시간은 벡터 내용이 아닌 N·dim·M에 의존하므로 speedup 수치(2.6x~15.8x)는 real data에서도 유사하게 유지될 것으로 예상.

모든 수치는 VM 실측값 (`bench/results/hnsw_import_bench.csv`, 2026-06-01)

### 포지셔닝

| 시나리오 | 권장 경로 |
|---|---|
| GPU VRAM에 인덱스가 들어가는 경우 | pg_cuvs CAGRA (GPU 서치) |
| VRAM 부족/없음 + 빠른 빌드 필요 | **CAGRA build + pg_cuvs_import_hnsw → pgvector HNSW 서치** |
| 온프렘 RAG에서 embedding GPU 풀이 이미 있는 경우 | **GPU pool로 batch/reindex 가속, 온라인 검색은 pgvector HNSW** |
| CPU-only 환경 + 빠른 빌드 불필요 | pgvector HNSW native |

### 검증 (2026-06-01, VM E2E)

- `make installcheck` 6/6 통과 (smoke, cpu_fallback, edge_cases, cpu_hnsw_fallback, hnsw_import, hnsw_edge_cases)
- restart-after-import: 재시작 전후 결과 동일 확인 (`bench/test_3i_restart.sh`)
- VACUUM / REINDEX on imported HNSW: 정상 동작
- 안전 검증: non-HNSW 타겟 거부, dim mismatch 거부, metric mismatch 거부
- WAL safety: `log_newpage_buffer` full-page image 기록 (crash recovery 가능)

---

## §14 — GPU 자원 파라미터 튜닝 & MIG 검증

**날짜**: 2026-06-01  
**환경**: pg-cuvs-dev-mgpu (A100-40GB × 2), N=100K, dim=384, k=10, **GPU 1 전용**

### 14.1 GPU 자원 파라미터 벤치마크 (`bench/results/gpu_resources_bench.csv`)

CPU 벤치마크가 `work_mem`, `max_parallel_workers`를 명시하듯 GPU 파라미터도 실험 조건으로 기록한다.

#### T1: max_vram_mb (VRAM 예산 제한)

| max_vram_mb | build (s) | p50 (us) | recall@10 |
|---|---|---|---|
| 40000 (기본) | 2.63 | 1214 | 0.792 |
| 2048 (제한) | 2.61 | 1215 | 0.802 |

- 100K×384 인덱스 ≈ 750MB → 2GB 한도 이내 → 성능 차이 없음
- OOM 테스트는 N > corpus/VRAM_limit 인 경우에만 의미 있음 (N≈5M+ 예상)

#### T2: shard_count (명시적 멀티 GPU 샤딩)

| shard_count | p50 (us) | recall@10 | 비고 |
|---|---|---|---|
| 1 (단일 GPU) | 1224 | 0.816 | 기준선 |
| 2 (2 GPU 분산) | 2075 | **0.924** | latency **+70%**, recall +13% |

- 샤딩은 recall을 높이지만 shard dispatch + merge 오버헤드로 latency가 증가함
- VRAM 부족 시에만 강제 사용; 여유 있으면 shard_count=1이 latency 우위

#### T3: cuvs_k sweep (GPU candidate list 크기)

| cuvs_k | p50 (us) | recall@10 |
|---|---|---|
| 10 | 1207 | 0.620 |
| 50 | 1309 | 0.618 |
| 100 | 1333 | 0.798 |
| **200** | **1352** | **0.916** |

- cuvs_k ↑ → latency 소폭 증가, recall 크게 향상
- 고정밀 요건: `cuvs_k=200` (+12% latency, +15% recall)
- `cuvs_k=10`은 candidate 부족으로 recall 급락 → 기본값 100 이상 권장

#### T4: parallel_fanout (shard 병렬 dispatch)

| parallel_fanout | p50 (us) | recall@10 | 비고 |
|---|---|---|---|
| 0 (sequential) | **1936** | 0.924 | N=100K에서 더 빠름 |
| 1 (parallel) | 2298 | 0.922 | 스레딩 오버헤드 > 이득 |

- N=100K 소규모: sequential이 parallel보다 18% 빠름
- parallel_fanout=1이 유리한 구간: shard당 N이 충분히 커서 스레딩 비용을 상쇄할 때 (N>5M 예상)

---

### 14.2 MIG(Multi-Instance GPU) 기능 검증 (`bench/test_mig.sh`)

**환경**: A100-SXM4-40GB × 2, MIG mode 활성화 (reboot 필요)

| 시나리오 | MIG 구성 | shard_count | N | 결과 |
|---|---|---|---|---|
| 단일 MIG 인스턴스 | GPU 0: 1× 3g.20gb (20GB) | 1 | 50K | **PASS** |
| 멀티 MIG 인스턴스 | GPU 0: 3× 1g.5gb (5GB each) | 3 | 30K | **PASS** |

- MIG 인스턴스가 별도 CUDA device로 노출 → `CUDA_VISIBLE_DEVICES=MIG-uuid` 설정만으로 동작
- `cuvs_detect_gpus()`가 MIG instance의 VRAM(5GB/20GB)을 올바르게 열거
- pg_cuvs 코드 변경 없이 MIG 지원 완료

**MIG 활성화 순서** (GCP A100 기준):
```bash
# Step 1 (reboot 필요)
sudo bash bench/test_mig.sh --setup

# Step 2 (reboot 후 비동기 실행)
nohup sudo bash bench/test_mig.sh --test > /tmp/test_mig.log 2>&1 &
sudo tail -f /tmp/test_mig.log

# Teardown (reboot 필요)
sudo bash bench/test_mig.sh --teardown
```

---

### 14.3 파라미터별 설정 가이드

```
cuvs.k (=cuvs_k):
  default=100  → 기본 균형
  200          → 고정밀 요건 (latency +12%, recall +15%)
  10           → 일반 권장하지 않음 (100K×384 synthetic에서 recall 0.62;
                  테스트/디버그 목적으로는 사용 가능)

cuvs.shard_count:
  0 (auto)    → 권장 (VRAM 초과 시 자동 분산)
  1           → 단일 GPU 강제 (latency 우선)
  2           → VRAM 부족 시에만 명시 (latency +70% 감수)

cuvs.parallel_fanout:
  N < 1M: sequential(0) 권장 — 스레딩 오버헤드 > 병렬 이득 (실측)
  N > 5M: parallel(1) 유리 예상 — 가설 (대규모 검증 예정)
  실제 운영에서는 shard당 N 기준으로 A/B 후 결정 권장

max_vram_mb (daemon 시작 플래그):
  기본 40000  → 물리 GPU 전체
  축소 시     → 인덱스 크기(≈4×corpus)가 한도 이내면 성능 무관
```

---

## §16 — Cohere Wikipedia 1024d 실제 임베딩 벤치마크 (첫 번째 실측)

**날짜**: 2026-06-01  
**환경**: pg-cuvs-dev (단일 A100-40GB), N=1,000,000, dim=1024, metric=cosine(L2-normed)  
**데이터**: Cohere `wikipedia-2023-11-embed-multilingual-v3` EN subset 1M rows  
(L2-normalized → L2 거리 = cosine 거리, `bench/run_cohere.sh` 실행)

**이 섹션은 §11~§14의 synthetic 데이터 결과를 실제 임베딩으로 처음 검증한 것이다.**

### 16.1 검색 성능 비교 (best-recall 기준)

| 시스템 | recall@10 | QPS | p50 | 설정 |
|--------|-----------|-----|-----|------|
| pg_cuvs CAGRA (GPU search) | **0.9912** | 227 | 4.4ms | k=100 fixed |
| pgvector HNSW | 0.9891 | 45 | 22ms | ef=400 |
| pgvector HNSW | 0.9392 | 130 | 7.6ms | ef=80 |
| pgvector IVFFlat | 0.9766 | 8.6 | 115ms | probes=128 |
| pg_cuvs_import_hnsw (3I) | **0.9993** | 16.9 | 61ms | ef=512 |
| cagra-hnsw-cpu (cuVS lib) | 0.9975 | 611¹ | 12ms | ef=512 |

¹ cagra-hnsw-cpu는 12-core 배치 QPS; 단일 쿼리 latency는 p50=12ms.

### 16.2 빌드 시간 비교 (1M×1024)

| 빌드 경로 | 빌드 시간 | 비고 |
|-----------|-----------|------|
| pgvector HNSW native | 285s | m=16, ef=64, 16GB mem, 7 parallel workers |
| **3I: CAGRA build + import_hnsw** | **142s** | CAGRA 85s + import_hnsw 57s |
| cagra-hnsw (cuVS lib, CPU search) | **12s** | GPU build only; no PostgreSQL COPY overhead |

**3I speedup: 2.0× vs pgvector native (1M×1024)**

### 16.3 synthetic vs real 비교

| 항목 | synthetic (§13, 1M×384) | real Cohere (§16, 1M×1024) |
|------|-------------------------|----------------------------|
| 3I 빌드 speedup | **13.9×** | **2.0×** |
| CAGRA recall@10 | 0.978 | 0.991 |
| 3I recall@10 (ef=128) | 1.000¹ | 0.992 |

¹ synthetic random 데이터의 특성 (균일 분포, near-exact kNN).

**speedup 차이 원인**:
- dim 증가(384→1024): CAGRA build + HNSW sidecar 직렬화가 더 오래 걸림
- HNSW native가 16GB mem + 7 workers로 더 빠름 (synthetic은 2GB)
- `from_cagra()` CPU 직렬화가 dim 증가에 따라 느려짐

**결론**: 실 데이터에서도 3I 경로가 pgvector native보다 2× 빠르며, recall이 동등하거나 더 높다.

### 16.4 핵심 결과 요약

- **pg_cuvs CAGRA GPU search**: 실 데이터 recall@10=0.991, p50=4.4ms — synthetic(0.978)보다 높음
- **3I import**: recall@10=0.992 (ef=128), 2.0× build speedup — synthetic 결과를 실 데이터로 검증
- **cagra-hnsw-cpu (cuVS lib)**: GPU 12s 빌드 후 CPU search, recall@10=0.985, 611 QPS (배치) — 가장 빠른 빌드
- 실 데이터에서 IVFFlat(vchordrq)는 synthetic에서의 유리했던 clustered 구조 이점이 줄어들 것으로 예상

### 16.5 하네스

```bash
# VM에서 실행 (nohup 비동기)
nohup bash bench/run_cohere.sh --n 1000000 --gpu 0 > /tmp/cohere_bench.log 2>&1 &
```

결과: `bench/results/cohere_N1000000.jsonl` + `cohere_N1000000_summary.csv`

> **주의**: `pg_cuvs_server` 바이너리 업데이트는 `make install-server` 별도 필요.
> `make install`만으로는 `.so`만 갱신되며, HNSW 사이드카 직렬화 등 데몬 기능이 반영되지 않는다.

---

## §17 — Phase 3J: Direct CAGRA→pgvector 성능 비교

**날짜**: 2026-06-01  
**환경**: pg-cuvs-dev (단일 A100-40GB), N=1,000,000, dim=1024, Cohere Wikipedia 1024d  
**비교**: `pg_cuvs_import_cagra` (direct, hnswlib 없음) vs `pg_cuvs_import_hnsw` (hnswlib 경유)

### 측정 결과

| 경로 | CAGRA build | import | **합계** | recall@10 (ef=200) | QPS |
|------|-------------|--------|----------|-------------------|-----|
| `pg_cuvs_import_cagra` (direct) | 55.7s (sidecar 없음) | 63.3s | **119.0s** | 0.9963 | 41.4 |
| `pg_cuvs_import_hnsw` (hnswlib) | 83.4s (sidecar 포함) | 57.3s | **140.7s** | 0.9962 | 40.4 |
| pgvector native HNSW build | — | — | 285s | baseline | — |

**전체 speedup**: 1.18x (direct vs hnswlib) → native 대비 **2.4x**

### 분석

**직접 경로가 1.18x 빠른 이유**: from_cagra() CPU 변환 ~28s(=83.4-55.7)를 제거.

**import 단계는 direct가 6s 느린 이유**: IPC 경유 GPU→CPU 벡터 전송(~4.25GB)이 .hnsw 파일 읽기보다 느림.

**recall/QPS는 사실상 동일**: flat HNSW(level 0만) ≈ multi-level HNSW for CAGRA-built graphs. 계층 없이도 CAGRA 그래프 품질이 충분해 탐색 성능 차이 없음.

### 선택 가이드

| 요구사항 | 권장 경로 |
|----------|----------|
| 가장 빠른 import, recall 동일 | `pg_cuvs_import_cagra` + UNLOGGED (~96s) |
| multi-level HNSW 품질 보장 | `pg_cuvs_import_hnsw` (~140s) |
| 가장 안전한 (WAL crash recovery) | `pg_cuvs_import_hnsw` + LOGGED |
| cpu_hnsw_fallback 경로 필요 | `pg_cuvs_import_hnsw` (사이드카 재사용) |
