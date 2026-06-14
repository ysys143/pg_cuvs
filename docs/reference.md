# pg_cuvs Reference

> **Current-state reference (SSOT)** for the user-facing surface: index access methods, search
> modes, GUCs, reloptions, SQL functions, and observability views. How the pieces fit together is
> in [ARCHITECTURE.md](../ARCHITECTURE.md); the rationale behind each is in
> [design/DECISIONS.md](../design/DECISIONS.md). Verified against `src/pg_cuvs.c` and
> `sql/pg_cuvs--0.4.0.sql` (extension version 0.4.0).

---

## 1. Index access methods

pg_cuvs registers three index AMs. All reuse pgvector's `vector` type and operators
(`<->` L2, `<=>` cosine, `<#>` inner product) via opclasses `vector_l2_ops` (default),
`vector_cosine_ops`, `vector_ip_ops`.

| AM | Create | Tuning (reloptions) | Served by | Sidecar files |
|----|--------|---------------------|-----------|---------------|
| `cagra` | `CREATE INDEX i ON t USING cagra (col vector_l2_ops)` | `graph_degree`, `intermediate_graph_degree`, `build_algo`, `index_dir` | GPU daemon | `.cagra`, `.tids`, (`.vectors`, `.delta`, `.tombstone`, `.stale`, `.shards`, `.sNNN.cagra`, `.relfilenode`) |
| `flat` | `CREATE INDEX i ON t USING flat (col vector_l2_ops) WITH (precision='float16')` | `precision`, `index_dir` | GPU daemon | `.tids`, `.vectors` (no `.cagra` graph) |
| `ivfpq` | `CREATE INDEX i ON t USING ivfpq (col vector_l2_ops)` | `n_lists`, `pq_bits`, `pq_dim` | GPU daemon | `.ivfpq`, `.tids` |
| `pg_cuvs_hnsw` | `CREATE INDEX i ON t USING pg_cuvs_hnsw (col vector_l2_ops) WITH (source='my_cagra', mode='nsw')` | `source`, `mode`, `m`, `ef_construction` | **pgvector** (CPU) | `.hnsw` (pgvector page format) |

`flat` is the first-class exact GPU brute-force AM (ADR-073, extension v0.4.0). It builds only a
`.tids` + `.vectors` store — no CAGRA graph — so the build is O(N) copy rather than graph
construction. Search is always exact (recall=1.0) regardless of `cuvs.search_mode`; the AM
forces brute-force internally. Freshness uses the same delta/tombstone mechanism as `cagra`
(INSERTs append to `.delta`, merged at search time). The planner picks `flat` via its own cost
function (`CUVS_FLAT_STARTUP_COST=50`, vs `CUVS_STARTUP_COST=1000` for cagra), so small-N tables
route to `flat` correctly. DROP removes `.tids` + `.vectors` and releases VRAM. The index is
restart-durable: `.vectors` is reloaded on daemon restart. Use `flat` for read-heavy, stable
corpora where recall=1.0 is required; use `cagra`/`ivfpq` for approximate search at large N.

> **Deprecated:** `SET cuvs.search_mode = 'brute_force'` on a `cagra` index was the previous way
> to get GPU exact BF. It still works (ADR-039) but is deprecated. `USING flat` is the
> first-class replacement: independent AM, honest build semantics, dedicated cost routing. The GUC
> path couples an exact-BF workload to an unnecessary graph build and cannot be independently cost-
> calibrated by the planner.

### Transient no-index GPU brute-force (`cuvs.gpu_bruteforce`)

When `cuvs.gpu_bruteforce = on`, the planner routes a no-index single-table
`ORDER BY veccol <-> q LIMIT k` query to a transient GPU exact brute-force Custom Scan node named
`CuvsTransientBF` — no index required. This is the W1 counterpart to `USING flat` (W2 resident
index): use it when write velocity or always-fresh semantics make maintaining a flat index
impractical.

Key properties:
- **Exact** (recall@k = 1.0) and **always fresh** — scans the live heap under the query snapshot
  every call; no delta or tombstone logic needed.
