#!/usr/bin/env bash
# spike-ivfpq-refine.sh — run the cuVS ivf_pq + refine() spike (handoff option B)
# on the VM's cohere data. NOT a protocol measurement: prints recall/latency to
# the job log, writes no CSV. Dispatched via bench.yml configs=spike-ivfpq-refine.
# Reuses _common.sh's conda + corpus/queries defaults. cell_id arg ignored.
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
exec python3 "$REPO/tools/ivfpq_refine_spike.py" \
  --corpus "$corpus" --queries "$queries" --n 100000 --nq 200 --n-probes 64
