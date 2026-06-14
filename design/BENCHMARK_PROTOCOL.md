# pg_cuvs 벤치마크 실험 프로토콜 (v3)

> **이 문서는 무엇인가**
> 우리가 직접 엄밀한 벤치마크를 돌리기 위한 **내부 실험 설계서**다.
> 운영자용 절차서(`docs/playbooks/benchmark-runbook.md`)가 아니다.
>
> **무엇을 고정하는가**: 엔진 인벤토리 · 경쟁자 선정 · 자원 공정성 · 셀 정의 · 단계 순서 · 합격 기준 · 통계 규약.
>
> **계보**: v2(ADR-069)를 잇는다. **v3 = 구현 반영 개정** — flat AM(A1)·transient BF(B)·하드웨어-포터블 물리 코스트모델이 머지(main 0.5.0, ADR-073/074/075)된 현실 위에 다시 세운다.
> 공개 산출물은 [`BENCHMARK.md`](../BENCHMARK.md), 원시 데이터는 `bench/results/`. 결정 근거: ADR-069(프로토콜)·ADR-073(엔진)·ADR-074(특성화)·ADR-075(코스트모델).

---

## v2 → v3 변경 요약 (왜 다시 쓰나)

구현과 실측이 v2의 전제 세 개를 바꿨다.

1. **엔진 인벤토리가 달라졌다.** v2의 `forced-bf`(cagra `search_mode` 옵션)는 1급 **`flat` AM(A1, 상주 exact GPU BF)** + **transient BF(B, 무인덱스)**로 승격·분리됐다(ADR-073). "HNSW vs CAGRA는 플래너 옵션이 아니다" — 한 컬럼은 한 인덱스만 가지므로 HNSW는 경쟁자(Ring A)일 뿐 플래너 결정면이 아니다. **플래너 결정면 = {cpu-seqscan, flat(GPU exact 상주), cagra(GPU approx), ivfpq, transient-B}**.
2. **코스트모델이 상수 휴리스틱에서 데이터-이동 물리 분해로 바뀌었다**(ADR-075, 구현됨). 그래서 Stage B는 더 이상 "`CUVS_STARTUP_COST` 상수를 내린다"가 아니라 **"물리 공식 + 하드웨어 probe가 옳게 라우팅하는지 검증한다"**가 된다.
3. **실측이 포지셔닝을 수정했다**(ADR-074). 벡터 kNN은 **데이터-이동 바운드**이고 **TOAST detoast가 벽**이다. **GPU는 상주(A1)일 때만 이긴다**(HBM에서 연산). transient-B는 PCIe에선 잉여(≈CPU), 쓰기-heavy는 pgvector 무인덱스가 정답. → 벤치마크의 헤드라인 질문이 "GPU vs CPU QPS 슛아웃"에서 **"상주-GPU가 detoast-바운드 CPU를 어디서·왜 이기고, 코스트모델이 거기로 옳게 라우팅하는가"**로 이동한다.

---

## 한눈에

**세 가지를 증명·개선·검증한다.**

- **P1 — 공정성**: "GPU에 VRAM 40GB, CPU엔 4GB·2프로세스"식 기울인 비교를 구조적으로 막는다($/J 등화).
- **P2 — 플래너**: planner-auto가 모든 구간에서 최적 엔진을 골라야 한다. 코스트모델이 못 고르는 지점을 고친다.
- **P3 (신규) — 물리 코스트모델 검증**: 코스트는 이제 데이터-이동 물리 분해(ADR-075)다. 벤치마크는 **(a) detoast-aware 라우팅이 dim/저장별로 옳게 움직이는지, (b) 하드웨어 probe/DEFAULT 폴백이 이식 가능하고 안전한지**를 실측 cross-check한다.

**핵심 아이디어 5개.**

| # | 아이디어 | 한 줄 |
|---|----------|-------|
| 1 | **물리 / 판단 분리** | 비싼 실측(forced 곡선)은 1회. 플래너 검증은 EXPLAIN만 → 코스트모델 고쳐도 전체 재실행 안 함 |
| 2 | **보정 루프 선행** | 코스트모델 동결 전엔 planner-auto가 끼는 스위트를 안 돌린다 |
| 3 | **$ / J 등화** | DRAM·VRAM을 GB로 등화 안 함. 돈·전력으로 등화, raw 자원 전면 공개 |
| 4 | **Ring 구조** | 경쟁자를 4링으로 분리, 시스템마다 공정한 단 하나의 링에만 |
| 5 | **데이터-이동이 비용이다** | kNN은 memory-bound. 측정·코스트·포지셔닝 모두 detoast·H2D·상주를 1급 변수로 |

**실행 순서.**

