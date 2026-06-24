# PG-Strom 대조 분석 — 왜 pg_cuvs는 영속 `.vectors`를 만들고 PG-Strom은 안 만드나

> 외부 소스 분석: PG-Strom **v6.2** (`github.com/heterodb/pg-strom`, 2026-06 클론).
> `파일:라인`은 그 PG-Strom 소스 기준이며 본 저장소 경로가 아니다.
> 목적: 두 GPU-PostgreSQL 확장의 **최적화 축이 정반대**임을 코드 근거로 정리해
> pg_cuvs flat 설계(ADR-073/074, ADR-064)의 포지셔닝을 보강한다.

## TL;DR

| | PG-Strom (OLAP full-scan offload) | pg_cuvs flat (ANN top-k) |
|---|---|---|
| 워크로드 | 매 쿼리 corpus를 **1회 full-scan**, 집계/조인 | **같은 corpus 반복 top-k** 검색 |
| GPU 데이터 표현 | 스캔마다 만들고 버림 (**transient**) | 1회 빌드 후 **warm 상주** (`.vectors`→VRAM) |
| 영속 GPU 아티팩트 | **없음** (원본=힙/Arrow 파일) | **있음** (`.vectors`/`.tids` 사이드카) |
| 이득의 레버 | ① 컬럼 프루닝 ② 집계/조인 결과 축소 ③ 컬럼 벡터화 ④ (GDS면) I/O DMA 할인 | 데이터 이동(detoast+H2D)의 **반복 분할상환** |
| 상시 GPU 캐시 | **폐기** (GPU Cache v6.2 제거) | ANN corpus 한정 + 경량 delta 동기화 |

핵심: PG-Strom은 "**적게 읽고 많이 줄이는**" 구조라 transient가 성립한다. ANN은
"**다 읽고 안 줄이는**" 구조라 그 레버가 없어, 유일한 레버인 warm 상주를 택한다.

---

## 1. PG-Strom은 영속 GPU 아티팩트를 만들지 않는다 (transient)

- **힙 스캔 = 페이지 그대로 래핑.** 힙 경로는 `KDS_FORMAT_BLOCK` — 8KB 힙 페이지를
  컬럼나로 *재배치하지 않고* 그대로 KDS 버퍼에 담아(`relscan.c:449,575`) GPU에 보내고,
  디바이스 커널이 GPU에서 튜플을 파싱한다.
- **GPUDirect SQL은 페이지 단위 선택적.** all-visible + clean 페이지는 shared buffer를
  우회해 NVMe→VRAM DMA(`relscan.c:622-655`, `gpu_service.c:3510`), 나머지는 buffered
  복사 + MVCC 검사. VFS 폴백 내장.
- **transient 확정.** 청크 버퍼는 매번 `resetStringInfo`로 재사용(`relscan.c:580`),
  GPU 메모리는 처리 후 해제(`gpu_service.c:2047,2774`). warm 상주 캐시 없음.

## 2. GPU Cache는 v6.2에서 제거됐다 (상시 GPU 동기화 패러다임 폐기)

명시적 근거 — `pg_strom--6.0--6.2.sql:14`:
```sql
--- GPU Cache is removed at v6.2
DROP FUNCTION IF EXISTS pgstrom.gpucache_sync_trigger();
DROP FUNCTION IF EXISTS pgstrom.gpucache_apply_redo(regclass);
DROP FUNCTION IF EXISTS pgstrom.gpucache_compaction(regclass);
DROP FUNCTION IF EXISTS pgstrom.gpucache_recovery(regclass);
```
이 객체들은 `pg_strom--4.0--5.0.sql:137-185`에서 생성되던 옛 GPU Cache의 **트리거 기반
redo-log 동기화** API다. v6.2에서 전부 DROP되고, `gpu_cache.c`/`gstore_fdw.c`는
`deadcode/`로 이동(빌드 미포함). 대체재 `parquet_cache.c`는 **VRAM pin이 아니라
호스트측 Parquet/Arrow 컬럼 파일 LRU 캐시**(`parquet_cache.c:14-40`) — "상시 GPU 상주 +
트리거 동기화"가 아니라 "읽기전용 컬럼 파일 + on-the-fly 로딩"으로 갈아탔다.

> **시사점**: 범용 테이블을 GPU에 상주-동기화하는 비용(redo·recovery·compaction)이
> 효용보다 컸다는 신호. pg_cuvs는 이 함정을 ANN corpus 한정 상주 + 힙-recheck 기반
> 경량 delta(무거운 redo 없음, ADR-047)로 피한다.

## 3. PG-Strom 이득의 두 축 — compute(GDS 무관) + I/O(GDS 의존)

비용 모델(`gpu_scan.c`)이 둘을 분리한다:

- **I/O 레그**: GDS 불가 시 `avg_page_cost = spc_seq_page_cost`(`:197`) — **할인 0**,
  일반 seqscan과 동일. GDS 가능 시 all-visible 분율만큼 `pgstrom_gpu_direct_seq_page_cost`로
  할인(`:191-192`).
