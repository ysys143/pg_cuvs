# 설계 리뷰 — TimescaleDB 2.27 기법의 pg_cuvs 적용 가능성

> 출처: <https://www.tigerdata.com/blog/timescaledb-2-27> ("Broader Vectorized
> Execution, Up to 160x More Efficient UPDATE/DELETE, and Smarter UPSERT Pruning")
> 원본 리포 분석: `timescale/timescaledb` @ `2275ecd` (CHANGELOG 2.27.0~2.27.2),
> `tsl/src/compression/`, `tsl/src/nodes/{columnar_scan,vector_agg}/`.
> 작성: 2026-06-14. 본 문서는 **외부 설계의 평가 노트**이며 결정(ADR)이 아니다.

---

## 0. 한 줄 요약

2.27의 세 핵심 설계 중 **둘은 pg_cuvs에 비적용/이미흡수**, **하나(bloom sparse-index
배치 프루닝)만 진짜 이식 후보**다. 그 하나가 깔끔하게 새 값을 더하는 자리는 **단 한 군데
— 멀티-GPU 샤딩 fanout**(매치 0 샤드를 검색 dispatch 전에 스킵)이다. 구현 권고가 아니라
**측정 게이트(filtered+sharded 워크로드에서 fanout 절약 실측) 후 등재 권고**.

> **정정(2026-06-15)**: 초판은 (a) 샤딩/Streaming-BF를 "미구현 트리거 기질"처럼 표현했고
> (b) Streaming-BF를 bloom 적합처로 나열했다. 둘 다 틀렸다 — 샤딩(3E/3F/3G)·Streaming-BF
> (ADR-064)는 **이미 구현됨**. 그리고 `handle_search_stream_bf`는 rev-map으로 **통과 벡터만
> 정확히 pread**(`gather_chunk_pread`)하므로 통과 0 영역은 이미 공짜로 스킵 → **거기엔 bloom이
> 잉여**. 적합처는 샤딩 fanout 하나로 좁혀진다. 또한 bloom이 아끼는 건 detoast(~535ms)가
> *아니다* — detoast는 쿼리당 벡터 재마샬 경로(transient-B/CPU BF)의 비용이고, 샤딩 CAGRA는
> 벡터 VRAM 상주라 쿼리당 detoast가 없다. 샤딩 fanout에서 bloom이 아끼는 건 샤드
> dispatch/IPC/검색-셋업 비용(실측 병목보다 작은 이득).

이름 주의: 2.27의 "vectorized"는 **임베딩/벡터검색이 아니라 컬럼스토어 SIMD 배치 실행**을
가리킨다. 제목만 보고 벡터 DB 기능으로 오해하기 쉬우나 무관하다.

---

## 1. 2.27가 실제로 들여온 설계 3종

