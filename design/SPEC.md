# pg_cuvs — GEARS Specification

GEARS (Generalized EARS) 구문:
```
[Where <정적 전제조건>]
[While <상태 전제조건>]
[When <트리거>]
The <주체> shall <행동>
```

이 문서는 `/ouroboros:interview` 세션에서 확정된 설계 결정을 GEARS 형식으로 정교화한 것이다.
ADR은 `design/DECISIONS.md` 참조. 이 문서는 ADR을 검증 가능한 요구사항으로 변환한다.

Scope note:
- 이 파일은 Phase 2 전용 완료 기준이 아니라 pg_cuvs의 전체 제품/장기 요구사항이다.
- Phase별 완료 판정은 `design/PLAN.md`와 해당 phase audit 문서를 따른다.
- Product Phase 2 single-node core 완료 판정은 `docs/phase2-exit-criteria.md`가 canonical이다.
- 이 파일의 요구사항 중 일부는 Phase 3 또는 deferred enhancement로 남을 수 있으며,
  그런 경우 각 항목에 phase note를 둔다.

---

## 1. IPC 레이어 (ADR-002)

**IPC-01**
```
When a PostgreSQL backend issues a vector similarity query targeting a cagra index,
the pg_cuvs extension shall send a JSON command frame over a Unix Domain Socket
to pg_cuvs_server.
```

**IPC-02**
```
When a PostgreSQL backend sends a search command,
the pg_cuvs extension shall place the query vector in a pre-allocated
shm_open shared memory segment and reference it by handle in the UDS command,
such that no vector data crosses the socket boundary.
```

**IPC-03**
```
Where `cuvs.socket_path` GUC is not set,
the pg_cuvs_server shall bind to `/tmp/.s.pg_cuvs.<postmaster_pid>`.
```

**IPC-04**
```
While pg_cuvs_server is unreachable (connect() returns ECONNREFUSED or ETIMEDOUT),
the pg_cuvs extension shall fall back to pgvector HNSW and emit a WARNING to the
PostgreSQL log with the reason.
```

---

## 2. 인덱스 영속화 (ADR-002, Q3)

**PERSIST-01**
```
Where `cuvs.index_dir` GUC is not set,
the pg_cuvs_server shall use `$PGDATA/cuvs_indexes/` as the index storage path.
```

**PERSIST-02**
```
When `CREATE INDEX USING cagra` completes successfully,
the pg_cuvs_server shall serialize the built index to
`<cuvs.index_dir>/<database_oid>_<index_oid>.cagra` using the cuVS native
serialization format.
```

**PERSIST-02A**
```
When `CREATE INDEX USING cagra` completes successfully,
the pg_cuvs_server shall have also persisted the corresponding TID mapping to
`<cuvs.index_dir>/<database_oid>_<index_oid>.tids`.
```

**PERSIST-02B**
```
When persisting a cagra index or TID mapping,
the pg_cuvs_server shall write to a temporary file, fsync the file, atomically
rename it into place, and fsync the containing directory before reporting
success to the PostgreSQL backend.
```

**PERSIST-02C**
```
When VRAM build succeeds but either `.cagra` serialization or `.tids`
persistence fails,
the pg_cuvs_server shall report a build failure status to the PostgreSQL backend
and the pg_cuvs extension shall fail `CREATE INDEX` with ERROR so PostgreSQL
rolls back the catalog entry.
```

**PERSIST-02D**
```
When rebuilding an existing cagra index,
the pg_cuvs_server shall not replace the currently resident index until the new
VRAM index and both persistent artifacts have been created successfully.
```

**PERSIST-03**
```
When pg_cuvs_server receives SIGTERM,
the signal handler shall only request shutdown using signal-safe operations, and
the main server loop shall serialize all VRAM-resident indexes to
`cuvs.index_dir` before releasing CUDA resources and exiting.
```

**PERSIST-04**
```
When pg_cuvs_server starts,
the pg_cuvs_server shall scan `cuvs.index_dir` and load only indexes that have a
valid `.cagra` and `.tids` artifact pair and whose corresponding PostgreSQL
index OIDs still exist in `pg_catalog.pg_index`.
```

**PERSIST-05**
```
When `DROP INDEX` is executed on a cagra index,
the pg_cuvs extension shall notify pg_cuvs_server to unload the index from VRAM
and delete the corresponding file from `cuvs.index_dir`.
```

