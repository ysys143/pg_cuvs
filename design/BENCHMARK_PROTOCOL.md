# pg_cuvs 벤치마크 실험 프로토콜 (v2 — 엄밀)

> **성격**: 내부 실험 프로토콜이다. 운영자 대면 절차서(`docs/playbooks/benchmark-runbook.md`)가 아니라,
> 우리가 공정성 시비 없는 비교와 코스트모델 보정을 수행하기 위한 실험 설계 — 셀 정의, 단계 순서,
> 합격 기준, 통계 규약을 고정한다.
>
> **관계**: [`BENCHMARK_CROSSOVER.md`](BENCHMARK_CROSSOVER.md)(1차 설계 + §11–17 실측 결과 보관)를
> 계승·확장한다. 공개 산출물은 [`BENCHMARK.md`](../BENCHMARK.md)(요약)와 `bench/results/`(원시).
> 전략 근거는 ADR-061 / [`STRATEGY_NOTES.md`](STRATEGY_NOTES.md), 본 프로토콜 결정은 **ADR-069**.

---

## 0. 두 목적과 핵심 설계 결정

| 목적 | 내용 | 해법 (본 문서) |
|------|------|----------------|
| **P1 공정성** | "GPU에 VRAM 40GB를 주고 CPU에 4GB·2프로세스만 주는" 식의 자원-기울임 비교를 구조적으로 차단 | §2 자원 공정성 모델 — GB로 등화하지 않고 **$/hr·J/query로 등화** + raw 자원 전면 공개 |
| **P2 플래너** | planner-auto가 전 구간에서 두 엔진의 상위 포락선(envelope)을 따라야 하는데, 깨지는 지점에서 코스트모델/플래너를 개선 | §5–7 보정 루프 — **물리/판단 분리**로 루프 비용을 EXPLAIN 재스윕 수준으로 절감, **루프를 전체 스위트보다 앞에 배치** |

### 4대 설계 결정 (요약)

1. **물리/판단 분리**: forced 곡선 2개(=물리)는 코스트모델과 무관한 불변 자산. planner-auto(=판단)는
   둘 중 하나를 고를 뿐이므로 **EXPLAIN-only 스윕**으로 검증 → 코스트모델 수정이 전체 재실행을
   강제하지 않는다. (§5–6)
2. **보정 루프 선행**: 코스트모델 동결(Stage C) 전에는 planner-auto가 개입하는 어떤 스위트(필터·증분·
   Pareto)도 돌리지 않는다 — 동결 전 결과는 전부 무효화되므로. (§7)
3. **교차-적응 샘플링**: 균일 밀집 격자가 아니라 거친 log-격자 + 부호전환 구간만 이분탐색. 교차가
   사는 N 구간(10K–100K)은 셀당 실측이 가장 싼 곳이다. (§5.2)
4. **regret 기준**: 보정 합격은 "오분류 0"이 아니라 "**ε-무차별 밴드 밖에서 regret>0인 셀 0개**".
   교차점 근방에서는 어느 선택도 손해 ~0이므로 오분류 자체는 무의미하다. (§6.3)

---

## 1. 경쟁자 선정과 공정성 계약 (Ring 구조)

각 시스템은 **비교가 공정해지는 단 하나의 링에만**, 명시된 클레임을 지지할 때만 입장한다.
8개를 한 표에 넣지 않는다 — 링마다 클레임·공정조건이 다르다.

### Ring A — 1차 링: in-Postgres, iso-recall 정면승부

같은 SQL surface·MVCC·heap·운영 모델. **유일한 진짜 정면승부.**

| 시스템 | 엔진 | 지지 클레임 | 공정 조건 |
|--------|------|-------------|-----------|
| **pgvector** (HNSW/IVFFlat) | CPU | "Postgres에 GPU 붙일 가치" 기준선 — 반드시 이겨야 함 | `ef_search` sweep 상한까지; `iterative_scan` 켠 상태(필터 셀) |
| **pgvectorscale** | DiskANN CPU/NVMe | RAM-경계 working-set의 in-PG 대안 | `search_list_size` 튜닝; working-set 스케일에서만 (10억은 Ring D) |
| **VectorChord** (vchord) | RaBitQ/IVF CPU | 최신 in-PG 경쟁자 | `probes` 튜닝; clustered 합성의 IVF 편향 보정(실 데이터 병행) |

