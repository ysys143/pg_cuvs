# pg_cuvs

GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS — a heterogeneous acceleration path that keeps Postgres as the control plane.

**Built on**: [pgvector](https://github.com/pgvector/pgvector) · [RAPIDS cuVS](https://github.com/rapidsai/cuvs) · [PG-Strom](https://github.com/heterodb/pg-strom) (architectural inspiration) · [pgvectorscale](https://github.com/timescale/pgvectorscale) (DiskANN reference)

## What it is

pg_cuvs is **not** a replacement for pgvector. It is a GPU acceleration layer that sits on top of pgvector's interface. SQL syntax, transaction semantics, MVCC, and planner decisions remain entirely within PostgreSQL.

It supports two complementary modes:

- **GPU search tier**: keep CAGRA indexes resident in GPU VRAM and use the GPU as a candidate generator.
- **GPU build accelerator tier**: use nearby GPUs to build indexes quickly, then convert the result into a standard pgvector HNSW index (`USING pg_cuvs_hnsw`) and serve queries on CPU.

```sql
-- No query changes required. pg_cuvs accelerates this transparently.
SELECT * FROM items ORDER BY embedding <=> $1 LIMIT 10;
```

The GPU acts as a **candidate generator** — returning the top-K TID candidates and distances — and PostgreSQL handles heap access, visibility checks, joins, and filters as usual.

For on-prem or private RAG systems, this is useful even when search serving stays on CPU. These deployments often already run GPU servers for embedding models, rerankers, or batch embedding jobs. pg_cuvs can reuse that nearby GPU pool for slow index build/rebuild windows, while keeping the online serving path as ordinary PostgreSQL + pgvector HNSW.

## CI / testing

Two tiers (`design/CI_STRATEGY.md`, ADR-067):

- **Tier 1 — CPU reference (every PR, hosted, free).** The single GPU boundary
  (`src/cuvs_wrapper.h`) is replaced by an exact-kNN **CPU shim** (`make
  PGCUVS_CPU_SHIM=1`, zero CUDA), and the full daemon + IPC + fail-closed +
  correctness regression suite runs against it. This catches the bug class that
  actually bites — IPC/struct drift, `search_mode` labeling, fail-closed rejects,
  manifest contract, recall — without a GPU.
- **Tier 2 — real A100 (on demand).** GPU-kernel correctness, **approximate-recall
  regression**, and real VRAM/mempool behavior, run when explicitly triggered
  (not every PR).

> A green CI badge means the **CPU-reference** suite passed — it does **not** mean
> GPU kernels or approximate recall were verified. Those are the on-demand Tier 2 run.

## Architecture

pg_cuvs uses a **tightly coupled sidecar model** (PG-Strom style), not an in-process CUDA context per backend:

```
┌─────────────────────────────────────────────┐
│  PostgreSQL                                  │
│  ┌──────────┐   Shared Memory IPC            │
│  │ Backend 1│ ──────────────────────────┐    │
│  │ Backend 2│ ──────────────────────────┤    │
│  │ Backend N│ ──────────────────────────┤    │
│  └──────────┘                           │    │
│                                         ▼    │
│  ┌──────────────────────────────────────┐    │
│  │  pg_cuvs_server (GPU Service Daemon) │    │
│  │  - Single CUDA context               │    │
│  │  - CAGRA/IVF index in VRAM           │    │
│  │  - Task queue + thread pool          │    │
│  └──────────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

Why not in-process? Each PostgreSQL backend is a separate process. Initializing a CUDA context per backend wastes hundreds of MB of VRAM per connection and takes hundreds of milliseconds. The sidecar model shares a single context across all sessions.

If the GPU service dies, PostgreSQL **gracefully degrades** to CPU-based pgvector (HNSW/IVFFlat) or SeqScan — no service interruption.

## Index Types

| Index | Storage | Use Case |
|-------|---------|----------|
| `USING cagra` | GPU VRAM | Low latency, hot data, fits in GPU VRAM |
| `USING pg_cuvs_hnsw` (built from heap, or reuses a `USING cagra` source) | GPU build / CPU serve | Fast GPU build (13x vs pgvector), then served by pgvector HNSW on CPU |

> **DiskANN/NVMe cold tier** (Phase 3B): spike completed, no-go for now — cuVS PQFlash
> API is unstable in cuVS 26.04 and DiskANN at 50M×384 timed out at 2 GB cache.
> Revisit when demand for billion-scale is confirmed. See `design/PHASE_3B_DECISION.md`.

Both use the same `vector <->` / `<=>` / `<#>` operator interface from pgvector. The planner selects the cheaper path based on a cost model that accounts for IPC overhead, GPU kernel cost, and data transfer:

```
Cost_total = Cost_IPC + Cost_GPU_kernel + Cost_CPU_recheck
```

Small tables route to CPU; large tables route to GPU automatically.

## When to Use

| Scenario | Recommended path | Notes |
|----------|-----------------|-------|
| N < 10K, any dim | pgvector HNSW | IPC + daemon round-trip overhead exceeds GPU search benefit |
| N 100K–10M, VRAM fits index | pg_cuvs CAGRA (GPU search) | Hot tier, low latency; crossover vs pgvector appears around N≈50K (synthetic clustered data; verify with your embedding distribution) |
| Fast build/rebuild needed, CPU serving preferred | CAGRA build + `USING pg_cuvs_hnsw` | 14x faster than pgvector native build at 1M×384; serves as standard pgvector HNSW afterward |
| On-prem RAG, embedding GPU pool already available | Reuse GPU for batch index build via 3I | GPU pool not idle; online search stays on pgvector HNSW CPU path |
| Larger-than-VRAM / billion-scale / NVMe cold tier | pgvectorscale DiskANN or VectorChord | pg_cuvs 3B DiskANN path is a no-go in cuVS 26.04; revisit when demand is confirmed |
| Multi-GPU horizontal scale | pg_cuvs CAGRA with `shard_count` | Recall improves with sharding; latency increases due to merge overhead |

## Feature Support

| Feature | Status | Verified on |
|---------|--------|-------------|
| CAGRA GPU search | Production-tested | A100 VM, MIG, multi-GPU |
| GPU Build Accelerator (`USING pg_cuvs_hnsw`) | Production-tested | VM E2E, 1M×384, recall=1.0 |
| Multi-GPU sharding (`shard_count`) | Production-tested | 2×A100, shard_count=1/2 |
| MIG (Multi-Instance GPU) | Verified | No code change; `CUDA_VISIBLE_DEVICES=MIG-uuid` |
| GCS snapshot restore | Production-tested | Phase 3G |
| CPU HNSW fallback (`cuvs.cpu_hnsw_fallback`) | Production-tested | Phase 3I-1 |
| DiskANN / NVMe cold tier | Deferred (no-go) | cuVS 26.04 PQFlash API unstable; see `design/PHASE_3B_DECISION.md` |

## Current Status

Implemented on GCP (NVIDIA A100-40GB × 2, PostgreSQL 16), VM E2E verified:

- [x] PostgreSQL C extension with C++ wrapper (resolves `float4` type collision between PG and CUDA headers)
- [x] CAGRA index build + GPU search via cuVS `cuvs::neighbors::cagra`
- [x] pgvector `vector` type as native input; operator classes for L2 (`<->`), Cosine (`<=>`), Inner Product (`<#>`)
- [x] Index Access Method handler (`cuvsamhandler`) registered with PostgreSQL planner
- [x] Cost model with `startup_cost=1000` (models CUDA context + transfer overhead)
- [x] `enable_cuvs`, `cuvs.cpu_hnsw_fallback` GUCs for runtime GPU toggle and CPU fallback
- [x] `pg_stat_gpu_search` view: per-index GPU stats (build time, search count, p50/p95 latency, recall)
- [x] `CREATE INDEX ... USING pg_cuvs_hnsw`: GPU CAGRA → pgvector HNSW DDL (Phase 3K). `source` optional — built from the heap (ephemeral CAGRA, auto-dropped) or reused from a `USING cagra` index; served by pgvector
- [x] Multi-GPU sharding (`shard_count`), GCS snapshot restore (Phase 3G)
- [x] MIG verified (no code changes needed)

Benchmark results (A100-40GB, 1M×384, VM E2E, **synthetic random data**):

| Method | Build time | p50 latency | Recall@10 |
|--------|-----------|-------------|-----------|
| pgvector HNSW (native) | 918s | — | baseline |
| CAGRA build + HNSW import | **66s** | 1.65ms | 1.0000* |
| pg_cuvs CAGRA search | 27s build | 1.65ms | 0.978** |

\* recall=1.0000 on synthetic random data (uniform distribution); expect lower on real embeddings.  
\*\* 0.978 on synthetic clustered data (20 clusters, sigma=0.05). Real embedding datasets pending.  
See `design/BENCHMARK_CROSSOVER.md` for full methodology.

## Roadmap

See [design/PLAN.md](design/PLAN.md) for the full product-roadmap plan. Some committed test-hardening work used its own "Phase 2/3/4/5" task labels; the table below refers only to the product roadmap.

| Phase | Goal | Status |
|-------|------|--------|
| 1 — Proof of Mechanism | PostgreSQL pipeline + sidecar CAGRA search | Done |
| 1.5 — Test & Ops Hardening | DDL durability, large-data benchmarks, GPU e2e, playbooks | Done |
| 2 — Production Ready | `pg_stat_gpu_search`, LIMIT-k/metric, write/staleness, large-build memory, tiered cache | Done |
| 3A~3G — Scale Out | pending-delta, snapshots, replicas, multi-GPU sharding, query optimization | Done (3G complete; 3B DiskANN → **no-go**, see PHASE_3B_DECISION.md) |
| 3I — GPU Build Accelerator | CAGRA build → pgvector HNSW export (13x faster build, recall=1.0 @ 1M×384) | Done (VM E2E verified, MIG tested) |
| 3K — HNSW DDL | `CREATE INDEX ... USING pg_cuvs_hnsw`: standard DDL for the build accelerator; `source` optional (ephemeral CAGRA from heap) + metric pre-check | Done (VM E2E, installcheck 8/8) |
| 3H — Ops Playbooks | sizing guide, when-to-use, runbooks | In progress |
| Release Hardening | compat matrix, known limitations, README, upgrade path | Planned |

## Compatibility

| Component | Tested version | Minimum |
|-----------|---------------|---------|
| PostgreSQL | 16.3 | 16 |
| pgvector | 0.8.x | 0.5.0 |
| NVIDIA CUDA | 12.4 | 12.0 |
| cuVS / RAPIDS | 26.04 | 24.12 |
| NVIDIA driver | 550.x | 525 |
| GCC | 11.4 | 11 |
| CMake | 3.26 | 3.26 |
| OS | Ubuntu 22.04 | Ubuntu 20.04 |

`pg_cuvs_hnsw` (via `src/hnsw_export.c`) is pinned to pgvector's HNSW page layout
(`HNSW_VERSION=1`, stable since pgvector 0.5.0 / Aug 2023) and mirrors pgvector 0.8.0's
hnsw opclass support functions. A pgvector major version bump that changes the on-disk
format or opclass procs would require a matching update to `src/hnsw_export.c` and the
`pg_cuvs_hnsw` operator classes.

## Known Limitations

| Limitation | Detail |
|------------|--------|
| `USING pg_cuvs_hnsw` build is offline | A `CREATE INDEX` build takes normal index-build locks; there is no `CONCURRENTLY` support for the GPU import path |
| pgvector layout dependency | `hnsw_export.c` hardcodes pgvector 0.5.0+ page format; pgvector major version upgrade requires validation |
| DiskANN / NVMe cold tier not supported | Phase 3B was spiked and abandoned; cuVS 26.04 PQFlash API is unstable. See `design/PHASE_3B_DECISION.md` |
| MIG requires VM reboot on GCP | `nvidia-smi -mig 1` only sets pending mode; reboot required to activate or deactivate |
| GCS snapshot restore requires bucket setup | Phase 3G restore path needs `cuvs.gcs_bucket` GUC set and credentials available |
| `parallel_fanout` at N > 5M unverified | Parallel dispatch may help at large scale; current measurement only covers N ≤ 100K |
| Crash during build rolls back | A crash during the `USING pg_cuvs_hnsw` build rolls back the `CREATE INDEX` (no partial index left behind); re-run required |
| No online index swap | No built-in equivalent of `CREATE INDEX CONCURRENTLY` for import; use table rename pattern for minimal downtime |

## Requirements

- PostgreSQL 16
- NVIDIA GPU with CUDA 12+ (A100, L4, H100, or similar; MIG supported)
- RAPIDS cuVS 26.04+ (`libcuvs`) — installed via Conda/Mamba
- pgvector 0.5.0+
- GCC 11.4+, CMake 3.26+

## Install

```bash
# 1. Install RAPIDS cuVS (if not already)
conda create -n cuvs_dev -c rapidsai -c nvidia -c conda-forge \
    cuvs=26.04 cuda-version=12.4 python=3.11
conda activate cuvs_dev

# 2. Build pg_cuvs
source ~/miniforge3/bin/activate cuvs_dev
make
sudo make install

# 3. Start the GPU daemon
sudo systemctl enable --now pg-cuvs-server

# 4. Load extensions in PostgreSQL
CREATE EXTENSION pgvector;
CREATE EXTENSION pg_cuvs;
```

### GPU Build Accelerator (Phase 3I / 3K) Quick Start

```sql
-- Simplest: one self-contained DDL. ambuild builds an ephemeral CAGRA on the
-- GPU, converts it into this index's own pages (pgvector-HNSW format, no CPU
-- build), then drops the temporary CAGRA. The result is a first-class catalog
-- index (pg_indexes / DROP INDEX / REINDEX / pg_dump all work), and REINDEX
-- rebuilds from the heap with no extra dependency.
CREATE INDEX my_hnsw ON items USING pg_cuvs_hnsw (embedding vector_l2_ops);

-- Serve queries via standard pgvector HNSW on CPU (GPU not required).
SELECT * FROM items ORDER BY embedding <-> $1 LIMIT 10;

-- To ALSO keep a CAGRA index for GPU search, build it first and reuse it as the
-- source — one GPU build then powers both indexes (no second build):
CREATE INDEX my_cagra  ON items USING cagra (embedding vector_l2_ops);
CREATE INDEX my_hnsw2  ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
    WITH (source = 'my_cagra', mode = 'nsw');
```

> The older `SELECT pg_cuvs_import_hnsw(cagra, hnsw)` two-step form (create an
> empty `USING hnsw` target, then import) is removed. `pg_cuvs_build_hnsw(cagra,
> mode)` still works but is deprecated in favor of the DDL above.

## Usage

```sql
-- CAGRA GPU search
CREATE TABLE items (id bigint, embedding vector(1536));
CREATE INDEX ON items USING cagra (embedding vector_l2_ops);
SELECT id FROM items ORDER BY embedding <-> '[...]'::vector LIMIT 10;

-- GPU brute-force exact search (Phase 3L)
SET cuvs.search_mode = 'brute_force';  -- GPU exact search, recall@10 = 1.0
SET cuvs.bf_precision = 'float16';     -- float16 mode: VRAM half, recall preserved
SELECT id FROM items ORDER BY embedding <-> '[...]'::vector LIMIT 10;

-- Batch search (Phase 3M) — Q queries in one GPU dispatch
SELECT query_idx, id, distance
FROM pg_cuvs_batch_search(
    'items'::regclass,
    ARRAY[q1, q2, q3]::vector[],  -- Q query vectors
    k := 10
) b
JOIN items t ON t.ctid = b.ctid
ORDER BY query_idx, distance;

-- Runtime toggles
SET enable_cuvs = off;              -- force CPU path
SET cuvs.cpu_hnsw_fallback = on;   -- serve via CPU HNSW sidecar if loaded
SET cuvs.k = 200;                  -- increase GPU candidate list for higher recall
SET cuvs.shard_count = 0;          -- auto shard across available GPUs

-- Monitor GPU search stats
SELECT index_name, search_count, avg_latency_us, p50_us, p95_us, search_mode
FROM pg_stat_gpu_search;
```

See `design/OPS_GPU_PLAYBOOK.md` for parameter tuning and MIG operations.

## License

PostgreSQL License. Copyright (c) 2026, JAESOL SHIN. See [LICENSE](LICENSE) for details.

## Related Work

- [pgvector](https://github.com/pgvector/pgvector) — PostgreSQL vector type and CPU indexes (HNSW, IVFFlat)
- [pgvectorscale](https://github.com/timescale/pgvectorscale) — DiskANN for PostgreSQL (the CPU/SSD reference we build on)
- [RAPIDS cuVS](https://github.com/rapidsai/cuvs) — GPU ANN library (CAGRA, IVF-Flat, IVF-PQ, brute force)
- [PG-Strom](https://github.com/heterodb/pg-strom) — GPU-accelerated SQL for PostgreSQL (architectural inspiration)
