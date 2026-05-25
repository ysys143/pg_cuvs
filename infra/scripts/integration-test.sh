#!/bin/bash
# integration-test.sh — fault-injection integration tests for pg_cuvs.
#
# Drives the daemon durability contract end to end against a SEPARATE test
# daemon built with -DCUVS_TEST_HOOKS, on a TEST socket + TEST index dir so
# the production pg-cuvs-server unit and its data are never touched. Runs on
# the GPU VM:
#   make gpu-test-daemon
#
# Scenarios:
#   1. daemon DOWN   -> CREATE INDEX USING cagra ERRORs (UNAVAILABLE),
#                       catalog rolls back, CPU-fallback search still works.
#   2. SERIALIZE fault -> CREATE INDEX ERRORs (PERSIST), catalog rolls back,
#                       no stray .cagra/.tids left in the test index dir.
#   3. TIDS_WRITE fault -> same contract as #2.
#   4. clean run     -> CREATE INDEX succeeds, NN search ordered correctly.
#   5. registry-full eviction-save -> the 65th CREATE INDEX ERRORs (PERSIST):
#                       registry is full (MAX_INDEXES=64 resident), evict_lru's
#                       save_index is forced to fail (CUVS_FAULT_SAVE_INDEX),
#                       so no slot frees and the build rolls back its persist.
#   6. dim-mismatch search -> clear error, daemon stays alive, valid search OK.
#   7. single-row build  -> clean error (n_vecs<2), daemon stays alive.
#   8. pg_stat_gpu_search -> search_count increments; daemon-down view is empty
#                       (never errors) and the backend survives.
#
# Requires: pg_cuvs installed; the production pg-cuvs-server systemd unit
# (stopped during the run, restarted at cleanup); CONDA_ENV exported so the
# test daemon can be compiled with `make server-test`.

set -e

REPO=~/pg_cuvs
DB=postgres
TEST_SOCK=/tmp/.s.pg_cuvs_test
TEST_IDX=/tmp/cuvs_indexes_test
TEST_BIN="$REPO/pg_cuvs_server_test"
DAEMON_PID=""
FAILED=0

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILED=1; }

# Run a SQL snippet with the test socket + index dir GUCs set. Returns psql
# exit status. Captures stderr+stdout into the named global OUT for asserts.
run_sql() {
    OUT=$(psql -d "$DB" -v ON_ERROR_STOP=1 2>&1 <<SQL
SET cuvs.socket_path = '$TEST_SOCK';
SET cuvs.index_dir = '$TEST_IDX';
$1
SQL
)
    return $?
}

start_test_daemon() {
    # Extra env (fault vars) passed as "VAR=1" arguments before the binary.
    echo "[it] start test daemon ($*)"
    env "$@" "$TEST_BIN" \
        --socket "$TEST_SOCK" \
        --index-dir "$TEST_IDX" \
        --max-vram-mb 20480 \
        >/tmp/pg_cuvs_test_daemon.log 2>&1 &
    DAEMON_PID=$!
    # Wait for readiness by polling for the UDS, which the daemon binds only
    # AFTER GPU init + warm-up. A fixed sleep races the warm-up (which now
    # includes a CAGRA build), causing the first request to hit UNAVAILABLE.
    for _ in $(seq 1 60); do
        [ -S "$TEST_SOCK" ] && break
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            echo "[it] test daemon died during startup; log:"
            cat /tmp/pg_cuvs_test_daemon.log || true
            return 1
        fi
        sleep 0.5
    done
    if [ ! -S "$TEST_SOCK" ]; then
        echo "[it] test daemon socket never appeared; log:"
        cat /tmp/pg_cuvs_test_daemon.log || true
        return 1
    fi
}