### Ring B — 오버헤드 앵커: 같은/유사 커널, DB 없음

목적은 QPS 슛아웃이 아니라 **통합 세금의 상한** 측정. 저들은 durability/MVCC/SQL 세금을 안 낸다.

| 시스템 | 역할 |
|--------|------|
| **python cuVS** (raw) | 가장 깨끗한 앵커 — pg_cuvs의 바로 그 백엔드. "raw cuVS의 ~N%" 클레임을 정당화하는 유일한 상대 |
| **faiss-gpu** | 2차 GPU 라이브러리 참조 — pg_cuvs GPU 경로가 커널 성능을 흘리지 않는지 sanity 체크 + 대규모 GT 생성기(§9) |
| **faiss-cpu** | CPU 거울 — pgvector도 faiss-cpu 대비 "Postgres 통합 세금"을 낸다는 대칭 인사이트 |

### Ring C — 외부 벡터 DB: 다른 data plane (별도 system-level 문서)

| 시스템 | 정당성 | 필수 보정 |
|--------|--------|-----------|
| **milvus** | Milvus-GPU도 cuVS CAGRA 백엔드 → *같은 커널, 다른 호스트 통합* 격리 비교 | QPS 단독 보고 금지 |
| **qdrant** | "별도 벡터 DB를 둘 것인가"의 시장 대안 | 운영 비대칭 명기 |
| **lancedb** | 임베디드/디스크 레짐 — 분모가 다름, 참조용 | 정면 비교 부적합 |

**Ring C 규약**: pg_cuvs = 1 시스템(벡터+관계 co-located, 트랜잭션). 외부 DB = 2 시스템 + ETL 동기화
+ eventual consistency + 별도 운영. 이 아키텍처 비용을 함께 보고하지 않는 QPS 비교는 양쪽 모두에게
불공정 → latency 분해 벤치가 아니라 **별도 system-level 비교 문서**로 분리한다.

### Ring D — 링 밖: 천장 기록

10억+ / larger-than-VRAM / 저QPS 아카이브(벡터당-비용 지배)는 head-to-head가 아니라 **"우리가 안
노는 천장"**으로만 기록한다 (기존 50M 결과가 전례: CAGRA FAILED를 그대로 공개).

---

## 2. 자원 공정성 모델 (P1 해법)

**원칙: DRAM과 VRAM을 GB로 등화하지 않는다. $/hr과 J/query로 등화하고, raw 자원을 전부 공개해
독자가 자기 가격표로 재정규화할 수 있게 한다.**

### 2.1 두 개의 자원 baseline (둘 다 보고)

| Baseline | 정의 | 누구에게 공정한가 |
|----------|------|--------------------|
| **(a) same-box** | GPU 인스턴스에 딸려오는 CPU/DRAM 전체로 CPU 엔진 실행 (예: `a2-highgpu-1g` = A100-40GB + 12 vCPU + 85GB RAM) | "이미 GPU 박스가 있다" 시나리오. CPU를 굶기지 않음 — CPU에 보수적 |
| **(b) iso-$** | GPU 인스턴스와 시간당 단가가 같은 CPU 전용 인스턴스 (GPU 값을 안 내므로 vCPU/RAM이 훨씬 큼) | "greenfield, 같은 돈을 어디 쓸까". CPU에 가장 유리 |

VRAM은 GB당 단가가 DRAM보다 훨씬 높다 → iso-$에서 CPU가 GB를 많이 받는 것이 **요점**이다.
CPU의 강점(싸고 풍부한 메모리) vs GPU의 강점(대역폭·연산)의 트레이드를 Pareto 곡선이 포착하고,
교차점이 산출물이다. 인스턴스 단가는 **실행 시점 실제 단가**를 결과 행에 기록한다(가격 변동 대비).

### 2.2 항상 기록하는 raw 자원 (재정규화 가능성 보장)

peak VRAM · peak host RSS · CPU-core-seconds · GPU-seconds · disk written · WAL bytes ·
index size(VRAM/host/disk 각각) · **Joules**(GPU: `nvidia-smi --query-gpu=power.draw` 적분,
CPU: RAPL `powercap` — 둘 다 불가 시 N/A 명기).

### 2.3 환산 지표

