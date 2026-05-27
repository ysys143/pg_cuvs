# Phase 2 Test Matrix

Maps each Product Phase 2 feature to the tests that cover it. Layers:
- **Unit** — `test/unit/test_cuvs_util.c` (`make test-unit`, no GPU/PG).
- **Regress** — `test/sql/edge_cases.sql` + `test/expected/edge_cases.out` (`make gpu-test-regress`).
- **Integ** — `infra/scripts/integration-test.sh` scenarios (`make gpu-test-daemon`).
- **E2E** — `infra/scripts/e2e-smoke.sh` (`make gpu-test-e2e`).
- **Bench** — `infra/anbench` / `make gpu-bench*` (Phase 1.5 infra).

| Feature (Step) | Unit | Regress | Integ | E2E | Bench |
|---|---|---|---|---|---|
| Observability `pg_stat_gpu_search` (1) | percentile histogram (`test_lat_histogram`) | view row: stale/requested_k/metric, daemon-off no-crash | Sc 8 (search_count++, daemon-down empty) | — | — |
| LIMIT-k via `cuvs.k` (2) | — (GUC, no helper) | `cuvs.k=7` -> `requested_k=7`, rows bounded | Sc 9 (`cuvs.k=9` -> `requested_k=9`) | — | k sweep (anbench KS) |
| Metric from opclass (2) | `cuvs_metric_from_opclass_name` | view `metric` = l2/cosine/ip | Sc 9 (cosine build -> metric=cosine) | l2/cosine/ip restart-stable | metric sweep |
| Planner / cost model (3) | — (cost inline) | EXPLAIN plan-shape: GPU chosen / enable_cuvs=off seqscan / large-k stays GPU | — | — | plan vs bench (manual) |
| Write/staleness (4) | — (daemon-side) | INSERT->stale=t, REINDEX->stale=f, **DELETE+VACUUM->stale=t**; stale query returns CPU ground truth | Sc 10 (stale + .stale restart-persist + REINDEX + Seq Scan reroute + CPU ground truth; delete-drift gate) | — | — |
| Build memory cap (5) | — (inline, /proc) | — (MB cap not triggerable on tiny tables) | Sc 11 (cap=1 -> fail-fast, rollback, daemon alive; high cap -> ok) | — | RSS (deferred) |
| VRAM tiered cache (6) | — | — (MB budget not triggerable on tiny tables) | Sc 12 (evict-to-fit + reload; `pg_stat_gpu_cache` evictions/reloads) | — | — |
| Persistence / corruption (1.5) | .tids CRC/magic/header rejections | — | Sc 2/3 persist fault, Sc 5 registry-full | restart reload + .tids magic | — |

## Intentional deferrals (not gaps)
- **cost / build-size pure unit helpers**: logic is inline and PG/`/proc`-coupled; covered by
  regression (plan-shape) and integration (Sc 11) instead of forced pure-helper refactors.
- **metric legacy-mismatch (`CUVS_STATUS_METRIC_MISMATCH`)**: only reachable for indexes built before
  the opclass-metric fix; a freshly built index always matches its opclass, so there is no SQL path to
  construct a mismatch in tests. Daemon guard + backend ERROR are exercised by code review.
- **build streaming / mmap staging (Step 5 2차)**: deferred to a later step; cap = ERROR for oversized.
- **automatic peak backend RSS in benchmarks**: needs cross-process PID tracking; deferred.
- **delta correction**: Phase 3 (`pg_cuvs.c` gettuple STALE branch has the seam comment).
- **10M-scale benchmark**: VM-capacity dependent; run when feasible.