stop_test_daemon() {
    if [ -n "$DAEMON_PID" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "[it] stop test daemon (pid $DAEMON_PID)"
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    DAEMON_PID=""
    rm -f "$TEST_SOCK"
}

cleanup() {
    echo "[it] cleanup"
    stop_test_daemon
    rm -rf "$TEST_IDX"
    psql -d "$DB" -c "DROP TABLE IF EXISTS it_items;" >/dev/null 2>&1 || true
    psql -d "$DB" -c "DROP TABLE IF EXISTS rf_items;" >/dev/null 2>&1 || true
    echo "[it] restart production pg-cuvs-server"
    sudo systemctl start pg-cuvs-server 2>/dev/null || true
}
trap cleanup EXIT

# Number of cagra index rows for it_items in the catalog (expect 0 on rollback).
count_cagra_index() {
    psql -d "$DB" -At -c \
        "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid \
         JOIN pg_am a ON a.oid = c.relam \
         WHERE i.indrelid = 'it_items'::regclass AND a.amname = 'cagra';" \
        2>/dev/null || echo "ERR"
}

# Reset table + clean test index dir before each scenario.
fresh_fixture() {
    rm -rf "$TEST_IDX"
    mkdir -p "$TEST_IDX"
    psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS it_items;
CREATE TABLE it_items (id bigint, embedding vector(4));
INSERT INTO it_items VALUES
  (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
  (5,'[0.9,0.1,0,0]'),(6,'[0,0.9,0.1,0]'),(7,'[0,0,0.9,0.1]'),(8,'[0.8,0,0,0.2]');
SQL
}

# Assert no stray persisted index artifacts remain in the test index dir.
no_stray_artifacts() {
    local n
    n=$(find "$TEST_IDX" -maxdepth 1 \( -name '*.cagra' -o -name '*.tids' \) 2>/dev/null | wc -l | tr -d ' ')
    [ "$n" = "0" ]
}

echo "[it] === pg_cuvs fault-injection integration tests ==="

echo "[it] build CUVS_TEST_HOOKS daemon"
( cd "$REPO" && make server-test )
test -x "$TEST_BIN" || { echo "[it] FAIL: $TEST_BIN not built"; exit 1; }

echo "[it] stop production pg-cuvs-server (will be restarted at cleanup)"
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
rm -f "$TEST_SOCK"
sleep 1

# --- Scenario 1: daemon DOWN --------------------------------------------
echo "[it] --- scenario 1: daemon DOWN ---"
fresh_fixture
# No test daemon started; socket_path points at a dead socket.
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "daemon-down: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "daemon-down: CREATE INDEX ERRORed (status reported)"
    else
        fail "daemon-down: CREATE INDEX failed but error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "daemon-down: catalog rolled back (no cagra pg_index row)"
else
    fail "daemon-down: stray cagra index in pg_index"
fi
# CPU fallback search still works with no cagra index present.
if run_sql "SET enable_cuvs = off; SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;"; then
    if echo "$OUT" | grep -q "^[[:space:]]*1$"; then
        pass "daemon-down: CPU-fallback search returned rows (nearest id=1)"
    else
        fail "daemon-down: CPU-fallback search output unexpected: $OUT"
    fi
else
    fail "daemon-down: CPU-fallback search errored: $OUT"
fi

# --- Scenario 2: SERIALIZE fault ----------------------------------------
echo "[it] --- scenario 2: CUVS_FAULT_SERIALIZE ---"
fresh_fixture
start_test_daemon CUVS_FAULT_SERIALIZE=1
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "serialize-fault: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "serialize-fault: CREATE INDEX ERRORed (persist failure)"
    else
        fail "serialize-fault: error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "serialize-fault: catalog rolled back (no cagra pg_index row)"
else
    fail "serialize-fault: stray cagra index in pg_index"
fi
if no_stray_artifacts; then
    pass "serialize-fault: no stray .cagra/.tids in test index dir"
else
    fail "serialize-fault: stray artifacts left: $(ls -1 "$TEST_IDX")"
fi
stop_test_daemon

# --- Scenario 3: TIDS_WRITE fault ---------------------------------------
echo "[it] --- scenario 3: CUVS_FAULT_TIDS_WRITE ---"
fresh_fixture
start_test_daemon CUVS_FAULT_TIDS_WRITE=1
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "tids-fault: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "tids-fault: CREATE INDEX ERRORed (persist failure)"
    else
        fail "tids-fault: error text unexpected: $OUT"
    fi
fi
if [ "$(count_cagra_index)" = "0" ]; then
    pass "tids-fault: catalog rolled back (no cagra pg_index row)"
else
    fail "tids-fault: stray cagra index in pg_index"
fi
if no_stray_artifacts; then
    pass "tids-fault: no stray .cagra/.tids in test index dir"
else
    fail "tids-fault: stray artifacts left: $(ls -1 "$TEST_IDX")"
fi
stop_test_daemon

# --- Scenario 4: clean run ----------------------------------------------
echo "[it] --- scenario 4: clean build + search ---"
fresh_fixture
start_test_daemon
if run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    pass "clean: CREATE INDEX succeeded"
else
    fail "clean: CREATE INDEX errored: $OUT"
fi
if [ "$(count_cagra_index)" = "1" ]; then
    pass "clean: cagra index present in pg_index"
else
    fail "clean: expected 1 cagra index, found $(count_cagra_index)"
fi
if run_sql "SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;"; then
    # Nearest to [1,0,0,0]: id 1 ([1,0,0,0]) then 5 ([0.9,0.1,0,0]).
    NN=$(echo "$OUT" | grep -E "^[[:space:]]*[0-9]+$" | head -1 | tr -d ' ')
    if [ "$NN" = "1" ]; then
        pass "clean: GPU search returned nearest id=1"
    else
        fail "clean: nearest neighbor was '$NN', expected 1 (out: $OUT)"
    fi
else
    fail "clean: search errored: $OUT"
fi
stop_test_daemon

# --- Scenario 5: registry-full eviction-save -> PERSIST_FAILED ----------
# Fill the registry to MAX_INDEXES=64 with a clean daemon, then RESTART the
# daemon with CUVS_FAULT_SAVE_INDEX=1 so it reloads the 64 persisted indexes
# (registry full again). The 65th CREATE INDEX persists, hits registry-full,
# calls evict_lru -> save_index -> forced fail -> slot not freed -> the build
# rolls back its own persist and returns PERSIST_FAILED.
echo "[it] --- scenario 5: registry-full eviction-save -> PERSIST_FAILED ---"
MAXI=64

# Fresh test index dir + table dedicated to this scenario (64 tiny indexes on
# one column => 64 distinct index OIDs => 64 registry slots; VRAM negligible so
# only the registry-full path can fire, never ensure_vram eviction).
rm -rf "$TEST_IDX"
mkdir -p "$TEST_IDX"
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS rf_items;
CREATE TABLE rf_items (id bigint, embedding vector(4));
INSERT INTO rf_items VALUES
  (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]');
SQL

# Count *.cagra pairs persisted in the test index dir (one per resident index).
count_cagra_files() {
    find "$TEST_IDX" -maxdepth 1 -name '*.cagra' 2>/dev/null | wc -l | tr -d ' '
}
# Count rf_idx_* cagra indexes in the catalog.
count_rf_catalog() {
    psql -d "$DB" -At -c \
        "SELECT count(*) FROM pg_class c JOIN pg_am a ON a.oid = c.relam \
         WHERE a.amname = 'cagra' AND c.relname LIKE 'rf_idx_%';" \
        2>/dev/null || echo "ERR"
}
# True if a named index exists in the catalog.
rf_index_exists() {
    local n
    n=$(psql -d "$DB" -At -c \
        "SELECT count(*) FROM pg_class WHERE relname = '$1';" 2>/dev/null || echo 0)
    [ "$n" = "1" ]
}

# (a) Clean daemon, fill the registry to MAX_INDEXES.
start_test_daemon
rf_build_ok=1
for i in $(seq 1 "$MAXI"); do
    if ! run_sql "CREATE INDEX rf_idx_$i ON rf_items USING cagra (embedding vector_l2_ops);"; then
        echo "[it] registry-full: CREATE INDEX rf_idx_$i errored unexpectedly: $OUT"
        rf_build_ok=0
        break
    fi
done
if [ "$rf_build_ok" = "1" ] && [ "$(count_rf_catalog)" = "$MAXI" ]; then
    pass "registry-full: built $MAXI cagra indexes (catalog)"
else
    fail "registry-full: expected $MAXI cagra indexes, found $(count_rf_catalog)"
fi
# Durability: each succeeded CREATE INDEX persisted a .cagra so the restart can
# reload it. Require >= MAXI persisted so the reload refills the registry.
n_persist=$(count_cagra_files)
if [ "$n_persist" -ge "$MAXI" ]; then
    pass "registry-full: $n_persist .cagra files persisted (>= $MAXI, restart will refill)"
else
    fail "registry-full: only $n_persist .cagra files persisted, need >= $MAXI"
fi
stop_test_daemon

# (b) Restart with the fault set. The daemon reloads the persisted indexes,
# refilling the registry to (at least) MAXI. getenv is read at process start,
# so the fault MUST be injected at daemon startup -> restart-with-env required.
start_test_daemon CUVS_FAULT_SAVE_INDEX=1
n_reloaded=$(count_cagra_files)
if [ "$n_reloaded" -ge "$MAXI" ]; then
    pass "registry-full: $n_reloaded indexes resident after fault restart (>= $MAXI)"
else
    fail "registry-full: only $n_reloaded resident after restart, need >= $MAXI"
fi

# (c) The 65th build: persists, registry full, evict_lru -> save_index forced
# fail -> no slot freed -> rollback -> PERSIST_FAILED.
if run_sql "CREATE INDEX rf_idx_65 ON rf_items USING cagra (embedding vector_l2_ops);"; then
    fail "registry-full: 65th CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status 6"; then
        pass "registry-full: 65th CREATE INDEX ERRORed with status 6 (PERSIST)"
    elif echo "$OUT" | grep -q "BUILD failed (status"; then
        fail "registry-full: 65th errored but not status 6 (persist): $OUT"
    else
        fail "registry-full: 65th error text unexpected: $OUT"
    fi
fi
# Catalog rolled back: rf_idx_65 must be absent.
if rf_index_exists rf_idx_65; then
    fail "registry-full: rf_idx_65 left in catalog (rollback failed)"
else
    pass "registry-full: catalog rolled back (rf_idx_65 absent from pg_index)"
fi
# Rollback unlinked the 65th's just-persisted artifacts: no rf_idx_65 file pair.
# (Files are named by OID, so assert the resident count did not grow past the
# reloaded set — the 65th left nothing behind.)
n_after=$(count_cagra_files)
if [ "$n_after" -le "$n_reloaded" ]; then
    pass "registry-full: 65th left no extra .cagra/.tids ($n_after <= $n_reloaded)"
else
    fail "registry-full: stray artifacts after rollback ($n_after > $n_reloaded)"
fi
stop_test_daemon

# Cleanup scenario 5: drop the table and purge the 64+ test index files so they
# do not accumulate. The EXIT trap still restores the production daemon.
psql -d "$DB" -c "DROP TABLE IF EXISTS rf_items;" >/dev/null 2>&1 || true
rm -rf "$TEST_IDX"

# --- Scenario 6: dim-mismatch search must NOT crash the daemon ----------
# A query whose dimension differs from the index used to ABORT the daemon
# (cuVS RAFT failure -> sticky CUDA error -> SIGABRT), taking down every
# backend. The daemon now rejects it before cuVS. Assert: clear error AND the
# daemon is still alive afterwards.
echo "[it] --- scenario 6: dim-mismatch search keeps daemon alive ---"
fresh_fixture
start_test_daemon
run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);" || \
    fail "dim-mismatch: setup CREATE INDEX errored: $OUT"
# Force the cagra path so the daemon (not pgvector seqscan) handles the query.
run_sql "SET enable_seqscan = off; SELECT id FROM it_items ORDER BY embedding <-> '[1,2,3]'::vector LIMIT 3;" || true
if echo "$OUT" | grep -qi "does not match"; then
    pass "dim-mismatch: query ERRORed with a clear dimension message"
else
    fail "dim-mismatch: expected a dimension-mismatch error, got: $OUT"
fi
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "dim-mismatch: daemon still alive after the bad query"
else
    fail "dim-mismatch: daemon DIED on a dim-mismatch query (crash regression)"
fi
if run_sql "SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;"; then
    pass "dim-mismatch: a valid search still works afterwards"
else
    fail "dim-mismatch: valid search failed after the bad one: $OUT"
fi
stop_test_daemon

# --- Scenario 7: single-row build -> clean error, daemon survives -------
# CAGRA cannot build a graph from one point; cuVS used to ABORT the daemon.
# The daemon now rejects n_vecs < 2 cleanly. Assert: clean error AND survival.
echo "[it] --- scenario 7: single-row build keeps daemon alive ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS it_one;
CREATE TABLE it_one (id bigint, embedding vector(4));
INSERT INTO it_one VALUES (1, '[1,0,0,0]');
SQL
start_test_daemon
if run_sql "CREATE INDEX it_one_cagra ON it_one USING cagra (embedding vector_l2_ops);"; then
    fail "single-row: CREATE INDEX unexpectedly succeeded"
else
    if echo "$OUT" | grep -q "BUILD failed (status"; then
        pass "single-row: CREATE INDEX ERRORed cleanly"
    else
        fail "single-row: error text unexpected: $OUT"
    fi
fi
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "single-row: daemon still alive after the n=1 build"
else
    fail "single-row: daemon DIED on a single-row build (crash regression)"
fi
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS it_one;" >/dev/null 2>&1 || true

# --- Scenario 8: pg_stat_gpu_search observability ------------------------
# The view is daemon-backed and cross-backend. After GPU searches, the row
# must show search_count >= 1; when the daemon is down the view must return
# ZERO rows (never error) and leave the backend alive.
echo "[it] --- scenario 8: pg_stat_gpu_search view ---"
fresh_fixture
start_test_daemon
if ! run_sql "CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    fail "stats: setup CREATE INDEX errored: $OUT"
fi
# Force the cagra path (tiny table would otherwise seqscan) and run a few searches.
run_sql "SET enable_seqscan=off;
         SELECT id FROM it_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;
         SELECT id FROM it_items ORDER BY embedding <-> '[0,1,0,0]'::vector LIMIT 3;
         SELECT id FROM it_items ORDER BY embedding <-> '[0,0,1,0]'::vector LIMIT 3;" >/dev/null

# search_count for the index (last line = the value under psql -At).
SC=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT coalesce(max(search_count),0) FROM pg_stat_gpu_search WHERE index_oid='it_cagra'::regclass;
SQL
)
if [ -n "$SC" ] && [ "$SC" -ge 1 ] 2>/dev/null; then
    pass "stats: search_count incremented to $SC after GPU searches"