`$/1M queries` · `$/sustained-QPS@SLO(p99)` · `J/query`. perf/Watt는 논문 트랙의 독립 축
($/QPS에서 져도 J/query에서 이기는 구간이 존재할 수 있다).

---

## 3. 실험 축과 셀 정의

### 3.1 공통 축

| 축 | 값 | 비고 |
|----|----|----|
| **N** | 1K, 10K, 100K, 1M, 10M (+ 이분점, §5.2) | 50M은 Ring D 천장 셀만 (§8.6) |
| **dim / 데이터** | **1024 실 임베딩(Cohere Wikipedia, 1차)** + 384 합성(메커니즘·스케일링 검증용) | 합성 recall은 진실값 아님 (§10) |
| **k** | 10, 100 | |
| **recall target** | 0.95, 0.99 | iso-recall 등화점 (§3.3) |
| **config** | `forced-hnsw` / `forced-cuvs` / `auto` (+ N≤100K에서 `forced-seqscan` 참조) | auto는 Stage B 전에는 EXPLAIN-only |
| **동시성** | Stage A: c ∈ {1, 8}. 전체 sweep {1,8,32,128}은 Stage D | |
| **상태** | Stage A: warm 고정. cold/warmup은 Stage D 별도 모듈 | |

### 3.2 노브 공간 — phase 바인딩 + self-tuning 규칙 (차원 폭발 제어)

격자 전수 탐색을 금지한다. 대신:

1. **노브를 phase에 바인딩**:
   - `maintenance_work_mem`, `max_parallel_maintenance_workers` → **빌드 셀만**
   - `shared_buffers`, `effective_cache_size` → **쿼리 상주** (셀별 sweep 아님 — baseline당 1회 설정)
   - `max_parallel_workers_per_gather` → **pgvector 쿼리 병렬** (CPU 엔진의 지배 노브)
2. **엔진별 self-tuning (노브 교차 금지)**: 각 (N, recall) 셀에서 *각 엔진이 자기 지배 노브 2–3개만*
   작은 sweep → **자기 Pareto-best 점**을 plot. CPU 노브는 pgvector 최적화에, GPU 노브(`cuvs.k`,
   `shard_count`, `graph_degree`)는 pg_cuvs에. 각자 제 최선이므로 공정하다.
3. **사용한 모든 파라미터는 `params_json`으로 결과 행에 공개** (튜닝 주관성 방어).
4. **자원 예산은 §2.1의 2점만** — 예산 자체를 sweep하지 않는다.

### 3.3 iso-recall 등화 절차

각 엔진의 recall 노브(`hnsw.ef_search` / `vchord probes` / `cuvs.k`·`itopk`)를 sweep해 target을
만족하는 **최소값**을 채택, 그 작동점의 latency/QPS/자원을 비교한다. sweep 상한에서도 미달이면
달성 recall을 행에 병기하고 미달로 표기한다(기존 §11 방식). BF 모드는 구성상 recall=1.0 — exact
경로는 등화 없이 별도 행.

---

## 4. 관측 스키마 (생명주기 × 자원 — 모든 셀의 백본)

모든 `(system, phase, cell)`에 대해 동일 튜플을 기록한다.

| Phase | 성능 | 자원 |
|-------|------|------|
| **build** (CREATE INDEX) | wall-clock, 처리량(rows/s) | peak VRAM, peak RSS, CPU-core-s, GPU-s, disk written, WAL bytes, index size(VRAM/host/disk) |
| **maint** (insert/upsert/delete, VACUUM, compact, REINDEX) | per-op latency, 처리량, **recall drift(시계열)** | 시계열 VRAM/RSS, index 성장, delta backlog(`delta_rows`), compaction 이벤트(`extend_count`/`compact_count`) |
| **query** (steady-state) | QPS, p50/p95/p99/p99.9, recall@k | 쿼리 중 peak VRAM/RSS, CPU/GPU util, J/query |

### 결과 CSV 스키마 (`bench/results/protocol/*.csv`)

기존 CROSSOVER §6 스키마를 확장. **append-only·불변, 행 단위 완결**(행만 보고 재현 가능해야 함):