- **연산 레그 (GDS 무관)**: GPU qual을 `qcost.per_tuple × pgstrom_gpu_operator_ratio()
  × ntuples / parallel_divisor`로 평가(`:259-261`) — CPU operator보다 싸고 병렬. 추가
  비용은 결과 GPU→Host DMA뿐이고 그것도 selectivity 적용 후(`:272,275`).
- **비동기 파이프라이닝**: 호스트 page read와 GPU 커널 실행을 청크로 오버랩
  (`executor.c:1095-1121`, `max_async_tasks`).

→ **GDS 없이도** 연산 오프로드 + 오버랩으로 힙 풀스캔에서 소폭 이득. 단 I/O 할인이 0이라
"약간"에 그친다.

## 4. Arrow 컬럼나 — SSB에서 GDS 없이도 큰 이득 (3–20× 급)

힙의 "약간"과 달리 **Arrow_Fdw는 차원이 다르다.** 이득은 GDS가 아니라 컬럼나 구조에서:

- **컬럼 프루닝 = I/O 구조적 감소.** `baserel->pages`가 **참조 컬럼 바이트로만** 산정된다
  (`arrow_fdw.c:4300-4301`, `referenced` 비트셋). SSB `lineorder`(17컬럼 와이드)에서 쿼리가
  ~3–5컬럼만 touch → row-store 대비 그 분율만 읽음. GDS와 독립.
- **GPU-네이티브 컬럼 포맷** `KDS_FORMAT_ARROW`(`arrow_fdw.c:3907`): 행 파싱·detoast 없이
  컬럼 벡터화 처리 → 힙 KDS_FORMAT_BLOCK이 GPU에서 내던 deform 오버헤드가 사라짐.
- **GpuPreAgg/GpuJoin**: star-join + 집계 무거운 SSB를 GPU 병렬 처리하고 **축소된 결과만**
  반환.

스케일 추세도 일관: SF 작으면 캐시-적합 + GPU 오버헤드 가시(~3×), SF 크면 컬럼나+GPU가
스케일하는데 CPU row-store가 막힘(~20×).

> **정직한 경계(확정 vs 인용)**: 메커니즘(컬럼 프루닝·`KDS_FORMAT_ARROW`·GpuPreAgg)은
> **코드로 확정**. SSB SF1–100의 3–20× 수치는 PG-Strom **공개 발표값**(`docs/blob/
> 20180417_PGStrom_v2.0_TechBrief.pdf` 등 repo 동봉)으로 **인용**이며 본 분석에서 재현하지
> 않았다.

## 5. 왜 이 승리가 ANN으로 전이되지 않나 — pg_cuvs flat의 근거

Arrow OLAP 이득의 세 축이 ANN top-k엔 **전부 부재**한다:

| 레버 | OLAP(Arrow) | ANN(flat) |
|------|-------------|-----------|
| 컬럼 프루닝 | ✓ 일부 컬럼만 | ✗ 거리계산은 **전 차원** 필요 |
| 집계/조인 결과 축소 | ✓ 크게 줄임 | ✗ top-k는 corpus 전체를 봐야 |
| 컬럼 벡터화 | ✓ | △ `.vectors`가 이미 contiguous |

게다가 pg_cuvs는 ADR-074에서 flat의 병목이 **연산이 아니라 데이터 이동**(TOAST detoast
~535ms, 거리계산은 memory-bound로 ≈0)임을 실측했다. 즉 PG-Strom이 "GDS 없이도 소폭 이기는"
그 **연산 오프로드 축**은 ANN에 효용이 거의 없다 — 비싼 건 매쿼리 이동이다.

→ ANN의 유일한 레버는 같은 corpus 반복 검색을 **warm 상주로 분할상환**하는 것. 그래서
pg_cuvs는 PG-Strom과 **정반대로** 영속 `.vectors` + VRAM warm을 만든다. 두 설계는 서로
다른 워크로드의 올바른 귀결이며, 대체재가 아니다.

> **추정 표시**: "반복 검색이라 warm 상주가 분할상환된다"의 **정량적 손익분기점**(몇 회
> 재사용부터 영속이 유리한가)은 PG-Strom 코드가 아니라 워크로드 특성에서의 설계적 추정이다.
> VRAM 초과 + 고선택성 필터의 out-of-core 경로는 별도로 존재하나 기본 off다(ADR-064, 좁은
> win-region + crossover 미측정).

---

**관련**: ADR-073(flat AM A1), ADR-074(detoast 병목·포지셔닝), ADR-064(streaming BF),
ADR-047(delta/tombstone MVCC), ADR-077(flat 동시-쓰기 reader 락). 측정: `bench/protocol/
HANDOFF.md §D3`, 보고서 `docs/reports/2026-06-17-stage-d3-concurrent.md`.
