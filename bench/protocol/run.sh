#!/usr/bin/env bash
# run.sh — single entry point for the benchmark protocol harness.
#
# Contract: bench/protocol/CONTRACT.md. Config comes from env (CONTRACT §2).
# Boundary (reconciled with observe.py, issue #56):
#   * observe.py (infra/anbench)  → resource sampling + protocol-CSV writer
#     (PROTOCOL_FIELDS is the CSV source of truth; header written once).
#   * the per-engine python runner → imports observe, wraps build/query in
#     `with ResourceSampler(...)`, then write_protocol_row(...). CSV write
#     happens INSIDE the runner, never here.
#   * run.sh (this file)          → cell expansion, dispatch, resume, dry-run,
#     engines/<config>.sh calls, PGCUVS_RESULT marker. It does NOT write the CSV.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$HERE/lib"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
# repo-root results/protocol — MUST match bench.yml's publish path (`results/protocol`).
RESULTS_ROOT="${PGCUVS_RESULTS_ROOT:-$REPO_ROOT/results/protocol}"

# ── env (CONTRACT §2) ────────────────────────────────────────────────────────
STAGE="${PGCUVS_STAGE:?PGCUVS_STAGE required (A|B|C|D)}"
MODULE="${PGCUVS_MODULE:-physics}"
CELLS="${PGCUVS_CELLS:?PGCUVS_CELLS required (resolve_cells grammar)}"
CONFIGS="${PGCUVS_CONFIGS:-forced-hnsw,forced-cuvs,auto}"
BASELINE="${PGCUVS_BASELINE:-same-box}"
DATASET="${PGCUVS_DATASET:-cohere-1m}"
REPS="${PGCUVS_REPS:-5}"
RUN_ID="${PGCUVS_RUN_ID:-local-$(date -u +%Y%m%dT%H%M%SZ)}"
DRY_RUN="${PGCUVS_DRY_RUN:-0}"
RESUME="${PGCUVS_RESUME:-0}"
CPU_SHIM="${PGCUVS_CPU_SHIM:-0}"
COST_MODEL_VERSION="${PGCUVS_COST_MODEL_VERSION:-unset}"
RUNTIME_ROUTING_VERSION="${PGCUVS_RUNTIME_ROUTING_VERSION:-unset}"

# observe.protocol_path convention: <base>/<stage>.csv (append-only, run_id col).
# progress/manifest are per-run-id siblings (resume tracks ONE run's measurements).
CSV="$RESULTS_ROOT/$STAGE.csv"
PROGRESS="$RESULTS_ROOT/$STAGE.$RUN_ID.progress"
MANIFEST="$RESULTS_ROOT/$STAGE.$RUN_ID.manifest.json"
mkdir -p "$RESULTS_ROOT"

log(){ echo "[run.sh] $*" >&2; }

# ── resolve plan = cells × configs ───────────────────────────────────────────
mapfile -t CELL_IDS < <("$LIB/resolve_cells.sh" "$CELLS")
IFS=',' read -ra CONFIG_LIST <<< "$CONFIGS"
PLAN=()
for c in "${CELL_IDS[@]}"; do
  for cfg in "${CONFIG_LIST[@]}"; do PLAN+=("$c|$cfg"); done
done
TOTAL=${#PLAN[@]}

if [ "$DRY_RUN" = "1" ]; then
  log "DRY RUN — stage=$STAGE module=$MODULE baseline=$BASELINE dataset=$DATASET reps=$REPS"
  log "resolved ${#CELL_IDS[@]} cells × ${#CONFIG_LIST[@]} configs = $TOTAL measurements:"
  printf '%s\n' "${PLAN[@]}"
  echo "PGCUVS_RESULT: status=OK cells_done=0/$TOTAL (dry-run)"
  exit 0
fi

# ── resume ledger ────────────────────────────────────────────────────────────
touch "$PROGRESS"
declare -A DONE=()
if [ "$RESUME" = "1" ]; then
  while IFS= read -r line; do [ -n "$line" ] && DONE["$line"]=1; done < "$PROGRESS"
  log "resume: ${#DONE[@]} measurements already complete — skipping those"
fi

write_manifest(){
  # Dispatch-level metadata (env snapshot). The richer pg_settings/version dump
  # is folded in by the runner/observe path when measurements run.
  cat > "$MANIFEST" <<EOF
{
  "run_id": "$RUN_ID", "stage": "$STAGE", "module": "$MODULE",
  "baseline": "$BASELINE", "dataset": "$DATASET", "reps": $REPS,
  "cpu_shim": $CPU_SHIM,
  "cost_model_version": "$COST_MODEL_VERSION",
  "runtime_routing_version": "$RUNTIME_ROUTING_VERSION",
  "csv": "$CSV", "started": "$(date -u +%Y-%m-%dT%H:%M:%SZ)", "total": $TOTAL
}
EOF
}
write_manifest

measure(){
  local cell_id="$1" config="$2"
  # CPU-shim: a tiny python runner exercises observe.write_protocol_row so the
  # plumbing (dispatch → observe CSV writer) is testable without GPU/PG.
  if [ "$CPU_SHIM" = "1" ]; then
    python3 "$LIB/shim_runner.py" \
      --csv "$CSV" --stage "$STAGE" --cell "$cell_id" --config "$config" \
      --dataset "$DATASET" --run-id "$RUN_ID" \
      --cost-model-version "$COST_MODEL_VERSION" \
      --runtime-routing-version "$RUNTIME_ROUTING_VERSION"
    return $?
  fi
  # ══════════════════════════ SEAM (real) ══════════════════════════
  # engines/<config>.sh activates the right conda env and calls the adapted
  # python runner (imports observe → ResourceSampler + write_protocol_row).
  local eng="$HERE/engines/${config}.sh"
  if [ ! -x "$eng" ]; then
    log "ERROR: engine '$config' not wired ($eng missing). observe.py vendored; engines pending."
    return 3
  fi
  CSV="$CSV" RUN_ID="$RUN_ID" CELL_ID="$cell_id" STAGE="$STAGE" \
  DATASET="$DATASET" REPS="$REPS" BASELINE="$BASELINE" \
  COST_MODEL_VERSION="$COST_MODEL_VERSION" \
  RUNTIME_ROUTING_VERSION="$RUNTIME_ROUTING_VERSION" \
    "$eng" "$cell_id"
}

# ── main loop: per-cell-per-config atomic unit (CONTRACT §3) ──────────────────
status=OK; done_n=0
for item in "${PLAN[@]}"; do
  if [ -n "${DONE[$item]:-}" ]; then done_n=$((done_n+1)); continue; fi
  cell_id="${item%%|*}"; config="${item##*|}"
  if measure "$cell_id" "$config"; then
    echo "$item" >> "$PROGRESS"
    sync -f "$PROGRESS" 2>/dev/null || sync   # checkpoint: durable before next cell
    done_n=$((done_n+1))
  else
    status=FAIL
    log "measurement FAILED at $item — stopping (resume with PGCUVS_RESUME=1)"
    break
  fi
done

# terminal marker (CONTRACT §3) — stdout last line, for webhook/log diagnosis
echo "PGCUVS_RESULT: status=$status cells_done=$done_n/$TOTAL"
[ "$status" = OK ]