```
run_id, date, stage, phase, cell_id, config,
system, system_version, system_commit, index_type,
N, dim, k, recall_target, dataset, query_set_id, seed,
clients, warm_state,
-- 성능
build_s, qps, p50_us, p95_us, p99_us, p999_us, avg_latency_us, recall_at_k,
-- 자원
peak_vram_mb, peak_rss_mb, cpu_core_s, gpu_s, energy_j,
disk_bytes_written, wal_bytes, index_bytes_vram, index_bytes_host, index_bytes_disk,
-- 경제
instance_type, price_usd_hr, usd_per_1m_queries,
-- 통계
reps, agg_method, dispersion,            -- 예: median-of-5, iqr=...
-- 정확도 근거
gt_method,                               -- exact-cpu | faiss-gpu-flat | windowed
-- 버전 게이트 (§6.4)
cost_model_version, runtime_routing_version,
-- 모듈별 (해당 시)
selectivity, correlation, filter_mode,   -- M1
stream_op, ops_done, delta_rows,         -- M2
params_json, notes
```

Stage B 플래너 추정 덤프는 별도 파일(§6.2).

---

## 5. Stage A — 교차-밀집 물리 실측 (1회, 불변 자산)

**forced 곡선만 측정한다.** 이 결과는 코스트모델을 아무리 고쳐도 무효화되지 않는다.

### 5.1 측정 대상

- `forced-hnsw`(pgvector), `forced-cuvs`(CAGRA), N≤100K에서 `forced-seqscan` + `forced-bf`
  (ADR-039 BF 자동선택 경계 확인용 — 플래너의 3원 선택 seqscan/hnsw/cuvs 검증에 필요).
- 축: §3.1. baseline은 same-box 우선(iso-$ 인스턴스는 Stage D1에서 합류).
- **코스트모델이 소비하는/소비해야 할 변수를 셀 축에 반드시 포함**: rows(N), k, (필터 셀의)
  selectivity. cold/warm·delta backlog는 plan-time 인자 후보로 Stage D에서 별도 특성화(§8.7).

### 5.2 적응적 이분탐색 규칙

```
1. 거친 log-격자 실측: N ∈ {1K, 10K, 100K, 1M, 10M}
2. 각 인접 구간에서 ratio(N) = metric_cuvs(N) / metric_hnsw(N) 계산
   (metric = iso-recall p50 latency; QPS·$/QPS 교차는 동일 규칙 별도 적용)
3. ratio가 1을 가로지르는 구간만 log-공간 중점 분할
4. 종료 조건: 구간 양끝 N 비율 < 1.8× 또는 양끝 곡선 차이 < ε(10%)
5. 구간당 최대 3회 분할 (예상: 10K–100K → ≈18K, 32K, 56K)
```

교차는 점이 아니라 **면**이다: dim·k·동시성·selectivity가 교차 N을 이동시킨다. 1차 축(N)에서
이분 완료 후, 나머지 축은 교차 N이 어느 방향으로 얼마나 밀리는지 축당 2–3점만 측정한다.

### 5.3 실행 형태

- 양 인덱스를 **같은 컬럼에 공존**시킨다 (planner-auto의 자연 실험 조건과 동일하게).
  forced는 GUC 게이팅(`enable_cuvs=off` / `SET enable_indexscan` 조합)으로 플랜을 고정.
- 빌드 phase는 인덱스별 독립 측정(공존 전, 단독 빌드로 자원 회계 오염 방지).
- 모든 행에 §4 스키마 완결 기록.

---

## 6. Stage B — 코스트모델 보정 루프 (오프라인, 싸다, 자유 반복)

### 6.1 루프 구조

```
EXPLAIN 스윕(전 셀, 실행 없음, 셀당 ~ms)
  → regret 셀 목록 산출 (§6.3)
  → plan-time 상수 수정 (cuvsamcostestimate)
  → EXPLAIN 재스윕 (수 분)
  → 반복, 합격 기준 충족까지
```

### 6.2 EXPLAIN 스윕 산출물 (`bench/results/protocol/planner_est_*.csv`)

셀마다 양(3) 경로의 추정치 덤프:

```
cell_id, cost_model_version, path,        -- path ∈ {seqscan, hnsw, cuvs}
est_startup, est_total, est_rows,
chosen,                                    -- 플래너 1순위 여부
measured_ref                               -- Stage A 동일 셀 실측 행 참조
```

### 6.3 합격 기준 (regret, ε-밴드)

