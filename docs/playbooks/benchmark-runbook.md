# Playbook: 벤치마크 실행 (benchmark-runbook)

pg_cuvs 벤치마크 하네스 사용법, 결과 위치, 해석 기준.
`infra/scripts/benchmark.sh`(`make gpu-bench`), `bench/run_cohere.sh`(`make gpu-cohere`),
`infra/anbench/run_all.sh`(`make gpu-anbench`) 세 하네스를 다룬다.
장애 대응이 아닌 측정 절차이므로 각 섹션은 "결과 해석 → 다음 단계"로 구성된다.

---

## 1. 증상 (Symptoms)

이 playbook은 다음 상황에서 실행한다:

- 코드 변경 또는 GUC 조정 후 latency/recall/QPS 회귀 여부를 확인할 때.
- capacity-planning.md의 VRAM 추정과 실측 빌드 시간을 대조할 때.
- 경쟁 엔진(pgvector HNSW, VectorChord 등) 대비 crossover 좌표를 측정할 때.
- `design/BENCHMARK_CROSSOVER.md`의 미완료 셀(실제 임베딩 데이터, N 10M+,
  멀티 GPU sharded QPS)을 채울 때.

---

## 2. 확인 명령 (Diagnostic commands)

```bash
# GPU 상태 (벤치 전 사전 확인)
nvidia-smi --query-gpu=index,name,memory.total,memory.free \
  --format=csv,noheader,nounits
# 기대: free 충분, pg_cuvs_server 외 다른 CUDA context 없음
```

```bash
# pg_cuvs_server 단독으로 GPU 점유 중인지 확인 (ADR-002: 백엔드 직접 CUDA 금지)
nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
  --format=csv,noheader
# 기대: pg_cuvs_server 프로세스만 있어야 함
```

```sql
-- 데몬 상주 인덱스 및 현재 통계
SELECT index_name, n_vecs, dim, search_count, avg_latency_us,
       p50_latency_us, p95_latency_us, p99_latency_us,
       error_count, resident
  FROM pg_stat_gpu_search
 ORDER BY n_vecs DESC;
```

---

## 3. 원인 분기 (Cause branches)

### A. 결과 해석 기준

| 관측 | 의미 | 다음 단계 |
|------|------|-----------|
| `jit_section: yes` + p95 < p50×3 | JIT 무해 또는 이득 | threshold sweep 생략 가능 |
| `jit_section: yes` + p95 >= p50×3 | JIT이 latency spike 유발 | `jit-threshold-sweep.md` 실행 |
| `fallbacks: 0` | VRAM 충분 | 정상 |
| `fallbacks > 0` | VRAM 부족 | `--max-vram-mb` 조정 또는 `vram-oom-fallback.md` |
| nvidia-smi에 PG 백엔드가 CUDA context 소유 | ADR-002 위반 | 즉시 에스컬레이션 |
| recall이 기대보다 낮음 | `cuvs.k` 부족 또는 shard_count 과다 | `cuvs.k=200` 시도 또는 `OPS_GPU_PLAYBOOK.md §1` 참조 |

### B. 합성 데이터 vs 실제 임베딩 데이터 해석 주의사항

- **clustered synthetic 데이터**(pilot §11, §12)의 recall은 실제 분포보다 낙관적.
  IVF 계열(VectorChord vchordrq) recall이 과도하게 높게 나온다. 실 데이터에서는
  probes가 훨씬 높아야 동일 recall 달성.
- **synthetic random**(§13 3I import)의 recall=1.0000은 균일 분포의 특성. 실제
  임베딩(Cohere 1024d 실측 §16: 3I recall=0.992)에서는 낮아진다.
- **build speedup**(`2.6x–15.8x`, §13)은 데이터 독립적: CAGRA build/HNSW import
  시간은 벡터 내용이 아닌 N·dim·M에 의존. 실 데이터에서도 유사 예상.
- **latency crossover(N≈10k~100k, §11)**는 clustered synthetic + 단일 A100 기준.
  실 데이터·다른 dim·다른 HW에서는 crossover 좌표가 달라질 수 있다.

### C. p50_latency_us 비교 시 주의
`pg_stat_gpu_search.p50/p95/p99_latency_us`는 **log2 버킷** 양자화값이다.
두 설정의 latency 차이가 같은 octave 안에 있으면 동일 값으로 보인다.
정밀 비교는 `avg_latency_us`(= total_time / search_count)를 사용한다.
(검증됨: sequential avg 1492µs vs parallel avg 1053µs가 p50으로는 동일하게 표시됨,
`multi-gpu-sharding-ops.md §3C` 참조.)

---

## 4. 복구 절차 (Recovery steps)