```
Stage A  교차-밀집 물리 실측 ─── forced 곡선(cpu-seq/flat/cagra/transient-B). 코스트모델 무관 불변 자산
   │                            + 데이터-이동 분해(scan/detoast/move/compute) 측정
   ▼
Stage B  코스트모델 검증 루프 ── EXPLAIN 스윕(물리 공식) + Tier-2 실측 cross-check
   │                            합격: ε-밴드 밖 regret>0 셀 0개 + 판별 플립 정확 + DEFAULT 폴백 안전
   ▼
Stage C  동결 · 버전 태깅 ───── cost_model_version + hw_profile 버전. 여기 전엔 auto 스위트 금지
   ▼
Stage D  전체 스위트 ────────── 필터(M1)·증분(M2)·$-Pareto·동시성(SLA-bounded)·저장(PLAIN)·빌드비용·논문
```

**왜 이 순서인가**: 코스트모델을 고치면 planner-auto의 모든 결과가 무효화된다. 보정(B)을 먼저 끝내 동결(C)한 뒤 비싼 스위트(D)를 돌린다. 보정은 물리(A)를 재사용하므로 싸다. **단, v3의 코스트모델은 이미 구현(ADR-075)**되어 있어 Stage B는 "수정 루프"보다 **"검증 + 잔여 오보정 교정"**에 가깝다.

---

## 0. 구현된 엔진 인벤토리 & 플래너 결정면 (신규)

main 0.5.0 기준 실제 경로. **한 cagra/flat 인덱스 + heap이 여러 실행 경로를 제공**하므로, 결정면은 "어느 인덱스를 빌드했나" + "플래너가 무엇을 고르나"의 곱이다.

| 경로 | 무엇 | recall | 빌드 | 신선도 | 비용 동인 |
|------|------|--------|------|--------|-----------|
| **cpu-seqscan** (pgvector 무인덱스) | CPU exact | 1.0 | 없음 | 항상 최신 | **detoast** + 연산(memory-bound) |
| **HNSW** (pgvector) | CPU ANN | <1 | 그래프 | mutable(WAL) | 그래프 탐색 (Ring A 경쟁자, 플래너 옵션 아님) |
| **flat AM (A1)** `USING flat WITH(precision)` | **GPU exact 상주** | 1.0 | **vectors-only(O(N) 복사, 그래프 없음)** | cagra delta/tombstone 재사용 | 빌드 1회 분할상환 → 읽기 **HBM 연산 ~1ms** |
| **cagra** `USING cagra` | GPU ANN 상주 | <1(target) | 그래프 | delta/tombstone | N-독립 ~ms (graph) |
| **ivfpq** `USING ivfpq` | GPU PQ 상주 | <1 | 클러스터+PQ | rebuild-only | sublinear |
| **transient-B** `SET cuvs.gpu_bruteforce=on` | GPU exact 무인덱스 | 1.0 | **없음** | 항상 최신 | **매쿼리 detoast+H2D** (PCIe에선 ≈CPU) |

**라우팅 규칙 (ADR-075, 구현됨)**:
- 코스트 = `scan(N) + detoast(m,storage) + move(m,link) + compute(m,engine) + topk(m,k) + fetch(k)` (행 수 휴리스틱 아님).
- **exact-우선 단방향**: exact(cpu-seq/flat/B)가 cagra보다 **싸거나 같으면 exact**; 더 비싸면 cagra 유지(사용자가 DDL로 속도-위해-recall 선택). recall을 비용으로 거래하지 않는다.
- 멘탈 모델: **"flat 인덱스 생성 여부 = read-heavy(W2)/write-heavy(W1) 스위치."** transient-B는 `on` 전용(experimental, ADR-074).

**측정된 포지셔닝(ADR-074, A100/PCIe, 100k×768)**: 읽기→**A1 1.09ms**(seqscan 559ms 대비 ~500×) / 쓰기→**pgvector 무인덱스**(A1 쓰기 1.77ms/행 = 무인덱스 0.13의 ~13×) / **transient-B = ≈CPU**(1103ms, PCIe 잉여 — 통합메모리 GH200/MI300A에서만 1급).

### 0.1 구현 세부 (GUC · 진단 · 제약) — 셀 정의/검증이 의존

