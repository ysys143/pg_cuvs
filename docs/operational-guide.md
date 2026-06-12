# pg_cuvs Operational Guide — Choosing a Vector-Search Path

> What to run for *your* workload, with the measured numbers behind each choice.
> All data: single **A100-40GB**, Cohere embeddings **dim=1024**, **k=10**,
> approximate paths held at **iso-recall@10 ≈ 0.95**, exact paths at **recall = 1.0**.
> Raw rows: [`docs/data/concurrency_consolidated.csv`](data/concurrency_consolidated.csv).
> Numbers are single-run on one GPU — treat as **directional (±~30% run-to-run)**, not spec.

## The five paths

| Path | What it is | Recall | Build | In the planner? |
|------|------------|--------|-------|-----------------|
| **cpu-seq** | CPU brute force (PostgreSQL seq scan) | exact | none | yes (default small-N) |
| **gpu-cagra** | GPU graph ANN (`USING cagra`) | approx | graph (GPU-fast) | yes (the cuvs index) |
| **gpu-bf** | GPU exact brute force (`cuvs.search_mode=brute_force`) over the cagra `.vectors` sidecar | exact | *(rides on a cagra build — see caveat)* | runtime mode (GUC) |
| **gpu-bf+batch** | gpu-bf + daemon request **coalescing** (`cuvs.bf_batch_wait_us>0`) | exact | same | runtime mode (GUC) |
| **hnsw (cpu)** | pgvector HNSW / cagra→hnsw CPU export (`pg_cuvs_import_cagra`) | approx | graph | a **different deployment** (CPU-serve), not a per-query option vs cagra |

> **hnsw is a deployment/competitor choice, not a planner option.** A column carries
> *one* index; the planner never picks "hnsw vs cagra" in a query. hnsw rows below are the
> CPU-ANN reference, not part of the GPU-box decision surface (`cpu-seq / gpu-bf / gpu-cagra`).

## Quick decision guide

1. **Need exact (recall = 1.0)?**
   - Trivially small table (≲ a few hundred rows): **cpu-seq**.
   - Otherwise, low/moderate concurrency: **gpu-bf** (flat ~1–2 ms up to ~100k).
   - Sustained **high** concurrency + can tolerate a few ms extra latency: **gpu-bf+batch**.
2. **Approximate OK (recall target < 1.0)?** → **gpu-cagra**: ~1.5 ms latency at *any* N,
   ~1200 QPS, SLA-robust. The GPU workhorse.
3. **No GPU at query time?** → hnsw (build on GPU via cagra, export to CPU).

## Single-client latency (c=1) — p50 / p99 in ms

| N | cpu-seq | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|---|
| 1k | – | 0.52 / 0.65 | 1.25 / 1.42 | – | – |
| 10k | 73.2 / 75.6 | 1.12 / 1.52 | 1.60 / 4.13 | 1.14 / 1.30 | 2.29 / 2.49 |
| 32k | – | 4.07 / 6.14 | 1.66 / 4.31 | – | – |
| 100k | 994 / 1032 | 6.13 / 8.93 | 1.43 / 1.62 | 1.90 / 2.10 | 3.34 / 3.60 |
| 200k | – | 6.92 / 9.99 | 1.68 / 4.22 | – | – |
| 500k | – | 10.69 / 14.94 | 1.70 / 4.33 | – | – |
| 1M | – | 11.18 / 16.05 | 1.50 / 1.69 | 6.90 / 11.10 | 8.08 / 13.47 |

**Reading it:**
- **gpu-cagra latency is ~flat (~1.5 ms) from 1k→1M.** hnsw climbs 0.5→11 ms; cpu-seq is hopeless past ~1k.
- **gpu-bf is exact and flat-fast up to ~100k**, then its per-query cost grows with N (1M ≈ 7 ms) — exact brute force genuinely scans more.
- **gpu-bf+batch is *slower* single-client** (the coalescing window adds latency with nothing to batch). It is a throughput tool, not a latency tool.

## Throughput — peak QPS (best across c = 1…64)

| N | cpu-seq | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|---|
| 1k | – | 16,652 | 1,356 | – | – |
| 10k | 82 | 8,438 | 930 | 1,892 | **5,146** |
| 32k | – | 1,911 | 921 | – | – |
| 100k | 8 | 1,201 | 1,206 | 776 | **4,226** |
| 200k | – | 1,049 | 914 | – | – |
| 500k | – | 679 | 910 | – | – |
| 1M | – | 647 | 1,203 | 160 | **1,660** |

