#!/usr/bin/env bash
# 1M x 1536 standalone benchmark: fbin → psql COPY STDIN (no 14GB base.copy on disk)
set -euo pipefail
DB=bench
DATA=/home/jaesolshin/bench/data
IDX=/tmp/cuvs_indexes
OUT=/home/jaesolshin/bench/results/pilot.csv
PY=/home/jaesolshin/spike3b/dvenv/bin/python
HERE=$(cd "$(dirname "$0")" && pwd)

echo "[1m1536] step 1: disk cleanup"
dropdb $DB 2>/dev/null || true
rm -rf "$DATA" && mkdir -p "$DATA"
rm -f "$IDX"/*.cagra "$IDX"/*.tids "$IDX"/*.shards 2>/dev/null || true
echo "[1m1536] free: $(df -BG /home --output=avail | tail -1 | tr -dc '0-9') GB"

echo "[1m1536] step 2: generate fbin only (no base.copy)"
$PY - <<'PY'
import numpy as np, sys
sys.path.insert(0, '/home/jaesolshin/bench')
from common import write_fbin
DATA = '/home/jaesolshin/bench/data'
rng = np.random.default_rng(1234)
N, D, Q = 1000000, 1536, 1000
nc = max(8, N // 5000)
centers = rng.random((nc, D), dtype=np.float32)
base  = (centers[rng.integers(0, nc, N)] + 0.05 * rng.standard_normal((N, D))).astype(np.float32)
query = (centers[rng.integers(0, nc, Q)] + 0.05 * rng.standard_normal((Q, D))).astype(np.float32)
write_fbin(f'{DATA}/base.fbin', base)
write_fbin(f'{DATA}/query.fbin', query)
print(f'[gen] base={base.shape} query={query.shape}')
PY

echo "[1m1536] step 3: ground truth"
cd "$HERE" && $PY gt.py --data "$DATA" --k 100

echo "[1m1536] step 4: query.copy (small: 1000 x 1536)"
$PY - <<'PY'
import sys; sys.path.insert(0, '/home/jaesolshin/bench')
from common import read_fbin
q = read_fbin('/home/jaesolshin/bench/data/query.fbin')
with open('/home/jaesolshin/bench/data/query.copy', 'w') as f:
    for i in range(q.shape[0]):
        f.write(f'{i}\t[')
        f.write(','.join(f'{x:.6g}' for x in q[i].tolist()))
        f.write(']\n')
print(f'[gen] query.copy rows={q.shape[0]}')
PY

echo "[1m1536] step 5: create DB + stream fbin -> psql COPY STDIN"
createdb $DB
psql -d $DB -q -c "CREATE EXTENSION IF NOT EXISTS pg_cuvs CASCADE;"
psql -d $DB -q -c "CREATE TABLE items(id int PRIMARY KEY, v vector(1536));"
psql -d $DB -q -c "CREATE TABLE queries(id int PRIMARY KEY, v vector(1536));"

$PY - <<'PY' | psql -d bench -c "COPY items(id,v) FROM STDIN"
import numpy as np, struct, sys, time
t0 = time.time()
FBIN = '/home/jaesolshin/bench/data/base.fbin'
with open(FBIN, 'rb') as f:
    n, d = struct.unpack('ii', f.read(8))
    sys.stderr.write(f'streaming {n}x{d}...\n')
    BATCH = 10000
    out = sys.stdout
    for start in range(0, n, BATCH):
        sz = min(BATCH, n - start)
        chunk = np.frombuffer(f.read(sz * d * 4), dtype=np.float32).reshape(sz, d)
        for i in range(sz):
            out.write(str(start + i) + '\t[')
            out.write(','.join(f'{x:.6g}' for x in chunk[i].tolist()))
            out.write(']\n')
        if (start // BATCH) % 20 == 0:
            sys.stderr.write(f'  {start+sz}/{n} ({time.time()-t0:.0f}s)\n')
            out.flush()
PY

psql -d $DB -q -c "\copy queries(id,v) FROM '$DATA/query.copy'"
echo "[1m1536] loaded N=$(psql -d $DB -Atc 'SELECT count(*) FROM items')"

echo "[1m1536] step 6: delete base.fbin (GT done, table loaded)"
rm "$DATA/base.fbin"
echo "[1m1536] free: $(df -BG /home --output=avail | tail -1 | tr -dc '0-9') GB"

cd "$HERE"

echo "[1m1536] step 7: HNSW"
ENGINE=hnsw N=1000000 DIM=1536 K=10 RECALL_TARGET=0.95 DATA=$DATA PY=$PY \
  IDX_DIR=$IDX OUT=$OUT DB=$DB SKIP_LOAD=1 \
  bash run_pilot.sh > /tmp/hnsw_1m1536.log 2>&1 \
  && echo "[1m1536] hnsw OK" || echo "[1m1536] hnsw FAIL"
grep -E '\[pilot\]' /tmp/hnsw_1m1536.log | tail -8

echo "[1m1536] step 8: CAGRA (ensure daemon up)"
sudo systemctl start pg-cuvs-server 2>/dev/null || true; sleep 2
printf "daemon=" && systemctl is-active pg-cuvs-server 2>/dev/null || echo "?"
ENGINE=cagra N=1000000 DIM=1536 K=10 RECALL_TARGET=0.95 DATA=$DATA PY=$PY \
  IDX_DIR=$IDX OUT=$OUT DB=$DB SKIP_LOAD=1 \
  bash run_pilot.sh > /tmp/cagra_1m1536.log 2>&1 \
  && echo "[1m1536] cagra OK" || echo "[1m1536] cagra FAIL"
grep -E '\[pilot\]' /tmp/cagra_1m1536.log | tail -8

echo "=== CSV tail ==="; tail -3 "$OUT"
echo "[1m1536] DONE"
