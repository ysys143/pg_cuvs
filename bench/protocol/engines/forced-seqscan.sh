#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROTO="$(cd "$HERE/.." && pwd)"
source "$HERE/_common.sh"
run_engine forced-seqscan "${1:?cell_id}"