## Throughput — SLA-bounded (the *fair* comparison)

Peak QPS flatters `gpu-bf+batch`, which buys throughput by **trading latency** (coalescing).
Holding a p99 ceiling is the honest comparison:

**max QPS with p99 < 20 ms**

| N | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|
| 100k | 1,012 | 1,206 | 776 | **3,811** |
| 1M | 342 | **1,203** | 140 | 890 |

**max QPS with p99 < 50 ms**

| N | hnsw(cpu) | gpu-cagra | gpu-bf | gpu-bf+batch |
|---|---|---|---|---|
| 100k | 1,201 | 1,206 | 776 | **4,226** |
| 1M | 556 | 1,203 | 160 | **1,441** |

**Reading it:**
- **gpu-cagra is SLA-robust**: its ~1200 QPS survives even a tight 20 ms p99 (peak is at low c, low latency).
- **gpu-bf+batch wins big at 100k** under either SLA (exact, 3–4× cagra).
- **At 1M the SLA decides it**: under 20 ms p99, cagra (1203) **beats** bf+batch (890); loosen to 50 ms and bf+batch (1441) wins. This is the crux of "bf+batch is regime-specific."

## Key crossovers (single A100)

| Comparison | Crossover | Note |
|---|---|---|
| cpu-seq ↔ gpu (any) | ~ a few hundred rows | GPU wins almost immediately; cpu-seq 13 QPS @10k vs gpu-bf 862 |
| gpu-bf (exact) ↔ gpu-cagra (approx), single-stream | ~50k | below: exact BF is *faster*; above: cagra (approx) wins |
| hnsw ↔ gpu-cagra, single-client latency | ~20–30k | cagra flat ~1.5 ms overtakes climbing hnsw |
| hnsw ↔ gpu-cagra, peak throughput | ~270k | hnsw per-core QPS falls below cagra's ~1200 floor |
| **planner seqscan → cuvs (modeled)** | **~23k** | **miscalibrated**: cuvs actually wins from ~1k → `CUVS_STARTUP_COST=1000` ~20× too high |

## gpu-bf+batch — when the coalescing window pays off

`cuvs.bf_batch_wait_us` makes the daemon hold incoming brute-force requests for a short
window and fuse the concurrent ones into a single GPU pass. It is the **only** path whose
throughput *rises* with concurrency (others plateau):

```
N=100k   c1    c4    c8   c16   c32   c64
gpu-cagra 696  1206  1194  1183  1184  1181   ← flat (single-stream ceiling)
gpu-bf    524   776   772   771   769   767   ← flat
bf+batch  298  1144  2188  3094  3811  4226   ← scales with load
```

Use it when **all** hold: exact required, **sustained high concurrency** (requests actually
overlap in the window), and the SLA tolerates the added queueing latency. At low concurrency
it is a net loss (the c=1 penalty above). It is **not** the default — it is a high-throughput
specialization. (Tune the window per workload; `cuvs.bf_float16` halves VRAM / raises throughput.)

## Caveats & provenance

- **Single A100-40GB, single GPU.** Crossovers move with CPU cores (hnsw/seq scaling), GPU
  model/count (the ~900–1200 single-stream GPU ceiling), and memory bandwidth. Re-measure per spec.
- **gpu-bf currently requires a cagra build.** BF only needs the raw vectors (`.vectors`
  sidecar), but pg_cuvs ships it as a *mode of the cagra index*, so you pay the (GPU-fast but
  non-zero, VRAM-resident) graph build to use it. A true **no-build standalone BF** — BF's
  signature value (instant, always-fresh, exact) — is a pending design item (ADR).
- **`.vectors` sidecar = full raw corpus** (N·dim·4 B; 1M·1024 ≈ 4 GB), resident on top of the
  graph. This is the real cost of exact GPU search; `bf_float16` halves it.
- **Numbers are single-run on one GPU**; ~30% run-to-run variation observed (e.g. cagra@100k
  peak 914 vs 1206 across runs). Use ranges, not the last digit.
- **Results branch overwrites per run** — this file + the consolidated CSV are the durable record.
