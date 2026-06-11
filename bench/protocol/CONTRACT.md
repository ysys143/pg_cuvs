# bench/protocol — 스크립트 인터페이스 계약

> 워크플로우 스켈레톤(로컬)과 하네스 스크립트(웹)가 **독립적으로 작업해도 맞물리도록** 고정한 계약.
> 실행 아키텍처는 [`README.md`](README.md), 실험 설계는
> [`design/BENCHMARK_PROTOCOL.md`](../../design/BENCHMARK_PROTOCOL.md)(ADR-069).

---

## 1. 디렉터리 / 진입점

```
bench/protocol/
  run.sh                  # 단일 진입점. 워크플로우는 이것만 호출
  lib/
    resolve_cells.sh      # env → cell 목록 전개
    collect.sh            # phase별 자원 수집(VRAM/RSS/core-s/GPU-s/J) 래퍼
    setup_tables.sh       # per-engine 테이블 생성/적재 (§4)
    engines/{hnsw,cuvs,seqscan,bf}.sh
results/protocol/
  <stage>/<run_id>.csv            # §6 스키마, append-only, 행 완결
  <stage>/<run_id>.manifest.json  # env·pg_settings·버전·gt_method·터미널 상태
  <stage>/<run_id>.progress       # 완료 cell_id 목록 (resume용)
  planner_est/<run_id>.csv        # Stage B 전용 (BENCHMARK_PROTOCOL §6.2)
```

`run.sh`는 **`results/protocol/` 밑 파일만 쓴다. git은 절대 안 건드림** → push는 워크플로우(로컬)가.
자격증명이 스크립트로 새지 않는다.

---

## 2. 입력 (워크플로우 → 스크립트, 전부 env)

워크플로우 `workflow_dispatch` input을 그대로 env로 매핑한다. 이 표가 양쪽의 SSOT.

| env | 값 | 비고 |
|-----|----|----|
| `PGCUVS_STAGE` | `A`\|`B`\|`C`\|`D` | |
| `PGCUVS_MODULE` | A: `physics` / D: `filter`\|`incremental`\|`pareto`\|`concurrency`\|`coldstart`\|`ceiling` | |
| `PGCUVS_CELLS` | `N=1k,10k,100k,1m;dim=1024;k=10;recall=0.95,0.99` | `resolve_cells.sh`가 전개 |
| `PGCUVS_CONFIGS` | `forced-hnsw,forced-cuvs,auto`(+`forced-seqscan,forced-bf`) | |
| `PGCUVS_BASELINE` | `same-box`\|`iso-$` | §BENCHMARK_PROTOCOL 2 |
| `PGCUVS_DATASET` | `cohere-1m`\|`cohere-10m`\|`synth-clustered`\|`synth-random` | |
| `PGCUVS_REPS` | 기본 `5` | latency 셀 반복 |
| `PGCUVS_RUN_ID` | 워크플로우 주입 (GHA run id + ts) | 출력 경로·CSV 행에 박힘 |
| `PGCUVS_DRY_RUN` | `1`=cell 전개만 출력, 실행·기록 없음, exit 0 | GPU 쓰기 전 검증 |
| `PGCUVS_RESUME` | `1`=`.progress`의 완료 cell 스킵 | 멱등 재실행 |
| `PGCUVS_COST_MODEL_VERSION` | 문자열 | 모든 행에 스탬프 (§BENCHMARK_PROTOCOL 6.4) |
| `PGCUVS_RUNTIME_ROUTING_VERSION` | 문자열 | 동일 |
| `PGHOST/PGPORT/PGDATABASE/PGUSER` | 접속 | conda 활성화·`CUVS_INDEX_DIR`은 runner가 |
| `PGCUVS_CPU_SHIM` | `1`=GPU 없이 plumbing 검증 | §7 |

---

## 3. 출력 계약

- **셀 1개 = 원자 단위**. 셀 완료 시: CSV 행 append → `.progress`에 cell_id append → **fsync**.
  중간에 죽으면 `PGCUVS_RESUME=1` 재dispatch로 이어감(멱등).
- **종료 마커**를 stdout 마지막 줄에:
  `PGCUVS_RESULT: status=OK|FAIL|OOM cells_done=N/M` → 웹의 로그읽기+웹훅 진단용.