### (A) Bloom sparse-index 배치 프루닝 — *쓰기 경로로 확장*
- **메커니즘**: 압축 배치(compressed batch)마다 equality-적격 컬럼에 대해 작은 bloom
  필터(`bloom1`)를 빌드 타임에 계산해 배치 메타데이터로 저장. 쿼리 타임에 equality
  술어의 상수를 해싱해 `bloom1_contains`로 검사 → **이 배치엔 매치가 없음**이 증명되면
  decompress를 건너뜀.
  - 구현 디테일(`batch_metadata_builder_bloom1.c`): 6 해시, FP ≈ 2.2%, 256-bit 블록
    지역성, ≤64-bit는 메인 테이블에 인라인(fits-in-row), composite(다중 컬럼) 지원,
    NULL 마커로 `IS NULL`도 커버.
  - composite 다중 필터는 **컬럼 수 내림차순(=가장 선택적 우선)**으로 평가(`bloom_filter_check_cmp`).
  - 해시는 **플랜 타임에 선계산**(#9475)해 실행 루프에서 재해싱 제거.
  - **fail-open**: false positive는 불필요한 decompress 한 번일 뿐 결과는 항상 정확.
- **2.27의 새로움**: 기존엔 `SELECT`만 쓰던 프루닝을 `UPDATE`/`DELETE`/`UPSERT`
  **쓰기 경로**로 확장(#9374/#9399). EXPLAIN에 "Compressed batches filtered" 등
  관측 카운터 추가. 일부 케이스 최대 160×.

### (B) Vectorized filter 실행 — 컬럼스토어 fast-path 확대
- Hypercore 엔진이 WHERE 필터를 **Arrow 포맷 배치 위에서 표준 PG 함수 경로로 인라인
  평가**해 validity 비트맵 생성(`columnar_scan/compressed_batch.c`). 더 많은 쿼리(CAgg
  refresh 포함)가 컬럼스토어 빠른 경로를 타게 됨. 30%~2× 향상.

### (C) Continuous Aggregate 자동 쿼리 재작성 — 실험적/opt-in
- 플래너가 쿼리의 집계가 CAgg 정의와 정확히 일치하면 투명하게 CAgg로 라우팅(#8967).

---

## 2. pg_cuvs 매핑

### (C) CAgg 재작성 → **비적용**
벡터 검색엔 머티리얼라이즈드 집계 개념이 없다. 유사물 없음. 제외.

### (B) Columnar + Vectorized filter → **이미 있음(핫 컬럼) / 의도적 위임, 저가치**

세 TSDB 설계 중 **가장 가치 낮음**. 단 이유는 "GPU가 빨라서"가 아니라 아래 셋이다.

**Columnar storage는 이미 핫 컬럼에 실현됨.** TSDB columnar의 핵심 = 컬럼을 행저장소에서
빼내 압축 연속 배치로. pg_cuvs `.vectors` 사이드카 = 임베딩 컬럼의 dense `N×dim` columnar
projection(row-major, `offset=header+item_id*dim*4`), CAGRA/IVF-PQ = 그 컬럼의 압축 표현
(IVF-PQ는 문자 그대로 PQ 압축 10–100×). 스칼라 메타데이터 컬럼만 PG heap에 남기는데, 그건
PG 인덱스가 처리하므로 갭 아님.

**Vectorized filter는 두 층으로 갈라 봐야 한다.**
1. *스칼라 술어 평가*(`tenant_id=42`): pg_cuvs는 **의도적으로 PG executor에 위임**
   (데몬 value-agnostic, 백엔드가 통과 TID→BITSET 변환 전달). 건전한 경계 — B-tree가
   빠르고 선택적. 여기 재벡터화 = executor 재발명이고 필터평가는 병목도 아님
   (ADR-044 GPU-bound, ADR-074 병목=detoast).
2. *다운스트림 소비자 부재*: TSDB에서 vectorized filter가 값을 발하는 건 그 비트맵이
   **vectorized aggregation**(OLAP)으로 흐르기 때문. pg_cuvs 쿼리는 top-k라 그 "집계"
   등가물(top-k 머지)을 이미 GPU에서 함 → 비트맵을 먹일 소비자가 없음.
3. *유일한 새 변종 — GPU-fused filter*: 스칼라 컬럼을 GPU 상주시켜 술어평가+거리계산을
   한 커널에 융합(BITSET 마샬링 제거). pg_cuvs가 **명시적으로 비선택**한 설계(데몬 단순성·
   on-device 타입시스템·payload VRAM 회피). 이기는 구간 = **저선택성 + 벡터 상주**인데,
   pg_cuvs 표적(ADR-061)은 정반대 **고선택성 멀티테넌트**라 regime이 어긋남. 저선택성에선
   거리계산이 지배 → 마샬링 절약 marginal. 재평가 트리거 = 저선택성 filtered 워크로드 실수요.

3O **BITSET prefilter가 곧 GPU validity 비트맵**의 등가물이라는 점은 유효하나, 위 (1)~(3)이
"왜 추격 안 하나"의 정확한 근거다.

### (A) Bloom 배치 프루닝 → **유일한 진짜 이식 후보**

**구조적 일치**: "equality 컬럼에 대한 *값싼 블록 단위 skip 인덱스*로 *비싼 블록 단위
연산*을 통째로 회피한다." TSDB에서 비싼 연산 = decompress. pg_cuvs에서 그 "비싼 연산"이
무엇이고 어디서 발생하는지를 정확히 짚어야 한다(초판은 detoast로 잘못 일반화 — 위 정정).
대상은 **filtered 쿼리**(벡터 ORDER BY 옆에 equality 술어)에 한하고, 그게 정확히
STRATEGY_NOTES/ADR-061이 표적으로 잡은 **멀티테넌트 filtered-RAG** 세그먼트다.

**적합처는 한 군데 — 멀티-GPU 샤딩 fanout** (`pg_cuvs_server.c` shard_count≥2):
- 필터 쿼리는 모든 샤드에 fanout되고, 각 샤드는 자기 안에서 rev-map/BITSET으로 정확히
  거른다. 그러나 **"이 샤드에 매치가 한 건이라도 있나?"를 검색 전에 알 수단이 없어**
  매치 0 샤드에도 dispatch + GPU 검색 셋업이 든다.
- 필터 컬럼(예: `tenant_id`)에 대한 **per-shard bloom/min-max** → 매치 0 샤드를 검색
  **이전에** 제외 → 샤드 dispatch/IPC/검색-셋업 절약. **기존 샤딩 위에 얹는 순수 추가분**.
- 아끼는 건 **detoast가 아니다**: 샤딩 CAGRA는 벡터 VRAM 상주라 쿼리당 detoast 없음.
  detoast(~535ms, ADR-074)는 쿼리당 벡터 재마샬 경로(transient-B/CPU BF)의 비용이고
  거긴 적합처가 아니다. 따라서 이 레버의 이득은 **실측 병목보다 작은** dispatch급.
- **주의(검증함)**: 샤드는 **행-범위 파티션을 GPU에 round-robin** 배치이지 테넌트-정렬이
  아니다(`shard_*` 경로는 row-range). per-shard 필터의 선택도는 **인입 클러스터링에 의존**
  — 테넌트별 배치 적재면 잘 듣고, 인터리브면 약함. 테넌트-정렬 배치와 짝지을 때 최대 효과.

**부적합처 — Streaming-BF**(초판이 잘못 나열, 위 정정): `handle_search_stream_bf`는
3O rev-map으로 **통과 벡터만 정확히 pread**(`gather_chunk_pread`가 `item_ids[]`만 읽음)
→ 통과 0 영역은 이미 공짜로 스킵된다. 청크를 다 훑는 게 아니라 통과분만 랜덤액세스하므로
per-chunk bloom이 더할 게 없다(**잉여**).

**기존 prefilter와의 구분(중복 아님)**:
- 3O rev-map + BITSET = **정확·쿼리별·벡터 입도**의 선택.
- bloom = **값싼·블록/샤드 입도의 사전 패스**.
- 둘은 **합성**된다: bloom이 블록/샤드를 쳐내고, 생존 블록 안에서 rev-map/BITSET이
  정확 선택. bloom은 **fail-open**(FP=헛수고 1회, recall 불변)이라 pg_cuvs의
  fail-closed/regret-averse 문화와 충돌 없음 — **결과를 절대 바꾸지 않는다**.

**그대로 훔칠 만한 엔지니어링 디테일**(추진 시):
- 플랜 타임 해시 선계산(#9475) — pg_cuvs도 plan-time에서 필터 상수 알 수 있음.
- composite 필터 most-selective-first 정렬.
- ≤64-bit fits-in-row 풋프린트(사이드카 비대 회피 — `index_dir`/basebackup 가드 철학과 일관).
- EXPLAIN/`pg_stat_gpu_*` 프루닝 카운터(관측성 문화와 일관).

---

## 3. 판정 / 권고

| 설계 | 판정 | 근거 |
|------|------|------|
| (C) CAgg 재작성 | 비적용 | 벡터 DB에 집계 머티리얼라이즈 없음 |
| (B) Columnar + Vectorized filter | 이미있음/위임·저가치 | columnar는 `.vectors`+IVF-PQ로 이미 실현(핫 컬럼); 스칼라 필터는 PG 위임이 옳은 경계; agg 소비자 부재; GPU-fused는 표적과 반대 regime |
| (A) Bloom 배치 프루닝 | **이식 후보(샤딩 fanout 한정)** | 매치 0 샤드를 검색 전 스킵; 단 기질은 이미 구현됨 → 남은 건 페이오프 *측정* |

**권고**: (A)를 **지금 구현하지 말 것**. 단 이유는 "기질이 없어서"가 아니다(샤딩·
Streaming-BF는 이미 구현됨). 이유는 **이득이 dispatch급(실측 병목보다 작음)이고, filtered
+sharded 워크로드에서 fanout 절약이 실측으로 드러나야 가치가 증명되기 때문**이다:
- **release-prep에 끼워넣지 않는다**(릴리스 준비는 문서·벤치·플레이북 순차 경로).
- ROADMAP **"filtered 교차점 + 런타임 적응 라우팅"** 측정 게이트에 *후보 레버*로 등재.
  멀티테넌트 filtered+sharded에서 fanout 절약이 실측되면 그때 ADR로 승격.

요컨대 이 블로그에서 pg_cuvs가 얻을 건 **단일 아이디어 한 줄**이다: *"filtered 샤딩 검색
에서, 매치 0인 샤드는 검색 dispatch 전에 per-shard skip-stat으로 쳐내라."* 신규 워크
스트림이 아니라 기존 샤딩 경로의 작은 가속 레버다.

> **부록 — "rev-map을 bloom으로 개선?"(2026-06-15 질의)**: 대체 불가. rev-map은 TID→item_id
> *매핑(값 반환)*이고 bloom은 *멤버십(yes/no, lossy)*이라 일이 다르다. bloom으로는 item_id를
> 못 돌려준다. 셋째 경우 — rev-map 이진탐색 *앞단의 음성 사전검사* — 도 미스가 이미 O(log n)로
> 싸서 한계가치. 유일하게 의미 있는 건 **per-shard 사전 패스**(샤드 rev-map 프로빙 자체를
> 스킵)인데, 이는 위 샤딩 fanout 레버와 동일하다. 게다가 **샤드가 행-범위(≈TID-범위) 파티션**
> 이므로 그 케이스는 **per-shard TID min/max가 bloom보다 우월**(정확·FP 없음·16B). bloom이
> min/max를 이기는 건 **고카디널리티·산재(interleaved) 값 필터**(예: 테넌트 인터리브)뿐이고,
> 그건 데몬이 현재 안 받는 **필터 컬럼 값을 빌드 타임에 공급**하는 새 배선이 필요하다(현
> rev-map/BITSET은 의도적으로 value-agnostic — 백엔드가 TID만 전달). 결론: 니치 속의 니치.
