# Document Map

pg_cuvs documentation splits into three layers. The distinction that matters: **current-state
SSOT** (what the product *is* now) vs **historical record** (how it got built and why) vs
**operational** (how to run it). When a fact and a historical doc disagree, the SSOT wins.

## Current-state SSOT — "what it is now"

Maintained to reflect the shipping product. Start here.

| Doc | Answers |
|-----|---------|
| [README.md](../README.md) | Overview, install, requirements, quickstart |
| [ARCHITECTURE.md](../ARCHITECTURE.md) | How it works: components, IPC, index lifecycle (incl. `flat` AM build/search/evict/restart), VRAM accounting, sharding, GCS, write path, key techniques, limitations |
| [docs/reference.md](reference.md) | The surface: index AMs (`cagra`, `flat`, `ivfpq`, `pg_cuvs_hnsw`), search modes, GUCs, reloptions, SQL functions, observability views |
| [docs/operational-guide.md](operational-guide.md) | Workload selection guide: flat vs cagra vs ivfpq, measured latency/throughput, crossovers, cost characteristics |
| [docs/best-practices.md](best-practices.md) | Build-time recommendations (TOAST/PLAIN, index_dir placement, version pinning) |
| [BENCHMARK.md](../BENCHMARK.md) | Published performance + overhead characterization |

## Historical record — "how it was built / why"

Preserved for provenance. **Not** kept in sync with the current product; read as history.

| Doc | Role |
|-----|------|
| [design/DECISIONS.md](../design/DECISIONS.md) | ADRs — every design decision, alternatives, rejection reasons. The "why" of record. Key recent: **ADR-073** (`flat` AM, supersedes ADR-071), ADR-072 (DiskANN direction), ADR-070 (resource governance) |
| [design/PLAN.md](../design/PLAN.md) | **Frozen.** Per-phase as-built spec + completion criteria + verification evidence. Its planning role ended when implementation completed |
| [design/SPEC.md](../design/SPEC.md), [design/STRATEGY_NOTES.md](../design/STRATEGY_NOTES.md), [design/PROJECT_POSITIONING.md](../design/PROJECT_POSITIONING.md) | Earlier requirements / strategy / positioning; superseded by the SSOT docs for current state |
| [design/BENCHMARK_CROSSOVER.md](../design/BENCHMARK_CROSSOVER.md), [docs/profiling-results.md](profiling-results.md) | Measurement methodology + raw profiling that BENCHMARK.md cites |
| [design/PHASE_3B_*.md](../design/), [docs/spec-audit-*.md](.) | Spike/decision records for specific phases |
| [docs/filter-threshold-experiment.md](filter-threshold-experiment.md), [docs/bruteforce-acceleration-lessons.md](bruteforce-acceleration-lessons.md), [docs/phase2-exit-criteria.md](phase2-exit-criteria.md), [docs/phase2-test-matrix.md](phase2-test-matrix.md), [docs/ecosystem-strategy.md](ecosystem-strategy.md), [docs/reports/](reports/) | Experiment results, lessons, phase-completion criteria, ecosystem strategy (ADR-062), prerelease reports |

## Active planning — "what's next"

| Doc | Role |
|-----|------|
| [ROADMAP.md](../ROADMAP.md) | Sequence of remaining work + trigger-based backlog. New sequencing goes here, not PLAN.md |
| [design/BENCHMARK_PROTOCOL.md](../design/BENCHMARK_PROTOCOL.md) | The rigorous benchmark + cost-model calibration protocol (ADR-069). New benchmark planning goes here |
| [design/CI_STRATEGY.md](../design/CI_STRATEGY.md) | 2-tier CI design (ADR-067) |
| [design/REFACTOR_PLAN.md](../design/REFACTOR_PLAN.md) | Complexity / orphan-code audit + ordered refactor plan (2026-06-12 3-agent audit) |

## Operational — "how to run it"

| Doc | Role |
|-----|------|
| [design/OPS_GPU_PLAYBOOK.md](../design/OPS_GPU_PLAYBOOK.md) | Unified GPU operations reference: tuning, MIG, monitoring views, orphan cleanup |
| [docs/playbooks/](playbooks/) | Task-oriented runbooks (build/test, daemon recovery, VRAM OOM, sharding, GCS snapshot, persistence corruption, capacity planning, release upgrade, replica bootstrap, …) |
| [docs/ci-gpu-setup.md](ci-gpu-setup.md) | GPU CI runner setup (WIF keyless auth, self-hosted) — companion to design/CI_STRATEGY.md |

---

### Where a new fact goes

- A capability changed (GUC, reloption, function, view, search mode) → **docs/reference.md** (+ code).
- How a subsystem behaves changed → **ARCHITECTURE.md**.
- A new design decision → a new ADR in **design/DECISIONS.md**.
- A new step of remaining work → **ROADMAP.md** (sequence) or its trigger backlog.
- A benchmark result → **BENCHMARK.md**; new benchmark methodology → **design/BENCHMARK_PROTOCOL.md**.
- An operational procedure → **docs/playbooks/** (+ link it from OPS_GPU_PLAYBOOK).
- Never re-open **design/PLAN.md**; it is frozen history.