- **Filter-first**: WHERE clauses are applied before the GPU handoff, not post-filtered.
- **Exec-time parameter binding**: `ORDER BY col <-> $1 LIMIT k` parametrized queries are NOT
  downgraded to approximate search.
- **Fires only for the bounded shape**: single base relation (no join), no OFFSET, no GROUP
  BY/DISTINCT/HAVING/window, a plan-time Const LIMIT in 1..100000, single distance ORDER BY key.
  Any other shape falls back to seqscan. The planner keeps `Limit → Sort → CustomScan` (no
  pathkeys claimed).
- **Host cap** (`cuvs.gpu_bruteforce_max_mb`, default 2048): if the per-query corpus materialized
  on the host exceeds this limit, the query fails closed with a clear ERROR rather than silently
  falling back.
- **Non-evicting VRAM admission**: the daemon never evicts a resident `flat` or `cagra` index to
  make room for a transient corpus. If the corpus exceeds the VRAM budget the daemon returns
  `OOM_FALLBACK` and the backend raises an ERROR; no crash.
- **Write cost zero** (no index to maintain). Read cost is paid per query: heap scan + detoast +
  H2D transfer every call. For read-heavy stable corpora, `USING flat` (W2) is more efficient.

Mental model: "flat index present → use it (A1/W2); no index + `gpu_bruteforce=on` → B (W1)."

Verified Tier-2 on A100/PG16 (2026-06-14): `make installcheck` 32/32 GREEN + isolation 3/3 GREEN.

```sql
SET cuvs.gpu_bruteforce = on;
-- No index needed; a plain ORDER BY query uses CuvsTransientBF:
SELECT ctid, embedding <-> '[1,2,3]' AS dist
FROM items
ORDER BY embedding <-> '[1,2,3]'
LIMIT 10;
```