- **레거시 BF deprecated**: `cuvs.search_mode='brute_force'`(cagra 인덱스 위 옵션)는 flat AM(A1)으로 대체·deprecated(ADR-073/039). 벤치는 flat을 1급 exact BF 경로로 측정하고, 레거시는 회귀/등가 확인용만.
- **VRAM 예산 축**: flat `.vectors` = `N·dim·4B`(1M×1024 ≈ 4GB), `WITH(precision='float16')`로 절반. 빌드시 free VRAM 초과 → 전용 ERROR → `precision=float16` / 샤딩 / **ivfpq**(PQ로 VRAM 10–100× 절감, approx)로 강등. → **D1/D6에 VRAM-예산 셀** 1급 축.
- **transient-B 제약**: 호스트 코퍼스 캡 `cuvs.gpu_bruteforce_max_mb`(기본 2048MiB, 초과=fail-closed ERROR·데몬 생존), **비축출 VRAM admission**(one-shot 코퍼스가 상주 flat/cagra를 **절대 안 밀어냄**). → D2/D4 transient 셀의 구조적 제약.
- **freshness/유지보수**: flat 쓰기 = delta append(INSERT/UPDATE, cagra 3A/3Q 경로 상속), 주기 compaction = **REINDEX 재빌드**(flat은 in-place compact 없음 — `pg_cuvs_compact`는 `e->handle` 요구라 cagra 전용). → D3 축에 직접 반영. MVCC: gettuple은 TID만 반환+`xs_recheck=true`(GPU 코퍼스/delta/tombstone = 후보 생성기일 뿐, executor가 스냅샷 recheck) → isolation 6/6 GREEN 검증됨.
- **진단/관측 표면 = `pg_cuvs_hw_profile()`**: `gpu_name · source∈{measured,default} · probe_status(측정된 계수 비트마스크) · matches_running_daemon · link_bw · hbm_bw · gpu_cagra_lat_us · ipc_rtt`. `<index_dir>/cuvs_hw_profile` 사이드카(magic+version+CRC+env_tag). Stage B 검증·§12 재현성·라우팅 레짐 판정의 단일 관측 창구.

---

## 1. 경쟁자 — Ring 구조

> **규칙**: 각 시스템은 비교가 공정해지는 **단 하나의 링**에만, 명시된 클레임을 지지할 때만.

### Ring A — 1차 링 (in-Postgres 정면승부)
같은 SQL·MVCC·heap. iso-recall 등화. **HNSW는 여기 산다(플래너 옵션이 아니라 배포 대안).**

| 시스템 | 엔진 | 클레임 |
|--------|------|--------|
| **pgvector HNSW/IVFFlat** | CPU ANN | "GPU 붙일 가치" 기준선 — 반드시 이겨야 함 |
| **pgvector 무인덱스(seqscan)** | CPU exact | **쓰기-heavy(W1)의 정답** — A1이 여기선 13× 쓰기세로 짐(ADR-074) |
| **pgvectorscale** | DiskANN CPU/NVMe | RAM-경계 working-set 대안 |
| **VectorChord** | RaBitQ/IVF CPU | 최신 in-PG 경쟁자 |

튜닝: pgvector `ef_search`(+필터 셀 `iterative_scan`), pgvectorscale `search_list_size`, vchord `probes`.

### Ring B — 오버헤드 앵커 (같은 커널, DB 없음)
> "Postgres 통합이 bare-metal 대비 얼마를 먹나"의 상한. QPS 슛아웃 아님.

| 시스템 | 역할 |
|--------|------|
| **python cuVS (raw)** | 가장 깨끗한 앵커 — pg_cuvs의 바로 그 백엔드. "raw의 ~N%" 근거 |
| **faiss-gpu** | 2차 GPU 참조 + 대규모 GT 생성(§9) |
| **faiss-cpu** | CPU 거울 — pgvector도 통합세를 낸다는 대칭 |

### Ring C — 외부 벡터 DB (별도 system-level 문서)
milvus(같은 CAGRA 커널, 다른 호스트) · qdrant(시장 대안) · lancedb(임베디드). **QPS 단독 보고 금지, 운영 비대칭 병기.** pg_cuvs=1시스템(co-located) vs 외부=2시스템+ETL+eventual consistency.

### Ring D — 링 밖 (천장 기록)
10억+ / larger-than-VRAM = 벡터당-비용 지배. head-to-head 아닌 천장만(전례: 50M CAGRA FAILED 그대로).

---

## 2. 자원 공정성 (P1 해법)

> DRAM·VRAM을 GB로 등화하지 않는다. **$/hr·J/query로 등화**, raw 전면 공개.

### 두 baseline 동시 보고
| Baseline | 정의 | 누구에 공정 |
|----------|------|-------------|
| **(a) same-box** | GPU 인스턴스 부속 CPU/DRAM 전체로 CPU 엔진 | "이미 GPU 박스 있다" |
| **(b) iso-$** | GPU 인스턴스와 시간당 단가 같은 CPU 전용 | "같은 돈 어디 쓸까" |

예: `a2-highgpu-1g` = A100-40GB + 12 vCPU + 85GB. (a)는 이 자원으로 pgvector, (b)는 동일 $/hr CPU 박스(더 큰 vCPU/RAM).

### 항상 기록하는 raw 자원
peak VRAM · peak host RSS · CPU-core-s · GPU-s · disk · WAL · index size(VRAM/host/disk) · **Joules**(GPU `power.draw` 적분 / CPU RAPL, 불가 시 N/A) · **detoast-ms**(ADR-074: kNN 비용의 바닥, 별도 회계).

