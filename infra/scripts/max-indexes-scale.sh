#!/bin/bash
# max-indexes-scale.sh — MAX_INDEXES soft-cap: more tenants than registry slots
# must work (build without ERROR + queries auto-reload evicted indexes to GPU).
#
#   make gpu-test-maxidx                          # real GPU daemon (Tier 2)
#   PG_CUVS_SERVER_BIN=./pg_cuvs_server bash \
#     infra/scripts/max-indexes-scale.sh          # CPU shim daemon (Tier 1, per-PR)
#
# Runs a DEDICATED daemon with a TINY cap (--max-indexes 4), builds N=10 tenant
# indexes (> cap), then queries every tenant. Asserts:
#   1. all builds succeed (no "registry full" ERROR) — eviction frees slots,
#   2. every tenant query returns correct rows from its own index,
#   3. pg_stat_gpu_cache shows evictions>0 AND reloads>0 — the working set churns
#      and EVICTED indexes re-hydrate on query (the auto-reload fix), not a silent
#      CPU fallback.
# Eviction is driven by the registry SLOT cap (cap < tenants), not VRAM, so the
# scenario is GPU-agnostic: it reproduces deterministically under the CPU shim,
# which is why Tier-1 CI runs it every PR. Override the daemon binary with
# PG_CUVS_SERVER_BIN (default: the installed server). The production
# pg-cuvs-server unit, if present, is stopped for the run and restarted after.

set -e

DB=postgres
SOCK=/tmp/.s.pg_cuvs_maxidx
IDX=/tmp/cuvs_indexes_maxidx
BIN=${PG_CUVS_SERVER_BIN:-/usr/lib/postgresql/16/bin/pg_cuvs_server}
LOG=/tmp/pg_cuvs_maxidx_daemon.log
CAP=4
NTEN=10
DAEMON_PID=""
FAILED=0

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILED=1; }

cleanup() {
    echo "[maxidx] cleanup"
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2>/dev/null || true
    rm -f "$SOCK"; rm -rf "$IDX"
    for i in $(seq 0 $((NTEN-1))); do
        psql -d "$DB" -c "DROP TABLE IF EXISTS ten_$i;" >/dev/null 2>&1 || true
    done
    echo "[maxidx] restart production pg-cuvs-server"
    sudo systemctl start pg-cuvs-server 2>/dev/null || true
}
trap cleanup EXIT

run_sql() {
    psql -d "$DB" -tA -v ON_ERROR_STOP=1 2>&1 <<SQL | grep -v '^SET$'
SET cuvs.socket_path = '$SOCK';
SET cuvs.index_dir = '$IDX';
SET cuvs.search_mode = cagra;
SET cuvs.k = 5;
SET max_parallel_workers_per_gather = 0;
SET enable_seqscan = off;   -- tiny tables: force the GPU index so the daemon
                            -- (and its reload-on-miss) is actually exercised,
                            -- not bypassed by the cost model picking seqscan.
$1
SQL
}

echo "[maxidx] stop production daemon for the duration"
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
rm -rf "$IDX"; mkdir -p "$IDX"; chmod 0777 "$IDX"

echo "[maxidx] start daemon --max-indexes $CAP (tiny cap)"
"$BIN" --socket "$SOCK" --index-dir "$IDX" --max-vram-mb 38000 --max-indexes "$CAP" \
       >"$LOG" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 60); do
    [ -S "$SOCK" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { echo "[maxidx] daemon died:"; cat "$LOG"; exit 1; }
    sleep 0.5
done
[ -S "$SOCK" ] || { echo "[maxidx] socket never appeared:"; cat "$LOG"; exit 1; }
chmod 666 "$SOCK" 2>/dev/null || true
chmod 0777 "$IDX" 2>/dev/null || true

# Confirm the daemon honored --max-indexes (not the 1024 default).
grep -q "index registry capacity: $CAP" "$LOG" \
    && pass "0. daemon registry capacity = $CAP (--max-indexes honored)" \
    || fail "0. --max-indexes not honored";

# ---- Build N tenant indexes (> cap) -- must NOT ERROR --------------------------
echo "[maxidx] building $NTEN tenant indexes (cap=$CAP)"
BUILD_OK=1
for i in $(seq 0 $((NTEN-1))); do
    OUT=$(run_sql "
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS ten_$i;
CREATE TABLE ten_$i (id int, v vector(4));
INSERT INTO ten_$i SELECT $i*1000 + g,
       ('[' || array_to_string(array_fill(($i*0.1 + g*0.0001)::real, ARRAY[4]), ',') || ']')::vector
FROM generate_series(1, 30) g;
CREATE INDEX ten_${i}_cagra ON ten_$i USING cagra (v vector_l2_ops);
") || { echo "[maxidx] build $i FAILED: $OUT"; BUILD_OK=0; break; }
done
[ "$BUILD_OK" -eq 1 ] \
    && pass "1. built $NTEN tenant indexes with cap $CAP — no registry-full ERROR" \
    || fail "1. a tenant build ERRORed under the cap"

# ---- Query every tenant -- correct rows from its own index --------------------
WRONG=0
for i in $(seq 0 $((NTEN-1))); do
    # Query point = value (i*0.1) in all 4 dims, built in SQL (no bc dependency).
    GOT=$(run_sql "SELECT id FROM ten_$i
                   ORDER BY v <-> ('[' || array_to_string(array_fill(($i*0.1)::real, ARRAY[4]), ',') || ']')::vector(4)
                   LIMIT 1;" | tail -1)
    # The nearest id must be in tenant i's id range [i*1000, i*1000+30].
    if [ -n "$GOT" ] && [ "$GOT" -ge $((i*1000)) ] 2>/dev/null && [ "$GOT" -le $((i*1000+30)) ] 2>/dev/null; then
        :
    else
        echo "[maxidx] tenant $i wrong NN: got=[$GOT] expected in [$((i*1000)),$((i*1000+30))]"; WRONG=1
    fi
done
[ "$WRONG" -eq 0 ] \
    && pass "2. all $NTEN tenant queries returned correct rows (auto-reload of evicted indexes)" \
    || fail "2. a tenant query returned wrong/empty result"

# ---- Eviction + reload actually happened (the soft-cap proof) -----------------
EVICT=$(run_sql "SELECT COALESCE(sum(evictions),0) FROM pg_stat_gpu_cache;" | tail -1)
RELOAD=$(run_sql "SELECT COALESCE(sum(reloads),0) FROM pg_stat_gpu_cache;" | tail -1)
echo "[maxidx] evictions=$EVICT reloads=$RELOAD"
if [ "${EVICT:-0}" -gt 0 ] 2>/dev/null && [ "${RELOAD:-0}" -gt 0 ] 2>/dev/null; then
    pass "3. working set churned (evictions=$EVICT) and evicted indexes re-hydrated to GPU (reloads=$RELOAD)"
else
    fail "3. expected evictions>0 and reloads>0 (got evict=$EVICT reload=$RELOAD)"
fi

if [ "$FAILED" -eq 0 ]; then
    echo "[maxidx] ALL PASS — MAX_INDEXES is a soft LRU cap, not a hard wall"
else
    echo "[maxidx] FAILURES present"
fi
exit $FAILED