```
regret(cell) = measured(플래너 선택 엔진) − measured(최선 엔진)     [iso-recall p50 기준]
ε-무차별 밴드: 두 forced 곡선 차이 < 10% 인 셀 = don't-care
합격: ε 밴드 밖에서 regret > 0 인 셀 = 0개
보고: 오선택 셀 목록을 regret 크기 내림차순으로 (가장 아픈 버그부터)
```

ε-밴드의 좌표 자체가 산출물이다 — "곡선이 겹치는 구간"의 정확한 위치. 그 안은 어느 선택도 정당.

### 6.4 오염 경계 규율 (분리가 깨지는 곳)

물리/판단 분리는 수정이 **plan-time 상수**(`CUVS_STARTUP_COST`/`CUVS_K_COST`/`CUVS_ROWS_COST` 및
`cuvsamcostestimate` 내부 인자)에 머물 때만 성립한다.

- **루프 중 허용**: plan-time 상수/공식 수정만.
- **runtime 라우팅**(`filter_auto_threshold`, `stream_bf_selectivity_threshold`, delta merge 경로,
  search_mode 내부 자동선택 등) 수정이 필요하다고 판명되면 — 그 자체가 발견 — **영향받는 셀만
  스코프 재실측** 후 루프 복귀.
- 모든 결과 행에 `cost_model_version` + `runtime_routing_version` 태그. 버전 불일치 행 혼합 집계 금지.

### 6.5 수렴 후 확인 실행 (소량)

planner-auto **실행**은 (a) 결정 경계 근방 셀, (b) 무작위 spot-check 셀만 — "auto가 고른 플랜의
실측 ≡ 해당 forced 셀 실측"(plan-time 오버헤드 차이 없음) 확인용. 전 격자 재실행 불필요.

---

## 7. Stage C — 동결·버전 태깅

- `cost_model_version`을 올리고(소스 상수 옆 주석 + 결과 행), 보정 보고서
  `docs/cost-model-calibration.md`를 산출: before/after 상수, regret 표, ε-밴드 좌표,
  도달 불가 구간(§13)과 그 원인 분류.
- **동결 이후에만 Stage D 진입.** 동결 전 auto-config 결과는 폐기 대상이다.

---

## 8. Stage D — 동결된 모델 위의 전체 스위트

### D1. 자원-성능 Pareto + $ 정규화 (P1 본체)

Stage A 셀 재사용 + **iso-$ CPU 인스턴스 합류**(§2.1b). 산출: (N, recall) 셀별
`$/1M queries`·`$/QPS@SLO` Pareto frontier, same-box/iso-$ 양 축, 교차 좌표 표.

### D2. M1 — 필터 (selectivity × correlation)

실사용 1차 시나리오이자 차별화 본령(ADR-061 D-wedge).

| 축 | 값 |
|----|----|
| selectivity | 0.1%, 1%, 5%, 10%, 50% |
| correlation | random / mixed / spatial (기존 `docs/filter-threshold-experiment.md` 정의 재사용) |
| N | 1M (1차), 100K(보조) |
| pg_cuvs 경로 | auto 라우팅(D-wedge/3O/stream-bf) + forced 변형별 |
| pgvector 경로 | **`hnsw.iterative_scan` ∈ {off, strict_order, relaxed_order}** — 끄고 비교하면 불공정 |
| 측정 | recall@k, QPS, **p99 tail** |

예상 차별화 셀: pg_cuvs filtered BF는 구성상 exact·latency flat; iterative_scan은 근사이고
저selectivity에서 tail 폭발(필터 통과 행을 채우려 그래프 계속 스캔). 산출 = "selectivity 하강 시
목표 recall R 유지하며 내는 QPS" 곡선. **필터 경로의 runtime 라우팅 임계 보정은 §6.4 규율 적용**
(스코프 재실측).

### D3. M2 — 증분 insert/upsert + FIFO

3A(.delta)·3Q(EXTEND/Merge)·4C(auto compaction)·VACUUM tombstone 시험. **정직 조건**: pgvector
HNSW는 native mutable(WAL-logged, 데몬 왕복 없음) — 구조적 강점 구간을 그대로 보고.

| 시나리오 | 내용 |
|----------|------|
| (a) 연속 append | base N₀ 빌드 후 스트림 INSERT |
| (b) FIFO 윈도우 | head INSERT + tail DELETE, 윈도우 크기 고정 |
| (c) upsert 혼합 | UPDATE 비율 혼합 |