### 하네스 A — `make gpu-bench` (benchmark.sh, 기본 합성 벤치)

**용도**: 단일 셀(N×dim) 빠른 sanity check, JIT 확인, VRAM 사용량 측정.

```bash
# 기본 (10K×384)
make gpu-bench

# 임의 셀 지정
make gpu-bench N=1000000 DIM=384 K=100

# 1M×1536 (PLAN 완료 게이트)
make gpu-bench-1m
```

내부적으로 `infra/scripts/benchmark.sh`를 VM SSH로 실행하고 결과를
`design/bench_<timestamp>.log`에 tee한다.

**기대 출력 (SUMMARY 블록):**
```
[bench] SUMMARY
build_time_s: 28.3
cagra_bytes: ...
vram_used_mb_after_build: 1638
exec_p50_ms: 1.2
exec_p95_ms: 1.8
fallbacks: 0
jit_section: no
```

**결과 위치:** `design/bench_<YYYYMMDD_HHMM>.log`

| 항목 | 기대 | 이상 시 |
|------|------|---------|
| `jit_section` | `no` | `yes` → p95/p99 확인 후 `jit-threshold-sweep.md` |
| `fallbacks` | `0` | `>0` → VRAM 부족 → `vram-oom-fallback.md` |
| `build_time_s` (1M×384) | ≈ 28s | 크게 늘면 VRAM 경쟁 또는 thermal throttling 확인 |

---

### 하네스 B — `make gpu-cohere` (bench/run_cohere.sh, 실제 임베딩 벤치)

**용도**: Cohere Wikipedia 1024d 실제 임베딩으로 recall/latency/QPS 측정.
현재까지 유일한 실제 임베딩 결과가 이 하네스로 생성됐다(§16, N=1M×1024).

```bash
# 기본 (N=1M, GPU 0)
make gpu-cohere
# 내부적으로:
# nohup bash bench/run_cohere.sh --n 1000000 --gpu 0 > /tmp/cohere_bench.log 2>&1 &
```

데이터셋 사전 준비가 필요하다(`infra/anbench/fetch_dataset.py`로 Cohere Wikipedia
1024d EN subset 다운로드). 초회 실행 시 데이터 다운로드 시간이 길므로
nohup + log tail로 진행률을 확인한다:

```bash
# VM에서 진행 확인
ssh $GCP_VM 'tail -f /tmp/cohere_bench.log'
ssh $GCP_VM 'grep -E "recall|QPS|build|p50" /tmp/cohere_bench.log | tail -20'
```

**결과 위치:** `bench/results/cohere_N<N>.jsonl` +
`bench/results/cohere_N<N>_summary.csv`

**실측 참조값 (§16, 1M×1024, 단일 A100-40GB):**

| 시스템 | recall@10 | QPS | p50 | 설정 |
|--------|-----------|-----|-----|------|
| pg_cuvs CAGRA | 0.9912 | 227 | 4.4ms | k=100 |
| pgvector HNSW | 0.9891 | 45 | 22ms | ef=400 |
| 3I import_hnsw | 0.9993 | 16.9 | 61ms | ef=512 |

**빌드 시간 참조값 (§16, 1M×1024):**
- pgvector HNSW native: 285s
- 3I (CAGRA 85s + import_hnsw 57s): **142s** → **2.0× speedup**

---

### 하네스 C — `make gpu-anbench` (infra/anbench/run_all.sh, ann-benchmarks 호환)

**용도**: ann-benchmarks 스타일의 다중 엔진 표준 비교.
recall@k, QPS, build time을 통일된 형식으로 수집한다.

```bash
# 전체 실행 (VM에서)
make gpu-anbench
# 내부적으로:
# bash infra/anbench/run_all.sh

# 개별 엔진 직접 실행 (VM에서)
python3 infra/anbench/run_cuvs.py      # pg_cuvs CAGRA
python3 infra/anbench/run_pg.py        # pgvector HNSW
python3 infra/anbench/run_pg_3i.py     # 3I import 경로
python3 infra/anbench/run_faiss.py     # FAISS (baseline)
python3 infra/anbench/run_cagra_hnsw.py  # cuVS lib CPU search
```

데이터셋 준비:
```bash
# Cohere Wikipedia 1024d (또는 다른 데이터셋) 다운로드
python3 infra/anbench/fetch_dataset.py
```

GT(ground truth) 생성:
```bash
# GT 재생성 (gt_faiss.py 수정 이후에는 반드시 재실행 — §12 GT 버그 수정 참조)
python3 infra/anbench/build_gt.py
```

**결과 위치:** `design/anbench/` 디렉토리