else
    fail "stats: search_count did not increment (got '$SC')"
fi

# The view exposes the index row with a resolvable name + percentile columns.
ROW=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT index_name||'|'||metric||'|'||(p99_latency_us >= p50_latency_us)
  FROM pg_stat_gpu_search WHERE index_oid='it_cagra'::regclass;
SQL
)
# NB: boolean concatenated via || renders as 'true'/'false' (not the tabular t/f).
if echo "$ROW" | grep -q "^it_cagra|l2|true$"; then
    pass "stats: view row has index_name=it_cagra, metric=l2, p99>=p50"
else
    fail "stats: unexpected view row: '$ROW'"
fi

# Daemon down -> view returns zero rows, no error, backend survives.
stop_test_daemon
if run_sql "SELECT count(*) AS n FROM pg_stat_gpu_search;"; then
    if echo "$OUT" | grep -qE "^[[:space:]]*0$"; then
        pass "stats: daemon-down view returned empty set (no error)"
    else
        fail "stats: daemon-down view non-empty/unexpected: $OUT"
    fi
else
    fail "stats: daemon-down view query ERRORed (should be empty, not error): $OUT"
fi
psql -d "$DB" -c "DROP TABLE IF EXISTS it_items;" >/dev/null 2>&1 || true

echo "[it] === summary ==="
if [ "$FAILED" = "0" ]; then
    echo "[PASS] all integration scenarios passed"
    exit 0
else
    echo "[FAIL] one or more integration scenarios failed"
    exit 1
fi