경로: pg_cuvs EXTEND(실시간 가시성, ~68ms/행) vs delta-append+compaction(~0.028ms/행) vs
pgvector native insert. 시계열 곡선: ingest 처리량 · **recall drift**(체크포인트마다 윈도우 GT
재계산) · **ingest 중 동시 query QPS/p99** · VRAM/RSS 성장 · compaction 이벤트 · "언제 rebuild가
필요한가" 임계. 결론 틀: "EXTEND = 저빈도 실시간 가시성, delta+compaction = bulk" 트레이드 정량화.

### D4. 동시성·tail under load

c ∈ {1, 8, 32, 128} (pgbench). GPU 단일 데몬 천장(~1K QPS 소규모) vs CPU 코어 스케일 교차 곡선,
p99/p99.9 (데몬 큐잉 tail spike). 온라인 RAG 클레임의 SLO 근거.

### D5. cold-start / warmup / 멀티테넌트 LRU

time-to-first-query-at-target-QPS: GPU VRAM reload(실측 ~150MB/s) vs pgvector `shared_buffers`
warm. 멀티테넌트 LRU churn(STRATEGY_NOTES §G) + GCS warmup(3C/3D) 연계. 캐시-미스 꼬리 분포.

### D6. 천장 셀 (Ring D)

50M×384(+가능 시 50M×1024): 경쟁자 ceiling + pg_cuvs는 GPU-search **N/A(VRAM 천장)를 그대로
기록**, 3I build-accelerator 레인과 stream-BF(ADR-064)만 해당 스케일에서 의미. multi-GPU 샤딩
스케일 곡선(shard_count sweep)은 여기 부속.

### D7. 3I build-accelerator 별도 레인

"GPU 빌드 → pgvector HNSW 서빙" 경로는 쿼리 축이 pgvector와 동일하므로 **빌드 축만**의 비교다.
Ring A 표에 섞지 않고 별도 레인으로 보고 (기존 §13/§16/§17 계승).

---

## 9. Ground Truth 전략 (스케일별)

| N | GT 방법 | 비고 |
|---|---------|------|
| ≤ 1M | exact brute-force (numpy/faiss-cpu flat) | 기존 `bench/gt.py` |
| ≥ 10M | **faiss-gpu flat** (exact) | cuVS와 독립 구현 → 측정 대상(cuVS CAGRA)과의 순환성 회피 |
| M2 시계열 | 체크포인트마다 윈도우 GT 재계산 (faiss-gpu) | drift 측정의 전제 |

셀마다 `gt_method` 기록. 쿼리셋은 고정(1,000 queries, seed 고정, `query_set_id`로 식별).

---

## 10. 데이터셋 전략

- **1차 앵커 = 실 임베딩**: Cohere Wikipedia 1024d (기존 §16 하네스 재사용, 10M까지 확장).
  recall·교차 좌표의 진실값은 실 데이터에서만 주장한다.
- **합성은 메커니즘·스케일링 전용**: clustered는 IVF 편애, uniform random은 recall=1.0 함정 —
  기존 CROSSOVER "데이터 방법론 요약"의 주의를 전 셀에 계승. 합성 결과에 recall 클레임 금지.
- 분포 민감도가 의심되는 결론(교차 좌표 등)은 실/합성 양쪽 표기.

---

## 11. 통계 규약

- **반복**: latency 셀당 ≥5 reps(고정 쿼리셋 1,000 queries/rep); QPS는 60s 윈도우 ×3, 첫
  윈도우(warmup) 폐기. 보고 = median + 산포(IQR 또는 std), `reps`/`agg_method`/`dispersion` 행 기록.
  **단일 측정 보고 금지.**
- **환경 통제**: CPU baseline은 NUMA 핀닝(`numactl`) — 12-vCPU pgvector가 NUMA 무시로 불공정하게
  느려지는 것 방지. governor=performance, THP 상태 기록. cloud noisy-neighbor는 논문 트랙에서
  날짜 분산 반복으로 완화.
- **warm 정의 통일**: pg_cuvs = daemon resident(인덱스 로드 완료 확인), pgvector = 인덱스 전체
  `pg_prewarm` 또는 충분한 사전 쿼리. cold는 D5에서만, 정의 명시.

---

## 12. 재현성 규약

