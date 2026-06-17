# 세션 보고서 — Stage D3 concurrent 검증 (CAGRA query-QPS-under-ingest)

| 항목 | 값 |
|------|-----|
| 날짜 | 2026-06-17 |
| 범위 | D3 incremental의 마지막 미측정 셀 — `forced-cuvs`(CAGRA) concurrent query-QPS-under-ingest |
| 런 | `gha-27665874191` (A100, real cuVS, Stage D / module=incremental) |
| 결과 | **CAGRA 757.8 → 9.8 qps, 98.7% degradation**. no-index 0% 저하, flat FAILED(사전 진단 finding 재현). status=OK, cells_done=3/3 |
| 산출물 | `bench-results/protocol:results/protocol/D.csv` (commit `4fa15d9`), HANDOFF.md 갱신 (commit `61464c8`) |

---

## 1. 요약 (TL;DR)

D3(incremental)에서 유일하게 "build=true VM 런 대기" 상태로 남아 있던 셀 —
**CAGRA가 백그라운드 스트리밍 ingest(`cuvsCagraExtend`) 중 쿼리 처리량을 얼마나
잃는가** — 를 A100 실측으로 채웠다.

결과는 명확하다. CAGRA 쿼리 처리량은 동시 ingest 하에서 **757.8 → 9.8 qps,
98.7% 붕괴**한다. 원인은 GPU 커널이 아니라 **데몬 내 `g_index_mutex` 단일 락이
extend↔search를 직렬화**하는 구조다. 이는 ADR-074(read-heavy→flat, write-heavy
→no-index)의 근거를 정량적으로 확정한다.

---

## 2. 측정 셀 / 구성

| 항목 | 값 |
|------|-----|
| 데이터셋 | cohere-1m, N=100k, dim=1024, k=10, recall_target=0.99 |
| 시나리오 | `PGCUVS_INC_SCENARIO=concurrent` — base 98k 로드 후, 쿼리 처리량을 (1) ingest 없이, (2) 백그라운드 INSERT 스레드 동시 실행 하에서 각각 측정 |
| 엔진 | `forced-cuvs`(CAGRA, 실 `cuvsCagraExtend`), `forced-flat`, `forced-noindex` |
| 하드웨어 | A100, real cuVS (build=true), `pg-cuvs-a100` self-hosted runner |
| 설계 | best-effort — 개별 셀 실패는 CSV `notes`로 surface하되 런은 실패시키지 않음 |

---

## 3. 결과

| config | index | baseline qps | under-ingest qps | degradation | 비고 |
|--------|-------|-------------:|-----------------:|------------:|------|
| `forced-cuvs` | **CAGRA** | 757.8 | 9.8 | **98.7%** | peak VRAM 384 MB, avg lat 253 µs, gpu 0.68 s |
| `forced-flat` | flat | — | — | **FAILED** | `delta sidecar unusable mid-scan; retry will replan to CPU` |
| `forced-noindex` | seqscan | 1.5 | 1.5 | 0% | CPU seqscan, GPU 경합 무관 |

`PGCUVS_RESULT: status=OK cells_done=3/3` — 3셀 모두 측정 완료(실패는 best-effort로 기록).

---

## 4. 해석

### 4.1 CAGRA — 동시 ingest 중 처리량 99% 붕괴 (핵심 finding)

CAGRA는 단독 쿼리 시 757.8 qps를 내지만, 백그라운드 INSERT 스레드(실 `cuvsCagraExtend`
스트리밍 빌드)가 동시에 돌면 **9.8 qps로 무너진다**. GPU 커널 자체의 문제가 아니다 —
peak VRAM 384 MB, GPU busy 0.68 s로 자원은 여유롭다. 병목은 **데몬의 단일
`g_index_mutex`가 extend와 search를 직렬화**해, 두 세션이 락을 두고 줄을 서는 데 있다.
즉 동시성 한계는 메모리/연산이 아니라 **락 구조**다.

### 4.2 no-index — 0% 저하, 단 절대값 1.5 qps

seqscan 경로라 GPU 데몬을 거치지 않아 ingest와 경합하지 않는다(0% 저하). 그러나 절대
처리량이 1.5 qps로, "동시성에 강하다"가 아니라 "애초에 인덱스 가속이 없다"는 뜻이다.

### 4.3 flat — 사전 진단된 FAILED finding 재현 (회귀 아님)

flat은 `cuvsCagraExtend`가 없어(ADR-073 BF-only, `handle==NULL`) INSERT가 CPU `.delta`
경로로 빠진다. 동시 insert+search는 `.vectors` 카운트 vs in-memory 카운트 shape
mismatch를 일으켜 resident BF가 무효화되고, `delta sidecar unusable mid-scan →
replan-to-CPU`로 떨어진다. 이는 이번 런 이전에 이미 HANDOFF.md에 root-cause로 문서화된
**기존 finding의 재현**이며, 이번 변경으로 생긴 회귀가 아니다.

---

## 5. 결론 / 후속

- **D3(incremental) GPU 런 대기 항목 종결.** CAGRA concurrent 셀이 측정되어 Stage D에는
  더 이상 build=true VM 런을 기다리는 incremental 셀이 없다.
- **ADR-074 정량 근거 확보** — read-heavy는 flat, write-heavy는 no-index. CAGRA는 동시
  ingest+query 워크로드에 부적합(98.7% 저하)함이 실측으로 확정됐다.
- **엔지니어링 후속(벤치 범위 밖)** — `g_index_mutex`의 extend↔search 직렬화를 reader/writer
  또는 더블버퍼링으로 완화하면 CAGRA의 동시 처리량을 회복할 여지가 있다. 별도 설계(ADR)로
  escalate.

---

## 부록 — 재현

```
PGCUVS_STAGE=D PGCUVS_MODULE=incremental \
PGCUVS_CELLS='N=100k;dim=1024;k=10;recall=0.99' \
PGCUVS_CONFIGS=forced-cuvs-concurrent,forced-flat-concurrent,forced-noindex-concurrent \
PGCUVS_DATASET=cohere-1m PGCUVS_INC_SCENARIO=concurrent \
bash bench/protocol/run.sh
```

원시 결과: `bench-results/protocol:results/protocol/D.csv` (run_id `gha-27665874191-1`),
런 로그 아티팩트 `bench-log-27665874191`.
