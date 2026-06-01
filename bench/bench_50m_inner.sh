#!/usr/bin/env bash
# Runs INSIDE the mgpu VM via nohup. No SSH needed after launch.
# Start with: nohup bash ~/bench/bench_50m_inner.sh > ~/bench/bench_50m_full.log 2>&1 &
set -euo pipefail

PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
DB=bench_50m
N=${N:-50000000}; DIM=${DIM:-384}; K=${K:-10}; QUERIES=${QUERIES:-1000}
IDX_DIR=/tmp/cuvs_indexes
DATA=/home/jaesolshin/bench/data_50m
OUT=/home/jaesolshin/bench/results/competitive.csv
HERE=/home/jaesolshin/bench
RECALL_TARGET=0.95

echo "[inner] START $(date)"
mkdir -p "$DATA" "$(dirname "$OUT")"
rm -f "$OUT"

echo "[inner] step1: daemon + pg setup"
sudo systemctl start pg-cuvs-server 2>/dev/null || true
sleep 3; printf "daemon="; systemctl is-active pg-cuvs-server || true

echo "[inner] step2: create DB"
dropdb $DB 2>/dev/null || true; createdb $DB
psql -d $DB -q -c "CREATE EXTENSION IF NOT EXISTS pg_cuvs CASCADE;"
psql -d $DB -q -c "CREATE EXTENSION IF NOT EXISTS vectorscale CASCADE;"
psql -d $DB -q -c "CREATE EXTENSION IF NOT EXISTS vchord CASCADE;"
psql -d $DB -q -c "CREATE TABLE items(id int PRIMARY KEY, v vector($DIM));"
psql -d $DB -q -c "CREATE TABLE queries(id int PRIMARY KEY, v vector($DIM));"
echo "[inner] step2 done"

echo "[inner] step3: binary COPY load $N×$DIM"
$PY $HERE/load_binary.py --n $N --dim $DIM --queries $QUERIES \
  --db $DB --host /var/run/postgresql --data $DATA
echo "[inner] step3 done $(date)"

echo "[inner] step4: GT via faiss (regen from seed, no PG read)"
$PY $HERE/gt_faiss.py --regen --seed 1234 \
  --db $DB --host /var/run/postgresql \
  --data $DATA --k 100 --dim $DIM --n $N --gpu 0
echo "[inner] step4 done $(date)"

echo "[inner] step5: diskann (BUILD_MEM=2GB)"
N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 \
  BUILD_MEM=2GB BUILD_WORKERS=16 STORAGE_LAYOUT=plain \
  bash $HERE/run_pgvectorscale.sh
echo "[inner] step5 done $(date)"

echo "[inner] step6: hnsw"
ENGINE=hnsw N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 BUILD_WORKERS=16 \
  bash $HERE/run_pilot.sh
echo "[inner] step6 done $(date)"

echo "[inner] step7: vchordrq (LISTS=8192)"
N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=$PY \
  OUT=$OUT DB=$DB SKIP_LOAD=1 LISTS=8192 BUILD_WORKERS=16 \
  bash $HERE/run_vectorchord.sh
echo "[inner] step7 done $(date)"

echo "[inner] step8: cagra 2x sharding"
ENGINE=cagra N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 SHARD_COUNT=2 \
  bash $HERE/run_pilot.sh
echo "[inner] step8 done $(date)"

echo "[inner] === DONE $(date) ==="
cat "$OUT"