- 고정·기록: pg_cuvs commit, pgvector/pgvectorscale/vchord 버전+커밋, PG 버전, cuVS/CUDA/드라이버
  버전, 인스턴스 타입·zone, 실행 시점 단가.
- 모든 run에서 `SELECT name, setting FROM pg_settings WHERE source <> 'default'` 덤프 동봉.
- `bench/results/protocol/`은 append-only. 수정·재실측은 새 `run_id`로.
- 논문 트랙은 artifact 평가 기준(스크립트+데이터+결과 번들로 제3자 재현 가능)을 충족시킨다.

---

## 13. 정직한 한계 (사전 등록)

- **plan-time에 원리적으로 알 수 없는 신호**: cold/warm 상주 여부, delta backlog, 데몬 큐 깊이.
  상수 보정으로 envelope에 도달할 수 없는 구간이 존재할 수 있다 — 이는 보정 실패가 아니라
  **"plan-time 정보 부족" 클래스의 발견**으로 분류·문서화한다. 처방은 상수가 아니라 런타임
  적응(예: cold면 첫 쿼리 fallback) 후보로 백로그에 적재.
- **GB 등화 불가능성**: §2의 $/J 등화도 가격표 의존이다 — raw 공개로 보완하나, 단일 "공정" 숫자는
  존재하지 않음을 명시한다.
- **pg_cuvs는 WAL-logged mutable native index가 아니다** — 비교의 전제(정적/배치 인덱스 + 별도
  freshness 경로)를 모든 공개 산출물에 명기 (PROJECT_POSITIONING 계승).

---

## 14. 우선순위 — 두 트랙

### Track P (제품·코스트모델, 엔지니어링 구동)

| # | 항목 | 비용 | 비고 |
|---|------|------|------|
| **P0** | 관측 하네스 + 자원 회계 (§4 스키마 구현: VRAM/RSS/core-s/GPU-s/J/$ 수집기) | 중 | **전부의 전제** |
| **P1** | **Stage A** 교차-밀집 물리 실측 (same-box, 1K–1M 밀집 + 10M 1점, 실 임베딩 1차) | 중 | 불변 자산 |
| **P2** | **Stage B→C** 보정 루프 + 동결 (`docs/cost-model-calibration.md`, `cuvsamcostestimate` 수정) | 저–중 | **최고 ROI** — 플래너 버그 목록 직접 산출 |
| **P3** | **D2** 필터 M1 (iterative_scan 포함) | 중 | 실사용 1차 + 차별화 본령 |
| **P4** | **D3** 증분 M2 | 중 | freshness 구현 입증 |
| **P5** | **D1** iso-$ Pareto | 저 (셀 재사용 + 인스턴스 1종 추가) | P1 목적 완결 |

### Track R (논문 — 제품 안정화와 독립 일정)

| # | 항목 | 비용 |
|---|------|------|
| R1 | 10M 전 셀 확장 + 실 임베딩 전면 + 반복·분산 풀셋 + energy(J/query) | 높음 |
| R2 | D4 동시성/tail 전체 곡선 | 중 |
| R3 | D5 cold-start/warmup + 멀티테넌트 LRU | 중 |
| R4 | D6 천장 + multi-GPU 스케일 + D7 레인 | 높음 |
| R5 | Ring B 앵커 정량화(python cuVS/faiss) + Ring C system-level 별도 문서 | 중 |

논문 서사 후보(참고): (i) 통합 세금 분해(Ring B 앵커, BENCHMARK.md §1 계승), (ii) 코스트모델
보정 방법론 자체(물리/판단 분리 + regret — Stage B 산출물이 그대로 evaluation), (iii) plan-time
정보 부족 구간의 분류와 런타임 적응(§13).

---

## 15. 산출물 목록

| 산출물 | 위치 |
|--------|------|
| 원시 결과 (불변) | `bench/results/protocol/*.csv` + `planner_est_*.csv` |
| 보정 보고서 | `docs/cost-model-calibration.md` |
| 코스트모델 수정 | `src/pg_cuvs.c` `cuvsamcostestimate` + `cost_model_version` |
| 교차 좌표/ε-밴드 표 | `BENCHMARK.md` 갱신 (동결 후) |
| 필터·증분 곡선 | `BENCHMARK.md` §3 확장 + 논문 트랙 figure |
| Ring C 비교 | 별도 문서 (착수 시 명명) |
