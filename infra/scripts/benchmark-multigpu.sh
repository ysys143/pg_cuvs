#!/usr/bin/env bash
# benchmark-multigpu.sh — Phase 3E-2 multi-GPU partitioned-table benchmark
#
# Measures:
#   1. Correctness: GPU top-k recall vs CPU brute-force ground truth
#   2. Throughput: queries/sec single-GPU vs multi-GPU partitioned
#   3. Build time: single index vs partitioned indexes
#
# Usage:
#   bash infra/scripts/benchmark-multigpu.sh [N_PARTITIONS] [N_ROWS] [DIM] [K]
#
# Requires: psql, pg_cuvs + vector installed, daemon running with N GPUs.

set -euo pipefail

DB="${DB:-postgres}"
USER="${USER:-ubuntu}"
NP="${1:-2}"        # number of partitions (= GPUs ideally)
NR="${2:-100000}"   # rows
DIM="${3:-128}"     # vector dimension
K="${4:-10}"        # top-k
QUERIES=100         # number of test queries for throughput

echo "=== pg_cuvs multi-GPU benchmark ==="
echo "partitions=$NP rows=$NR dim=$DIM k=$K queries=$QUERIES"
echo ""

psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SQL

# --- Helper ---
run_sql() { psql -U "$USER" -d "$DB" -tAX -v ON_ERROR_STOP=1 -c "$1"; }
run_sql_time() { psql -U "$USER" -d "$DB" -tAX -v ON_ERROR_STOP=1 -c "\\timing on" -c "$1" 2>&1; }

# ============================================================================
# 1. Setup: single table (baseline) + partitioned table
# ============================================================================
echo "--- setup ---"

psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
DROP TABLE IF EXISTS bench_single CASCADE;
DROP TABLE IF EXISTS bench_part CASCADE;

-- Single table (baseline)
CREATE TABLE bench_single (id bigint PRIMARY KEY, embedding vector($DIM));
INSERT INTO bench_single
SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector($DIM)
FROM generate_series(1, $NR) g, generate_series(1, $DIM) d GROUP BY g;

-- Partitioned table
CREATE TABLE bench_part (id bigint NOT NULL, embedding vector($DIM))
    PARTITION BY HASH (id);
SQL

# Create N partitions dynamically
for (( i=0; i<NP; i++ )); do
    psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE TABLE bench_part_p${i} PARTITION OF bench_part
    FOR VALUES WITH (MODULUS $NP, REMAINDER $i);
SQL
done

psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
INSERT INTO bench_part SELECT id, embedding FROM bench_single;
SQL

echo "rows: single=$(run_sql 'SELECT count(*) FROM bench_single'), partitioned=$(run_sql 'SELECT count(*) FROM bench_part')"

# Partition distribution
echo "partition distribution:"
run_sql "SELECT tableoid::regclass, count(*) FROM bench_part GROUP BY 1 ORDER BY 1;"

# ============================================================================
# 2. Build: single index vs partitioned indexes
# ============================================================================
echo ""
echo "--- build ---"

T0=$(date +%s%N)
psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE INDEX bench_single_cagra ON bench_single USING cagra (embedding vector_l2_ops);
SQL
T1=$(date +%s%N)
SINGLE_BUILD_MS=$(( (T1 - T0) / 1000000 ))
echo "single index build: ${SINGLE_BUILD_MS} ms"

T0=$(date +%s%N)
for (( i=0; i<NP; i++ )); do
    psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE INDEX bench_part_p${i}_cagra ON bench_part_p${i} USING cagra (embedding vector_l2_ops);
SQL
done
T1=$(date +%s%N)
PART_BUILD_MS=$(( (T1 - T0) / 1000000 ))
echo "partitioned build ($NP partitions): ${PART_BUILD_MS} ms"

# GPU placement
echo ""
echo "--- GPU placement ---"
run_sql "SELECT index_name, gpu_device_id, n_vecs FROM pg_stat_gpu_search WHERE index_name LIKE 'bench_%' ORDER BY index_name;"

# ============================================================================
# 3. Correctness: recall@K
# ============================================================================
echo ""
echo "--- correctness (recall@$K) ---"

CORRECT=0
TOTAL=$QUERIES
for (( q=1; q<=QUERIES; q++ )); do
    QID=$(( (RANDOM % NR) + 1 ))
    CPU=$(run_sql "SET enable_indexscan=off; SET enable_bitmapscan=off;
        SELECT array_agg(id ORDER BY id) FROM (
            SELECT id FROM bench_single
            ORDER BY embedding <-> (SELECT embedding FROM bench_single WHERE id=$QID)
            LIMIT $K
        ) s;")
    GPU=$(run_sql "SET enable_seqscan=off;
        SELECT array_agg(id ORDER BY id) FROM (
            SELECT id FROM bench_part
            ORDER BY embedding <-> (SELECT embedding FROM bench_single WHERE id=$QID)
            LIMIT $K
        ) s;")
    if [ "$CPU" = "$GPU" ]; then
        CORRECT=$((CORRECT + 1))
    fi
done
echo "recall@$K: $CORRECT / $TOTAL ($(( CORRECT * 100 / TOTAL ))%)"

# ============================================================================
# 4. Throughput: single vs partitioned (serial queries)
# ============================================================================
echo ""
echo "--- throughput (serial, $QUERIES queries) ---"

T0=$(date +%s%N)
for (( q=1; q<=QUERIES; q++ )); do
    QID=$(( (RANDOM % NR) + 1 ))
    run_sql "SET enable_seqscan=off; SELECT id FROM bench_single ORDER BY embedding <-> (SELECT embedding FROM bench_single WHERE id=$QID) LIMIT $K;" >/dev/null
done
T1=$(date +%s%N)
SINGLE_MS=$(( (T1 - T0) / 1000000 ))
SINGLE_QPS=$(( QUERIES * 1000 / (SINGLE_MS + 1) ))
echo "single index: ${SINGLE_MS} ms total, ~${SINGLE_QPS} qps"

T0=$(date +%s%N)
for (( q=1; q<=QUERIES; q++ )); do
    QID=$(( (RANDOM % NR) + 1 ))
    run_sql "SET enable_seqscan=off; SELECT id FROM bench_part ORDER BY embedding <-> (SELECT embedding FROM bench_single WHERE id=$QID) LIMIT $K;" >/dev/null
done
T1=$(date +%s%N)
PART_MS=$(( (T1 - T0) / 1000000 ))
PART_QPS=$(( QUERIES * 1000 / (PART_MS + 1) ))
echo "partitioned ($NP GPU): ${PART_MS} ms total, ~${PART_QPS} qps"

echo ""
echo "--- per-GPU cache stats ---"
run_sql "SELECT gpu_device_id, resident_count, vram_used_mb FROM pg_stat_gpu_cache;"

# ============================================================================
# 5. Cleanup
# ============================================================================
echo ""
echo "--- cleanup ---"
psql -U "$USER" -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
DROP TABLE IF EXISTS bench_single CASCADE;
DROP TABLE IF EXISTS bench_part CASCADE;
SQL
echo "done"
