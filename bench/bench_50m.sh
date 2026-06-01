#!/usr/bin/env bash
# 50M×384 competitive benchmark: DiskANN(2GB) vs HNSW vs CAGRA 2x-sharding.
# All engines run on pg-cuvs-dev-mgpu (2x A100, 170GB RAM).
# DiskANN is constrained to maintenance_work_mem=2GB (pgvectorscale ref conditions).
#
# Usage (run locally, SSHes into mgpu):
#   bash bench/bench_50m.sh
#
# Result appended to bench/results/competitive.csv (same schema).
set -euo pipefail

ZONE=us-central1-f
PROJ=gpu-experiment-wdl-2026
VM=pg-cuvs-dev-mgpu
DB=bench_50m
N=${N:-50000000}; DIM=${DIM:-384}; K=${K:-10}; QUERIES=${QUERIES:-1000}
IDX_DIR=/tmp/cuvs_indexes
DATA=/home/jaesolshin/bench/data_50m
OUT=/home/jaesolshin/bench/results/competitive.csv
HERE=/home/jaesolshin/bench
RECALL_TARGET=0.95

SSH="gcloud compute ssh $VM --zone=$ZONE --project=$PROJ --ssh-flag=-T"
SCP_TO() { gcloud compute scp "$1" "$VM:$2" --zone=$ZONE --project=$PROJ 2>&1 | grep -vE "Warning:|NumPy|tunnel" | tail -1 || true; }

# ── 0. SCP latest harness ─────────────────────────────────────────────────
echo "[50m] step0: SCP harness to mgpu VM"
for f in bench/run_pilot.sh bench/run_pgvectorscale.sh bench/run_vectorchord.sh \
          bench/load_binary.py bench/gt_faiss.py bench/recall.py bench/pctl.py \
          bench/common.py; do
  SCP_TO "$f" "/home/jaesolshin/bench/$(basename $f)"
done

# ── 1. VM setup: pg, daemon, pgvectorscale ───────────────────────────────
echo "[50m] step1: VM setup"
$SSH --command='
pg_isready 2>&1 || { echo "PG not ready"; exit 1; }

# start pg_cuvs daemon with shard_count=2
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
sudo systemctl start pg-cuvs-server 2>/dev/null || true
sleep 3; printf "daemon="; systemctl is-active pg-cuvs-server

# pgvectorscale: check or build
if psql -d postgres -Atc "SELECT default_version FROM pg_available_extensions WHERE name='"'"'vectorscale'"'"';" 2>/dev/null | grep -q .; then
  echo "pgvectorscale: already installed"
else
  echo "pgvectorscale: building from source..."
  source $HOME/.cargo/env 2>/dev/null || curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
  source $HOME/.cargo/env
  [ -d /tmp/pgvectorscale ] || git clone --depth 1 --branch 0.9.0 https://github.com/timescale/pgvectorscale /tmp/pgvectorscale
  wget -q "https://raw.githubusercontent.com/timescale/pgvectorscale/0.9.0/pgvectorscale/Cargo.toml" -O /tmp/pgvs_cargo.toml
  PGRX_VER=$(grep '"'"'pgrx'"'"' /tmp/pgvs_cargo.toml | grep -oE "[0-9]+\.[0-9]+\.[0-9]+" | head -1)
  cargo install --locked cargo-pgrx --version "$PGRX_VER" 2>&1 | tail -2
  cd /tmp/pgvectorscale/pgvectorscale
  cargo pgrx init --pg16 $(which pg_config) 2>&1 | tail -3
  sudo -E $HOME/.cargo/bin/cargo pgrx install --release --pg-config $(which pg_config) 2>&1 | tail -5
fi

# faiss-gpu: check or install in cuvs_dev env
/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python -c "import faiss" 2>/dev/null && echo "faiss: ok" || {
  echo "faiss: installing in cuvs_dev..."
  /home/ubuntu/miniforge3/bin/conda install -y -c pytorch faiss-gpu -n cuvs_dev 2>&1 | tail -3
}
echo "step1 done"
' 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -20

