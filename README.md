# pg_cuvs

GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS — a heterogeneous acceleration path that keeps Postgres as the control plane.

## What it is

pg_cuvs is **not** a replacement for pgvector. It is a GPU acceleration layer that sits on top of pgvector's interface. SQL syntax, transaction semantics, MVCC, and planner decisions remain entirely within PostgreSQL.

It supports two complementary modes:

- **GPU search tier**: keep CAGRA indexes resident in GPU VRAM and use the GPU as a candidate generator.
- **GPU build accelerator tier**: use nearby GPUs to build indexes quickly, then import the result into a standard pgvector HNSW index and serve queries on CPU.

```sql
-- No query changes required. pg_cuvs accelerates this transparently.
SELECT * FROM items ORDER BY embedding <=> $1 LIMIT 10;
```

The GPU acts as a **candidate generator** — returning the top-K TID candidates and distances — and PostgreSQL handles heap access, visibility checks, joins, and filters as usual.

For on-prem or private RAG systems, this is useful even when search serving stays on CPU. These deployments often already run GPU servers for embedding models, rerankers, or batch embedding jobs. pg_cuvs can reuse that nearby GPU pool for slow index build/rebuild windows, while keeping the online serving path as ordinary PostgreSQL + pgvector HNSW.

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
| `USING cagra` + `pg_cuvs_import_hnsw` → `USING hnsw` | GPU build / CPU serve | Fast build (13x vs pgvector), then pgvector HNSW CPU search |

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
| Fast build/rebuild needed, CPU serving preferred | CAGRA build + `pg_cuvs_import_hnsw` | 14x faster than pgvector native build at 1M×384; serves as standard pgvector HNSW afterward |
| On-prem RAG, embedding GPU pool already available | Reuse GPU for batch index build via 3I | GPU pool not idle; online search stays on pgvector HNSW CPU path |
| Larger-than-VRAM / billion-scale / NVMe cold tier | pgvectorscale DiskANN or VectorChord | pg_cuvs 3B DiskANN path is a no-go in cuVS 26.04; revisit when demand is confirmed |
| Multi-GPU horizontal scale | pg_cuvs CAGRA with `shard_count` | Recall improves with sharding; latency increases due to merge overhead |

## Feature Support

| Feature | Status | Verified on |
|---------|--------|-------------|
| CAGRA GPU search | Production-tested | A100 VM, MIG, multi-GPU |
| GPU Build Accelerator (`pg_cuvs_import_hnsw`) | Production-tested | VM E2E, 1M×384, recall=1.0 |
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
- [x] `pg_cuvs_import_hnsw(cagra_oid, hnsw_oid)`: CAGRA-to-pgvector HNSW page-level import
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
| 3I — GPU Build Accelerator | `pg_cuvs_import_hnsw`: CAGRA build → pgvector HNSW export (13x faster build, recall=1.0 @ 1M×384) | Done (VM E2E verified, MIG tested) |
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

`pg_cuvs_import_hnsw` is pinned to pgvector's HNSW page layout (`HNSW_VERSION=1`, stable
since pgvector 0.5.0 / Aug 2023). A pgvector major version bump that changes the on-disk
format would require a matching update to `src/hnsw_export.c`.

## Known Limitations

| Limitation | Detail |
|------------|--------|
| `pg_cuvs_import_hnsw` is offline-only | Acquires `AccessExclusiveLock`; blocks all concurrent queries on the target HNSW index until commit |
| pgvector layout dependency | `hnsw_export.c` hardcodes pgvector 0.5.0+ page format; pgvector major version upgrade requires validation |
| DiskANN / NVMe cold tier not supported | Phase 3B was spiked and abandoned; cuVS 26.04 PQFlash API is unstable. See `design/PHASE_3B_DECISION.md` |
| MIG requires VM reboot on GCP | `nvidia-smi -mig 1` only sets pending mode; reboot required to activate or deactivate |
| GCS snapshot restore requires bucket setup | Phase 3G restore path needs `cuvs.gcs_bucket` GUC set and credentials available |
| `parallel_fanout` at N > 5M unverified | Parallel dispatch may help at large scale; current measurement only covers N ≤ 100K |
| Import crash leaves index empty | Crash during `pg_cuvs_import_hnsw` recovers to pre-truncation or empty state; re-import required |
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

### GPU Build Accelerator (Phase 3I) Quick Start

```sql
-- Build CAGRA index (GPU; sidecar .hnsw written automatically)
CREATE INDEX my_cagra ON items USING cagra (embedding vector_l2_ops);

-- Create empty pgvector HNSW target
CREATE INDEX my_hnsw ON items USING hnsw (embedding vector_l2_ops);

-- Import: GPU-built index → pgvector pages (offline, ~66s for 1M×384)
SELECT pg_cuvs_import_hnsw('my_cagra'::regclass, 'my_hnsw'::regclass);

-- Serve queries via standard pgvector (GPU not required)
SELECT * FROM items ORDER BY embedding <-> $1 LIMIT 10;
```

## Usage

```sql
-- CAGRA GPU search
CREATE TABLE items (id bigint, embedding vector(1536));
CREATE INDEX ON items USING cagra (embedding vector_l2_ops);
SELECT id FROM items ORDER BY embedding <-> '[...]'::vector LIMIT 10;

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

## Related Work

- [pgvector](https://github.com/pgvector/pgvector) — PostgreSQL vector type and CPU indexes (HNSW, IVFFlat)
- [pgvectorscale](https://github.com/timescale/pgvectorscale) — DiskANN for PostgreSQL (the CPU/SSD reference we build on)
- [RAPIDS cuVS](https://github.com/rapidsai/cuvs) — GPU ANN library (CAGRA, IVF-Flat, IVF-PQ, brute force)
- [PG-Strom](https://github.com/heterodb/pg-strom) — GPU-accelerated SQL for PostgreSQL (architectural inspiration)