---

## 3. VRAM 관리 — 3계층 OOM 정책 (Q5)

**VRAM-01**
```
While loading a new index into VRAM,
the pg_cuvs_server shall check free VRAM against the index's estimated footprint
before invoking cuVS build or load APIs.
```

**VRAM-02**
```
When VRAM is insufficient to load a requested index and at least one other index
is resident in VRAM,
the pg_cuvs_server shall serialize the least-recently-used resident index to
`cuvs.index_dir`, free its VRAM allocation, and retry the load operation.
```

**VRAM-03**
```
When VRAM eviction of all resident indexes is insufficient to accommodate the
requested index,
the pg_cuvs_server shall refuse the load request and notify the pg_cuvs
extension to fall back to pgvector HNSW for that query.
```

**VRAM-04**
```
Where `cuvs.max_vram_mb` GUC is set,
the pg_cuvs_server shall treat that value as the upper bound on VRAM allocation
regardless of physical GPU VRAM availability.
```

---

## 4. CPU Fallback — 4가지 트리거 (ADR-003, Q6)

**FALLBACK-01 — 코스트 모델**
```
When the PostgreSQL planner estimates the cagra index scan cost to exceed the
sequential scan or HNSW scan cost (accounting for startup_cost=1000 IPC overhead
and per_tuple_cost=0.0001),
the planner shall select the cheaper CPU path without invoking pg_cuvs_server.
```

**FALLBACK-02 — GUC 수동 전환**
```
Where `enable_cuvs = off`,
the pg_cuvs extension shall not contact pg_cuvs_server and shall route all
queries to pgvector HNSW.
```

**FALLBACK-03 — 데몬 장애**
```
While pg_cuvs_server is unavailable,
the pg_cuvs extension shall fall back to pgvector HNSW for every query and
emit a WARNING once per backend session until the daemon recovers.
```

**FALLBACK-04 — Circuit Breaker**
```
While the number of consecutive GPU errors for a given index exceeds
`cuvs.circuit_breaker_threshold` (default: 3),
the pg_cuvs extension shall disable GPU routing for that index,
emit an ERROR-level log entry, and fall back to CPU until
`SELECT pg_cuvs_reset_circuit('<index_name>')` is called.
```

---

## 5. 쓰기 처리 — Stale Safety, Lazy Rebuild, Delta Correction (Q9)

**WRITE-01**
```
In Phase 2, when a row is inserted, updated, or deleted in a table that has a
cagra index, the pg_cuvs extension shall mark the cagra index stale without
modifying the VRAM-resident base index.
In Phase 3A, INSERT/UPDATE new versions append to a valid pending-delta
artifact and leave the base CAGRA index searchable. DELETE/VACUUM records
tombstones when possible; delta/tombstone write failure or cap exhaustion shall
mark the index stale and route to CPU.
```

**WRITE-02**
```
While a cagra index is stale and no valid pending-delta correction path exists,
the pg_cuvs extension shall not use the stale GPU CAGRA path for query results.
The default policy is CPU fallback selected before execution. Returning false
from `amgettuple` is not CPU fallback; it is an empty index scan and is
non-compliant with this requirement.
```

**WRITE-03**
```
When REINDEX rebuilds a stale cagra index from the current heap state,
the pg_cuvs_server shall replace the resident VRAM index atomically and clear
the stale marker only after durable persistence succeeds.
```

**WRITE-04**
```
In Phase 3, the pg_cuvs extension shall provide a pending-delta correction path:
INSERT/UPDATE new versions are searched with exact distance over a bounded
delta store, and base CAGRA candidates plus delta candidates are over-fetched,
merged, and re-ranked before PostgreSQL heap recheck. The CPU merge path is the
correctness fallback; the daemon-side resident GPU delta cache is the
performance path. DELETE/UPDATE-old versions are represented by a bounded
tombstone sidecar populated from `ambulkdelete`; the backend applies those
tombstones with snapshot visibility before returning candidates.
```

**WRITE-04A**
```
The pending-delta path shall not treat a base CAGRA index as complete unless a
valid delta artifact for the matching base generation is available. If a daemon
restart, missing delta artifact, corrupt delta artifact, generation mismatch, or
cleanup failure prevents safe delta correction, query execution shall fail
closed to CPU fallback rather than serving the incomplete base CAGRA graph as
fresh.
```