### 환산 지표
`$/1M queries` · `$/sustained-QPS@p99` · `J/query`. perf/Watt는 논문 트랙 독립 축.

---

## 3. 실험 축과 셀 정의

### 3.1 공통 축
| 축 | 값 |
|----|----|
| **N** | 1K · 10K · 100K · 1M · 10M (+ 이분점 §5.2) — 50M은 Ring D 천장 |
| **데이터** | 1024d 실 임베딩(Cohere) 1차 + 384/768d 합성(메커니즘·dim 스케일링) |
| **dim** | 384 · 768 · 1024 — **κ∝1/dim·detoast∝dim이라 라우팅 교차의 1급 변수**(ADR-075) |
| **storage** | **TOAST(기본) · PLAIN** — detoast 벽이 kNN 비용을 지배하므로 1급 축(ADR-074, 신규) |
| **k** | 10 · 100 |
| **recall target** | 0.95 · 0.99 (exact 경로 cpu-seq/flat/B는 1.0, 별도 행) |
| **config** | cpu-seqscan · flat · cagra · ivfpq · transient-B(on) · auto · pgvector-HNSW(Ring A) |
| **workload regime** | **read-heavy(W2) · write-heavy(W1) · 혼합** — A1 vs pgvector-무인덱스 포지셔닝(신규) |
| **동시성** | Stage A: 1·8 / 전체 sweep(1·8·32·64·128)은 Stage D4 |
| **상태** | Stage A: warm 고정 / cold·warmup은 Stage D5 |

### 3.2 노브 공간 — 차원 폭발 제어
격자 전수 금지. 4규칙:
1. **노브를 phase에 묶는다.** `maintenance_work_mem`·`max_parallel_maintenance_workers`→빌드 셀만 / `shared_buffers`·`effective_cache_size`→쿼리 상주(baseline당 1회) / `max_parallel_workers_per_gather`→pgvector 쿼리 병렬(CPU seqscan 지배 노브, ADR-073 캘리브레이션은 병렬 seqscan 기준).
2. **엔진별 self-tuning.** 각 (N,recall) 셀에서 엔진이 자기 지배 노브 2–3개만 sweep → 자기 Pareto-best. flat: `precision∈{float32,float16}`. cagra: `graph_degree`·`build_algo`. ivfpq: `n_lists`·`pq_bits`·`n_probes`.
3. **모든 파라미터 `params_json` 공개.**
4. **자원 예산은 §2의 2점만.**

### 3.3 iso-recall 등화 / exact 분리
approx 엔진(HNSW/cagra/ivfpq)은 recall 노브 sweep해 target 최소값 작동점. **exact 경로(cpu-seq/flat/transient-B)는 recall=1.0 — 등화 없이 별도 행**, 단 같은 (N,dim,storage,k)에서 absolute latency/throughput/자원으로 직접 비교.

---

## 4. 관측 스키마 (생명주기 × 자원)

모든 `(system, phase, cell)`에 동일 튜플.

| Phase | 성능 | 자원 |
|-------|------|------|
| **build** | wall-clock, rows/s, **빌드 종류(graph/vectors-only/none)** | peak VRAM·RSS, CPU-core-s, GPU-s, disk, WAL, index size |
| **maint** | per-op latency(**A1 extend ~1.77ms/행**), 처리량, recall drift(시계열) | VRAM/RSS 시계열, `delta_rows`, extend/compact_count, HOT 비활성 여부 |
| **query** | QPS, p50/p95/p99/p999, recall@k, **SLA-bounded QPS** | peak VRAM/RSS, CPU/GPU util, J/query, **detoast-ms** |

### 결과 CSV (`bench/results/protocol/*.csv`)
append-only · 불변 · 행 단위 완결. v2 스키마에 신규 컬럼 추가:

```
... (v2 컬럼 전부) ...
storage,                                      # TOAST | PLAIN (신규)
detoast_ms,                                   # kNN 비용 바닥 분해 (신규, ADR-074)
build_kind,                                   # graph | vectors_only | none (신규)
sla_p99_us, sla_bounded_qps,                  # SLA 하 최대 QPS (신규, 동시성 정직 비교)
# 코스트모델 (ADR-075)
cost_model_version, hw_profile_version, link_bw_gbps, phys_cost_enabled,
runtime_routing_version,
params_json, notes
```

---

## 5. Stage A — 교차-밀집 물리 실측 (1회, 불변 자산)

> forced 곡선 + **데이터-이동 분해**를 측정한다. 코스트모델 무관, 불변.

### 5.1 측정 대상
- **forced**: `cpu-seqscan`, `flat`(A1), `cagra`, `transient-B`. N≤100K에서 ivfpq 참조.
- HNSW는 Ring A(경쟁자)로 같은 축에서 병행 측정하되 플래너 결정면 분석에선 제외.
- 축은 §3.1. baseline same-box 우선, iso-$는 D1 합류.
- **코스트모델이 쓰는 변수를 셀 축에 필수 포함**: N, k, dim, **storage**, selectivity(필터 셀).

