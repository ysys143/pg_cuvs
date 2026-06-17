#!/usr/bin/env bash
# forced-cuvs-concurrent.sh — D3 query-QPS-under-ingest for cagra (the GPU
# streaming engine: INSERT -> cuvsCagraExtend). module=incremental.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
export PGCUVS_INC_SCENARIO=concurrent
run_engine forced-cuvs "${1:?cell_id}"
