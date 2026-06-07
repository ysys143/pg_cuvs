# pg_cuvs 전략 노트 (경쟁 포지셔닝 · 워크로드 표적)

> 상태: **작업 노트.** 전략 방향(A·B·C·D·E)은 **ADR-061로 승격·채택**(2026-06-07). 이 노트는 ADR 뒤의 상세 분석·미해결 쟁점을 보관하는 스크래치패드로 유지.
> 작성 계기: VectorChord 0.4 / 3B 사례 분석에서 "cuVS를 *왜* 쓰는가"를 다시 물었고,
> 어제까지의 "3P가 핵심" 결론이 데이터로 뒤집혔다. 쟁점이 많아 먼저 노트로 고정.
>
> 관계:
> - [`PROJECT_POSITIONING.md`](PROJECT_POSITIONING.md) = *엔지니어링 정합성* 포지셔닝(source-of-truth/MVCC/fail-closed). **이 노트와 충돌점 있음** (아래 쟁점 E).
> - [`ROADMAP.md`](../ROADMAP.md) = 실행 순서. 이 노트가 우선순위 재평가를 제안.
> - [`DECISIONS.md`](DECISIONS.md) ADR-048(3O 보류), ADR-049(3P) = 이 노트가 재해석 대상.

---

## 0. 쟁점 맵 (네비게이션)

| # | 쟁점 | 한 줄 | 상태 |
|---|------|-------|------|
| A | 분모: 벡터당 vs 쿼리당 비용 | 어느 분모에 지배되는 워크로드냐가 CPU/GPU를 가른다 | 프레임 확정 |
| B | 메모리 계층 절벽 | GPU는 VRAM→PCIe 절벽, spill 계층 없음 = 규모 약점의 *근본* 원인 | 프레임 확정 |
| C | 알고리즘은 하드웨어 종속 | 필터 검색 최적해가 CPU(graph traversal)와 GPU(brute-force)에서 다름 | 핵심 통찰 |
| D | exact filtered brute-force wedge | C+B의 합성. 3O·3P를 동시 흡수하는 후보 | **검증됨 (cuVS API, 2026-06-07)** |
| E | 포지셔닝 충돌 | 기존 문서가 "exact GPU search"를 Avoid로 못박음 ↔ D의 클레임 | **해소됨 (2026-06-07)** |
| F | 작업 우선순위 재평가 | north-star / 3O / 3P / 3Q / D / GPU-native pipeline | 잠정 |
| G | 온라인 대용량 = 파티션 pruning + LRU | tenant 파티션→prune→작은 인덱스→VRAM LRU 캐시. 기존 코드로 작동 | **스파이크 검증됨 (2026-06-07)** |
| H | streaming/out-of-core BF | VRAM 초과 데이터를 청크 스트리밍 + running top-k. exact, throughput 플레이 | 분석만 (미착수) |

---

## A. 분모 — 벡터당 비용 vs 쿼리당 비용

두 진영은 다른 분모를 최적화한다. 이게 표적 고객을 가른다.

- **벡터당 비용 지배** (거대 corpus, 낮은 QPS, 지연 너그러움 = 아카이브/배치) → **CPU+디스크**.
  - 근거: VectorChord 32억 벡터(384-dim halfvec), 3× i8g.16xlarge(512GB RAM/64vCPU/15TB NVMe), **월 $12k**, p50 **761ms**(top-500). 명시적으로 "in-memory/GPU의 막대한 비용 회피".
- **쿼리당 비용 지배** (working-set 적당, 높은 QPS, 저지연 = 온라인 RAG/추천/실시간) → **GPU**.

**함의**: pg_cuvs는 벡터당-비용 고객을 쫓으면 안 된다 — 구조적으로 못 이기고, 이겨도 고객이 761ms→1ms를 가치로 안 느낀다. **표적 = 쿼리당-비용 지배 워크로드.**

---

## B. 메모리 계층 절벽 (GPU 규모 약점의 근본)

- CPU 시스템: RAM(~$4/GB-mo) → NVMe(~$0.1/GB-mo) → S3. **매끄러운 비용 경사.** VectorChord는 RaBitQ 1-bit 코드(~48B/벡터)를 RAM에 hot, 풀정밀 벡터를 NVMe에 cold, rerank 때만 fetch.
- GPU: VRAM(~$10-20/GB-mo 환산) → **[PCIe 절벽, ~25GB/s vs VRAM ~2TB/s = 80×]** → host RAM. **spill이 우아하지 않다.**