### 5.2 데이터-이동 분해 (신규, ADR-074 방법론)
교차곡선만으론 *왜* 이기는지 모른다. 대표 셀(예 100k×768)에서 비용을 항별로 분해:
```
heap-scan only  /  +detoast(vector_dims, 연산 X)  /  +detoast+L2  → compute 항 격리
TOAST vs STORAGE PLAIN 대조 → detoast 벽 정량화
transient-B 백엔드/데몬 분해 → 회피가능 복사·H2D(link_bw) 격리
```
산출: "kNN은 memory-bound(0.75 flop/byte), 비용은 데이터 이동, detoast가 바닥" 증거 + 엔진별 이동 항. **이게 ADR-075 물리 공식의 실측 근거다.**

### 5.3 적응적 이분탐색
거친 log-격자(N∈{1K,10K,100K,1M,10M}) → ratio(N)=metric_A/metric_B 가 1 가로지르는 구간만 log-중점 분할(최대 3분할). 교차는 **면** — dim·storage·k·동시성·selectivity가 교차 N을 민다. N축 완료 후 나머지 축당 2–3점.

### 5.4 실행 형태
- 인덱스 공존(planner-auto 자연 조건), forced는 GUC 게이팅(`enable_cuvs`/`enable_indexscan`/`cuvs.gpu_bruteforce`).
- 빌드는 인덱스별 단독 측정(자원 회계 오염 방지) — **flat의 vectors-only(O(N), 그래프 없음) vs cagra(graph) 빌드비용 대조가 핵심 산출**.
- 모든 행 §4 완결 기록.
- **하네스 규율(세션 교훈)**: gpu-singleton + cancel-in-progress:false → running 1 + pending 1만(3번째 디스패치가 pending evict) → **one-ahead 디스패치**. publish가 run마다 덮어쓰므로 raw는 즉시 `docs/data/` 통합 커밋(휘발 방지). 엔진/코드 변경 시 `build=true`(사이드카·데몬 최신화). config마다 `engines/<config>.sh` 래퍼 필요.

---

## 6. Stage B — 코스트모델 검증 루프 (ADR-075, 구현됨)

> v2의 "상수 내리기"는 폐기. v3 코스트모델은 **데이터-이동 물리 분해 + 하드웨어 probe**(이미 구현). Stage B = 그게 옳게 라우팅하는지 **검증 + 잔여 오보정 교정**.

### 6.1 루프
```
EXPLAIN 스윕 (전 셀, 실행 없음) — 물리 공식이 고른 경로 덤프
  → regret 셀 (§6.3) + 판별 플립 검사 (§6.4)
  → 교정 (물리 공식/계수, 또는 probe 정밀화 — §6.5 규율)
  → EXPLAIN 재스윕 + Tier-2 실측 cross-check
  → 반복
```

### 6.2 EXPLAIN 산출물 (`planner_est_*.csv`)
셀마다 경로별(seqscan/flat/cagra/ivfpq/B) 추정 덤프 + **물리 항 분해**:
```
cell_id, cost_model_version, hw_profile_version, path,
est_scan, est_detoast, est_move, est_compute, est_topk,   # 물리 분해 (신규)
est_total, chosen, phys_cost_enabled, measured_ref
```

### 6.3 합격 기준 — regret + ε-밴드
```
regret(cell) = measured(플래너 선택) − measured(최선)   [exact: absolute; approx: iso-recall p99]
ε-밴드 = 두 최선 곡선 차이 < 10% 인 셀 = don't-care
합격 = ε-밴드 밖 regret>0 셀 0개 (regret 내림차순 보고)
```
**v2 대비 변경**: 메트릭 주(主)는 **p99**(ADR: p50 비교 부당). exact-우선 규칙 위반(exact가 더 싼데 cagra 골랐거나 그 역)도 regret으로 카운트.