**WRITE-04B**
```
Tombstone correction shall be snapshot-aware. A global tombstone shall not
remove an UPDATE old tuple that remains visible to an older transaction
snapshot. Heap recheck is not sufficient to recover candidates removed before
the access method returns them.
```

**WRITE-04C**
```
The delta merge implementation shall define metric-compatible ordering for L2,
cosine, and inner product, including over-fetch or retry/fallback behavior when
heap recheck removes candidates and fewer than k visible rows remain.
```

**WRITE-05**
```
While the pending-delta or tombstone set for a cagra index exceeds
`cuvs.rebuild_threshold`, `cuvs.max_delta_rows`, or another configured resource
limit, the pg_cuvs extension shall stop using GPU+delta correction and fall back
to CPU until a successful REINDEX or explicit rebuild policy restores a compact
base index.
```

---

## 6. Phase 3B — DiskANN / Vamana Local NVMe (Q7)

Phase 2 completion does not require DiskANN. DiskANN/Vamana belongs to Product
Phase 3B, after Phase 3A pending-delta correctness.

**DISKANN-01**
```
When `CREATE INDEX USING diskann` is executed,
the pg_cuvs_server shall build a Vamana graph using cuVS GPU acceleration
(`cuvsVamanaBuild`) and serialize it to DiskANN binary format in `cuvs.index_dir`.
```

**DISKANN-02**
```
When a similarity query targets a diskann index,
the pg_cuvs extension shall perform CPU-side Vamana search
(`cuvsVamanaSearch`) on the NVMe-stored index without loading the full graph into VRAM.
```

**DISKANN-03**
```
Where a cagra index exists and `cuvs.export_hnsw = on`,
the pg_cuvs_server shall additionally export an HNSW-format copy via
`cuvs::neighbors::hnsw::from_cagra()` to support CPU fallback without re-reading
the heap.
```

**DISKANN-04 — Tiered Search**
```
While a cagra (VRAM-hot) index and a diskann (NVMe-cold) index both exist on the
same column,
the pg_cuvs extension shall query the cagra index first and supplement with
diskann results only when the cagra candidate set is below `k`.
```

---

## 7. 운영 가시성 — `pg_stat_gpu_search`

**STAT-01**
```
When GPU search, GPU fallback, index reload, or daemon-side search error occurs,
the pg_cuvs_server shall update per-index search statistics.
```

**STAT-02**
```
The pg_cuvs extension shall expose a SQL view named `pg_stat_gpu_search` that
retrieves daemon-maintained statistics through an IPC status or stats command.
```

**STAT-03**
```
For Product Phase 2 single-node core, the observability surface shall expose:
per-index database OID, index OID, index name, dimension, metric, vector count,
VRAM bytes, resident flag, search count, error count, average latency,
p50/p95/p99 wall-clock latency, last status, last error, last search timestamp,
requested k, returned k, stale flag, and stale timestamp via `pg_stat_gpu_search`;
and daemon-global cache hit/miss/eviction/reload/persist-failure/residency
counters via a separate cache stats view.
```

**STAT-03A — Target Observability**
```
For a later observability phase, the pg_cuvs extension should add detailed
fallback reason, rows-returned aggregates, GPU kernel time, IPC time,
CPU recheck time, and per-index cache/reload counters when those measurements
are available without distorting hot-path latency.
```

**STAT-04**
```
When a search falls back to CPU,
the pg_cuvs extension or pg_cuvs_server shall expose enough machine-readable
status for playbook diagnosis. Product Phase 2 exposes stale/error status and
global cache counters; detailed fallback reason is a deferred enhancement.
```

**STAT-05 — Multi-GPU Shard Observability**
```
When a logical cagra index is internally split across multiple GPU shards, the
pg_cuvs extension shall expose shard-level placement and health through a SQL
view such as `pg_stat_gpu_shards`. At minimum it shall report index identity,
shard id, GPU device id, vector count, resident state, VRAM bytes, search count,
error count, and last status. A sharded logical index shall not be represented
as if it lived on a single GPU in `pg_stat_gpu_search`.
```

---

## 8. Phase 3C — Object Storage Snapshot (Q8)

