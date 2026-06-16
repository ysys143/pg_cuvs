#!/usr/bin/env bash
# spike-rabitq.sh — run the numpy RaBitQ feasibility spike on the VM's real
# cohere data (NOT a protocol measurement: it writes no CSV row, it prints
# PASS/FAIL to the job log). Dispatched via bench.yml with configs=spike-rabitq
# so we can reach ~/anbench/data on the self-hosted runner. Reuses _common.sh's
# conda + corpus/queries env defaults. The cell_id arg is ignored.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$PROTO/../.." && pwd)"

conda="${PGCUVS_CONDA:-$HOME/miniforge3}"
env="${PGCUVS_ENV:-cuvs_py}"
corpus="${PGCUVS_CORPUS:-$HOME/anbench/data/corpus.fbin}"
queries="${PGCUVS_QUERIES:-$HOME/anbench/data/queries_10k.fbin}"

# shellcheck disable=SC1091
source "$conda/bin/activate" "$env"
exec python3 "$REPO/tools/rabitq_spike.py" \
  --corpus "$corpus" --queries "$queries" --n 100000 --nq 200