**결과**:
- GPU는 rerank-from-disk 트릭을 못 쓴다 (32억 감당의 CPU 비결이 GPU엔 적용 불가).
- 3P(IVF-PQ)조차 이 절벽을 못 없앤다 — VRAM에 더 욱여넣을 뿐, spill 계층은 안 생김. 80GB/96B ≈ 8억 코드가 여전히 하드천장 + rerank 원본 둘 곳 없음.
- pg_cuvs의 `.delta`/streaming(3A/3Q)은 사실 **GPU에 없는 freshness/spill 계층을 인위 구축**하는 작업 = 근본 약점 보완.

---

## C. 알고리즘은 하드웨어 종속 (핵심 통찰)

필터 검색 3가지 방식:

| 방식 | 정확도 | 존재 이유 |
|---|---|---|
| ACORN식 (탐색 중 predicate-aware 이웃 확장) | 근사 | CPU는 brute-force 불가 → 그래프 타며 필터, 연결성 유지하려 2-hop 확장 |
| 탐색 노드 skip | 근사, **고선택성 recall 붕괴** | 가장 단순, 그래프 disconnect 위험 |
| **filter → brute-force** | **정확 (recall=1.0)** | CPU엔 사치, **GPU엔 거의 공짜** |

**통찰**: ACORN/skip은 둘 다 "CPU는 exhaustive 계산을 감당 못 한다"는 제약에서 태어난 우회로다. GPU는 그 제약이 없다 (A100 brute-force: M≈100만 후보 top-k가 ~30µs–수백µs; cuVS `brute_force` = 타일드 GEMM + top-k, 텐서코어). **→ GPU에선 filter→brute-force가 지배 전략. pg_cuvs는 VectorChord의 prefilter(bit-vector graph scan)를 베끼지 말고, CPU가 못 하는 GPU-네이티브 exact filtered brute-force를 해야 한다.**

부수 효과:
- **IPC 경계 비관론(어제) 무효화**: 벡터가 VRAM 상주면 IPC로 넘길 건 작은 bitset뿐(2,500만 → ~3MB; 고선택성이면 sparse ID 리스트). cuVS brute-force bitset prefilter에 그대로 투입. "탐색 내내 qual 필요"가 아니라 "검색 시작 시 bitset 한 번" → IPC 모델과 잘 맞음.
- **PG AM-qual 벽 우회**: qual을 AM에 밀어넣을 필요 없이, PG가 `WHERE`를 bitmap scan으로 이미 평가 → TID 집합 → corpus position bitset. 벽을 뚫는 게 아니라 옆으로 돈다.

---

## D. exact filtered brute-force wedge (cuVS API 검증됨 2026-06-07)

C(GPU brute-force)와 B(계층 절벽)의 합성. **네 통찰**: 필터된 부분만 GPU에 있으면 되므로 — 상주 요구량 ≈ working-set = **selectivity × N**.

- **near-term (고확신)**: 벡터 VRAM 상주 + bitset prefilter. exact, sub-ms~수 ms, IPC 비용은 bitset만. 고선택성 필터 세그먼트를 정확히·빠르게. *규모 천장은 안 올라가지만* 필터 쿼리의 exactness+속도를 확보.
- **확장판 (고보상, 고난도)**: 전체 corpus는 host RAM/NVMe(싼 계층), 쿼리마다 필터된 부분만 VRAM gather. **selectivity가 PCIe 절벽 통행료를 결정** → 절벽이 하드월에서 per-query 비용으로. working-set 자연 키 = **파티션**(멀티테넌트/recency/카테고리) → **VRAM = 파티션 LRU 캐시, host = backing store**. **GPU가 끝내 못 가진 메모리 계층을 필터를 캐시 키로 획득** (B 정면 해소).

**Killer app**: 멀티테넌트 SaaS RAG. 테넌트당 문서 적음(≤수십만~백만), 항상 `WHERE tenant_id=X`, 높은 QPS, 빡빡한 지연 = **쿼리당-비용 지배(A) = 표적 세그먼트.** 전체 corpus가 수십억이어도 GPU는 한 테넌트만 exact·sub-ms. VectorChord가 32억을 761ms로 하는 걸, pg_cuvs는 "필터된 테넌트 한 명"을 정확히·수 ms로 — 다른 게임.

