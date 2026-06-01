#!/usr/bin/env bash
# VectorChord vchordrq benchmark: build index then sweep vchordrq.probes for recall Pareto.
# SKIP_LOAD=1 to reuse existing items/queries table (same DB used across engine comparisons).
#
#   N=100000 DIM=384 K=10 LISTS=1000 RECALL_TARGET=0.95 bash bench/run_vectorchord.sh
set -euo pipefail

ENGINE=vchordrq; AM=vchordrq; EXT=vchord
N=${N:-100000}; DIM=${DIM:-384}; K=${K:-10}
LISTS=${LISTS:-1000}                       # IVF cluster count; rule: sqrt(N)~1000 for 100K
BUILD_MEM=${BUILD_MEM:-'64GB'}             # maintenance_work_mem (CPU 엔진 빌드 메모리)
BUILD_WORKERS=${BUILD_WORKERS:-8}          # max_parallel_maintenance_workers
BUILD_IO=${BUILD_IO:-200}                  # maintenance_io_concurrency
DB=${DB:-bench}; DATA=${DATA:-bench/data}
RECALL_TARGET=${RECALL_TARGET:-0.95}
M=${M:-200}                                # latency sample queries
CLIENTS=${CLIENTS:-8}; JOBS=${JOBS:-4}; DURATION=${DURATION:-15}
OUT=${OUT:-bench/results/competitive.csv}
PY=${PY:-python3}
HERE="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$(dirname "$OUT")"

BASE_OPTS="-c enable_seqscan=off"
psql_q() { PGOPTIONS="$BASE_OPTS -c vchordrq.probes=$1" psql -d "$DB" -v ON_ERROR_STOP=1 -At -F' '; }

# 1. schema + load (skip if SKIP_LOAD=1) --------------------------------
if [ "${SKIP_LOAD:-0}" != "1" ]; then
  createdb "$DB" 2>/dev/null || true
  psql -d "$DB" -v ON_ERROR_STOP=1 -q <<SQL
CREATE EXTENSION IF NOT EXISTS $EXT CASCADE;
DROP TABLE IF EXISTS items, queries;
CREATE TABLE items(id int PRIMARY KEY, v vector($DIM));
CREATE TABLE queries(id int PRIMARY KEY, v vector($DIM));
\copy items(id,v)   FROM '$DATA/base.copy'
\copy queries(id,v) FROM '$DATA/query.copy'
SQL
fi
echo "[vc] loaded N=$(psql -d "$DB" -Atc 'SELECT count(*) FROM items')"

# 2. build index ---------------------------------------------------------
t0=$(date +%s.%N)
psql -d "$DB" -v ON_ERROR_STOP=1 -q <<SQL
SET maintenance_work_mem='$BUILD_MEM';
SET max_parallel_maintenance_workers=$BUILD_WORKERS;
SET max_parallel_workers=$BUILD_WORKERS;
SET maintenance_io_concurrency=$BUILD_IO;
SET jit=off;
DROP INDEX IF EXISTS bench_idx;
CREATE INDEX bench_idx ON items USING $AM (v vector_l2_ops)
  WITH (options = \$\$
[build.internal]
lists = [$LISTS]
\$\$);
SQL
t1=$(date +%s.%N); BUILD_S=$(echo "$t1 - $t0" | bc)
echo "[vc] build_s=$BUILD_S"
IDX_BYTES=$(psql -d "$DB" -Atc "SELECT pg_relation_size('bench_idx');")

# 3. probes sweep for recall Pareto -------------------------------------
RESULTS=$(mktemp); CHOSEN=""; CHOSEN_RECALL=""
SWEEP=${SWEEP:-"5 10 20 50 100 200 400"}
for p in $SWEEP; do
  psql_q "$p" <<SQL > "$RESULTS"
SELECT q.id, t.id
FROM queries q,
LATERAL (SELECT i.id FROM items i ORDER BY i.v <-> q.v LIMIT $K) t
ORDER BY q.id;
SQL
  r=$($PY "$HERE/recall.py" --gt "$DATA/gt.ibin" --results "$RESULTS" --k "$K")
  echo "[vc] probes=$p recall@$K=$r"
  awk "BEGIN{exit !($r >= $RECALL_TARGET)}" && { CHOSEN=$p; CHOSEN_RECALL=$r; break; }
done
if [ -z "$CHOSEN" ]; then
  CHOSEN=$(echo "$SWEEP" | awk '{print $NF}'); CHOSEN_RECALL=$r
  echo "[vc] WARN target $RECALL_TARGET unmet; using max probes=$CHOSEN (recall=$CHOSEN_RECALL)"
fi
echo "[vc] chosen probes=$CHOSEN recall=$CHOSEN_RECALL"

# 4. latency sample (M single queries) ----------------------------------
LSQL=$(mktemp); LOUT=$(mktemp)
QN=$(psql -d "$DB" -Atc 'SELECT count(*) FROM queries')
{ echo "\\timing on";
  for i in $(seq 1 "$M"); do
    qid=$(( RANDOM % QN ));
    echo "SELECT i.id FROM items i ORDER BY i.v <-> (SELECT v FROM queries WHERE id=$qid) LIMIT $K;";
  done; } > "$LSQL"
PGOPTIONS="$BASE_OPTS -c vchordrq.probes=$CHOSEN" psql -d "$DB" -q -f "$LSQL" 2>&1 \
  | grep -oE 'Time: [0-9.]+ ms' | grep -oE '[0-9.]+' > "$LOUT" || true
PCTL=$($PY "$HERE/pctl.py" < "$LOUT")

# 5. throughput (pgbench) -----------------------------------------------
PGB=$(mktemp)
cat > "$PGB" <<SQL
\\set qid random(0, $((QN-1)))
SELECT i.id FROM items i ORDER BY i.v <-> (SELECT v FROM queries WHERE id=:qid) LIMIT $K;
SQL
QPS=$(PGOPTIONS="$BASE_OPTS -c vchordrq.probes=$CHOSEN" \
  pgbench -n -f "$PGB" -c "$CLIENTS" -j "$JOBS" -T "$DURATION" "$DB" 2>/dev/null \
  | grep -oE 'tps = [0-9.]+' | head -1 | grep -oE '[0-9.]+' || echo "")

# 6. CSV row -------------------------------------------------------------
to_us() { awk "BEGIN{printf \"%.1f\", $1*1000}"; }
P50=$(echo "$PCTL" | grep -oE 'p50=[0-9.]+' | cut -d= -f2); P50=$(to_us "${P50:-0}")
P95=$(echo "$PCTL" | grep -oE 'p95=[0-9.]+' | cut -d= -f2); P95=$(to_us "${P95:-0}")
P99=$(echo "$PCTL" | grep -oE 'p99=[0-9.]+' | cut -d= -f2); P99=$(to_us "${P99:-0}")
AVG=$(echo "$PCTL" | grep -oE 'avg=[0-9.]+' | cut -d= -f2); AVG=$(to_us "${AVG:-0}")
if [ ! -s "$OUT" ]; then
  echo "system,index,N,dim,k,recall_target,build_s,qps,p50_us,p95_us,p99_us,avg_us,recall_at_k,params,index_bytes" > "$OUT"
fi
echo "vchordrq,$AM,$N,$DIM,$K,$RECALL_TARGET,$BUILD_S,${QPS:-NA},$P50,$P95,$P99,$AVG,$CHOSEN_RECALL,lists=${LISTS}_probes=$CHOSEN,$IDX_BYTES" >> "$OUT"
echo "[vc] appended row to $OUT"
rm -f "$RESULTS" "$LSQL" "$LOUT" "$PGB"
