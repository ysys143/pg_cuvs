# pg_cuvs Positioning, Differentiation, and Limits

## One-line Positioning

pg_cuvs is not a thin cuVS wrapper. It is a PostgreSQL semantic adapter and
runtime that uses NVIDIA cuVS as a derived GPU index accelerator while keeping
PostgreSQL as the source of truth.

In shorter terms:

```text
PostgreSQL owns data and transactional semantics.
cuVS generates fast vector candidates.
pg_cuvs makes that boundary safe enough to behave like a PostgreSQL index path.
```

## Why This Is More Than an Interface Adapter

Calling cuVS from PostgreSQL is the easy part. The hard part is making a
non-database GPU ANN library fit inside PostgreSQL's expectations for catalog
state, MVCC visibility, DDL failure behavior, crash recovery, and operational
fallbacks.

A thin adapter would do roughly this:

```text
heap vectors -> cuVS build/search -> TID candidates
```

pg_cuvs adds the database-facing correction layer around that primitive:

- durable `CREATE INDEX` semantics: GPU build and disk artifact persistence must
  both succeed before PostgreSQL catalog state commits successfully.
- generation-checked artifacts: `.tids`, `.shards`, shard `.sNNN.cagra` files,
  CRCs, manifests, and startup pair validation prevent stale/corrupt artifacts
  from being trusted.
- PostgreSQL MVCC boundary: cuVS returns TID candidates, then PostgreSQL heap
  access and visibility checks decide whether rows are visible to the current
  query snapshot.
- pending write correction: INSERT/UPDATE new versions are recorded in `.delta`
  and merged with base CAGRA candidates.
- delete correction: VACUUM `ambulkdelete` records `.tombstone` entries, with
  stale/CPU fallback when tombstone correction is unavailable.
- fail-closed behavior: corrupt, missing, incompatible, or partially loaded
  artifacts do not produce partial GPU results by default.
- daemon lifecycle and fallback: sidecar failure, crash, clean shutdown, runtime
  errors, and circuit breaker states are converted into CPU fallback or explicit
  PostgreSQL errors depending on DDL vs SELECT context.
- single logical multi-GPU index: one `CREATE INDEX ... USING cagra` can become
  multiple GPU-resident shards with daemon fanout and global top-k merge while
  keeping the SQL surface unchanged.
- object snapshot and warmup: derived GPU artifacts can be cached in GCS and
  rehydrated only when heap compatibility checks pass.

The engineering value is therefore not "PostgreSQL can call cuVS." The value is
"PostgreSQL can use cuVS as a derived accelerator without pretending cuVS is the
database."

## Source-of-Truth Model

pg_cuvs deliberately treats cuVS artifacts as derived data.

```text
PostgreSQL heap/catalog/WAL = source of truth
pg_cuvs artifacts           = derived accelerator cache
cuVS daemon                 = candidate generator runtime
```

This means:

- PostgreSQL heap tuples remain authoritative.
- PostgreSQL snapshots and MVCC decide final row visibility.
- cuVS artifacts can be rebuilt, discarded, reloaded, or downloaded from object
  storage without changing the logical table contents.
- missing or corrupt GPU artifacts must not corrupt PostgreSQL state.

This also means pg_cuvs does not try to give cuVS WAL-grade transactional
semantics. It builds a safety boundary around cuVS instead.

## What the Current Implementation Strongly Defends

### MVCC-visible wrong rows

GPU search returns heap TID candidates, not rows. The backend sets
`xs_heaptid` and uses PostgreSQL heap recheck/visibility logic. Aborted or
snapshot-invisible rows may exist in derived artifacts, but they should be
filtered before becoming SQL-visible results.

### Durable DDL boundary

`CREATE INDEX USING cagra` is not considered successful merely because cuVS
built an in-memory graph. The persistent artifact set must also be written,
fsynced, renamed, and validated. Build or persistence failure causes PostgreSQL
DDL failure rather than a catalog entry pointing at missing GPU artifacts.

### INSERT/UPDATE freshness in normal operation

New tuple versions are appended to a generation-bound `.delta` sidecar and
merged with base CAGRA candidates. The sharded path can use daemon-side GPU delta
merge, with backend CPU merge as the correctness fallback.

### DELETE safety

Deleted base TIDs are handled through tombstones when VACUUM calls
`ambulkdelete`. Tombstone failure, corruption, or cap overflow falls back to
stale/CPU routing.

### Sharded fail-closed behavior

A sharded logical index is only registered when the full shard set is valid.
Missing or corrupt shard artifacts do not silently produce partial ANN results.

### Runtime lifecycle

The daemon owns CUDA state. PostgreSQL backends do not create CUDA contexts.
Daemon failure, clean shutdown, crash, unavailable sockets, and DROP INDEX
cleanup are handled outside PostgreSQL's heap/catalog source of truth.

## What the Current Implementation Does Not Claim

### It is not a WAL-logged mutable PostgreSQL index