**경계 (정직하게)**:
1. 저선택성/무필터 쿼리는 여전히 CAGRA 필요 → **대체 아닌 보완.** selectivity 임계 GUC 분기 (ADR-048의 `cuvs.prefilter_threshold`와 같은 다이얼).
2. 확장판 PCIe 비용: 캐시 미스 시 M×dim×4B / 25GB/s (M=100만 → ~12ms). LRU amortize 없으면 transfer-bound → near-term부터.
3. PG plumbing이 진짜 일: `WHERE + ORDER BY <-> LIMIT`을 filter→GPU-brute-force 경로로 라우팅 (custom scan이 bitmap 소비 or brute-force 인덱스에 bitset 전달).
4. **cuVS API 검증 — 완료 (2026-06-07, VM cuvs_dev 헤더). D 실현 가능 확정.**
   - `cuvsBruteForceSearch(res, index, queries, neighbors, distances, cuvsFilter prefilter)` — brute-force가 prefilter 네이티브 지원 (`brute_force.h:148`).
   - `cuvsFilter = { uintptr_t addr; cuvsFilterType type }`, type ∈ `{NO_FILTER=0, BITSET=1, BITMAP=2}` (`common.h:23-39`). **BITSET = "filter an index with a bitset"(1D, row별 allow) = WHERE-필터 케이스에 정확히 맞음.** BITMAP은 2D(query×row).
   - CAGRA도 `cuvsFilter`(`cagra.h:704,713,886`), IVF-Flat/IVF-PQ도 지원 → 저선택성 경로는 CAGRA+filter(근사)로 분기 가능.
   - **단서**: bitset 빌더 **C API는 없음**(`core/bitset.hpp`는 C++ 전용). 필터는 DLManagedTensor(packed uint32 bits)로 우리가 구성 → 데몬(CUDA C++)이 `cuvs::core::bitset`로 또는 host packed-bits→shm→device. 작은 항목, 블로커 아님.

### 보너스 발견 — cuVS `tiered_index` (3A/3Q·B 계층에 직접 영향)

VM 헤더 조사 중 `cuvs/neighbors/tiered_index.h` 발견. **pg_cuvs가 `.delta`로 손수 만든 구조의 네이티브 버전이다:**
- `cuvsTieredIndexBuild/Search/Extend/Merge` + params `min_ann_rows`, `create_ann_index_on_extend`("incremental **bfknn** portion이 min_ann_rows 넘으면 새 ANN 인덱스 생성").
- 즉 **base ANN(CAGRA/IVF) + incremental brute-force 버퍼 + 자동 compaction**. INSERT는 bfknn 버퍼로, search는 base+버퍼 병합, 버퍼가 차면 ANN으로 흡수. = pg_cuvs `.delta` + GPU/CPU merge를 cuVS가 네이티브로.
- **incremental portion이 brute-force라 D와 같은 메커니즘** — D(exact filtered)와 3A/3Q(freshness)가 tiered_index 한 점으로 수렴.
- `cuvsCagraExtend`(`cagra.h:644`)·`cuvsCagraMerge`(`cagra.h:882`)도 확인 → **3Q PLAN의 `CUVS_OP_EXTEND/COMPACT` 프리미티브가 26.04에 존재.**
- **정직한 한계**: tiered_index는 *VRAM 내* 티어링(CAGRA+bfknn 버퍼)이지 VRAM↔host/NVMe 티어가 아니다. → **freshness 티어(3A/3Q)는 주지만 capacity-spill 티어(D 확장판/B 큰 그림)는 안 줌.** 혼동 금지.

---

## E. 포지셔닝 충돌 (해소됨 2026-06-07)

`PROJECT_POSITIONING.md`가 "exact GPU vector search for PostgreSQL"를 **Avoid 메시징**으로,
"It does not guarantee exact nearest-neighbor top-k"를 명시했다. 그런데 D의 클레임은 "**exact**
filtered vector search at GPU speed" — 충돌처럼 보였다.

**핵심 발견**: Avoid 문구가 **stale**이었다. pg_cuvs는 이미 **3L(ADR-039)에서 GPU exact brute-force
검색을 출하**했다 — `search_mode='brute_force'`, recall=1.0(fp16에서도 exact). ADR-039는 *"N이 작을수록
CAGRA ANN보다 저렴 → planner 자동 선택"*까지 적어, "bf가 더 싸고 정확"이 이미 설계에 있다. 즉 Avoid는
*이미 가진 기능을 부정*하던 문서 버그였다(CAGRA-중심 시각으로 BF 모드를 메시징에서 누락).

