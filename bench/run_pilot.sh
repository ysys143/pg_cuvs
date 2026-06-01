#!/usr/bin/env bash
# Pilot crossover benchmark: pgvector HNSW vs pg_cuvs CAGRA on IDENTICAL data,
# iso-recall comparison. Run ON the GPU VM (see design/BENCHMARK_CROSSOVER.md).
#
#   ENGINE=hnsw  N=100000 DIM=384 K=10 RECALL_TARGET=0.95 bash bench/run_pilot.sh
#   ENGINE=cagra N=100000 DIM=384 K=10 RECALL_TARGET=0.95 bash bench/run_pilot.sh
#
# Requires (hnsw):  pgvector installed.
# Requires (cagra): pg_cuvs installed + pg-cuvs-server daemon active, IDX_DIR
#                   matching the daemon --index-dir.
# Pre-req once: python3 bench/gen_dataset.py ... && python3 bench/gt.py ...
set -euo pipefail

ENGINE=${ENGINE:-hnsw}                 # hnsw | cagra
N=${N:-100000}; DIM=${DIM:-384}; K=${K:-10}
DB=${DB:-bench}; DATA=${DATA:-bench/data}
RECALL_TARGET=${RECALL_TARGET:-0.95}
M=${M:-200}                            # latency sample size (single-query timings)
BUILD_MEM=${BUILD_MEM:-'64GB'}         # maintenance_work_mem for index build (CPU engines)
BUILD_WORKERS=${BUILD_WORKERS:-8}      # max_parallel_maintenance_workers
BUILD_IO=${BUILD_IO:-200}              # maintenance_io_concurrency (SSD-tuned)
SHARD_COUNT=${SHARD_COUNT:-1}          # cuvs.shard_count for multi-GPU (1=unsharded)
CLIENTS=${CLIENTS:-8}; JOBS=${JOBS:-4}; DURATION=${DURATION:-15}
IDX_DIR=${IDX_DIR:-/tmp/cuvs_indexes}
OUT=${OUT:-bench/results/pilot.csv}
PY=${PY:-python3}

mkdir -p "$(dirname "$OUT")"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Per-engine knobs ---------------------------------------------------------
if [ "$ENGINE" = "hnsw" ]; then
  EXT="vector"; AM="hnsw"; OPCLASS="vector_l2_ops"
  SWEEP=${SWEEP:-"10 20 40 80 160 320"}      # hnsw.ef_search
  set_param() { echo "-c hnsw.ef_search=$1"; }
elif [ "$ENGINE" = "cagra" ]; then
  EXT="pg_cuvs"; AM="cagra"; OPCLASS="vector_l2_ops"
  SWEEP=${SWEEP:-"16 32 64 128 256"}          # cuvs.k (GPU search breadth)
  set_param() { echo "-c cuvs.k=$1 -c cuvs.index_dir=$IDX_DIR -c cuvs.shard_count=$SHARD_COUNT"; }
else
  echo "unknown ENGINE=$ENGINE (hnsw|cagra)"; exit 2
fi

BASE_OPTS="-c enable_seqscan=off"
psql_base() { PGOPTIONS="$BASE_OPTS" psql -d "$DB" -v ON_ERROR_STOP=1 -At "$@"; }
psql_tuned() { local p="$1"; shift; PGOPTIONS="$BASE_OPTS $(set_param "$p")" psql -d "$DB" -v ON_ERROR_STOP=1 -At "$@"; }

# 1. schema + load --------------------------------------------------------
# SKIP_LOAD=1: skip if table is pre-loaded externally (e.g. stream-loaded from fbin)
if [ "${SKIP_LOAD:-0}" != "1" ]; then
  createdb "$DB" 2>/dev/null || true
  psql -d "$DB" -v ON_ERROR_STOP=1 <<SQL
CREATE EXTENSION IF NOT EXISTS $EXT CASCADE;
DROP TABLE IF EXISTS items, queries;
CREATE TABLE items(id int PRIMARY KEY, v vector($DIM));
CREATE TABLE queries(id int PRIMARY KEY, v vector($DIM));
\copy items(id,v)   FROM '$DATA/base.copy'
\copy queries(id,v) FROM '$DATA/query.copy'
SQL
fi
echo "[pilot] loaded N=$(psql -d "$DB" -Atc 'SELECT count(*) FROM items')"

# 2. build index (timed) --------------------------------------------------
[ "$ENGINE" = "cagra" ] && export PGOPTIONS="-c cuvs.index_dir=$IDX_DIR"
t0=$(date +%s.%N)
psql -d "$DB" -v ON_ERROR_STOP=1 <<SQL
SET maintenance_work_mem='$BUILD_MEM';
SET max_parallel_maintenance_workers=$BUILD_WORKERS;
SET max_parallel_workers=$BUILD_WORKERS;
SET maintenance_io_concurrency=$BUILD_IO;
SET jit=off;
DROP INDEX IF EXISTS bench_idx;
CREATE INDEX bench_idx ON items USING $AM (v $OPCLASS);
SQL
t1=$(date +%s.%N); BUILD_S=$(echo "$t1 - $t0" | bc)
unset PGOPTIONS
IDX_BYTES=$(psql -d "$DB" -Atc "SELECT pg_relation_size('bench_idx');")
echo "[pilot] build_s=$BUILD_S index_bytes=$IDX_BYTES"

