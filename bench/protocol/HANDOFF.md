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
| **Stage D** (filter/incremental/Pareto/concurrency/storage) | **re-audited 2026-06-16** — most modules already exist or are small deltas; only D3 (incremental perf) is genuinely new. See §5. |

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

## 5. Next work — Stage D suite (RE-AUDITED 2026-06-16)

> **Re-audit correction.** The earlier "Stage D NOT started" framing was wrong. A
> full sweep of existing assets (`tools/`, `infra/anbench/`, `docs/`, `test/`) shows
> **most of Stage D already exists or is a small delta** — only **D3 (incremental
> perf)** is a genuinely new harness. Two parallel harnesses exist and are **not
> integrated**: **`bench/protocol/`** (this campaign — physics/concurrency/explain,
> writes the `observe.py` CSV) and **`infra/anbench/`** (an older competitor suite —
> `run_pg.py`/`run_cuvs.py`/`run_faiss.py`/`run_cagra_hnsw.py`, JSONL, `aggregate.py`
> Pareto plots, `run_all.sh`). **Reuse `infra/anbench/` for Ring A/B instead of rebuilding.**

### Status legend (audit evidence)

| Module | Status | Already exists (evidence) | Real remaining delta |
|--------|--------|---------------------------|----------------------|
| **D8 storage** | ✅ near-done | `docs/profiling-results.md §4`: TOAST vs PLAIN measured (PLAIN build +8%, search storage-independent); `runner.py setup_table` honors `PGCUVS_STORAGE=plain` | `bench.yml` `storage` input (~2 lines) + republish; *optional* one PLAIN run at dim≤768 |
| **D4 concurrency** | 🟡 ~80% | `runner_concurrency.py` + `docs/data/concurrency_consolidated.csv` (150 rows: cagra/hnsw/bf/bf-batch/seqscan, N 1k–1M); `observe.sla_bounded_qps` column exists | add `forced-flat`/`forced-transient-bf` configs (~5 lines) + emit `sla_bounded_qps` as headline + rerun |
| **D1 Pareto $** | 🟡 partial | cost/energy fields populated (`observe.price_usd_hr`, `usd_per_1m_queries`, `energy_j`); `aggregate.py` already plots Pareto; Stage A data | small aggregator over the protocol CSV (reuse `aggregate.py` logic) + **one** `baseline=iso-$` CPU dispatch + **VRAM-budget cell** (ivfpq vs cagra at fixed VRAM — the protocol's 1급 D1/D6 axis, §70/§84; ivfpq is the only engine that bends the resource-Pareto) |
| **D2 filter** | 🟡 partial | `tools/bench_filter_sweep.py` + `docs/filter-threshold-experiment.md`: pg_cuvs D-wedge sel×corr measured → `auto_threshold=0.05` | add **pgvector `iterative_scan` competitor + explicit p99 tail**; the pg_cuvs side is done |
| **Ring A competitors** | 🟡 partial | `infra/anbench/run_pg.py` (pgvector hnsw/ivfflat/exact) | add `run_pgvectorscale.py`/`run_vectorchord.py` on the `run_pg.py` skeleton; pgvector `iterative_scan` mode |
| **Ring B anchors** | ✅ exists | `run_cuvs.py` (raw CAGRA), `run_faiss.py` (gpu/cpu), `run_cagra_hnsw.py`, `aggregate.py`, `run_all.sh` | none new — just run + consolidate into `docs/data/` |
| **D3 incremental** | 🔴 missing (perf) | **correctness only** (`test/sql/cagra_streaming.sql`, `delta_recall.sql`, `auto_compact.sql`); ADR-074 1.77ms/row is a single non-repeatable point | **`runner_incremental.py` — the one genuinely new harness** |
| **D6 ceiling — CAGRA 50M** | 🔴 cite-only | **50M×384 already measured (ADR-025, 2026-05-30): CAGRA shard=2 & shard=4 both OOM on A100-40GB×2** (73.24 GiB raw f32 > 80 GB VRAM); competitor numbers (HNSW p50=13ms/QPS=546, vchordrq recall=0.9991) recorded there | **50M CAGRA = cite ADR-025, do NOT re-run** (same OOM, A100-80GB×2 needed) |
| **D6 ceiling — IVF-PQ 50M** | 🟡 **runnable, UNMEASURED** | **`ivfpq` AM implemented (ADR-049, 20/20 PASS 2026-06-08) — but landed AFTER the 50M run, so ADR-025 never tested it.** Compressed codes ≈ pq_dim·pq_bits/8 per vec: 50M×(192 B) ≈ **9.6 GB → fits a single A100-40GB** | **the real large-scale arm.** Add `forced-ivfpq` to `runner.py` (gap — see below), then 50M head-to-head: IVF-PQ recall@n_probes vs vchordrq 0.9991 vs HNSW. Compression ANN race, not exact |
| **D6 multi-GPU** | 🔴 out / escalate | terraform `gpu_count>1` path ready | **multi-GPU sharding NOT implemented in the product** (no daemon shard routing) → engineering, not a benchmark; 10M CAGRA = high $ |
| **IVF-PQ engine (axis-wide)** | 🟢 iso-recall validated | `ivfpq` AM (ADR-049); `forced-ivfpq` wired (`b858656`) + build-knob sweep (`2b06b3a`). A100 runs: #30 default pq_dim/2 → recall **0.937** (54 MB, 7.6× vs raw); #31 build sweep climbs pq_dim {256→512→1024}, stops at **pq_dim=1024 → recall 0.9651 ≥ 0.95** (`iso_recall_met=true`, builds_tried=3, n_probes=64, p50 1.57ms/p99 4.52ms) | none for the harness — both the n_probes knob and the build-knob sweep work. Open cost question is RaBitQ (below): ivfpq needs 1024 B/vec to hit 0.95, RaBitQ projects ~136 B |

> **⚠ Engine-axis gap (added 2026-06-16)**: IVF-PQ was under-counted in the first
> re-audit — fixed only at 50M, then realised it's missing axis-wide. The protocol
> SPEC already treats ivfpq as a first-class engine, but neither the **harness**
> (`runner.py`) nor this handoff carried it. The headline ivfpq deliverable is **not**
> 50M — it's the **VRAM-budget cell in D1/D6**: at a fixed VRAM, ivfpq trades recall
> for 10–100× capacity, so it is the only engine that changes the *shape* of the
> resource/$ Pareto. Everything ivfpq is blocked on one Tier-0 item: `forced-ivfpq`
> in `runner.py` (build reloptions `n_lists/pq_bits/pq_dim` + `cuvs.ivfpq_n_probes` sweep).

> **Schema note**: `observe.PROTOCOL_FIELDS` already has first-class `selectivity`,
> `correlation`, `filter_mode`, `stream_op`, `ops_done`, `delta_rows`,
> `sla_bounded_qps`, `detoast_ms`, `build_kind` — it was designed for D2/D3/D4.
> **No schema gap.** The old D-prep "promote columns to first-class" item is already done.

### Priority order (value / effort)

**Tier 0 — enablers & small deltas (cheap, mostly no GPU)**
- **`PGCUVS_STORAGE` → `bench.yml` input** (~2 lines + env map). Unblocks D8 dispatch (§3.8).
- **`auto` engine in `runner.py`** — ✅ **DONE + validated** (run #36). Builds the cagra index but does NOT force the plan; the ADR-075 cost model routes per query, and the chosen path is recorded (`params_json.chosen_plan`, `notes: auto→X`), not asserted. `engines/auto.sh` added. At dim=1024 both N=1k and N=100k routed to **cagra** (recall 1.0 / 0.9913) — correct: high dim → GPU wins from small N (κ ∝ 1/dim). The seqscan side of the flip needs a low-dim cell (→ `dim` integration item below).
- **`forced-ivfpq` config in `runner.py`** — ✅ **DONE + smoke-validated** (`b858656`; build `USING ivfpq` `n_lists`=√N + AM-default `pq_bits`/`pq_dim`; recall knob `cuvs.ivfpq_n_probes` [16..512]; plan guard; `engines/forced-ivfpq.sh`). A100 run #30 (`bench.yml` dispatch, `ref=docs/benchmark-handoff`, N=100k·d1024): PASS — plan guard OK, build 5.4s, **resident 54 MB vs 410 MB raw (7.6×)**, recall@10=0.937 @ n_probes=512, qps=510, p50=1.88ms/p99=4.67ms. Row in `bench-results/protocol` `results/protocol/A.csv`.
- **`pq_dim` build-knob sweep — ✅ DONE + validated** (`2b06b3a`). Run #30 found ivfpq recall caps at 0.937 < 0.95 because the loss is PQ quantization (a BUILD param), not the query-time `n_probes` knob (which already probed all √N≈316 lists). `build_knob_sweep` now climbs an ascending pq_dim ladder {dim/4, dim/2, dim} and stops at the most-compressed build meeting target. Run #31 confirmed: pq_dim=1024 → **recall 0.9651 ≥ 0.95** (builds_tried=3, n_probes=64). Cost paid: 3 rebuilds + 2× storage (pq_dim 512→1024 = 1024 B/vec). That storage cost is exactly what motivates the RaBitQ track (below).
- **D4 configs** — add `forced-flat`/`forced-transient-bf` to `runner_concurrency.py` (`CUVS_SEARCH`/`KNOB_GUC`); emit `sla_bounded_qps` as the headline; rerun the protocol cells and merge into `concurrency_consolidated.csv`.
- **`dim` synthetic integration** — `bench/gen_dataset.py` already has a `--dim` knob; wire it into the cell parser for dim ∈ {8,384,768} (the discriminating-flip sweep: dim=8 N=10000 legacy→seqscan vs physics→cagra). `infra/anbench/fetch_dataset.py` is 1024-only.
- **time-bounded exact path** — measure cpu-seq/transient-B at large N without the AdminShutdown flakiness (concurrency/pgbench `-T` path, or a hard per-cell wall-clock cap).

**Tier 1 — analysis / republish (reuse existing, no new measurement)**
- **D8** — republish `docs/profiling-results.md §4` (TOAST vs PLAIN) into the benchmark doc as the storage result. Optionally one PLAIN run at dim≤768 to confirm the 8% holds where detoast is a bigger share.
- **D1 Pareto** — write a small aggregator (reuse `aggregate.py` Pareto logic) over the protocol CSV's existing cost/energy fields; dispatch **one** `baseline=iso-$` CPU instance for the iso-$ arm; emit `$/1M` + `$/QPS@p99` curves + crossover coords.

**Tier 2 — differentiation deltas (small new code)**
- **D2** — extend the filter measurement with the **pgvector `iterative_scan ∈ {off,strict,relaxed}` competitor** and explicit **p99 tail** (the pg_cuvs D-wedge side is already measured in `bench_filter_sweep.py`). Either extend that script or a thin `runner_filter.py`. The B filtered crossover (filter-first vs CPU exact-filtered) is the ADR-073 carry-forward and the live-`auto` prerequisite.
- **Ring A competitors** — `run_pgvectorscale.py` / `run_vectorchord.py` on the `run_pg.py` skeleton (in-PG; identical load/build/query pattern).

**Tier 3 — genuinely new harness (the only big build)**
- **D3** — `runner_incremental.py`: scenarios append / FIFO window (head INSERT + tail DELETE) / upsert mix. Metrics: ingest throughput (rows/s), p50/p95/p99 per-row, VRAM growth, concurrent-query QPS during ingest, recall drift (window GT recompute). Frame **W1→pgvector no-index vs W2→flat** (ADR-074: flat write 1.77ms/row, HOT-disabled, compaction-via-REINDEX). The correctness tests above are prerequisites, **not** reusable for perf.

**Out of scope / escalate**
- **D6 CAGRA / multi-GPU** — 10M CAGRA (high $); **50M CAGRA = already measured in ADR-025 (OOM on A100-40GB×2). Do NOT re-run — cite the table, record the cell as "N/A — VRAM ceiling, A100-80GB×2 needed".** Multi-GPU sharding (**product feature not implemented** — no shard routing in the daemon; engineering, not benchmarking). Escalate before spending.

> **NOTE — 50M IVF-PQ is NOT out of scope** (was missed in the first draft). ADR-049's `ivfpq` AM landed *after* the ADR-025 50M run, so the large-scale CAGRA-OOM verdict never had an IVF-PQ counterpoint. IVF-PQ compresses 50M×384 to ≈9.6 GB → **fits a single A100-40GB**, so it is genuinely runnable and is the *intended* answer for that scale (ADR-049 guide: IVF-PQ ⟶ 100M+). Once `forced-ivfpq` exists (Tier 0), this becomes a real Tier-2/3 benchmark cell, not an escalation: 50M IVF-PQ recall@n_probes vs vchordrq (0.9991) vs CPU HNSW. Recall is medium and `n_probes`-sensitive — that tradeoff IS the result.
- **Ring C** (Milvus/Qdrant/LanceDB) — separate system-level doc, deprioritized.

### Research track — native RaBitQ quantizer (spike GREEN on cohere, ADR candidate)

> Why: ivfpq needs **1024 B/vec** to reach iso-recall 0.95 on cohere-1024 (run #31).
> vchordrq hits 0.9991 at 50M because **RaBitQ** (Gao & Long, SIGMOD'24) reranks
> with a *theoretical error bound* — high recall at low memory, no full-f32 rerank
> penalty. The recall idea is reachable via cuVS `refine()` (option B, deferred —
> needs original vectors resident-or-streamed, partially undoing PQ's VRAM win), but
> the *full* RaBitQ win needs the quantizer itself.

- **numpy feasibility spike — ✅ GREEN** (`tools/rabitq_spike.py`, `268b05b`). On synthetic clustered dim=1024 (N=20k/50k, 2 seeds): unbiased (standardized std=**1.001** — theoretical variance form matches), error-bound coverage **0.9901**, recall@10 **0.966 @ 5% rerank** / 0.994 @ 10%, storage **136 B/vec = 30× vs raw, 3.8× smaller than ivfpq**. The math checks (unbiased + bound) are data-agnostic → estimator is correct.
- **cohere VM validation — ✅ GREEN, knee characterized** (runs #32→#33, `engines/spike-rabitq.sh`, real `corpus.fbin` 100k×1024). Math identical to synthetic (unbiased std=**1.000**, coverage **0.9901**). Recall grid (rows=n_probes of 316 lists, cols=rerank budget):

  | n_probes | 0.1% | 0.5% | 1% | 2% | 5% |
  |---|---|---|---|---|---|
  | 316 (all) | 0.9995 | 1.0000 | 1.0000 | 1.0000 | 1.0000 |
  | 64 | 0.8875 | **0.9675** | 0.9675 | 0.9675 | 0.9675 |
  | 32 | 0.6980 | 0.9165 | 0.9185 | 0.9185 | 0.9185 |
  | 16 | 0.6540 | 0.8310 | 0.8510 | 0.8510 | 0.8510 |
  | 8 | 0.6140 | 0.6770 | 0.7660 | 0.7660 | 0.7660 |

  Resolves the run #32 "suspicious 1.0": the **quantizer is genuinely excellent** (probe-all = 0.9995 at just 0.1% rerank → RaBitQ ranks cohere near-perfectly, not a bug). The per-row ceiling is **IVF miss** (fraction of true-NN clusters probed), not a RaBitQ fault — and a tiny 0.5% rerank already reaches it. Gate met at a realistic **n_probes=64 → 0.9675 ≥ 0.95**; raise n_probes to lift the ceiling (cheap — 136 B codes). Storage **136 B/vec = 30× vs raw, 3.8× < ivfpq's 1024 B needed for the same recall**.
- **If cohere holds** → write an ADR (like ADR-049 for ivfpq) + port to a CUDA kernel + `rabitq` AM. Effort: spike S (done) → CUDA encoder/estimator/bound M–L (first self-authored ANN numerics, correctness-sensitive) → AM integration M (flat/ivfpq template) → validation harness M (non-negotiable). Tractable *because the blocker is our own bounded numerics, not an unstable upstream API* (unlike DiskANN, ADR-026) — and it extends the hot-tier value prop (more vectors/GPU at high recall), which is in-segment.
- **Deferred / measured-out: option B (cuVS `refine()` for ivfpq)** — tested for real (cuVS 26.04 python spike `tools/ivfpq_refine_spike.py`, run #35, cohere 100k): refine **works**, lifts recall@10 0.9095→**0.9685** (ratio≥2, sub-ms). But dataset device-resident (variant A) → VRAM = full f32 (~419 MB/100k), and same-VRAM **flat is exact (1.0) → dominates variant A**; RaBitQ hits the same 0.968 at **136 B/vec (30× less)**. So variant A has no product value (not building it). The valuable B (PQ codes resident + originals streamed NVMe→VRAM via **GDS**) needs GDS hardware (NVMe + nvidia-fs + cuFile) we don't have → moved to the ADR-072 cold-tier track. Plumbing path recorded: `refine_ratio` via the `ivfpq_n_probes` GUC→IPC→wrapper chain.

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