### 6.4 판별 플립 검증 (신규, ADR-075 핵심)
물리 코스트의 존재 이유 = **detoast/dim/하드웨어별로 교차가 움직인다.** 회귀 펜스로 고정:
- **detoast-aware**: 768-dim TOAST 셀에서 레거시는 seqscan 오선택(~16× 손해 가능), 물리는 flat/cagra. 실측 플립 확인.
- **판별 플립(검증됨)**: dim=8·N=10000 → 레거시=seqscan, 물리=cagra(A100 실측: 실제 per-query 지연이 10k행 CPU 스캔을 이미 이김). dim=8·N=2000 → 양쪽 seqscan(GPU 부당강제 안 함, anti-flip).
- **DEFAULT 폴백 안전**: `cuvs.enable_phys_cost=off` / hw_profile 부재 / Tier-1(GPU 비트 미충족) → **레거시 동작 복귀, un-probed 바이트 동일**. 3경로 모두 검증.
- **하드웨어 식별**: probe가 PCIe/NVLink를 `link_bw`로 구분, env-tag 불일치 → DEFAULT 안전 강등(클론/rsync mtime 보존 방어).
- **검증 창구 = `pg_cuvs_hw_profile()`**: 셀 측정 전 `source='measured'` ∧ `matches_running_daemon=true` ∧ `probe_status`가 해당 경로 필요비트 충족을 확인(아니면 그 셀은 레거시 cost로 라우팅됨을 행에 표기). 물리 레짐 A/B 토글은 `SET cuvs.enable_phys_cost=off`로 강제.

### 6.5 오염 경계 규율
- ✅ 루프 중 허용: 물리 공식/계수, probe 측정 정밀화.
- ⚠️ **runtime 라우팅 수정**(`gpu_bruteforce` auto 승격, `filter_auto_threshold`, delta merge)이 필요하면 — 그 자체가 발견 — 영향 셀만 스코프 재실측 후 복귀, `runtime_routing_version` 범프.
- 모든 행 `cost_model_version`+`hw_profile_version`+`runtime_routing_version` 태그. 버전 혼합 집계 금지.

### 6.6 수렴 후 확인 (소량)
planner-auto **실행**은 (a) 결정 경계 근방 + (b) 무작위 spot-check만 — "auto 플랜 실측 ≡ 해당 forced 실측" 확인. 전 격자 재실행 불필요.

---

## 7. Stage C — 동결 · 버전 태깅

- `cost_model_version` + **`hw_profile_version`**(probe 스키마) 범프(소스 주석 + 결과 행).
- 보정 보고서 `docs/cost-model-calibration.md`: 물리 공식, before/after 라우팅, regret 표, ε-밴드 좌표, 판별 플립, DEFAULT 폴백 증거, 도달 불가 구간(§13).
- **동결 이후에만 Stage D.** 동결 전 auto 결과는 폐기.

---

## 8. Stage D — 동결된 모델 위의 전체 스위트

### D1 · 자원-성능 Pareto + $ 정규화 (P1 본체)
Stage A 셀 재사용 + iso-$ 합류. 산출: (N,recall,dim,storage) 셀별 `$/1M`·`$/QPS@SLO` Pareto(same-box/iso-$ 양 축) + 교차 좌표.

### D2 · 필터 M1 (selectivity × correlation)
실사용 1차 + 차별화 본령(ADR-061 D-wedge, **transient-B가 그 plumbing 실현 — filter-first**).

| 축 | 값 |
|----|----|
| selectivity | 0.1% · 1% · 5% · 10% · 50% |
| correlation | random / mixed / spatial |
| N | 1M(1차), 100K(보조) |
| pgvector | `hnsw.iterative_scan ∈ {off, strict, relaxed}` |
| pg_cuvs | auto(D-wedge/3O/stream-bf) + flat-filtered + **transient-B filter-first** |
| 측정 | recall@k · QPS · p99 tail |

`iterative_scan` 끄고 비교 금지(불공정). 예상: pg_cuvs filtered exact·latency flat vs iterative_scan 근사+저selectivity tail 폭발. **B의 filtered 교차점(filter-first가 CPU exact-filtered 대비 우위 구간)은 ADR-073 carry-forward — live auto의 전제.**

### D3 · 증분 M2 (insert/upsert + FIFO)
3A(.delta)·3Q(EXTEND)·4C(compaction)·VACUUM tombstone.

| 시나리오 | 내용 |
|----------|------|
| (a) 연속 append | base N₀ 후 스트림 INSERT |
| (b) FIFO 윈도우 | head INSERT + tail DELETE |
| (c) upsert 혼합 | UPDATE 비율 혼합 |

**ADR-074 반영**: **A1 write = 1.77ms/행(무인덱스 0.13의 13×)** + HOT 비활성(벡터 UPDATE) + compaction 재빌드(전체 N 재detoast+H2D). 결론 틀: **쓰기-heavy(W1) = pgvector 무인덱스 정답, read-heavy(W2) = A1.** 시계열: ingest 처리량·recall drift(윈도우 GT 재계산)·ingest 중 동시 query QPS/p99·VRAM 성장·"언제 REINDEX 필요"(flat은 in-place compact 없음, REINDEX 재빌드).