- **manifest** (`<run_id>.manifest.json`): env 스냅샷 + `SELECT name,setting FROM pg_settings
  WHERE source<>'default'` 덤프 + 버전(pg_cuvs sha·cuVS·CUDA·드라이버) + dataset + `gt_method` +
  start/end + 터미널 상태. → BENCHMARK_PROTOCOL §12 재현성 자동 충족.

---

## 4. per-engine 테이블 분리 (플랜 강제)

같은 컬럼에 두 인덱스가 공존하면 `enable_*` GUC 강제가 취약하다. **테이블을 쪼개** 강제를 견고하게:

| 테이블 | 인덱스 | config |
|--------|--------|--------|
| `t_hnsw` | pgvector HNSW만 | `forced-hnsw` |
| `t_cuvs` | pg_cuvs cagra만 | `forced-cuvs` |
| `t_auto` | **둘 다** | `auto`(planner 선택) + Stage B EXPLAIN 스윕 |

- **같은 PG 인스턴스 · 같은 데이터 · 같은 데몬 1개.** 별도 PG를 띄우는 게 **아니다**.
  pgvector·pg_cuvs는 한 PG의 두 확장으로 공존.
- 빌드 phase는 어차피 엔진별로 따로 재므로 자연스럽다.
- **데몬 footprint 기록**: `forced-hnsw` 측정 중 pg_cuvs 데몬이 점유하는 idle VRAM/RSS를 manifest에
  기록(투명성). 더 엄밀히 가려면 hnsw 구간에 데몬 down 옵션(`PGCUVS_DAEMON_DOWN_FOR_CPU=1`, 기본 off).
- **iso-$ baseline에서만 진짜 분리**: pgvector가 별도 CPU 박스에서 돌아 데몬 자체가 없음.

---

## 5. iso-recall 등화 (config별 노브 sweep)

`run.sh`는 각 (N, recall) 셀에서 엔진 노브를 sweep해 target 만족 **최소값**을 채택한다
(`ef_search`/`probes`/`cuvs.k`). 상한 미달이면 달성 recall 병기 + 미달 표기.
BF 모드는 recall=1.0 — 등화 없이 별도 행. 상세 BENCHMARK_PROTOCOL §3.3.

---

## 6. 결과 CSV 스키마

BENCHMARK_PROTOCOL §4 스키마를 그대로 따른다(행 완결, append-only):

```
run_id, date, stage, phase, cell_id, config,
system, system_version, system_commit, index_type,
N, dim, k, recall_target, dataset, query_set_id, seed, clients, warm_state,
build_s, qps, p50_us, p95_us, p99_us, p999_us, avg_latency_us, recall_at_k,
peak_vram_mb, peak_rss_mb, cpu_core_s, gpu_s, energy_j,
disk_bytes_written, wal_bytes, index_bytes_vram, index_bytes_host, index_bytes_disk,
instance_type, price_usd_hr, usd_per_1m_queries,
reps, agg_method, dispersion, gt_method,
cost_model_version, runtime_routing_version,
selectivity, correlation, filter_mode,        # M1 (PGCUVS_MODULE=filter)
stream_op, ops_done, delta_rows,              # M2 (PGCUVS_MODULE=incremental)
params_json, notes
```

Stage B는 `planner_est/<run_id>.csv`(별도): `cell_id, cost_model_version, path, est_startup,
est_total, est_rows, chosen, measured_ref`.

---

## 7. CPU-shim 테스트 가능성

`PGCUVS_CPU_SHIM=1`에서 GPU 없이 돌아야 한다(Tier-1 shim 재사용). 측정값은 무의미하지만
**배선(cell 열거 · CSV 행 형식 · `.progress` 멱등 resume · manifest · 종료 마커)은 검증 가능**해야
한다. 웹은 첫 GPU dispatch 전에 이걸로 plumbing 버그를 잡는다.

---

## 8. 워크플로우 스켈레톤이 보장할 것 (로컬)

1. `runs-on: [self-hosted, gpu]` — `pg-cuvs-dev` 상주 runner.
2. `workflow_dispatch` inputs → §2 env 매핑(이름 그대로).
3. run 후 `results/protocol/**`를 results 브랜치에 commit & push (`GITHUB_TOKEN` 권한).
4. `concurrency: { group: gpu-singleton, cancel-in-progress: false }`.
5. 워크플로우 파일은 **default 브랜치(main)에 존재**해야 웹이 API로 dispatch 가능.
6. conda 활성화 + `CUVS_INDEX_DIR` 등 runner 환경은 step에서 세팅 후 `run.sh` 호출.