**교정 (PROJECT_POSITIONING.md, 2026-06-07 반영)**:
- "does not guarantee exact" 절 → **default CAGRA 경로는 근사**로 한정 + **BF 모드는 exact**(3L/ADR-039) 명시.
- assurance 표 → "Exact recall (brute-force mode) = Guaranteed" / "(CAGRA default) = Not guaranteed" 2행 분리.
- 메시징: Avoid에서 "exact GPU vector search" 제거 → Prefer에 scoped 클레임 추가
  ("BF 모드 exact, 소규모/필터·선택적에서 CAGRA보다 싸고 정확"). 새 Avoid = "default CAGRA가 exact다"(이건 거짓).
- Differentiation에 "exact GPU search on demand(BF) + planner 비용 선택" 1행 추가.

**정직성 경계 (새 overclaim 방지)**: BF는 무필터 대규모 N에선 O(N)이라 CAGRA보다 비싸다. 그래서 "항상 더 싸다"가
아니라 **"소규모/필터·선택적 쿼리에서 exact이면서 더 싸다"**로 한정 — ADR-039 planner 자동선택 임계가 그 경계.

---

## F. 작업 우선순위 재평가 (잠정)

| 항목 | 어제 평가 | 이 노트 이후 |
|---|---|---|
| **north-star (빌드/H2D 오버헤드)** | 배경 목표 | **해자 1순위.** 빌드 속도 = GPU의 가장 명확·베끼기 어려운 edge. 4A(ADR-057/8/9)·ADR-059가 옳은 투자였음 |
| **D: exact filtered brute-force** | (없던 항목) | **신규 1순위 후보.** 해자 + 차별화 제품 클레임 + killer app(멀티테넌트) 동시. 빌드 부담도 낮음(brute-force = 빌드 없음) |
| **3O prefilter (A/B)** | A 저비용/B 열위 | **D에 흡수됨.** D가 3O의 GPU-네이티브 정답 버전 |
| **3P IVF-PQ** | "규모 핵심" | 재정의: "규모"가 아니라 "VRAM working-set 천장 올리기". 압축 품질은 RaBitQ에 짐. D가 selectivity로 규모를 다르게 푸니 **우선순위 하락** |
| **3A (완료, ADR-047)** | 기능 | `.delta` pending-delta가 동작 중(이번 세션 certify). **미완 아님.** tiered_index 이관은 선택적 미래 리팩터 |
| **3Q (미래, 릴리스 후)** | 기능 | streaming updates(`cuvsCagraExtend/Merge`)로 `.delta` 대체. **cuVS `tiered_index`**(base ANN + bfknn 버퍼 + 자동 compaction)가 **네이티브 구현 후보** |
| **(미개척) GPU-native 임베딩 파이프라인** | — | wedge 후보. 임베딩이 VRAM에서 태어나면 D2H 없이 on-device 검색 = CPU가 구조적으로 못 따라옴 |

---

## G. 온라인 대용량 = 파티션 pruning + LRU (스파이크 검증 2026-06-07)

VRAM 초과 대용량을 온라인(저지연·고QPS)으로 푸는 답은 "한 인덱스를 더 욱여넣기(3P)"가 아니라
**문제를 VRAM 크기 조각으로 쪼개고(파티션) hot만 상주(LRU)**다. layer-B 절벽을 *없애려는 대신 밑으로 안 내려가게* 한다.

**층층 모델**:

| 쿼리 형태 | 처리 |
|---|---|
| 파티션 키 필터(`tenant_id=X`) | **PG pruning → 작은 파티션 인덱스 → VRAM LRU** (1차 selectivity, planner가 qual 소비 → AM-qual 벽 우회) |
| 파티션 내 2차 필터 | **D의 BITSET prefilter** (작은 인덱스 위 exact) |
| whale 파티션 > VRAM | 3P(IVF-PQ) / 샤딩 / H(스트리밍) |
| 파티션 키 없는 global | pruning 불가 → fallback (3P/DiskANN 영역) |