### D4 · 동시성 · tail under load (SLA-bounded)
c ∈ {1,8,32,64,128} pgbench. **세션 실측 반영**:
- **단일 데몬 천장**: CAGRA·plain-BF는 single-stream ~900–1200 QPS, 동시성 무스케일(평평).
- **flat/BF + 코얼레싱**(`cuvs.bf_batch_wait_us>0`)은 동시 요청을 한 GPU pass로 묶어 **스케일**(10k/100k 4000–5000 QPS, exact) — 단 **레이턴시를 throughput로 거래**.
- **peak QPS는 호도** → **SLA-bounded QPS(p99<20/50ms 하 최대 QPS)를 1급 지표로**. 예: 1M에서 p99<20ms면 cagra(1203) 승, <50ms면 bf+batch(1441) 승 — 레짐 의존.
- bf+batch는 **default 아님**: c=1 페널티(코얼레싱 창이 저부하 손해), "지속 고동시성 + 레이턴시 관대 + exact" 레짐 전용. CPU 코어 스케일 교차곡선 병기.

### D5 · cold-start · warmup · 멀티테넌트 LRU
time-to-first-query-at-target-QPS: GPU VRAM reload vs pgvector `shared_buffers` warm. flat은 `.vectors` 재로드(재시작 내구성 검증됨). LRU churn + GCS warmup(3C/3D), 캐시-미스 꼬리.

### D6 · 천장 셀 (Ring D) + multi-GPU
50M×384(+가능 시 1024): 경쟁자 ceiling + pg_cuvs GPU-search N/A(VRAM 천장) 그대로. stream-BF(ADR-064)·3I build-accel만 이 스케일 의미. **multi-GPU 샤딩 스케일(shard_count sweep) — 단일 데몬 천장↑·크로스오버 좌이동**(terraform `gpu_count>1` 경로 존재).

### D7 · 3I build-accelerator 별도 레인
"GPU 빌드 → CPU 서빙": `pg_cuvs_import_cagra`(GPU CAGRA 그래프 → pgvector HNSW로 export, `USING pg_cuvs_hnsw`) — 쿼리 축이 pgvector와 동일 → **빌드 축만** 비교. GPU 없는 쿼리 노드 배포 시나리오. Ring A에 안 섞고 별도 레인.

### D8 · 저장 레이아웃 (STORAGE PLAIN) 영향 (신규, ADR-074)
`SET STORAGE PLAIN`(dim≤~768 인라인)이 detoast 벽 제거 → CPU kNN 539→147ms(3.7×), transient 절대시간 급감. 단 **GPU 승리는 여전히 상주(A1)뿐**. 산출: storage×dim×엔진 매트릭스 + heap 부풀음 트레이드오프.

### §8.9 참고 — plan-time 미지 인자 (ADR-074/075)
cold/warm·delta backlog·**동시성 부하·CPU-offload 신호·통합메모리 link_bw**는 plan-time에 알 수 없다. transient-B의 진짜 가치(always-fresh·filtered·동시성 offload)도 런타임 속성 → 상수 아닌 **런타임 적응 라우팅 후보**(live `auto`의 전제). 하드웨어-포터블 cost(probe된 `link_bw`)가 통합메모리에서 B를 자동 활성화하는 메커니즘.

---

## 9. Ground Truth (스케일별)
| N | GT | 비고 |
|---|----|------|
| ≤ 1M | exact BF(numpy/faiss-cpu flat 또는 **flat AM 자체**) | 기존 `bench/gt.py` |
| ≥ 10M | **faiss-gpu flat** | cuVS 독립 → 순환성 회피 |
| M2 시계열 | 체크포인트마다 윈도우 GT 재계산 | drift 전제 |

셀마다 `gt_method`. 쿼리셋 고정(1,000 queries, seed, `query_set_id`).

---

## 10. 데이터셋
- **1차 = 실 임베딩**(Cohere 1024d → 10M 확장). recall·교차 진실값은 실 데이터에서만.
- **합성 = 메커니즘·dim 스케일링 전용**(clustered=IVF 편애, uniform=recall 함정 → recall 클레임 금지).
- 분포 민감 결론은 실/합성 양쪽 표기.

---

## 11. 통계 규약
- **반복**: latency 셀당 ≥5 reps(고정 1,000 queries). QPS는 60s×3, 첫 윈도우 폐기. 보고 = median + 산포(IQR). **단일 측정 금지.** run-to-run ~30% 변동 관측(cagra@100k 914 vs 1206) → 범위 보고.
- **메트릭**: 주=**p99**, 본문 참조=p95, dispersion 항상. **동시성은 peak QPS + SLA-bounded QPS 병기**(체리픽 금지).
- **환경 통제**: CPU baseline NUMA 핀닝(`numactl`), governor=performance, THP 기록. ADR-073 캘리브레이션은 `work_mem=4GB`·TIMING OFF·warm median/5 기준.
- **warm 통일**: pg_cuvs = 데몬 상주(로드 확인), pgvector = `pg_prewarm`. cold는 D5만.

---