`ivfpq` trades recall for 10–100× lower VRAM via product quantization. `pg_cuvs_hnsw` is the GPU
*build accelerator*: it builds a pgvector HNSW from a CAGRA graph without pgvector's CPU build,
then serves entirely through pgvector (its query path is pgvector's). See also
the deprecated function form `pg_cuvs_build_hnsw()` in §5.

> Cosine opclass note: for `pg_cuvs_hnsw` the cosine opclass mirrors pgvector exactly — proc 1 is
> the negative inner product and proc 2 is `vector_norm` (pgvector normalizes at build and ranks by
> inner product), not `cosine_distance`.

---

## 2. Search modes

`cagra` indexes choose an execution path at query time. Set the desired path with
`cuvs.search_mode` / related GUCs; the path actually taken is reported as the **string** in
`pg_stat_gpu_search.search_mode`. The integer below is the internal mode code — no SQL function
returns it, but it appears in daemon logs and test assertions, so this table doubles as the
decoder.

| `pg_stat_gpu_search.search_mode` | Code | Meaning |
|---|---|---|
| `gpu_cagra` | 0 | GPU CAGRA approximate NN (default) |
| `cpu_hnsw` | 1 | CPU HNSW fallback via `.hnsw` sidecar (`cuvs.cpu_hnsw_fallback`) |
| `cpu_fallback` | 2 | Generic CPU path (seqscan / pgvector) after a daemon-side gate |
| `brute_force` | 3 | GPU exact BF over the `.vectors` sidecar (`cuvs.search_mode='brute_force'` on a `cagra` index). **Deprecated** — use `USING flat` instead (ADR-073) |
| `cagra_prefilter` | 4 | CAGRA with GPU BITSET prefilter (3O filtered search) |
| `ivfpq` | 5 | GPU IVF-PQ (`ivfpq` AM) |
| `stream_bf` | 6 | Out-of-core filtered BF streamed from `.vectors` (ADR-064) |

> **Cancellation / timeouts:** an in-flight GPU search is interruptible — `statement_timeout` or a
> query cancel aborts the daemon round-trip within ~0.5s instead of blocking indefinitely (ADR-053,
> `recv_all_interruptible`). The daemon ignores SIGPIPE so it survives a client mid-reply disconnect.

---

## 3. GUC reference

Defaults and ranges are from source. "Set by" is the minimum role/scope: `USERSET` (any session),
`SUSET` (superuser), `SIGHUP` (config reload), `POSTMASTER` (server start).

### Core

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `enable_cuvs` | bool | `on` | — | USERSET | Master switch for the GPU path; off routes everything to CPU |
| `cuvs.debug` | bool | `off` | — | USERSET | Emit a per-search NOTICE with daemon latency + metric (for EXPLAIN VERBOSE) |
| `cuvs.socket_path` | string | `/tmp/.s.pg_cuvs` | — | SUSET | UDS path to the daemon |
| `cuvs.index_dir` | string | `$PGDATA/cuvs_indexes` | — | SUSET | Artifact directory (empty = resolved at runtime) |
| `cuvs.k` | int | `100` | 1–2000 | USERSET | GPU top-k candidates fetched per scan (cf. `hnsw.ef_search`) |
| `cuvs.circuit_breaker_threshold` | int | `3` | 1–100 | USERSET | Consecutive GPU errors before the (per-backend) breaker trips |

### Build

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.max_build_mem_mb` | int | `0` (auto) | 0–INT_MAX | USERSET | Backend memory cap for accumulating a build corpus; 0 = `MemAvailable × safety_ratio` |
| `cuvs.build_mem_safety_ratio` | real | `0.5` | 0.01–0.95 | USERSET | `MemAvailable` fraction usable when `max_build_mem_mb=0` |

### Search / recall / write-path routing

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.search_mode` | enum | `cagra` | `cagra`, `brute_force` | USERSET | CAGRA ANN vs GPU exact BF on a `cagra` index. `brute_force` value is **deprecated** — use `USING flat` AM instead. A `flat` index always forces exact BF regardless of this GUC |
| `cuvs.gpu_bruteforce` | enum | `off` | `off`, `auto`, `on` | USERSET | **EXPERIMENTAL, hardware-dependent (ADR-074).** Route a no-index single-table vector top-k query to the transient GPU BF CustomScan (`CuvsTransientBF`). On a **PCIe-attached GPU it does NOT beat CPU pgvector** (memory-bound + per-query H2D) — not recommended; use `USING flat` (read-heavy) or plain pgvector no-index (write-heavy). It becomes a first-class W1 path only on **unified-memory hardware** (Grace Hopper / MI300A) where the H2D penalty collapses. `auto` behaves as `off` (reserved until hardware-portable cost auto-enables it where it wins); `on` forces the path for qualifying single-table top-k shapes (ADR-073/ADR-074) |
| `cuvs.gpu_bruteforce_max_mb` | int | `2048` | 1–INT_MAX | USERSET | Host-side hard cap (MiB) on the per-query corpus materialized before the GPU handoff. Queries whose corpus exceeds this limit fail closed with an ERROR |
| `cuvs.bf_precision` | enum | `float32` | `float32`, `float16` | USERSET | Resident BF index precision; float16 halves VRAM |
| `cuvs.bf_batch_wait_us` | int | `0` (off) | 0–10000 | USERSET | Daemon BF micro-batch coalescing window (µs) |
| `cuvs.cpu_hnsw_fallback` | bool | `off` | — | USERSET | Serve from the `.hnsw` sidecar instead of GPU CAGRA |
| `cuvs.max_stale_fraction` | real | `0.10` | 0.0–1.0 | USERSET | Delete-drift fraction above which a CAGRA index reroutes to CPU; 1.0 disables |
| `cuvs.max_delta_rows` | int | `10000` | 0–INT_MAX | USERSET | Pending-insert rows merged before CPU reroute; 0 disables delta |
| `cuvs.delta_search` | enum | `auto` | `auto`, `cpu`, `gpu` | USERSET | Delta merge mode: GPU-with-CPU-fallback / always CPU / GPU-only |

### Filtered search (3O / D-wedge)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.filter_auto_threshold` | real | `0.05` | 0.0–1.0 | USERSET | Selectivity below which filtered BF uses the GPU BITSET prefilter instead of D-wedge post-filter |
| `cuvs.stream_bf_selectivity_threshold` | real | `0.0` (off) | 0.0–1.0 | USERSET | Selectivity below which filtered BF streams out-of-core from `.vectors` (ADR-064) |
| `cuvs.stream_bf_chunk_vectors` | int | `262144` | 1–INT_MAX | USERSET | Vectors per GPU chunk in streaming BF (footprint only; result is exact for any chunking) |
| `cuvs.filtered_knn_hook` | bool | `off` | — | USERSET | Enable the D-wedge Custom Scan hook (ADR-063 spike) |
| `cuvs.max_batch_queries` | int | `1024` | 1–4096 | USERSET | Max query vectors per `pg_cuvs_batch_search` call |

### Sharding (multi-GPU)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.shard_count` | int | `0` (auto) | 0–256 | USERSET | 0 = auto from VRAM budget, 1 = unsharded, ≥2 = force N shards (set at build) |
| `cuvs.shard_overfetch` | int | `0` | 0–4096 | USERSET | Extra candidates per shard before the global top-k merge |
| `cuvs.parallel_fanout` | bool | `on` | — | USERSET | Dispatch per-shard searches concurrently (off = sequential) |

### IVF-PQ / streaming updates

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.ivfpq_n_probes` | int | `64` | 1–4096 | USERSET | IVF clusters probed per query (≤ `n_lists`); higher = better recall |
| `cuvs.extend_chunk_size` | int | `0` (auto) | 0–65536 | USERSET | CAGRA `extend` max chunk size (Phase 3Q) |
| `cuvs.compact_delete_ratio` | real | `0.10` | 0.0–1.0 | USERSET | Deleted-vector fraction that triggers auto-compact after VACUUM |

### GCS snapshot / warmup (multi-node)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.snapshot_uri` | string | `` (off) | — | SUSET | GCS root URI for artifact snapshots, e.g. `gs://bucket/prefix` |
| `cuvs.cluster_id` | string | `` | — | SUSET | Cluster identifier in the GCS artifact path |
| `cuvs.gcs_key_file` | string | `` | — | SUSET | Service-account JSON path; empty = GCP instance metadata |
| `cuvs.warmup_threads` | int | `2` | 1–8 | SUSET | Background warmup (GCS download) thread pool size |

### Auto-compaction (Phase 4C bgworker)

| GUC | Type | Default | Range | Set by | Purpose |
|-----|------|---------|-------|--------|---------|
| `cuvs.auto_compact` | bool | `off` | — | SIGHUP | Auto-`REINDEX CONCURRENTLY` when delta growth crosses the threshold |
| `cuvs.auto_compact_check_interval` | int | `60` | 10–3600 | SIGHUP | Seconds between checks |
| `cuvs.auto_compact_threshold` | real | `0.10` | 0.01–1.0 | SIGHUP | Trigger when `extend_count / n_vecs` exceeds this |
| `cuvs.auto_compact_database` | string | `` | — | POSTMASTER | Database the bgworker monitors; empty = disabled |

### Daemon CLI flags (not GUCs)

`pg_cuvs_server` flags (set in the systemd unit `ExecStart`). Most mirror a `cuvs.*` GUC; the
daemon-only ones have no session equivalent.

| Flag | GUC equivalent | Default | Purpose |
|------|----------------|---------|---------|
| `--socket PATH` | `cuvs.socket_path` | `/tmp/.s.pg_cuvs` | UDS path |
| `--index-dir DIR` | `cuvs.index_dir` | — | Artifact dir the daemon serves from; must match the backend's `index_dir` or searches fall back to seqscan |
| `--max-vram-mb N` | — (daemon-only) | 90% of total VRAM | VRAM budget, enforced by self-accounting (ADR-065); the supported way to cap VRAM |
| `--max-indexes N` | — (daemon-only) | `1024` | **Soft** LRU working-set cap (ADR-068), not a hard wall (was 64). At the cap, `load_index` evicts the LRU index to free a slot and auto-reloads on next use; a build over the cap defers gracefully |
| `--gpu-devices LIST` | — (daemon-only) | all visible | CUDA devices the daemon uses (else honors `CUDA_VISIBLE_DEVICES`) |
| `--snapshot-uri URI` | `cuvs.snapshot_uri` | `` | GCS snapshot root |
| `--cluster-id ID` | `cuvs.cluster_id` | `` | Cluster id in the GCS artifact path |
| `--gcs-key-file PATH` | `cuvs.gcs_key_file` | `` | Service-account JSON path |
| `--warmup-threads N` | `cuvs.warmup_threads` | `2` | Background GCS warmup pool size |

---

## 4. Reloptions

### `flat`

| Reloption | Type | Default | Range / values | Notes |
|-----------|------|---------|----------------|-------|
| `precision` | enum | `float32` | `float32`, `float16` | Resident BF index precision. `float16` halves VRAM (N·dim·2 B vs ·4 B); falls back to `cuvs.bf_precision` GUC if not set |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory; same semantics as `cagra` (ADR-045) |

VRAM sizing: `float32` requires N·dim·4 bytes resident (e.g. 1M×1024 ≈ 4 GB); `float16` halves
it. If the corpus does not fit in free VRAM at build time the daemon returns a dedicated
no-fit error ("corpus N·dim·4B exceeds free VRAM; try precision=float16, shard, or use
cagra/ivfpq") rather than silently failing.

### `cagra`

| Reloption | Type | Default | Range / values | Notes |
|-----------|------|---------|----------------|-------|
| `graph_degree` | int | `64` | 8–512 | CAGRA output graph degree |
| `intermediate_graph_degree` | int | `128` | 8–1024 | Must be ≥ `graph_degree` (fail-closed) |
| `build_algo` | enum | `auto` | `auto`, `ivf_pq`, `nn_descent` | CAGRA build algorithm |
| `index_dir` | string | (uses `cuvs.index_dir`) | path | Per-index artifact directory (ADR-045); self-describes in `reloptions` so no-GUC sessions still find it |

### `ivfpq`

| Reloption | Type | Default | Range | Notes |
|-----------|------|---------|-------|-------|
| `n_lists` | int | `1024` | 1–65536 | IVF cluster count |
| `pq_bits` | int | `8` | 4–8 | Bits per PQ code |
| `pq_dim` | int | `0` (auto → ~dim/2) | 0–65536 | PQ subspace count |

### `pg_cuvs_hnsw`

| Reloption | Type | Default | Values | Notes |
|-----------|------|---------|--------|-------|
| `source` | string | `` | a `cagra` index name on the same table | empty = ephemeral CAGRA built from the heap |
| `mode` | string | `nsw` | `nsw`, `hnswlib`, `hnsw`, `hnswlib_file` | `nsw` and `hnswlib` are recommended |
| `m` | int | `16` | 2–100 | **informational** — the CAGRA graph degree drives the HNSW layout, not `m` |
| `ef_construction` | int | `64` | 4–1000 | **informational** |

> `m` / `ef_construction` are *accepted without error* and recorded in the index's reloptions for
> pgvector compatibility, but they do **not** affect the GPU-built graph. Control graph quality via
> the source `cagra` index's `graph_degree` / `intermediate_graph_degree`.

---

## 5. SQL functions

| Function | Returns | Purpose |
|----------|---------|---------|
| `pg_cuvs_reset_circuit(index regclass)` | void | Re-enable GPU routing after the (per-session) circuit breaker tripped |
| `pg_cuvs_build_hnsw(cagra regclass, mode text DEFAULT 'nsw')` | regclass | Build a pgvector HNSW from a CAGRA index without pgvector's CPU build. **Deprecated** in favor of `CREATE INDEX ... USING pg_cuvs_hnsw`; the older two-step `pg_cuvs_import_hnsw` form (empty `USING hnsw` target + import) is removed |
| `pg_cuvs_compact(index regclass)` | void | Remove tombstoned vectors via `cuvsCagraMerge`, rebuild `.cagra`/`.tids`, drop `.tombstone` |
| `pg_cuvs_batch_search(rel regclass, queries vector[], k int)` | SETOF (query_idx int, ctid tid, distance real) | Q queries in one IPC/GPU dispatch; JOIN on `ctid` for visible rows; honors `search_mode`/`bf_precision` |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids bigint[], k int)` | TABLE (ctid tid, distance float4) | Exact GPU BF restricted to a sorted TID set (`block<<16\|off`); NULL = unfiltered |
| `cuvs_filtered_knn(index regclass, query vector, filter_tids tid[], k int)` | TABLE (ctid tid, distance float4) | Type-safe `tid[]` overload (accepts `ctid` directly) |
| `pg_cuvs_gc_orphans(do_delete bool DEFAULT false)` | SETOF (db_oid oid, index_oid oid, reason text, action text) | Reconcile `index_dir` vs catalog; dry-run by default (ADR-046) |
| `pg_cuvs_last_search_latency_us()` / `_n_results()` / `_k()` / `_index()` / `_metric()` | int / int / int / oid / text | Process-local stats for the most recent scan in this backend; NULL if none |

### Internal / unsupported

The extension also defines fault-injection and test-harness functions (GPU VRAM pre-allocation, OOM
injection, and a non-persistent daemon budget override). They exist for the test suite only, are
**not a supported API**, and can OOM or crash the daemon — do not call them in production. The
supported way to cap VRAM is the daemon's `--max-vram-mb` flag.

---

## 6. Observability views

Four views back onto SRFs. All are **empty when the daemon is down** (except `pg_stat_gpu_fallback`,
which is backend-shmem sourced), so monitoring stays queryable. Counters reset on index
rebuild/reload or daemon restart.

### `pg_stat_gpu_search` — per-index search stats (daemon-sourced)

`database_oid, index_oid, index_name, dim, metric, n_vecs, vram_bytes, resident, search_count,
error_count, avg_latency_us, p50_latency_us, p95_latency_us, p99_latency_us, last_status, last_error,
last_search_at, requested_k, returned_k, stale, stale_since, delta_rows, delta_generation,
delta_vram_bytes, delta_merged_count, delta_search_mode, warmup_state, last_warmup_at,
warmup_duration_ms, download_count, cache_miss_count, gpu_device_id, shard_count, search_mode,
bf_batch_count, extend_count, compact_count, last_compact_at`

The single most useful row for "is this index healthy": `resident`, `search_mode` (did it stay on
GPU?), `error_count`, p50/p95/p99 latency, `stale`, `delta_rows`.

### `pg_stat_gpu_cache` — per-GPU VRAM cache counters (daemon-sourced)

`gpu_device_id, hits, misses, evictions, reloads, persist_failures, resident_count, vram_used_mb,
vram_budget_mb, bf_vram_mb, bf_precision`

Watch `vram_used_mb` vs `vram_budget_mb` for headroom, and `evictions`/`reloads` for thrashing.
**`persist_failures > 0` is serious** — eviction could not serialize an index to disk.

### `pg_stat_gpu_fallback` — per-index CPU fallback (backend-shmem sourced)

`index_oid, fallback_count, last_reason, last_fallback_at`

The only place plan-time CPU routing is visible. `last_reason` ∈ `disabled / circuit_breaker /
stale / delete_drift / daemon_down / no_artifact / delta_unusable / tombstone_unusable`. Watch the
trend against `pg_stat_gpu_search.search_count` to catch queries silently dropping to CPU. Counts
are a relative pressure signal (the cost hook can run more than once per query), not exact query
counts.

### `pg_stat_gpu_shards` — per-shard placement (daemon-sourced, sharded indexes only)

`database_oid, index_oid, index_name, shard_id, gpu_device_id, n_vecs, tid_offset, vram_used_mb,
search_count, error_count, resident, last_status`

One row per shard; empty for unsharded indexes.

---

## 7. On-disk artifacts

The sidecar file suffixes (`.cagra`, `.tids`, `.vectors`, `.delta`, `.tombstone`, `.stale`,
`.shards`, `.sNNN.cagra`, `.relfilenode`, `.hnsw`) and their formats/lifecycle are documented in
[ARCHITECTURE.md §5](../ARCHITECTURE.md#5-index-lifecycle-and-on-disk-artifacts).
