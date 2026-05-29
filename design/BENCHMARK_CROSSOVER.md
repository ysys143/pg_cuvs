# pg_cuvs Crossover Benchmark — 설계

> 목적: **"언제 pg_cuvs를 써야 하는가"를 숫자로 증명한다.**
> pgvector / pgvectorscale / VectorChord 대비 pg_cuvs(CAGRA, GPU)가 이기는
> (N, dim, QPS, p95/p99, cost) 구간(**crossover**)을 찾고, 지는 구간도 정직하게 기록한다.
> 본 문서는 [[PROJECT_POSITIONING.md]]의 차별성 주장을 숫자로 뒷받침하기 위한 설계다.

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

조건: `pg-cuvs-dev`(단일 A100-40GB), dim=384, k=10, **clustered** 합성(20 cluster),
queries=1000, concurrency=8, iso-recall target=0.95(미달 시 sweep 최대값 채택),
`maintenance_work_mem=2GB`(HNSW build). 원시 데이터: `bench/results/pilot.csv`.

| N | engine | build(s) | p50(us) | avg(us) | QPS(c=8) | recall@10 | param |
|---|---|---|---|---|---|---|---|
| 1,000 | HNSW | 0.25 | **224** | 239 | **24,513** | 0.988 | ef=10 |
| 1,000 | CAGRA | 0.14 | 871 | 886 | 1,864 | 1.000 | k=16 |
| 10,000 | HNSW | 2.25 | **865** | 893 | **7,862** | 0.985 | ef=80 |
| 10,000 | CAGRA | 0.33 | 1,146 | 1,163 | 1,270 | 0.995 | k=16 |
| 100,000 | HNSW | 25.5 | 8,232 | 7,515 | 876 | 0.932 | ef=320 |
| 100,000 | CAGRA | **2.77** | **1,228** | 1,244 | **1,206** | 0.982 | k=128 |

### 발견

- **Latency crossover ≈ N 10k–100k.** CAGRA는 N과 거의 무관하게 **~1.2ms floor**
  (IPC + GPU dispatch); HNSW는 N에 따라 224→865→8,232 us로 증가. 작은 N에선 HNSW가
  빠르고(**H3 확인**), 큰 N에선 CAGRA가 latency 6.7× 우위(**H1 확인, 100k**).
- **Build은 전 구간 CAGRA 우위** (0.14/0.33/2.77 vs 0.25/2.25/25.5 s; 100k에서 9×) —
  **H2 확인**.
- **Recall은 전 구간 CAGRA ≥ HNSW.** 100k에서 HNSW는 ef=320으로도 0.932 포화(거리 집중),
  CAGRA는 0.982.
- **QPS는 결이 다름.** CAGRA QPS가 ~1,200–1,900으로 거의 평평 = **단일 GPU daemon의
  throughput 천장**(동시 요청 직렬화). HNSW QPS는 N에 반비례(24,513→7,862→876). 그래서
  100k에서 latency는 6.7× 이겨도 QPS는 1.4×뿐 → 동시성 throughput은 daemon이 병목.
  multi-GPU sharding / `cuvs.parallel_fanout`(Phase 3F)이 이 천장을 올릴 지점.

### "when to use" 1차 결론 (이 슬라이스 한정)

```text
N <~ 10k, 단일 쿼리 latency / 높은 QPS 중시  -> pgvector HNSW (CPU)
N >~ 100k, 낮은 tail latency + 빠른 build    -> pg_cuvs CAGRA (GPU)
높은 동시 QPS가 목표              -> 단일 GPU daemon 천장 주의(멀티-GPU 필요)
```

### 한계 / 다음

단일 A100·clustered 합성·dim 384·k 10 한 슬라이스. iso-recall은 근사(행별
`recall_at_k` 병기). 미측정: **H4 cost($/QPS)**, dim 1536, N 1M/10M, 실제 임베딩셋,
경쟁자(pgvectorscale/VectorChord), multi-GPU sharded CAGRA의 QPS 천장. → full 단계.
