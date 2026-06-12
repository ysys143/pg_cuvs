# sourced by engines/<config>.sh — activates conda + execs runner.py.
# Expects $PROTO (bench/protocol dir) and the run.sh-exported env
# (CSV, RUN_ID, STAGE, DATASET, REPS, BASELINE, COST_MODEL_VERSION, ...).
#
# VM environment defaults are the pg-cuvs-dev ground truth (issue #56) and are
# overridable by bench.yml env so the same engine works elsewhere.
run_engine(){
  local config="$1" cell_id="${2:?cell_id}"
  : "${CSV:?}"; : "${RUN_ID:?}"; : "${STAGE:?}"; : "${PROTO:?}"

  local conda="${PGCUVS_CONDA:-$HOME/miniforge3}"
  local env="${PGCUVS_ENV:-cuvs_py}"
  local corpus="${PGCUVS_CORPUS:-$HOME/anbench/data/corpus.fbin}"
  local queries="${PGCUVS_QUERIES:-$HOME/anbench/data/queries_10k.fbin}"
  local gtdir="${PGCUVS_GT_DIR:-$HOME/anbench/data}"
  local index_dir="${CUVS_INDEX_DIR:-/tmp/cuvs_indexes}"
  local dbname="${PGDATABASE:-bench}"

  # daemon pid so ResourceSampler folds pg-cuvs-server host RSS in (CONTRACT §4)
  local dpid=""
  case "$config" in
    forced-cuvs*|auto)   # forced-cuvs, forced-cuvs-bf, forced-cuvs-bf-batch, auto
      dpid="$(systemctl show -p MainPID --value pg-cuvs-server 2>/dev/null || true)"
      [ "$dpid" = "0" ] && dpid="" ;;
  esac

  # shellcheck disable=SC1091
  source "$conda/bin/activate" "$env"
  # PGCUVS_MODULE selects the runner.
  local pyrunner="runner.py"
  case "${PGCUVS_MODULE:-physics}" in
    concurrency) pyrunner="runner_concurrency.py" ;;
    explain)     pyrunner="runner_explain.py" ;;
  esac
  exec python3 "$PROTO/$pyrunner" \
    --config "$config" --cell "$cell_id" \
    --csv "$CSV" --run-id "$RUN_ID" --stage "$STAGE" \
    --dataset "${DATASET:-cohere-1m}" --reps "${REPS:-5}" \
    --baseline "${BASELINE:-same-box}" \
    --corpus "$corpus" --queries "$queries" --gt-dir "$gtdir" \
    --index-dir "$index_dir" --dbname "$dbname" \
    ${dpid:+--daemon-pid "$dpid"} \
    ${PGCUVS_INSTANCE_TYPE:+--instance-type "$PGCUVS_INSTANCE_TYPE"} \
    ${PGCUVS_PRICE_USD_HR:+--price-usd-hr "$PGCUVS_PRICE_USD_HR"} \
    --cost-model-version "${COST_MODEL_VERSION:-unset}" \
    --runtime-routing-version "${RUNTIME_ROUTING_VERSION:-unset}"
}
