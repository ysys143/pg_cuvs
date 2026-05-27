# pg_cuvs

GPU-accelerated vector search for PostgreSQL via NVIDIA cuVS — a heterogeneous acceleration path that keeps Postgres as the control plane.

## What it is

pg_cuvs is **not** a replacement for pgvector. It is a GPU offloading layer that sits on top of pgvector's interface. SQL syntax, transaction semantics, MVCC, and planner decisions remain entirely within PostgreSQL. The only thing offloaded to the GPU is the vector similarity computation itself.

```sql
-- No query changes required. pg_cuvs accelerates this transparently.
SELECT * FROM items ORDER BY embedding <=> $1 LIMIT 10;
```

The GPU acts as a **candidate generator** — returning the top-K TID candidates and distances — and PostgreSQL handles heap access, visibility checks, joins, and filters as usual.

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
| `USING cagra` | GPU VRAM | Low latency, hot data, < ~35M vectors @ fp16/1024d |
| `USING diskann` | NVMe/SSD | Large datasets, cold data, billion-scale |

Both use the same `vector <->` / `<=>` / `<#>` operator interface from pgvector. The planner selects the cheaper path based on a cost model that accounts for IPC overhead, GPU kernel cost, and data transfer:

```
Cost_total = Cost_IPC + Cost_GPU_kernel + Cost_CPU_recheck
```

Small tables route to CPU; large tables route to GPU automatically.

## Current Status

Proof-of-concept implemented on GCP (NVIDIA L4 GPU, PostgreSQL 16):

- [x] PostgreSQL C extension with C++ wrapper (resolves `float4` type collision between PG and CUDA headers)
- [x] `cuvsBruteForceSearch` C API integration with DLPack tensor interface
- [x] pgvector `vector` type as native input type
- [x] Index Access Method handler (`cuvsamhandler`) registered with PostgreSQL planner
- [x] Cost model with `startup_cost=1000` (models CUDA context + transfer overhead)
- [x] `enable_cuvs` GUC for runtime GPU toggle
- [x] Operator classes for L2 (`<->`), Cosine (`<=>`), Inner Product (`<#>`)

Benchmark results (DBpedia 1M, 1536d, L4 GPU):

| Method | QPS | Latency | Recall@100 |
|--------|-----|---------|------------|
| CAGRA | 415 | 2.41ms | 0.9625 |
| Brute Force (cuVS API) | 39.8 | 25ms | 0.9999 |

## Roadmap

See [design/PLAN.md](design/PLAN.md) for the full product-roadmap plan. Some committed test-hardening work used its own "Phase 2/3/4/5" task labels; the table below refers only to the product roadmap.

| Phase | Goal | Status |
|-------|------|--------|
| 1 — Proof of Mechanism | PostgreSQL pipeline + sidecar CAGRA search | Done |
| 1.5 — Test & Ops Hardening | DDL durability, large-data benchmarks, GPU e2e, playbooks | Done (GPU VM verified) |
| 2 — Production Ready | `pg_stat_gpu_search`, LIMIT-k/metric correctness, write/staleness, large-build memory, tiered cache | Done (single-node core; streaming deferred) |
| 3 — Scale Out | pending-delta correction, DiskANN/Vamana, object-storage snapshots, replicas, multi-GPU | In progress |

## Requirements

- PostgreSQL 16+
- NVIDIA GPU (CUDA 12+)
- RAPIDS cuVS (`libcuvs`) — installed via Conda/Mamba
- pgvector (pg_cuvs depends on the `vector` type)
- GCC 11.4+, CMake 3.26+

## Build

```bash
# Activate cuVS environment
source ~/miniforge3/bin/activate cuvs_dev

# Build and install
make
sudo make install

# Load in PostgreSQL
CREATE EXTENSION pgvector;
CREATE EXTENSION pg_cuvs;
```

## Usage

```sql
-- Create table with pgvector type
CREATE TABLE items (id bigint, embedding vector(1536));

-- Create GPU-accelerated index
CREATE INDEX ON items USING cagra (embedding vector_l2_ops);

-- Search (same syntax as pgvector)
SELECT id FROM items ORDER BY embedding <-> '[...]'::vector LIMIT 10;

-- Toggle GPU acceleration
SET enable_cuvs = off;  -- fall back to CPU path
SET enable_cuvs = on;   -- use GPU path (default)
```

## Related Work

- [pgvector](https://github.com/pgvector/pgvector) — PostgreSQL vector type and CPU indexes (HNSW, IVFFlat)
- [pgvectorscale](https://github.com/timescale/pgvectorscale) — DiskANN for PostgreSQL (the CPU/SSD reference we build on)
- [RAPIDS cuVS](https://github.com/rapidsai/cuvs) — GPU ANN library (CAGRA, IVF-Flat, IVF-PQ, brute force)
- [PG-Strom](https://github.com/heterodb/pg-strom) — GPU-accelerated SQL for PostgreSQL (architectural inspiration)
