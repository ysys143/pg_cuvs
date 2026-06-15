#!/usr/bin/env bash
# resolve_cells.sh — expand a cell-spec into one deterministic cell_id per line.
#
# Usage:   resolve_cells.sh "N=1k,100k;dim=1024;k=10,100;recall=0.95,0.99"
# Output:  one cell_id per line, e.g.  N1k_d1024_k10_r0.95
#
# Grammar: semicolon-separated axes, each "axis=v1,v2,...".
#   axes (fixed order in cell_id): N, dim, k, recall.
#   N accepts k/m/g suffixes (1k=1000, 1m=1_000_000). dim/k/recall default if omitted.
#
# Pure string logic — no GPU/PG. CPU-shim testable (CONTRACT §7).
set -euo pipefail

spec="${1:?usage: resolve_cells.sh \"N=...;dim=...;k=...;recall=...\"}"

declare -A AX=( [N]="" [dim]="1024" [k]="10" [recall]="0.95" )

IFS=';' read -ra parts <<< "$spec"
for p in "${parts[@]}"; do
  [ -z "$p" ] && continue
  key="${p%%=*}"; val="${p#*=}"
  case "$key" in
    N|dim|k|recall) AX[$key]="$val" ;;
    *) echo "resolve_cells: unknown axis '$key'" >&2; exit 2 ;;
  esac
done
[ -n "${AX[N]}" ] || { echo "resolve_cells: N axis required" >&2; exit 2; }

norm_n() {  # 1k -> 1000
  local v="${1,,}"
  case "$v" in
    *k) echo $(( ${v%k} * 1000 )) ;;
    *m) echo $(( ${v%m} * 1000000 )) ;;
    *g) echo $(( ${v%g} * 1000000000 )) ;;
    *)  echo "$v" ;;
  esac
}
nlabel() {  # 1000 -> 1k  (compact, reversible label for cell_id)
  local v="$1"
  if   (( v % 1000000 == 0 )); then echo "$((v/1000000))m"
  elif (( v % 1000 == 0 ));    then echo "$((v/1000))k"
  else echo "$v"; fi
}

IFS=',' read -ra NS <<< "${AX[N]}"
IFS=',' read -ra DS <<< "${AX[dim]}"
IFS=',' read -ra KS <<< "${AX[k]}"
IFS=',' read -ra RS <<< "${AX[recall]}"

for n in "${NS[@]}"; do
  ni="$(norm_n "$n")"; nl="$(nlabel "$ni")"
  for d in "${DS[@]}"; do
    for k in "${KS[@]}"; do
      for r in "${RS[@]}"; do
        echo "N${nl}_d${d}_k${k}_r${r}"
      done
    done
  done
done
