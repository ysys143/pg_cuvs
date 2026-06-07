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
| D | exact filtered brute-force wedge | C+B의 합성. 3O·3P를 동시 흡수하는 후보 | **검증 필요** |
| E | 포지셔닝 충돌 | 기존 문서가 "exact GPU search"를 Avoid로 못박음 ↔ D의 클레임 | **해소 필요** |
| F | 작업 우선순위 재평가 | north-star / 3O / 3P / 3A·3Q / D / GPU-native pipeline | 잠정 |

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

## E. 포지셔닝 충돌 (해소 필요)

`PROJECT_POSITIONING.md`는 메시징에서 다음을 **Avoid로 명시**한다:
- line 211: "exact GPU vector search for PostgreSQL"
- line 128-132: "It does not guarantee exact nearest-neighbor top-k" (CAGRA = 근사)

그런데 D의 클레임은 "**exact** filtered vector search at GPU speed".

**해소 방향 (잠정)**: 둘은 *다른 경로에 대한 진술*이라 양립 가능하다 —
- CAGRA(무필터/저선택성 graph 경로) = 근사, "exact 아님"은 그 경로에 대해 여전히 참.
- filter→brute-force(고선택성 경로) = exact. 이건 brute-force라 진짜 exact.
- 따라서 정직한 클레임은 "**고선택성 필터 쿼리에 한해 exact**, 그 외 경로는 근사 ANN". 무조건 "exact GPU search"는 여전히 과장 → Avoid 유지.

**TODO**: D를 채택하면 PROJECT_POSITIONING.md에 "selective filtered 경로는 exact" 단서를 추가하고 Avoid 문구를 "무조건적 exact 주장"으로 한정. (지금은 노트로만, 미반영.)

---

## F. 작업 우선순위 재평가 (잠정)

| 항목 | 어제 평가 | 이 노트 이후 |
|---|---|---|
| **north-star (빌드/H2D 오버헤드)** | 배경 목표 | **해자 1순위.** 빌드 속도 = GPU의 가장 명확·베끼기 어려운 edge. 4A(ADR-057/8/9)·ADR-059가 옳은 투자였음 |
| **D: exact filtered brute-force** | (없던 항목) | **신규 1순위 후보.** 해자 + 차별화 제품 클레임 + killer app(멀티테넌트) 동시. 빌드 부담도 낮음(brute-force = 빌드 없음) |
| **3O prefilter (A/B)** | A 저비용/B 열위 | **D에 흡수됨.** D가 3O의 GPU-네이티브 정답 버전 |
| **3P IVF-PQ** | "규모 핵심" | 재정의: "규모"가 아니라 "VRAM working-set 천장 올리기". 압축 품질은 RaBitQ에 짐. D가 selectivity로 규모를 다르게 푸니 **우선순위 하락** |
| **3A/3Q (delta/streaming)** | 기능 | 숨은 정체성 = GPU에 없는 freshness 계층 인위 구축. **cuVS `tiered_index`가 네이티브 대체 후보** (base ANN + bfknn 버퍼 + 자동 compaction). 손수 만든 `.delta` 상당 부분 단순화 가능 → 별도 검토 |
| **(미개척) GPU-native 임베딩 파이프라인** | — | wedge 후보. 임베딩이 VRAM에서 태어나면 D2H 없이 on-device 검색 = CPU가 구조적으로 못 따라옴 |

---

## 잠정 결론 / 다음 한 수

1. **포지셔닝 한 줄 (가설)**: pg_cuvs는 "거대 corpus를 싸게"(CPU+디스크가 구조적으로 이김)가 아니라, **"VRAM에 들어가는 working-set에 대해 압도적 빌드 속도 + 고QPS 저지연, 특히 필터 selective + 데이터가 이미 GPU에 있을 때"**를 노린다.
2. **다음 한 수**:
   - (a) **cuVS API 검증 스파이크** — **완료 (2026-06-07).** D 실현 가능 확정(brute-force+BITSET prefilter), tiered_index 보너스 발견.
   - (b) **전략 ADR 승격** — **완료 → ADR-061** (A·B·C·D·E + 스파이크 결과). 다음 세션의 "3P가 핵심" 잘못된 출발 차단.
3. **미해결 (ADR-061 "미해결 쟁점"으로 이관)**: E(포지셔닝 충돌) 반영, D 확장판 LRU 파티션 캐시 PCIe 예산, 저선택성 임계값 추정, tiered_index가 `.delta`를 어디까지 대체하나(별도 스파이크).
