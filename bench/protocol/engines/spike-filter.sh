#!/usr/bin/env bash
# spike-filter.sh — D2 competitor arm: pgvector filtered kNN under
# hnsw.iterative_scan + p99 (prints to the job log; no CSV). Dispatched via
# bench.yml configs=spike-filter. Reuses _common.sh conda + corpus/queries.
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
exec python3 "$REPO/tools/filter_competitor_spike.py" \
  --corpus "$corpus" --queries "$queries" --n 100000 --nq 300 \
  --dbname "${PGDATABASE:-bench}"