# ── 2. Create DB + tables ─────────────────────────────────────────────────
echo "[50m] step2: create DB"
$SSH --command="
dropdb $DB 2>/dev/null || true; createdb $DB
psql -d $DB -q -c 'CREATE EXTENSION IF NOT EXISTS pg_cuvs CASCADE;'
psql -d $DB -q -c 'CREATE EXTENSION IF NOT EXISTS vectorscale CASCADE;'
psql -d $DB -q -c 'CREATE EXTENSION IF NOT EXISTS vchord CASCADE;'
psql -d $DB -q -c 'CREATE TABLE items(id int PRIMARY KEY, v vector($DIM));'
psql -d $DB -q -c 'CREATE TABLE queries(id int PRIMARY KEY, v vector($DIM));'
mkdir -p $DATA
echo 'tables created'
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -5

# ── 3. Load 50M vectors via binary COPY ───────────────────────────────────
echo "[50m] step3: binary COPY load (50M rows)"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
\$PY $HERE/load_binary.py --n $N --dim $DIM --queries $QUERIES \
  --db $DB --host /var/run/postgresql --data $DATA 2>&1
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -10

# ── 4. GT via faiss-gpu ───────────────────────────────────────────────────
echo "[50m] step4: faiss-gpu GT"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
\$PY $HERE/gt_faiss.py --db $DB --host /var/run/postgresql \
  --data $DATA --k 100 --dim $DIM --n $N 2>&1
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -8

# ── 5. DiskANN (BUILD_MEM=2GB — pgvectorscale reference conditions) ───────
echo "[50m] step5: diskann (BUILD_MEM=2GB)"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=\$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 \
  BUILD_MEM=2GB BUILD_WORKERS=8 STORAGE_LAYOUT=plain \
  bash $HERE/run_pgvectorscale.sh > /tmp/diskann_50m.log 2>&1 \
  && echo 'diskann OK' || echo 'diskann FAIL'
grep -E '\[pgvs\] (build_s|chosen|latency)' /tmp/diskann_50m.log | tail -4
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -8

# ── 6. HNSW (full RAM, reference) ────────────────────────────────────────
echo "[50m] step6: hnsw (full RAM reference)"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
ENGINE=hnsw N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=\$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 BUILD_WORKERS=16 \
  bash $HERE/run_pilot.sh > /tmp/hnsw_50m.log 2>&1 \
  && echo 'hnsw OK' || echo 'hnsw FAIL'
grep -E '\[pilot\] (build_s|chosen|latency)' /tmp/hnsw_50m.log | tail -3
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -6

# ── 7. vchordrq (IVF+RaBitQ) ─────────────────────────────────────────────
echo "[50m] step7: vchordrq (LISTS=8192)"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=\$PY \
  OUT=$OUT DB=$DB SKIP_LOAD=1 LISTS=8192 BUILD_WORKERS=16 \
  bash $HERE/run_vectorchord.sh > /tmp/vc_50m.log 2>&1 \
  && echo 'vc OK' || echo 'vc FAIL'
grep -E '\[vc\] (build_s|chosen|latency)' /tmp/vc_50m.log | tail -4
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -6

# ── 8. CAGRA 2x sharding ─────────────────────────────────────────────────
echo "[50m] step8: cagra 2x sharding"
$SSH --command="
PY=/home/ubuntu/miniforge3/envs/cuvs_dev/bin/python
ENGINE=cagra N=$N DIM=$DIM K=$K RECALL_TARGET=$RECALL_TARGET DATA=$DATA PY=\$PY \
  OUT=$OUT DB=$DB IDX_DIR=$IDX_DIR SKIP_LOAD=1 SHARD_COUNT=2 \
  bash $HERE/run_pilot.sh > /tmp/cagra_50m.log 2>&1 \
  && echo 'cagra OK' || echo 'cagra FAIL'
grep -E '\[pilot\] (build_s|chosen|latency)' /tmp/cagra_50m.log | tail -3
" 2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -6

# ── 8. Collect results ────────────────────────────────────────────────────
echo "[50m] === competitive.csv (50M rows) ==="
$SSH --command="grep ',50000000,' $OUT 2>/dev/null" \
  2>&1 | grep -vE "Warning: Permanently|^Warning:|NumPy|tunnel" | tail -5

echo "[50m] DONE — remember to stop the VM!"