**OBJSTORE-01**
```
Where `cuvs.snapshot_uri` GUC is configured,
when an index build or rebuild completes,
the pg_cuvs_server shall upload the serialized index artifacts to object
storage using a provider-neutral manifest. The MVP provider is GCS, using paths
under `gs://<bucket>/pg_cuvs/<cluster_id>/<database_oid>/<index_oid>/`.
This snapshot shall contain derived pg_cuvs index artifacts only, not PostgreSQL
heap/table data.
```

**OBJSTORE-02**
```
Where `cuvs.snapshot_uri` is configured,
when pg_cuvs_server starts and a local index file is missing from `cuvs.index_dir`,
the pg_cuvs_server shall register the index as cold, open its query socket
without waiting for object-storage hydration, and download/load the artifact via
bounded background warmup workers. Queries that arrive before warmup completes
shall fall back to CPU or requeue warmup rather than observing a partially loaded
GPU index. Warmup shall only load artifacts whose manifest heap identity is
compatible with the local PostgreSQL heap.
```

**OBJSTORE-03**
```
The pg_cuvs index files shall be treated as derived data and shall not be
included in PostgreSQL physical base backup payloads (pg_basebackup) or WAL.
Heap/table distribution remains the responsibility of PostgreSQL backup and
replication mechanisms.
```

> NOTE (2026-06-05): "physical base backup payload" is the precise membership —
> `pg_basebackup` copies the entire `$PGDATA` tree plus tablespaces, and there is
> no native per-directory exclusion. Compute locality (which physical volume) and
> backup membership (inside the `$PGDATA` tree or not) are **orthogonal**: placing
> `index_dir` in a sibling directory on the SAME local NVMe but OUTSIDE the
> `$PGDATA` tree preserves locality while excluding the artifacts. The default
> `$PGDATA/cuvs_indexes` does NOT satisfy this requirement (see DECISIONS.md
> ADR-013 + OPS_GPU_PLAYBOOK §6).

**OBJSTORE-04**
```
Where multiple PostgreSQL instances share the same `cuvs.snapshot_uri` and
`cuvs.cluster_id`, read-only replicas shall load indexes from object storage
without rebuilding from heap only if they already have a heap-compatible
PostgreSQL replica or restore. A node without compatible heap data shall not load
the index artifact.
```

---

## 9. Phase 3F — True Multi-GPU CAGRA Sharding

**MGPU-01 — Logical Index Sharding**
```
The pg_cuvs daemon shall support a logical cagra index whose base CAGRA graph is
split into multiple independent shard artifacts, each resident on exactly one
GPU. The SQL surface remains a single PostgreSQL index; query execution fans out
inside the daemon and returns one global top-k result set to the backend.
```

**MGPU-02 — Artifact Contract**
```
A sharded cagra index shall preserve the global `.tids` sidecar as the
generation and heap-TID source of truth. A `.shards` manifest shall describe
each shard's id, global TID offset, vector count, metric, dimension, assigned
GPU, artifact path, checksum, and base `.tids` CRC. The manifest is the commit
marker; startup shall not load a sharded index when the manifest or any shard
artifact is missing, corrupt, or generation-incompatible.
```

**MGPU-03 — Query Fanout and Merge**
```
For a sharded cagra search, pg_cuvs_server shall execute CAGRA search on every
resident shard, request at least k candidates per shard, and perform a
metric-compatible daemon-side merge to produce the final top-k TIDs/distances.
The backend IPC reply format shall remain a single ordered result stream.
```

**MGPU-04 — Fail-Closed Shard Semantics**
```
If any shard of a logical cagra index is unavailable, fails to reload, has a
corrupt artifact, or cannot fit in its assigned GPU's VRAM, pg_cuvs shall not
serve partial ANN results by default. SELECT shall fall back to CPU; CREATE
INDEX / REINDEX shall ERROR unless all shards build and persist successfully.
```

**MGPU-05 — Delta/Tombstone Compatibility**
```
Pending delta and tombstone artifacts remain tied to the global `.tids` CRC.
The initial sharded implementation may keep backend CPU delta merge as the
correctness fallback, but tombstone filtering must remain snapshot-aware and
must apply to the merged shard result before returning heap TIDs to PostgreSQL.
```

---

## 10. Phase 3G — True Sharding Optimization / Productization

**MGPUOPT-01 — Parallel All-Shard Fanout**
```
The pg_cuvs daemon shall support parallel fanout across all shards of a logical
CAGRA index. Each shard search may run on its assigned GPU concurrently, and
the daemon shall merge the returned candidates into one metric-compatible global
top-k result before replying to PostgreSQL. The merge may remain daemon-side CPU
top-k selection unless profiling shows that GPU-side merge is necessary.
Parallel fanout must preserve the Phase 3F fail-closed semantics: a missing,
corrupt, or failed shard shall not produce a silent partial result by default.
```

**MGPUOPT-02 — Auto VRAM-Based Shard Count**
```
The pg_cuvs build path shall support an automatic shard-count policy based on
estimated index VRAM, available GPU set, and per-GPU VRAM budget. An explicit
`cuvs.shard_count=N` shall remain a deterministic override for tests,
benchmarks, and operator control. If the automatic policy cannot find a feasible
placement, CREATE INDEX / REINDEX shall fail rather than building an unsafe
partial index.
```

**MGPUOPT-03 — Shard-Aware Delta Cache**
```
The daemon-side GPU delta cache shall integrate with sharded base indexes.
Delta candidates may initially be stored as a global GPU brute-force cache, but
the search path shall be aware of sharded base results and shall merge base and
delta candidates consistently. Backend CPU delta merge remains the correctness
fallback when the daemon cannot use the GPU delta path.
```

**MGPUOPT-04 — Per-Shard Candidate Sizing**
```
The daemon shall support per-shard candidate sizing beyond the Phase 3F
minimum of k candidates per shard. Over-fetch (`k + slop`) and adaptive policies
shall be validated against CPU exact results or an explicit recall target before
becoming default behavior.
```

**MGPUOPT-05 — Snapshot/Warmup Integration**
```
Object-storage manifests and async warmup shall fully cover sharded artifacts:
the global `.tids`, `.shards` manifest, and every shard `.sNNN.cagra` file. A
replica shall load a sharded logical index only when the complete artifact set
is present, checksum-valid, and heap-compatible.
```

**MGPUOPT-06 — Degraded Recall Policy**
```
The default query path shall remain all-shard fanout with fail-closed behavior.
Any partial-recall mode shall require explicit opt-in and shall expose its
recall trade-off in observability and documentation.
```

**MGPUOPT-07 — Excluded: Vector-Clustering Shard Assignment**
```
Vector-clustering shard assignment is not part of Phase 3G or its immediate
follow-up scope. It requires an expensive out-of-core or distributed clustering
job over the same large corpus that already exceeds a single GPU, and it brings
little benefit while pg_cuvs uses all-shard fanout. The feature may be revisited
only as part of a future incremental segment-rebuild index architecture where
cluster assignment enables rebuilding changed segments instead of the full
CAGRA graph.
```

---

## 11. 코스트 모델 (ADR-003)

**COST-01**
```
The `cuvsamcostestimate` function shall set `startup_cost = 1000` to model
UDS IPC round-trip and CUDA context warm-up overhead.
```

**COST-02**
```
The `cuvsamcostestimate` function shall set `cpu_per_tuple = 0.0001` to reflect
GPU batch parallelism advantage over CPU sequential distance computation.
```

**COST-03**
```
When estimating cagra index scan cost,
the `cuvsamcostestimate` function shall scale `startup_cost` by
`(vector_dimensions / 1536.0)` to account for PCI-e transfer cost
proportional to vector size.
```

**COST-04**
```
When estimating cagra index scan cost,
the `cuvsamcostestimate` function shall not call CUDA runtime APIs directly.
```

**COST-05**
```
When a query has an explicit LIMIT that can be used as top-k,
the pg_cuvs extension shall pass that k value to pg_cuvs_server instead of using
a fixed constant.
```

**COST-06**
```
When determining the cuVS distance metric for a cagra search,
the pg_cuvs extension shall derive L2, cosine, or inner product from the
operator or operator class identity, not from a strategy-number heuristic.
```

---

## 12. 테스트 및 운영 플레이북

**TEST-01**
```
Before Phase 2 feature work begins,
the project shall provide unit tests for circuit breaker behavior, TID
encoding/decoding, persisted artifact discovery, atomic persistence helpers,
IPC status mapping, and failure-injection hooks.
```

**TEST-02**
```
Before Phase 2 feature work begins,
the project shall provide integration tests covering extension load, GUC
registration, access method registration, daemon-unavailable DDL failure,
daemon-unavailable SELECT fallback, and daemon status-code handling.
```

**TEST-03**
```
Before Phase 2 feature work begins,
the project shall provide GPU VM e2e tests covering successful cagra build,
valid `.cagra` and `.tids` persistence, daemon restart reload, expected nearest
neighbor search, SELECT fallback when daemon is down, persistence failure causing
CREATE INDEX failure, and VRAM budget exhaustion behavior.
```

**TEST-04**
```
Before Phase 2 feature work begins,
the project shall provide large-dataset GPU VM benchmarks covering at least
10K, 1M, and 10M vector tiers where feasible, dimensions 384, 768, and 1536,
and k values 10, 100, and 1000.
```

**TEST-05**
```
The large-dataset benchmark shall record CREATE INDEX build time, peak backend
RSS, daemon RSS, GPU VRAM usage, artifact sizes, daemon restart reload time,
cold and warm backend planning time, execution latency p50/p95/p99, fallback
counts, JIT section presence in EXPLAIN ANALYZE, and backend CUDA context
presence in nvidia-smi.
```

**TEST-06**
```
The project shall not set global JIT thresholds in automated post-install
scripts until large-dataset benchmarks or Phase 2 cost-model experiments show
that JIT is triggered for pg_cuvs vector-search queries and causes unacceptable
latency variance.
```

**TEST-07**
```
When JIT is triggered for pg_cuvs vector-search queries,
the project shall determine any recommended `jit_above_cost`,
`jit_inline_above_cost`, or `jit_optimize_above_cost` values through a threshold
sweep rather than a fixed guess.
```

**TEST-08**
```
Before Phase 2 feature work begins, the project shall provide baseline
operational playbooks for GPU VM build/test, daemon large-dataset benchmark,
JIT threshold sweep, restart recovery, CREATE INDEX failure diagnosis,
persistence corruption recovery, VRAM OOM fallback, and rollback/cleanup.
```

**TEST-09**
```
Final Phase 3 operational playbooks shall be written after Phase 3G true
sharding optimization/productization is complete. These runbooks shall cover
replica bootstrap, object-storage artifact recovery, heap-compatibility
mismatch, async warmup and cache hydration, multi-GPU placement/warmup,
per-GPU VRAM pressure, placement failure/degraded mode, true-sharding manifest
or shard-artifact failures, parallel fanout, shard count sizing, shard-aware
delta cache behavior, per-shard over-fetch tuning, and capacity planning.
Earlier Phase 3 documents shall capture contracts and acceptance criteria, not
detailed operational runbooks that would become stale before 3G.
```

---

## 13. 운영 인터페이스 (GUC 목록)

| GUC | 타입 | 기본값 | 설명 |
|-----|------|--------|------|
| `enable_cuvs` | bool | on | GPU 경로 전역 토글 |
| `cuvs.socket_path` | string | (auto) | UDS 소켓 경로 |
| `cuvs.index_dir` | string | `$PGDATA/cuvs_indexes/` | 인덱스 직렬화 경로 |
| `cuvs.max_vram_mb` | int | 0 (unlimited) | VRAM 사용 상한 |
| `cuvs.circuit_breaker_threshold` | int | 3 | 연속 실패 임계값 |
| `cuvs.rebuild_threshold` | float | 0.10 | delta 비율 임계값 (REINDEX/lazy rebuild 권고) |
| `cuvs.export_hnsw` | bool | off | CAGRA 빌드 시 HNSW 병행 export |
| `cuvs.snapshot_uri` | string | '' | Phase 3C object storage root, e.g. `gs://bucket/prefix` (비어있으면 비활성) |
| `cuvs.cluster_id` | string | '' | Phase 3 멀티노드 식별자 |
| `cuvs.warmup_threads` | int | 2 | Phase 3D background artifact download/load worker count |
| `cuvs.shard_count` | int | 0 (off) | Phase 3F/3G logical CAGRA internal shard count; explicit positive values force deterministic sharding, auto policy is Phase 3G |
| `cuvs.shard_overfetch` | int | 0 (auto) | Phase 3G additional candidates per shard for daemon-side global top-k merge |