**스파이크 증거 (VM A100/PG16, LIST partition by tenant, 6×3000 dim128)**:
- **Part 1 pruning**: `WHERE tenant_id=3 ORDER BY emb<->q LIMIT 10` → 단일 `Index Scan using tv_t3_cagra`(Append 없음). 무필터는 `Merge Append` over 6. GPU pruned == CPU brute-force (exact).
- **Part 2 LRU 용량**: `--max-vram-mb 4`(6×1.5MB) → **2/6만 상주**. cold 파티션 접근 → 디스크 reload(LRU victim evict) + 정합 top-k 유지. 5→6→2 churn 시 상주 집합 회전(최종 {t2,t6}).
- **캐시-미스 꼬리**: cold reload **≈13–15ms** vs hot **<0.5ms** (1.5MB 인덱스 기준).

**핵심**: 새 아키텍처 0줄 — 기존 3D cold registry + LRU eviction(`pg_cuvs_server.c`)이 그대로 온라인-스케일 캐시로 동작. "될 것 같다"가 "된다"로 굳음.

**갭 (false-done 방지)**:
- 기존 `multigpu-partition-recipe.sql`은 hash-on-id(분산 목적) → prune 불가. **tenant LIST/RANGE + pruning recipe·회귀테스트 없음**(스파이크가 처음 실증).
- `MAX_INDEXES=64` 상주 캡 + cold registry @ 수천 테넌트 미검증; PG planning 오버헤드도.
- 캐시-미스 꼬리 @ 현실 인덱스 크기(수십~수백 MB) 미측정 — north-star(빠른 H2D 로드)가 갚는 자리.

## H. streaming / out-of-core brute-force (분석, 미착수)

VRAM 초과 데이터를 디스크에서 청크 스트리밍 → 청크별 거리계산 → running top-k 머지(결과만 Q×k 누적) → 종합.
**exact**(전체 스윕 = 전체 brute-force). cuVS brute_force가 VRAM 내 타일링은 이미 함 → host/disk로 확장.

**병목·정직한 경계**:
- **throughput 플레이지 latency 아님**: corpus는 *쿼리 배치당 한 번* 읽고 Q개로 amortize. Q=1이면 전체 스윕 부담(망함), 큰 Q면 ÷Q.
- `배치 wall-clock ≈ max(corpus_bytes/BW_disk→gpu, 2·Q·N·d/GPU_FLOPs)`. compute-bound 임계 **Q ≳ 5,000**(A100, NVMe ~10GB/s). 그 밑은 I/O-bound라 **GPU가 굶고 CPU가 거의 따라잡음** — GPU는 큰 Q / 압축저장 디코딩 / 무거운 rerank에서만 제값.
- **vs ANN/DiskANN**: 단일 온라인 쿼리는 ANN이 MB만 읽어 이김. 스트리밍 BF는 TB 스윕이라 단일-쿼리에선 짐.

**적소**: (1) exact 대규모 배치 스코어링 / ground-truth·recall 검증(ADR-039이 BF 용도로 명시), (2) **필터 결합**(필터된 부분집합만 스트리밍 → 스윕 selectivity×N). 온라인 단일-쿼리엔 부적합(=G의 파티션+LRU 자리).

---

## 잠정 결론 / 다음 한 수

1. **포지셔닝 한 줄 (가설)**: pg_cuvs는 "거대 corpus를 싸게"(CPU+디스크가 구조적으로 이김)가 아니라, **"VRAM에 들어가는 working-set에 대해 압도적 빌드 속도 + 고QPS 저지연, 특히 필터 selective + 데이터가 이미 GPU에 있을 때"**를 노린다.
2. **다음 한 수**:
   - (a) **cuVS API 검증 스파이크** — **완료 (2026-06-07).** D 실현 가능 확정(brute-force+BITSET prefilter), tiered_index 보너스 발견.
   - (b) **전략 ADR 승격** — **완료 → ADR-061** (A·B·C·D·E + 스파이크 결과). 다음 세션의 "3P가 핵심" 잘못된 출발 차단.
3. **검증 완료(이번 세션)**: ~~E~~ 해소(PROJECT_POSITIONING 교정), D cuVS API 검증, **G 온라인-스케일(파티션 pruning + LRU) 스파이크 검증**.
4. **미해결 (ADR-061 "미해결 쟁점")**: G 갭(tenant-pruning recipe/test 없음, MAX_INDEXES=64·cold registry @ 수천 테넌트, 캐시-미스 꼬리 @ 현실 크기), D PG-plumbing + 확장판 PCIe 예산, tiered_index가 `.delta` 대체 범위(3Q 후보, 별도 스파이크), H streaming BF(미착수).