> GT 버그 주의(§12, 수정 완료 2026-05-31): `gt_faiss.py --regen`의 배치 크기가
> `load_binary.py`와 달랐다면 모든 recall=0.0000. commit `6a74863`(GBATCH=50_000) 이후
> 빌드에서는 수정됐다. 기존 GT 파일이 있다면 재생성 후 사용.

---

### 파라미터 매트릭스별 셀 실행

```bash
# N/DIM 조합별 실행 예시
make gpu-bench N=100000  DIM=384
make gpu-bench N=1000000 DIM=384
make gpu-bench N=100000  DIM=1536
make gpu-bench N=1000000 DIM=1536

# 결과 집계 (bench/aggregate.py)
python3 bench/aggregate.py bench/results/
```

**결과 기록 형식 (`bench/results/*.csv`):**
```
system, version, index, N, dim, k, recall_target,
build_s, index_bytes_vram, qps, p50_us, p95_us, p99_us,
avg_latency_us, recall_at_k, hw, params_json
```

---

## 5. 검증 명령 (Verification commands)

```bash
# SUMMARY 블록 존재 확인 (benchmark.sh 정상 완료)
grep '\[bench\] SUMMARY' design/bench_*.log | tail -5
# 기대: 각 실행마다 SUMMARY 블록 1개
```

```bash
# fallbacks = 0 확인
grep 'fallbacks:' design/bench_*.log | tail -5
# 기대: fallbacks: 0
```

```bash
# nvidia-smi compute-apps: pg_cuvs_server만 있는지 확인 (ADR-002)
nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory \
  --format=csv,noheader
# 기대: pg_cuvs_server 프로세스만
```

```sql
-- avg_latency_us 기준으로 회귀 확인
-- (p50_latency_us는 log2 버킷이므로 비교에 부적합)
SELECT index_name, avg_latency_us, search_count, error_count
  FROM pg_stat_gpu_search;
-- 기대: error_count = 0, avg_latency_us가 참조값 범위 내
```

```bash
# 결과 파일 생성 확인
ls -lh bench/results/cohere_N*.jsonl bench/results/cohere_N*_summary.csv 2>/dev/null
ls -lh design/bench_*.log 2>/dev/null
```

- [ ] `jit_section: no` (또는 yes + p95/p99 허용 범위 내)
- [ ] `fallbacks: 0`
- [ ] nvidia-smi에 `pg_cuvs_server`만 있음 (ADR-002 준수)
- [ ] SUMMARY 블록에 `build_time_s`, `exec_p50_ms`, `exec_p95_ms` 값 존재
- [ ] 결과 CSV/JSONL 파일 생성됨

> 검증 근거: `large-dataset-benchmark.md`(benchmark.sh 래퍼, SUMMARY 형식, ADR-002
> compute-apps 검증); `BENCHMARK_CROSSOVER.md §11·§12·§13·§16`(참조값 + GT 버그 이력).

---

## 6. Escalation 기준 (When to escalate)

- `nvidia-smi compute-apps`에 PG 백엔드 프로세스가 CUDA context를 소유하면:
  즉시 에스컬레이션. ADR-002 위반.
- `build_time_s`가 capacity-planning.md 추정(1M×384 ≈ 28s)의 2배 이상:
  GPU thermal throttling 또는 다른 CUDA context 경쟁 → `nvidia-smi dmon` 확인.
- recall이 참조값(CAGRA 1M×384 ≈ 0.978)보다 크게 낮음(>0.05 차이):
  `cuvs.k` 값 확인 — 기본 100 미만이면 recall 급락. `cuvs.k=200` 시도.
- GT recall이 모두 0.0000: GT 파일이 배치 크기 불일치로 오염됐을 가능성 →
  `build_gt.py` / `gt_faiss.py --regen`으로 GT 재생성.
- 50M×384 벤치가 데몬 교착으로 실패: 단일 노드 한계(§12 확인됨).
  A100-80GB × 2(160 GB VRAM) 이상 필요. `capacity-planning.md` 참조.
- `make gpu-cohere` 실행 전 `fetch_dataset.py`가 없거나 데이터셋 미다운로드:
  `infra/anbench/fetch_dataset.py` 먼저 실행.

관련: `large-dataset-benchmark.md`(기존 벤치 절차), `capacity-planning.md`(VRAM 계획),
`jit-threshold-sweep.md`(JIT spike 시), `vram-oom-fallback.md`(fallback 시).  
설계 근거: `design/BENCHMARK_CROSSOVER.md`(crossover 가설 H1–H4, harness 설계 §6,
결과 스키마), `design/OPS_GPU_PLAYBOOK.md §3.2`(빌드 시간 참조).