# 3. iso-recall tuning: smallest sweep value meeting RECALL_TARGET --------
RESULTS=$(mktemp); CHOSEN=""; CHOSEN_RECALL=""
recall_query() {   # $1=param -> writes "qid nid" lines to $RESULTS
  psql_tuned "$1" -F' ' <<SQL > "$RESULTS"
SELECT q.id, t.id
FROM queries q,
LATERAL (SELECT i.id FROM items i ORDER BY i.v <-> q.v LIMIT $K) t
ORDER BY q.id;
SQL
}
for p in $SWEEP; do
  recall_query "$p"
  r=$($PY "$HERE/recall.py" --gt "$DATA/gt.ibin" --results "$RESULTS" --k "$K")
  echo "[pilot] param=$p recall@$K=$r"
  awk "BEGIN{exit !($r >= $RECALL_TARGET)}" && { CHOSEN=$p; CHOSEN_RECALL=$r; break; }
done
if [ -z "$CHOSEN" ]; then
  CHOSEN=$(echo "$SWEEP" | awk '{print $NF}'); CHOSEN_RECALL=$r
  echo "[pilot] WARN target $RECALL_TARGET unmet; using max param=$CHOSEN (recall=$CHOSEN_RECALL)"
fi
echo "[pilot] chosen param=$CHOSEN recall=$CHOSEN_RECALL"

# 4. latency sample (M single queries, client-side \timing) ---------------
LSQL=$(mktemp); LOUT=$(mktemp)
QN=$(psql -d "$DB" -Atc 'SELECT count(*) FROM queries')
{ echo "\\timing on";
  for i in $(seq 1 "$M"); do
    qid=$(( RANDOM % QN ));
    echo "SELECT i.id FROM items i ORDER BY i.v <-> (SELECT v FROM queries WHERE id=$qid) LIMIT $K;";
  done; } > "$LSQL"
PGOPTIONS="$BASE_OPTS $(set_param "$CHOSEN")" psql -d "$DB" -q -f "$LSQL" 2>&1 \
  | grep -oE 'Time: [0-9.]+ ms' | grep -oE '[0-9.]+' > "$LOUT" || true
PCTL=$($PY "$HERE/pctl.py" < "$LOUT")     # p50=.. p95=.. p99=.. avg=.. (ms)
echo "[pilot] latency(ms) $PCTL"

# 5. throughput (pgbench) -------------------------------------------------
PGB=$(mktemp)
cat > "$PGB" <<SQL
\\set qid random(0, $(($(psql -d "$DB" -Atc 'SELECT count(*) FROM queries')-1)))
SELECT i.id FROM items i ORDER BY i.v <-> (SELECT v FROM queries WHERE id=:qid) LIMIT $K;
SQL
QPS=$(PGOPTIONS="$BASE_OPTS $(set_param "$CHOSEN")" \
  pgbench -n -f "$PGB" -c "$CLIENTS" -j "$JOBS" -T "$DURATION" "$DB" 2>/dev/null \
  | grep -oE 'tps = [0-9.]+' | head -1 | grep -oE '[0-9.]+' || echo "")

# 6. emit CSV row ---------------------------------------------------------
to_us() { awk "BEGIN{printf \"%.1f\", $1*1000}"; }
P50=$(echo "$PCTL" | grep -oE 'p50=[0-9.]+' | cut -d= -f2); P50=$(to_us "${P50:-0}")
P95=$(echo "$PCTL" | grep -oE 'p95=[0-9.]+' | cut -d= -f2); P95=$(to_us "${P95:-0}")
P99=$(echo "$PCTL" | grep -oE 'p99=[0-9.]+' | cut -d= -f2); P99=$(to_us "${P99:-0}")
AVG=$(echo "$PCTL" | grep -oE 'avg=[0-9.]+' | cut -d= -f2); AVG=$(to_us "${AVG:-0}")
if [ ! -s "$OUT" ]; then
  echo "system,index,N,dim,k,recall_target,build_s,qps,p50_us,p95_us,p99_us,avg_us,recall_at_k,params,index_bytes" > "$OUT"
fi
echo "$ENGINE,$AM,$N,$DIM,$K,$RECALL_TARGET,$BUILD_S,${QPS:-NA},$P50,$P95,$P99,$AVG,$CHOSEN_RECALL,param=$CHOSEN,${IDX_BYTES:-NA}" >> "$OUT"
echo "[pilot] appended row to $OUT"
rm -f "$RESULTS" "$LSQL" "$LOUT" "$PGB"
