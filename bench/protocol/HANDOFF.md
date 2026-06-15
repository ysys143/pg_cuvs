# Benchmark Continuation — Handoff

> **Purpose**: hand off the pg_cuvs benchmark campaign to the next session/operator.
> Validation (cost model) is **done & merged**; this is the map for the remaining
> Stage D suite + the harness gaps each module needs. Read this, then
> [`design/BENCHMARK_PROTOCOL.md`](../../design/BENCHMARK_PROTOCOL.md) (v3, the design)
> and [`docs/cost-model-calibration.md`](../../docs/cost-model-calibration.md) (frozen result).

Last updated: 2026-06-15 (after #68 v3 protocol/calibration + #69 harness merged to main).

---

## 1. State snapshot

| Item | State |
|------|-------|
| **v3 protocol** (`design/BENCHMARK_PROTOCOL.md`) | merged (#68) — engines, axes, stages, P1/P2/P3 |
| **Cost-model validation** (Stage B + Stage A cross-check) | **DONE** — `docs/cost-model-calibration.md` (#68). `cost_model_version=v3-phys`, `hw_profile_version=v2` |
| **Harness** (`bench/protocol/`, `infra/anbench/observe.py`) | merged (#69). Engines: cagra, **flat**, **transient-bf**, seqscan, hnsw, bf+batch |
| **Operational guide** (`docs/operational-guide.md`) | v1, has the concurrency + single-client tables |
| **Measured data** (`docs/data/`) | `stageA_exact_v3.csv` (exact-tier), `concurrency_consolidated.csv` (150 rows) |
| **Stage A** (physics curves) | **partial** — exact-tier @10k/100k (TOAST, dim=1024) + concurrency @1k–1M done; full N×dim×storage sweep NOT done |
| **Stage C** (freeze) | done (the calibration report is the freeze) |
| **Stage D** (filter/incremental/Pareto/concurrency/storage) | **NOT started** — this is the next work |

**Engines implemented in main 0.5.0** (what the harness drives): `flat` AM (A1, resident exact GPU BF), `cagra` (GPU ANN), `ivfpq` (GPU PQ), transient-B (`cuvs.gpu_bruteforce=on`, indexless), pgvector HNSW, cpu-seqscan. Cost model = data-movement physics + `pg_cuvs_hw_profile()` probe (ADR-073/074/075).

---

## 2. How to run (dispatch interface)

Everything runs on the always-up A100 dev VM via the **`bench.yml`** workflow (`workflow_dispatch`), self-hosted runner. Dispatch from `main`, check out the harness from the `ref` input.

**Key inputs** (CONTRACT.md §2 → `PGCUVS_*` env):
- `ref` — harness branch (e.g. `main` now that #69 is merged). Builds extension from here if `build=true`.
- `stage` — A | B | C | D. `module` — `physics` (A), `explain` (B), `concurrency`/`filter`/`incremental`/`pareto`/`coldstart`/`ceiling` (D).
- `cells` — e.g. `N=10k,100k;dim=1024;k=10;recall=0.95`.
- `configs` — comma list: `forced-cuvs,forced-flat,forced-seqscan,forced-transient-bf,forced-hnsw,forced-cuvs-bf,forced-cuvs-bf-batch,auto`.
- `build` — `true` rebuilds+reinstalls the extension (needed when src changed or to guarantee the daemon/sidecar are current). `false` reuses installed 0.5.0.
- `reps`, `baseline` (`same-box`|`iso-$`), `dataset`, `stop_vm` (**always false** — keep warm).

**Engine → what it measures** (`runner.py` TABLES/build_index/knob_sweep):
- `forced-flat` → `USING flat` (A1), plan-guard = flat Index Scan, recall=1.0.
- `forced-cuvs` → `USING cagra`, plan-guard = cagra, iso-recall sweep on `cuvs.k`.
- `forced-seqscan` → CPU exact (no index), `enable_cuvs=off`.
- `forced-transient-bf` → `cuvs.gpu_bruteforce=on`, plan-guard = `CuvsTransientBF`.
- `forced-hnsw` → pgvector HNSW (Ring A competitor; **not** a planner option per §0 of the protocol).
- `auto` → planner-auto (NotImplementedError in runner.py — wire it for Stage D auto-envelope; the EXPLAIN runner already does auto-routing).

**Modules → runner**: `physics`→`runner.py`, `concurrency`→`runner_concurrency.py`, `explain`→`runner_explain.py`. Routing in `engines/_common.sh` via `PGCUVS_MODULE`.

**Example dispatches** (validated this session):
```
# Stage B physics cost validation (EXPLAIN-only, cheap):
stage=B module=explain ref=main configs=forced-cuvs build=false \
  cells="N=1k,10k,100k,1m;dim=1024;k=10;recall=0.95"

# Stage A exact-tier (slow paths capped):
stage=A module=physics ref=main build=false reps=1 \
  configs=forced-cuvs,forced-flat,forced-seqscan,forced-transient-bf \
  cells="N=10k,100k;dim=1024;k=10;recall=0.95"

# Concurrency (SLA-bounded QPS, pgbench):
stage=D module=concurrency ref=main build=false \
  configs=forced-cuvs,forced-cuvs-bf,forced-cuvs-bf-batch \
  cells="N=100k,1m;dim=1024;k=10;recall=0.95"
```

---

## 3. Gotchas & lessons (READ — these bit us)

1. **`ALTER EXTENSION pg_cuvs UPDATE` is required.** `build=true` reinstalls the .so/.sql but the `bench` DB keeps the old extension version; `CREATE EXTENSION IF NOT EXISTS` is a no-op → `access method "flat" does not exist`. The runners now do `ALTER EXTENSION pg_cuvs UPDATE`. Keep it for any new runner.
2. **gpu-singleton dispatch = 1 running + 1 pending MAX.** `bench.yml` concurrency group `gpu-singleton` + `cancel-in-progress:false` → dispatching a 3rd run **cancels the older pending one**. **Dispatch at most one-ahead.**
3. **Publish OVERWRITES per run.** Each run rewrites `results/protocol/*.csv` on the `bench-results/protocol` branch — cross-run data is lost. **Pull each run's CSV immediately and consolidate into `docs/data/`** (the durable record). This is why `docs/data/*.csv` exist.
4. **Slow exact paths need query caps.** cpu-seq/transient-B are ~0.1–1 s/query; `reps × 10k queries` = hours. `runner.py` caps them: measured phase `PGCUVS_SLOW_QCAP` (default 300) + warmup 20, **and** the iso-recall sweep (100 queries) — both were needed. cagra/flat (~ms) keep the full set.
5. **Shared dev VM restarts PG during multi-minute ops.** `AdminShutdown: terminating connection due to administrator command` hit cpu-seq@100k and transient-B@100k (each ran minutes) — fast GPU engines never hit it. **Environmental, not a measurement bug.** For robust large-N exact: shrink the cap further, or use a time-bounded pgbench path instead of the per-query loop.
6. **`build=false` is fine after the first `build=true`** of the session — the 0.5.0 binaries persist on the VM. Use `build=true` only when src changed or to refresh the daemon/sidecar.
7. **GT files** `gt_runner_<N>.npy` live on the VM (`~/anbench/data`), built by the physics runner on first use; concurrency runner requires them to pre-exist (run physics for that N first, or it self-builds via `build_gt.py`).
8. **`STORAGE PLAIN` axis is wired but not dispatchable** — `runner.py setup_table` honors `PGCUVS_STORAGE=plain`, but `bench.yml` has no input mapping it. Add a `storage` input → `PGCUVS_STORAGE` env to dispatch the TOAST/PLAIN contrast.

---

## 4. Validated results (don't re-derive)

- **Cost model PASS** (`docs/cost-model-calibration.md`): physics routes GPU from ~1k (legacy mis-routed cagra to seqscan until ~23k); `pg_cuvs_hw_profile()` source=measured/probe 6/6/daemon-match; exact-first holds; DEFAULT fallback safe.
- **ADR-074 reproduced** (Stage A @10k, TOAST, dim=1024, p50): flat 0.86ms(r=1.0) < cagra 1.26ms(r=0.998) ≪ cpu-seq 46.7ms(r=1.0) ≪ transient-B 129.6ms(r=1.0). flat 54× faster than seqscan; transient-B ≈/worse than cpu-seq on PCIe. cpu-seq@100k 697.8ms.
- **Concurrency** (`docs/data/concurrency_consolidated.csv`): single-stream cagra/bf ceiling ~900–1200 QPS; **bf+batch coalescing scales** (10k→5146, 100k→4226 QPS exact) but **SLA-dependent** (1M: cagra wins p99<20ms, bf+batch wins <50ms). **peak QPS is misleading → use SLA-bounded QPS.**

---

## 5. Next work — Stage D suite (priority order)

> Per the protocol, Stage D runs on the **frozen** cost model (done). Each module
> below lists what it measures + the **harness gap** to fill first.

### D-prep · harness gaps (do these first; mostly no GPU)
- **`auto` engine in `runner.py`** — currently `NotImplementedError`. Wire planner-auto (no GUC forcing; assert the chosen plan matches the forced-best). Needed for the "auto-envelope" claim.
- **`PGCUVS_STORAGE` as a `bench.yml` input** — to dispatch the TOAST/PLAIN contrast (D8). One-line workflow edit + env map.
- **`dim` synthetic corpus** — `infra/anbench/fetch_dataset.py` / a small generator for dim ∈ {8, 384, 768} to run the **discriminating-flip** sweep (dim=8 N=10000 legacy→seqscan vs physics→cagra). Currently only the unit fence proves it.
- **observe.py formal columns** — `storage`, `detoast_ms`, `build_kind`, `sla_bounded_qps`, hw_profile fields are in `params_json` now; promote to first-class columns if you want clean aggregation.
- **time-bounded exact path** — to measure cpu-seq/transient-B at large N without the AdminShutdown flakiness (use the concurrency/pgbench `-T` path, or a hard wall-clock cap per cell).

### D1 · Resource–performance Pareto + $ (P1 body)
Reuse Stage A cells + **`baseline=iso-$`** (a CPU-only instance at the same $/hr). Output: `$/1M`, `$/QPS@p99` Pareto (same-box + iso-$), crossover coords. Needs `observe.py` energy/$, already present (`price_usd_hr`, `usd_per_1m_queries`).

### D2 · Filter (selectivity × correlation) — **differentiation core**
`module=filter` (NOT yet implemented — write `runner_filter.py`). Axes: selectivity {0.1,1,5,10,50%} × correlation {random,mixed,spatial}, N=1M/100k. pgvector `hnsw.iterative_scan ∈ {off,strict,relaxed}`; pg_cuvs auto (D-wedge/3O/stream-bf) + flat-filtered + **transient-B filter-first**. Measure recall@k, QPS, p99 tail. The **B filtered crossover** (filter-first vs CPU exact-filtered) is the ADR-073 carry-forward and the live-`auto` prerequisite.

### D3 · Incremental (insert/upsert/FIFO)
`module=incremental` (write `runner_incremental.py`). Scenarios: append, FIFO window (head INSERT + tail DELETE), upsert mix. **ADR-074 reality**: flat write = 1.77ms/row (13× no-index) + HOT-disable + compaction-via-REINDEX (flat has no in-place compact). Frame: **write-heavy(W1) → pgvector no-index; read-heavy(W2) → flat**. Time-series: ingest throughput, recall drift (window GT recompute), concurrent-query QPS during ingest, VRAM growth.

### D4 · Concurrency · tail under load (partially done)
`module=concurrency` works (`runner_concurrency.py`). Done at 1k–1M for cagra/hnsw/bf/bf-batch. **Remaining**: add `forced-flat`/`forced-transient-bf` to the concurrency configs; report SLA-bounded QPS as the headline (not peak). Single-daemon ceiling vs CPU-core scaling crossover.

### D8 · Storage layout (TOAST vs PLAIN)
After the `PGCUVS_STORAGE` input lands: contrast at dim≤768. ADR-074: PLAIN removes the detoast wall (CPU kNN 539→147ms) but **GPU win stays resident-only**. Output: storage×dim×engine matrix.

### D6 · Ceiling (Ring D) + multi-GPU
50M×384(/1024): competitor ceiling + pg_cuvs N/A (VRAM ceiling) recorded as-is. multi-GPU sharding scale (`gpu_count>1` terraform path) moves the single-daemon ceiling. **High-cost — escalate before running.**

### Track 2 (competitor, separate)
Ring A iso-recall (pgvector/pgvectorscale/vchord) + Ring B anchors (raw cuVS/faiss) + Ring C external DBs (separate doc). These are the "which to deploy" comparison, distinct from the planner study.

---

## 6. Backlog / open items

- transient-B@100k+ measurement (env-flaky; predictable ~1.1s — low value).
- `cuvs.gpu_bruteforce=auto` is off (correct on PCIe); revisit on unified-memory HW (GH200/MI300A) — ADR-075 Phase 3.
- `bf+batch` window (`cuvs.bf_batch_wait_us`) tuning per workload.
- plot.py figures for the operational guide (latency-flat, SLA-bounded bars, concurrency scaling) — `uv run bench/protocol/plot.py`.
- #61/#62/#63 still open (superseded by #64; cleanup comments posted — local team to close).

---

## 7. References & coordination

- **Design**: `design/BENCHMARK_PROTOCOL.md` (v3) · ADR-069 (protocol) · ADR-073 (engines) · ADR-074 (characterization) · ADR-075 (cost model) · ADR-061 (strategy/segment).
- **Results**: `docs/cost-model-calibration.md` · `docs/operational-guide.md` · `docs/data/*.csv`.
- **Harness**: `bench/protocol/` (CONTRACT.md = interface SSOT, README.md = ownership) · `infra/anbench/observe.py`.
- **Coordination**: GitHub **issue #56** (web↔local benchmark channel). Diagnostic on the box: `SELECT * FROM pg_cuvs_hw_profile();`.
- **VM**: A100 `pg-cuvs-dev` (always up, PCIe). Never `stop_vm`. Shared with the local dev session → expect occasional PG restarts during long ops (§3.5).
