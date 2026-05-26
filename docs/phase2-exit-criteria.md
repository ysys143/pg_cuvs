# Phase 2 — Exit Criteria Audit

Status of the 10 PLAN.md Phase 2 exit criteria. Legend: **MET** / **MET (deviation)**.
Evidence points to the implementing step and its test(s). See `docs/phase2-test-matrix.md`
for the full feature->test matrix.

Note: 2026-05-26 re-verification found criterion #5 only half-wired — the code
marked stale and preserved `.stale`, but STALE at `amgettuple` returned false,
ending the index scan with zero rows instead of replanning to the CPU. Phase 2.1
closed this by making `cuvsamcostestimate` consult the `.stale` sidecar, so the
planner routes a stale index to seqscan/CPU. #5 is MET again.

| # | Criterion | Status | Evidence |
|---|---|---|---|
| 1 | `pg_stat_gpu_search` shows GPU success/fallback/error/cache/stale/latency | MET | Step 1 (search/error counts, last_status, p50/p95/p99) + Step 4 (stale/stale_since) + Step 6 (`pg_stat_gpu_cache`). Regress: view block; Integ Sc 8. |
| 2 | `LIMIT` reflected in daemon top-k; fixed `k=100` removed | MET (deviation) | Step 2: `cuvs.k` GUC drives daemon top-k; `requested_k` column verifies. Deviation: a PG index AM cannot read SQL `LIMIT` at `amgettuple`, so we use a session GUC (same model as pgvector `hnsw.ef_search`). The hardcoded 100 is gone. |
| 3 | L2/Cosine/IP metric from opclass/operator identity | MET | Step 2: `cuvs_index_metric` (opfamily identity) at build + scan. Regress: `metric` column = l2/cosine/ip; Integ Sc 9. |
| 4 | repeated scan / parameterized / transaction / cursor / LATERAL crash-free, locked in regression | MET | edge_cases.sql: repeated same-session scans, LATERAL rescan, SCROLL cursor in a txn block, and PREPARE/EXECUTE `$1` (Step 8). |
| 5 | After write/delete, a stale index is not silently used and query results fall back to CPU correctness | MET | Step 4/7 mark stale + persist `.stale`; Phase 2.1 wires the fallback: `cuvsamcostestimate` stat()s the `.stale` sidecar and forces cost `1e9` so the planner routes a stale index to seqscan/CPU. Integ Sc 10: on a 100k-row table the planner picks cagra when fresh, then replans to Seq Scan and returns correct rows (id=5, not empty) once stale. Regress: stale query returns the just-inserted row via CPU. |
| 6 | REINDEX clears stale + atomic durable artifact/resident swap | MET | Step 4: build swap resets stale + unlinks `.stale`; persisted `.cagra`/`.tids` atomic rename. Regress: REINDEX->stale=f; Integ Sc 10. |
| 7 | Large build OOM/failure policy is explicit | MET (deviation) | Step 5: `cuvs.max_build_mem_mb` (0=auto = MemAvailable*ratio) preflight + runtime guard -> fail-fast ERROR. Integ Sc 11. Deviation: cap rejects oversized builds; streaming/mmap handoff is deferred (out of Phase 2 scope). |
| 8 | VRAM budget overflow -> eviction/reload/fallback observable in stats + logs | MET | Step 6: `load_index` evict-to-fit, `pg_stat_gpu_cache` (evictions/reloads/hits/misses). Integ Sc 12. |
| 9 | JIT settings not globally changed without experimental evidence | MET | Step 3: cost stays well below `jit_above_cost`; `docs/playbooks/jit-threshold-sweep.md`; no global `jit=off`. |
| 10 | All Phase 1.5 regression stays green | MET | `smoke` + `cpu_fallback` + `edge_cases` green at every step (`make gpu-test-all`). |

## Documented deferrals (tracked, not blocking Phase 2)
- **Build streaming / mmap staging** (Step 5 2nd-tier): oversized builds ERROR for now.
- **Delta correction** for stale indexes: Phase 3 (seam comment in `pg_cuvs.c` gettuple STALE branch).
- **Automatic peak backend RSS** in benchmarks: needs cross-process PID tracking.
- **10M-scale benchmark**: run when VM capacity allows.
- **`gpu_kernel_us`/`ipc_us`/`cpu_recheck_us`** split in stats: single daemon timer today.
- **Delete drift when `ambulkdelete` is suppressed**: PostgreSQL VACUUM can skip
  index vacuuming (the < ~2%-dead-pages bypass, `vacuum_index_cleanup=off`, or the
  wraparound failsafe), so the `.stale` marker is not set and recall silently
  erodes (a deleted TID left in the CAGRA graph is filtered by heap recheck/MVCC,
  so it is recall loss, not a wrong answer). Phase 2.1 adds a plan-time backstop:
  `cuvsamcostestimate` compares the `.tids` build count to the live-row estimate
  and reroutes to CPU once the deleted fraction exceeds `cuvs.max_stale_fraction`
  (default 0.10; 1.0 disables). Integ Sc 10 isolates this with
  `vacuum_index_cleanup=off`. Under normal VACUUM the bypass already bounds drift
  to ~2% of the corpus, largely absorbed by `cuvs.k` over-fetch.