## 12. 재현성
- 고정·기록: pg_cuvs commit, 경쟁자 버전+커밋, PG·cuVS·CUDA·드라이버, 인스턴스·zone·단가, **`pg_cuvs_hw_profile()` 전체 덤프**(probe된 link_bw/hbm_bw/ipc_rtt/gpu_cagra_lat + source + probe_status + env_tag).
- **전제**: 라우팅 결정 셀은 `matches_running_daemon=true`에서만 측정(GPU 스왑/index_dir 복원 후 데몬 재시작으로 재probe). 불일치 행은 stale-profile 태그.
- 매 run `SELECT name,setting FROM pg_settings WHERE source<>'default'` 동봉.
- `bench/results/protocol/` append-only(publish 덮어쓰기 → `docs/data/` 통합본이 영구 기록). 재실측은 새 `run_id`.
- 논문 트랙은 artifact 평가 기준 충족.

---

## 13. 정직한 한계 (사전 등록)
- **plan-time 미지 신호** — cold/warm·delta backlog·데몬 큐 깊이·동시성 부하·**배포 link_bw**. 상수로 envelope 도달 못 하는 구간은 보정 실패가 아니라 **"plan-time 정보 부족" 발견** → 런타임 적응 후보.
- **transient-B는 PCIe에서 잉여**(ADR-074, 실측). 가치는 통합메모리(GH200 NVLink-C2C ~900GB/s / MI300A APU)에서만 실재 — experimental 강등, `auto` off. "no-build인데 GPU 빠름"은 형용모순(GPU 속도=데이터이동 분할상환=상주).
- **GPU kNN 승리는 상주뿐** — detoast가 CPU/transient 바닥, A1만 HBM에서 연산해 이긴다. STORAGE PLAIN은 CPU/transient 절대시간만 낮춘다(상주 우위 불변).
- **GB 등화 불가** — $/J도 가격표 의존, raw 공개로 보완.
- **pg_cuvs ≠ WAL-logged mutable native** — 비교 전제(상주 인덱스 + 별도 freshness; A1 쓰기 13×) 모든 산출물에 명기.

---

## 14. 우선순위 — 두 트랙

### Track P — 제품 · 코스트모델 (엔지니어링 구동)
| # | 항목 | 비용 | 상태/비고 |
|---|------|------|-----------|
| **P0** | 관측 하네스 + 자원 회계(§4, +detoast_ms·storage·SLA-bounded) | 중 | 대부분 구축(runner/observe) |
| **P1** | Stage A 물리 + **데이터-이동 분해**(cpu-seq/flat/cagra/B, 실 임베딩, dim×storage) | 중 | 부분 실측(동시성·exact-tier·분해 일부 완료) |
| **P2** | Stage B→C **물리 코스트모델 검증**(판별 플립·DEFAULT 폴백·exact-우선) + 동결 | 저–중 | 코스트모델 구현됨(ADR-075) → 검증 ROI 최고 |
| **P3** | D2 필터 M1(iterative_scan + B filter-first 교차점) | 중 | 차별화 본령 |
| **P4** | D3 증분 M2(A1 13× 쓰기·W1/W2 포지셔닝) | 중 | freshness 입증 |
| **P5** | D1 iso-$ Pareto + D8 STORAGE PLAIN | 저 | P1 완결 |

### Track R — 논문
| # | 항목 |
|---|------|
| R1 | 10M 전 셀 + 실 임베딩 전면 + 반복·분산 + energy |
| R2 | D4 동시성/tail SLA-bounded 전체 + bf_batch 코얼레싱 |
| R3 | D5 cold-start + 멀티테넌트 LRU |
| R4 | D6 천장 + multi-GPU 스케일 + D7 |
| R5 | Ring B 앵커 + Ring C system-level |

**논문 서사 후보**: (i) **kNN은 memory-bound, GPU 승리는 상주뿐** — 데이터-이동 분해(ADR-074), (ii) **하드웨어-포터블 물리 코스트모델** — 교차점이 아니라 대역폭/지연 상수를 probe(ADR-075), (iii) 통합 세금 분해(Ring B), (iv) plan-time 정보 부족 구간 분류 + 런타임 적응.

---

## 15. 산출물
| 산출물 | 위치 |
|--------|------|
| 원시 결과(불변) | `bench/results/protocol/*.csv` + `planner_est_*.csv` |
| **통합 영구 기록** | `docs/data/*.csv` (publish 덮어쓰기 방어) |
| **운영가이드** | `docs/operational-guide.md`(경로 선택, v1 완료) |
| 보정 보고서 | `docs/cost-model-calibration.md` |
| 코스트모델 | `src/pg_cuvs.c`(물리 공식·probe) + `cost_model_version`/`hw_profile_version` |
| 교차/ε-밴드/판별-플립 | `BENCHMARK.md`(동결 후) |
| 필터·증분·동시성·저장 곡선 | `BENCHMARK.md` 확장 + 논문 figure |
| Ring C 비교 | 별도 문서 |
