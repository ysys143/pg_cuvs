# bench/ — pg_cuvs Crossover Benchmark harness (pilot)

설계 근거: [`design/BENCHMARK_CROSSOVER.md`](../design/BENCHMARK_CROSSOVER.md).
목적: **동일 데이터 + iso-recall** 조건에서 pgvector HNSW vs pg_cuvs CAGRA의
latency / QPS / build time / recall 을 측정해 crossover 구간을 찾는다.

> 실행 위치: **GPU VM** (`pg-cuvs-dev`). 로컬엔 daemon/toolchain이 없다.
> CAGRA 측정은 `pg-cuvs-server` daemon이 떠 있어야 하고 `IDX_DIR`이 daemon
> `--index-dir`과 일치해야 한다.

## 구성

| 파일 | 역할 |
|---|---|
| `common.py` | fbin/ibin IO, brute-force L2 GT, recall@k (공유 헬퍼) |
| `gen_dataset.py` | 합성 데이터 생성 → `base/query.fbin` + `base/query.copy`(psql `\copy`용) |
| `gt.py` | exact L2 ground truth → `gt.ibin` |
| `recall.py` | "qid nid" 결과 파일 vs `gt.ibin` → recall@k |
| `pctl.py` | stdin의 latency(ms) → p50/p95/p99/avg |
| `run_pilot.sh` | 오케스트레이터: load→build→iso-recall 튜닝→latency→QPS→CSV |
| `results/` | `pilot.csv` 누적 (engine별 한 줄) |

## 공정성 규약 (요약)

- pgvector·pg_cuvs는 **같은 `vector` 컬럼 + 같은 SQL**(`ORDER BY v <-> q LIMIT k`)을
  쓰고 인덱스 정의만 `USING hnsw` vs `USING cagra`로 분기 → 측정 코드 공유.
- **iso-recall**: 각 engine의 노브(`hnsw.ef_search` / `cuvs.k`)를 sweep해서
  `RECALL_TARGET`을 만족하는 **최소값**을 고른 뒤 latency/QPS 비교.
- latency는 `pg_stat_gpu_search`(pgvector엔 없음)가 아니라 **클라이언트측 `\timing`**
  으로 두 engine을 동일 잣대로 측정.

## 사용법

```bash
# 0) (로컬 또는 VM) 데이터 + GT 생성 — diskannpy venv 등 numpy 환경에서
python3 bench/gen_dataset.py --n 100000 --dim 384 --queries 1000 --dist random --out bench/data
python3 bench/gt.py --data bench/data --k 100

# 1) (VM) pgvector HNSW
ENGINE=hnsw  N=100000 DIM=384 K=10 RECALL_TARGET=0.95 bash bench/run_pilot.sh
# 2) (VM) pg_cuvs CAGRA  (daemon 필요)
ENGINE=cagra N=100000 DIM=384 K=10 RECALL_TARGET=0.95 IDX_DIR=/tmp/cuvs_indexes bash bench/run_pilot.sh

# 결과
cat bench/results/pilot.csv
```

## Pilot 매트릭스 (BENCHMARK_CROSSOVER.md §5)

N ∈ {100K, 1M} × dim ∈ {384, 1536} × k ∈ {10, 100} × recall_target ∈ {0.95, 0.99}.
concurrency sweep는 `CLIENTS` 환경변수로 (기본 8). 각 셀을 hnsw·cagra 양쪽 실행.

## 한계 / TODO

- GT는 brute-force(O(nq·n·d)) — N≥10M은 GPU/배치 GT로 교체 필요(주석 참조).
- cost($/QPS), VRAM/host index size, GCS warmup time은 full 단계에서 추가.
- pgvectorscale / VectorChord는 competitive baseline 단계에서 같은 인터페이스로 합류.
- 합성 데이터 우선; 실제 임베딩셋(SIFT/GIST/Cohere 등)은 full 단계.