`.cagra`, `.tids`, `.shards`, `.delta`, and `.tombstone` are derived artifacts,
not PostgreSQL WAL records. They are fsynced and validated, but they are not
atomically logged with heap changes in the same way as native PostgreSQL index
pages.

### It does not claim the default CAGRA path is exact

The default `search_mode='cagra'` path is an approximate nearest-neighbor
structure; its recall must be measured and tuned. pg_cuvs does **not** turn that
approximate CAGRA path into exact search.

It does, however, ship an **exact** GPU search path: `search_mode='brute_force'`
(3L/ADR-039) returns recall=1.0, exact even at `bf_precision='float16'`. Exactness
is therefore a real, shippable property of the brute-force path — just not of
CAGRA. At small N the planner can auto-select brute-force because it is both
exact and cheaper than CAGRA ANN; for filtered/selective queries, brute-force
over the filtered subset is exact and cost-effective (no graph build; work
bounded by the filter), which is the basis of the ADR-061 exact-filtered wedge.
CAGRA remains the choice for large unfiltered corpora where O(N) brute-force
search is too costly.

### It does not make cuVS transactional

cuVS does not know PostgreSQL transactions, MVCC snapshots, rollback, catalog
state, or WAL. pg_cuvs compensates around cuVS; it does not change cuVS into a
database engine.

### It does not fully eliminate DELETE recall erosion

Dead TIDs can occupy candidate slots until tombstone correction, over-fetch,
drift gates, or CPU fallback compensate. Heap recheck prevents deleted rows from
being returned, but recall can still be affected in edge cases.

### It does not make object storage a heap replication mechanism

GCS snapshots store derived GPU artifacts only. A node must already have a
heap-compatible PostgreSQL table, via replication, restore, or another
PostgreSQL mechanism. pg_cuvs snapshots are warmup caches, not database backup.

## Current Assurance Level

| Area | Current level | Notes |
|---|---|---|
| PostgreSQL source-of-truth preservation | Strong | GPU never owns heap data |
| MVCC-visible wrong-row prevention | Strong | Final visibility is PostgreSQL heap recheck |
| DDL/artifact fail-closed behavior | Strong | Build + persistence + validation required |
| INSERT/UPDATE freshness | Strong in normal path | `.delta` + GPU/CPU merge; not WAL-grade |
| DELETE correctness | Mixed | Wrong-row prevention strong; recall remains conditional |
| Multi-GPU logical index behavior | Strong | 2x A100 hardware acceptance passed |
| GCS snapshot/warmup | Strong after real bucket verification | Real GCS round-trip exposed and fixed 3C bugs |
| Exact recall (brute-force mode) | Guaranteed | `search_mode='brute_force'`, recall=1.0 (3L/ADR-039), exact at fp16; O(N) search, cheapest when N small or filtered/selective |
| Exact recall (CAGRA default path) | Not guaranteed | Approximate ANN; recall measured/tuned |
| Native PostgreSQL index equivalence | Not claimed | Derived accelerator, not WAL-logged mutable index |

## Differentiation

The differentiator is not GPU acceleration alone. External vector databases and
GPU libraries already provide fast vector search.

The differentiator is the combination:

- PostgreSQL remains the query, transaction, and data-control plane.
- pgvector-compatible SQL surface is preserved.
- cuVS is used for high-cost candidate generation.
- exact GPU search on demand (`search_mode='brute_force'`, recall=1.0) alongside
  approximate CAGRA, with the planner choosing by cost at small N.
- database failure modes are explicit: fallback, ERROR, or fail-closed, rather
  than silent stale/partial GPU results.
- multi-GPU sharding is hidden behind one PostgreSQL index.
- object storage snapshots accelerate rebuild/warmup without replacing
  PostgreSQL replication or backup.

This positions pg_cuvs as:

```text
a PostgreSQL-native GPU ANN acceleration runtime,
not a replacement vector database and not a raw cuVS binding.
```

## Messaging Guidance

Prefer:

```text
pg_cuvs uses NVIDIA cuVS as a derived GPU accelerator under PostgreSQL index
semantics.
```

or:

```text
PostgreSQL remains the source of truth; cuVS provides GPU candidate generation.
```

or, where it is true (brute-force path):

```text
exact GPU vector search via brute-force mode (recall=1.0); for small-N or
filtered/selective queries it is exact and cheaper than approximate CAGRA.
```

Avoid:

```text
cuVS as a transactional database
```

```text
drop-in PostgreSQL native index equivalence
```

```text
the default CAGRA path is exact (it is approximate; only brute-force mode is exact)
```

Both remaining "Avoid" claims overstate the current design. But a blanket "no
exact GPU search" is itself wrong: brute-force mode (`search_mode='brute_force'`,
3L/ADR-039) delivers exact GPU search, and is the cost-effective exact path for
small-N and filtered/selective workloads (the ADR-061 wedge). The honest claim is
stronger: pg_cuvs respects the PostgreSQL/cuVS boundary AND offers exact GPU
search where it is cheap to do so, while being clear that the default CAGRA path
is approximate.
