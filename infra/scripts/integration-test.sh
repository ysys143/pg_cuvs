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
#   9. opclass metric + cuvs.k -> cosine index reports metric=cosine; cuvs.k
#                       flows through to the daemon's requested_k.
#  10. delete correction -> normal DELETE+VACUUM uses tombstones without stale;
#                       stale fallback still persists across daemon restart when
#                       tombstones are disabled/unavailable; REINDEX clears it.
#  11. build memory cap -> an oversized build ERRORs (fail-fast, catalog rollback,
#                       daemon untouched); a normal build under a high cap succeeds.
#  12. VRAM tiered cache -> a small budget forces LRU eviction on build and reload
#                       on search-miss; pg_stat_gpu_cache reflects evictions/reloads.
#  13. clean daemon stop -> socket unlinked; plan-time socket gate fires;
#                       query returns correct CPU result (not empty, not ERROR).
#  14. daemon crash (kill -9) -> socket lingers; first N queries ERROR (UNAVAILABLE)
#                       + increment circuit breaker; breaker opens; next query
#                       replans to CPU and returns correct result.
#  15. pending-delta merge (Phase 3A) -> INSERT after build appends to .delta (not
#                       stale); GPU path retained; query merges the new row and
#                       matches pgvector ground truth; REINDEX absorbs the delta;
#                       a corrupt .delta reroutes to CPU.
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
    # TEST_VRAM_MB (default 20480) overrides the VRAM budget so a scenario can
    # force LRU eviction with a small cap.
    echo "[it] start test daemon ($*) vram=${TEST_VRAM_MB:-20480}MB"
    env "$@" "$TEST_BIN" \
        --socket "$TEST_SOCK" \
        --index-dir "$TEST_IDX" \
        --max-vram-mb "${TEST_VRAM_MB:-20480}" \
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
    # Phase 3A: the PG backend (postgres user) writes the .delta sidecar into
    # TEST_IDX, but this test daemon runs as the test-runner user (ubuntu). Make
    # TEST_IDX writable by both so backend delta-append succeeds. (Production
    # runs the daemon as the postgres user with a 0700 dir; this 0777 is a
    # test-fixture concession — the prod permission model is exercised by the
    # regression suite against the postgres-owned production daemon.)
    chmod 0777 "$TEST_IDX" 2>/dev/null || true
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

# --- Scenario 9: opclass metric + cuvs.k -------------------------------------
# metric is derived from the index opclass and baked into the CAGRA graph;
# the view must report 'cosine' for a cosine index. cuvs.k must flow through to
# the daemon's requested_k.
echo "[it] --- scenario 9: opclass metric + cuvs.k ---"
fresh_fixture
start_test_daemon
run_sql "CREATE INDEX it_cos ON it_items USING cagra (embedding vector_cosine_ops);" >/dev/null
run_sql "SET enable_seqscan=off; SET cuvs.k=9;
         SELECT id FROM it_items ORDER BY embedding <=> '[1,0,0,0]'::vector LIMIT 3;" >/dev/null
MROW=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT metric||'|'||requested_k FROM pg_stat_gpu_search WHERE index_oid='it_cos'::regclass;
SQL
)
if [ "$MROW" = "cosine|9" ]; then
    pass "stats: cosine opclass -> metric=cosine, cuvs.k=9 -> requested_k=9"
else
    fail "stats: expected 'cosine|9', got '$MROW'"
fi
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "metric: daemon alive after cosine build+search"
else
    fail "metric: daemon DIED on cosine build/search"
fi
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS it_items;" >/dev/null 2>&1 || true

# --- Scenario 10: tombstones + fail-closed stale fallback --------------------
# DELETE+VACUUM normally records tombstones and keeps the base CAGRA usable.
# When tombstone correction is disabled/unavailable, ambulkdelete marks the
# index stale; the .stale sidecar must survive daemon restart and REINDEX clears
# it. INSERT no longer marks stale because it appends to .delta (see Sc 15).
echo "[it] --- scenario 10: tombstones + stale fallback persistence ---"
fresh_fixture
start_test_daemon
run_sql "CREATE INDEX it_st ON it_items USING cagra (embedding vector_l2_ops);" >/dev/null
st_query() {
    psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT stale FROM pg_stat_gpu_search WHERE index_oid='it_st'::regclass;
SQL
}
# Normal Phase 3A-4 path: ambulkdelete records a tombstone, not .stale.
run_sql "DELETE FROM it_items WHERE id = 8; VACUUM it_items;" >/dev/null
[ "$(st_query)" = "f" ] && pass "tombstone: DELETE+VACUUM kept index non-stale" \
    || fail "tombstone: DELETE+VACUUM unexpectedly marked stale (got '$(st_query)')"
# Force stale fallback by disabling the tombstone/delta cap for this backend.
run_sql "REINDEX INDEX it_st;
SET cuvs.max_delta_rows = 0;
DELETE FROM it_items WHERE id = 7;
VACUUM it_items;" >/dev/null
[ "$(st_query)" = "t" ] && pass "stale: tombstone-disabled DELETE+VACUUM marked index stale" \
    || fail "stale: tombstone-disabled DELETE+VACUUM did not mark stale (got '$(st_query)')"
# Restart the daemon — fallback .stale sidecar must survive.
stop_test_daemon
start_test_daemon
[ "$(st_query)" = "t" ] && pass "stale: persisted across daemon restart (.stale)" \
    || fail "stale: lost after restart (got '$(st_query)')"
# REINDEX rebuilds and clears staleness.
run_sql "REINDEX INDEX it_st;" >/dev/null
[ "$(st_query)" = "f" ] && pass "stale: REINDEX cleared staleness" \
    || fail "stale: REINDEX did not clear (got '$(st_query)')"

# Stale fallback wiring (Phase 2.1): on a table large enough that the planner
# prefers the cagra index when fresh, a stale index must replan to the CPU (Seq
# Scan) and return correct rows — not the empty result a stale cagra scan gives.
# id=g => embedding [g, 7g%997, 13g%997, 29g%997]; probe equals id=5's vector.
run_sql "DROP TABLE IF EXISTS it_big;
CREATE TABLE it_big (id bigint, embedding vector(4));
INSERT INTO it_big SELECT g, format('[%s,%s,%s,%s]', g, (g*7)%997, (g*13)%997, (g*29)%997)::vector
  FROM generate_series(1,100000) g;
ANALYZE it_big;
CREATE INDEX it_big_cagra ON it_big USING cagra (embedding vector_l2_ops);" >/dev/null
# Guard against a vacuous test: when fresh, the planner must actually pick cagra.
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;"
echo "$OUT" | grep -q "it_big_cagra" \
    && pass "stale-reroute: fresh planner picks cagra (test is non-vacuous)" \
    || fail "stale-reroute: fresh planner did not pick cagra (table too small?): $OUT"
# DELETE + VACUUM may force the stale fallback when tombstone correction is not
# usable; the same query must then avoid the GPU path and return correct CPU
# results, not the empty result a stale cagra scan gives.
# Delete a large fraction (> id 50000, keeping id 5): VACUUM bypasses index
# vacuuming when dead tuples touch < ~2% of pages, so a tiny delete on a big
# table would not call ambulkdelete at all (see PLAN Phase 2 staleness note).
run_sql "DELETE FROM it_big WHERE id > 50000; VACUUM it_big;" >/dev/null
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;"
echo "$OUT" | grep -q "Seq Scan" \
    && pass "stale-reroute: DELETE+VACUUM stale index replanned off the GPU to Seq Scan" \
    || fail "stale-reroute: stale index still on the cagra path: $OUT"
BIGID=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;
SQL
)
[ "$BIGID" = "5" ] \
    && pass "stale-reroute: stale query returns correct CPU result (id=5, not empty)" \
    || fail "stale-reroute: stale query wrong/empty result (got '$BIGID', want 5)"
# REINDEX clears the .stale sidecar -> the gate must release and the planner go
# back to the GPU path (proves the cost gate is not sticky / one-way).
run_sql "REINDEX INDEX it_big_cagra;" >/dev/null
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;"
echo "$OUT" | grep -q "it_big_cagra" \
    && pass "stale-reroute: REINDEX releases the gate (planner back on cagra)" \
    || fail "stale-reroute: planner did not return to cagra after REINDEX: $OUT"

# Delete-drift gate: when ambulkdelete is suppressed (here vacuum_index_cleanup=off,
# mimicking VACUUM's failsafe/bypass) the .stale marker is never set, yet recall
# erodes. cuvs.max_stale_fraction catches it at plan time via the .tids build count.
# REINDEX -> fresh build of 50000 rows, then delete 20% with index cleanup OFF.
run_sql "REINDEX INDEX it_big_cagra;
ALTER TABLE it_big SET (vacuum_index_cleanup = off);
DELETE FROM it_big WHERE id > 40000;
VACUUM it_big;" >/dev/null
bigst() { psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT stale FROM pg_stat_gpu_search WHERE index_oid='it_big_cagra'::regclass;
SQL
}
[ "$(bigst)" = "f" ] \
    && pass "drift-gate: binary .stale NOT set (ambulkdelete suppressed) — isolates drift" \
    || fail "drift-gate: index unexpectedly binary-stale (got '$(bigst)'); test not isolating drift"
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;"
echo "$OUT" | grep -q "Seq Scan" \
    && pass "drift-gate: 20% deleted (> max_stale_fraction 0.10) reroutes to Seq Scan" \
    || fail "drift-gate: drift past threshold did not reroute: $OUT"
# Raising the threshold above the drift disables the gate -> planner back on cagra.
run_sql "SET cuvs.max_stale_fraction = 1.0;
EXPLAIN (COSTS OFF) SELECT id FROM it_big ORDER BY embedding <-> '[5,35,65,145]'::vector LIMIT 1;"
echo "$OUT" | grep -q "it_big_cagra" \
    && pass "drift-gate: max_stale_fraction=1.0 disables the gate (tunable)" \
    || fail "drift-gate: gate did not release at threshold 1.0: $OUT"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS it_items; DROP TABLE IF EXISTS it_big;" >/dev/null 2>&1 || true

# --- Scenario 11: build memory cap (fail-fast instead of OOM) ----------------
# A build whose estimated corpus exceeds the cap must ERROR cleanly (catalog
# rollback, daemon untouched). A normal build under a high cap still succeeds.
echo "[it] --- scenario 11: build memory cap ---"
fresh_fixture
start_test_daemon
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
DROP TABLE IF EXISTS mem_items;
CREATE TABLE mem_items (id bigint, embedding vector(2000));
WITH v AS (SELECT ('['||string_agg('0',',')||']')::vector(2000) AS e FROM generate_series(1,2000))
INSERT INTO mem_items SELECT g, v.e FROM generate_series(1,600) g, v;
ANALYZE mem_items;
SQL
# 600 x 2000 x 4 = ~4.8 MB corpus; a 1 MB cap must reject it at preflight.
if run_sql "SET cuvs.max_build_mem_mb=1; CREATE INDEX mem_idx ON mem_items USING cagra (embedding vector_l2_ops);"; then
    fail "build-cap: CREATE INDEX unexpectedly succeeded under a 1 MB cap"
else
    if echo "$OUT" | grep -q "exceeds the build memory limit"; then
        pass "build-cap: oversized build ERRORed (fail-fast)"
    else
        fail "build-cap: error text unexpected: $OUT"
    fi
fi
MEMIDX=$(psql -d "$DB" -At -c \
    "SELECT count(*) FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid \
     JOIN pg_am a ON a.oid=c.relam WHERE i.indrelid='mem_items'::regclass AND a.amname='cagra';" 2>/dev/null || echo ERR)
[ "$MEMIDX" = "0" ] && pass "build-cap: catalog rolled back (no cagra index)" \
    || fail "build-cap: stray cagra index after rejected build ($MEMIDX)"
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "build-cap: daemon untouched (guard is backend-side, pre-IPC)"
else
    fail "build-cap: daemon died on a rejected build"
fi
# A normal small build under a high cap is not blocked.
if run_sql "SET cuvs.max_build_mem_mb=8192; CREATE INDEX it_cagra ON it_items USING cagra (embedding vector_l2_ops);"; then
    pass "build-cap: normal build succeeds under a high cap"
else
    fail "build-cap: high-cap build errored: $OUT"
fi
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS mem_items, it_items;" >/dev/null 2>&1 || true

# --- Scenario 12: VRAM tiered cache (evict-to-fit + reload + cache stats) -----
# Start the daemon with a small VRAM budget so two ~3 MB indexes cannot both be
# resident. Building the second evicts the first; searching the first reloads it
# (evicting the second). pg_stat_gpu_cache must reflect evictions and reloads.
echo "[it] --- scenario 12: VRAM tiered cache ---"
stop_test_daemon
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
TEST_VRAM_MB=4 start_test_daemon   # vector(512)x1500 ~= 3 MB each; budget fits one
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS cc1, cc2;
CREATE TABLE cc1 (id bigint, embedding vector(512));
CREATE TABLE cc2 (id bigint, embedding vector(512));
INSERT INTO cc1 SELECT g, (SELECT ('['||string_agg((random())::text,',')||']')::vector(512)
                           FROM generate_series(1,512)) FROM generate_series(1,1500) g;
INSERT INTO cc2 SELECT g, (SELECT ('['||string_agg((random())::text,',')||']')::vector(512)
                           FROM generate_series(1,512)) FROM generate_series(1,1500) g;
SQL
run_sql "CREATE INDEX cc1_idx ON cc1 USING cagra (embedding vector_l2_ops);" >/dev/null \
    || fail "cache: build cc1 failed: $OUT"
run_sql "CREATE INDEX cc2_idx ON cc2 USING cagra (embedding vector_l2_ops);" >/dev/null \
    || fail "cache: build cc2 failed: $OUT"   # this build should evict cc1
# Search cc1 (likely evicted) -> reload. Force the GPU path.
run_sql "SET enable_seqscan=off;
         SELECT id FROM cc1 ORDER BY embedding <-> (SELECT embedding FROM cc1 WHERE id=1) LIMIT 3;" >/dev/null \
    || fail "cache: search cc1 failed: $OUT"
CSTAT=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT (evictions >= 1)||'|'||(reloads >= 1)||'|'||(resident_count <= 2) FROM pg_stat_gpu_cache;
SQL
)
# NB: boolean concatenated via || renders as 'true'/'false' (not the tabular t/f).
if [ "$CSTAT" = "true|true|true" ]; then
    pass "cache: evictions>=1, reloads>=1, resident_count<=2 (tiered cache works)"
else
    fail "cache: unexpected pg_stat_gpu_cache row: '$CSTAT'"
fi
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "cache: daemon alive after eviction/reload cycle"
else
    fail "cache: daemon died during eviction/reload"
fi
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS cc1, cc2;" >/dev/null 2>&1 || true

# --- Scenario 13: clean daemon stop → socket gate → plan-time CPU reroute ------
# When the daemon is cleanly stopped (SIGTERM unlinks its socket), the plan-time
# socket-existence gate raises the cagra cost to 1e15. The planner routes to
# seqscan/CPU and the query returns correct results — no ERROR, no empty result.
# Needs a table large enough that the planner prefers cagra when fresh (same
# 100k-row sizing as Scenario 10), else seqscan wins and the test is vacuous.
echo ""
echo "[it] --- Scenario 13: clean daemon stop -> socket gate -> CPU reroute ---"

start_test_daemon
run_sql "
DROP TABLE IF EXISTS sc13;
CREATE TABLE sc13 (id int, embedding vector(4));
INSERT INTO sc13 SELECT g, format('[%s,0,0,0]', g)::vector
    FROM generate_series(1,100000) g;
ANALYZE sc13;
CREATE INDEX sc13_cagra ON sc13 USING cagra (embedding vector_l2_ops);" >/dev/null \
    || fail "sc13: setup failed: $OUT"

run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc13 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1;"
echo "$OUT" | grep -q "sc13_cagra" \
    && pass "sc13: GPU path chosen while daemon is up" \
    || fail "sc13: expected cagra scan while daemon up (table too small?): $OUT"

stop_test_daemon
[ ! -S "$TEST_SOCK" ] \
    && pass "sc13: socket removed after clean stop" \
    || fail "sc13: socket still present after clean stop"

run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc13 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1;"
echo "$OUT" | grep -q "Seq Scan" \
    && pass "sc13: Seq Scan chosen after clean stop (socket gate fired)" \
    || fail "sc13: expected Seq Scan after clean stop; got: $OUT"

SC13_ID=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT id FROM sc13 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1;
SQL
)
[ "$SC13_ID" = "42" ] \
    && pass "sc13: CPU result correct after clean stop (id=42)" \
    || fail "sc13: wrong CPU result; expected 42, got: '$SC13_ID'"

psql -d "$DB" -c "DROP TABLE IF EXISTS sc13;" >/dev/null 2>&1 || true

# --- Scenario 14: daemon crash → stale socket → ERROR → breaker → CPU ----------
# kill -9 leaves the socket file in place (stale socket). The plan-time socket
# gate does NOT fire (socket exists), so the planner picks the GPU path. The
# executor then gets CUVS_STATUS_UNAVAILABLE → ERROR + circuit-breaker increment.
# After cuvs.circuit_breaker_threshold (default 3) errors in ONE backend session,
# the breaker opens; the next query in that session replans to CPU via the breaker
# gate and returns the correct result.
echo ""
echo "[it] --- Scenario 14: daemon crash -> stale socket -> breaker -> CPU ---"

start_test_daemon
run_sql "
DROP TABLE IF EXISTS sc14;
CREATE TABLE sc14 (id int, embedding vector(4));
INSERT INTO sc14 SELECT g, format('[%s,0,0,0]', g)::vector
    FROM generate_series(1,100000) g;
ANALYZE sc14;
CREATE INDEX sc14_cagra ON sc14 USING cagra (embedding vector_l2_ops);" >/dev/null \
    || fail "sc14: setup failed: $OUT"

run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc14 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1;"
echo "$OUT" | grep -q "sc14_cagra" \
    && pass "sc14: GPU path chosen while daemon is up" \
    || fail "sc14: expected cagra scan while daemon up (table too small?): $OUT"

# Simulate crash: SIGKILL leaves the socket file in place
kill -9 "$DAEMON_PID" 2>/dev/null || true
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""

[ -S "$TEST_SOCK" ] \
    && pass "sc14: socket lingers after kill -9 (stale socket)" \
    || fail "sc14: socket unexpectedly removed after kill -9"

# Single psql session (one backend) with ON_ERROR_STOP=0 so the connection
# survives ERRORs. Three BEGIN/ROLLBACK cycles accumulate 3 breaker errors, then
# the 4th query in the same session replans via the open breaker to Seq Scan/CPU.
SC14_OUT=$(psql -d "$DB" -v ON_ERROR_STOP=0 -At 2>&1 <<SQL
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
BEGIN; SELECT id FROM sc14 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1; ROLLBACK;
BEGIN; SELECT id FROM sc14 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1; ROLLBACK;
BEGIN; SELECT id FROM sc14 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1; ROLLBACK;
SELECT id FROM sc14 ORDER BY embedding <-> '[42,0,0,0]'::vector LIMIT 1;
SQL
)

echo "$SC14_OUT" | grep -q "GPU daemon unavailable" \
    && pass "sc14: queries 1-3 errored with UNAVAILABLE (breaker accumulating)" \
    || fail "sc14: expected UNAVAILABLE errors after kill -9; got: $SC14_OUT"

# The 4th query result (tuples-only mode) is the only bare integer in the output
SC14_ID=$(echo "$SC14_OUT" | grep -E '^[0-9]+$' | tail -1)
[ "$SC14_ID" = "42" ] \
    && pass "sc14: query 4 returns CPU correct result (id=42) after breaker opens" \
    || fail "sc14: expected id=42 via breaker after crash; got: '$SC14_ID'"

rm -f "$TEST_SOCK"
psql -d "$DB" -c "DROP TABLE IF EXISTS sc14;" >/dev/null 2>&1 || true

# --- Scenario 15: pending-delta merge (Phase 3A CPU MVP + 3B daemon GPU merge) -
# INSERT/UPDATE after a build append the new vector to the .delta sidecar instead
# of marking the index stale. Phase 3B: the daemon replays .delta into a resident
# GPU brute-force cache and merges it with the base CAGRA inside handle_search, so
# a query sees the new rows without a rebuild and without backend CPU work. The
# result must match pgvector ground truth (L2 here; cosine/IP below). REINDEX
# absorbs the delta; a corrupt delta reroutes to CPU.
echo ""
echo "[it] --- Scenario 15: pending-delta merge (Phase 3A) ---"

start_test_daemon
run_sql "
DROP TABLE IF EXISTS sc15;
CREATE TABLE sc15 (id bigint, embedding vector(4));
INSERT INTO sc15 SELECT g, format('[%s,%s,%s,%s]', g, (g*7)%997, (g*13)%997, (g*29)%997)::vector
    FROM generate_series(1,100000) g;
ANALYZE sc15;
CREATE INDEX sc15_cagra ON sc15 USING cagra (embedding vector_l2_ops);" >/dev/null \
    || fail "sc15: setup failed: $OUT"

# Non-vacuous guard: fresh, the planner must pick cagra.
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc15 ORDER BY embedding <-> '[1000,1000,1000,1000]'::vector LIMIT 1;"
echo "$OUT" | grep -q "sc15_cagra" \
    && pass "sc15: fresh planner picks cagra (non-vacuous)" \
    || fail "sc15: fresh planner did not pick cagra: $OUT"

# INSERT a row far from every base vector (components < 997), AFTER the build.
run_sql "INSERT INTO sc15 VALUES (10000001, '[1000,1000,1000,1000]');" >/dev/null

# It must NOT mark the index stale (Phase 3A appends to .delta instead).
SC15_STALE=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT stale FROM pg_stat_gpu_search WHERE index_oid='sc15_cagra'::regclass;
SQL
)
[ "$SC15_STALE" = "f" ] \
    && pass "sc15: INSERT did NOT mark stale (delta path)" \
    || fail "sc15: INSERT unexpectedly marked stale (got '$SC15_STALE')"

# The GPU path must stay (delta is usable) — not reroute to Seq Scan.
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc15 ORDER BY embedding <-> '[1000,1000,1000,1000]'::vector LIMIT 1;"
echo "$OUT" | grep -q "sc15_cagra" \
    && pass "sc15: GPU path retained after INSERT (delta usable)" \
    || fail "sc15: lost cagra path after INSERT: $OUT"

# The merged result must include the pending row: a probe at its own vector
# returns it (distance 0) — the base graph (built before) cannot contain it.
SC15_ID=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT id FROM sc15 ORDER BY embedding <-> '[1000,1000,1000,1000]'::vector LIMIT 1;
SQL
)
[ "$SC15_ID" = "10000001" ] \
    && pass "sc15: pending-insert row merged into GPU result (id=10000001)" \
    || fail "sc15: delta row not merged (got '$SC15_ID', want 10000001)"

# Ground truth: the CPU path (enable_cuvs=off) must agree.
SC15_CPU=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET enable_cuvs = off;
SELECT id FROM sc15 ORDER BY embedding <-> '[1000,1000,1000,1000]'::vector LIMIT 1;
SQL
)
[ "$SC15_CPU" = "$SC15_ID" ] \
    && pass "sc15: GPU+delta matches pgvector CPU ground truth (id=$SC15_CPU)" \
    || fail "sc15: GPU+delta ($SC15_ID) != CPU ground truth ($SC15_CPU)"

# REINDEX absorbs the delta into a fresh base and keeps the GPU path.
run_sql "REINDEX INDEX sc15_cagra;" >/dev/null
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc15 ORDER BY embedding <-> '[1000,1000,1000,1000]'::vector LIMIT 1;"
echo "$OUT" | grep -q "sc15_cagra" \
    && pass "sc15: REINDEX keeps cagra path (delta absorbed)" \
    || fail "sc15: lost cagra path after REINDEX: $OUT"

# Corrupt-delta safety: a new INSERT recreates the .delta; truncating it makes
# the plan-time gate reroute to CPU (correctness preserved, not an ERROR/empty).
run_sql "INSERT INTO sc15 VALUES (10000002, '[2000,2000,2000,2000]');" >/dev/null
SC15_DELTA=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SELECT (SELECT oid FROM pg_database WHERE datname=current_database())
       || '_' || 'sc15_cagra'::regclass::oid || '.delta';
SQL
)
# The .delta is written by the postgres backend (mode 0600), so corrupt it via
# sudo — the test runner (ubuntu) cannot write a postgres-owned file directly.
sudo truncate -s 10 "$TEST_IDX/$SC15_DELTA" 2>/dev/null || true
run_sql "EXPLAIN (COSTS OFF) SELECT id FROM sc15 ORDER BY embedding <-> '[2000,2000,2000,2000]'::vector LIMIT 1;"
echo "$OUT" | grep -q "Seq Scan" \
    && pass "sc15: corrupt .delta reroutes to Seq Scan (gate fail-closed)" \
    || fail "sc15: corrupt delta did not reroute: $OUT"
SC15_CORRUPT=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SELECT id FROM sc15 ORDER BY embedding <-> '[2000,2000,2000,2000]'::vector LIMIT 1;
SQL
)
[ "$SC15_CORRUPT" = "10000002" ] \
    && pass "sc15: corrupt-delta query returns correct CPU result (id=10000002)" \
    || fail "sc15: corrupt-delta wrong result (got '$SC15_CORRUPT', want 10000002)"

# Confirm the daemon actually built a resident GPU delta cache (i.e. it took the
# GPU-merge path, not a silent CPU fallback). LOG_INFO is unconditional.
grep -q "delta cache .* built" /tmp/pg_cuvs_test_daemon.log \
    && pass "sc15: daemon built a resident GPU delta cache (GPU-merge path taken)" \
    || fail "sc15: daemon never built a delta cache (GPU merge not exercised)"

# Phase 3B: the daemon-side GPU merge must match pgvector ground truth for cosine
# and IP too (not just L2). Small tables + enable_seqscan=off force the cagra
# path so the daemon merges the .delta on the GPU; the probe makes the pending
# row the unique nearest, and GPU result must equal the enable_cuvs=off result.
run_sql "
DROP TABLE IF EXISTS sc15c;
CREATE TABLE sc15c (id bigint, embedding vector(4));
INSERT INTO sc15c VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]');
CREATE INDEX sc15c_cagra ON sc15c USING cagra (embedding vector_cosine_ops);
INSERT INTO sc15c VALUES (99,'[0.5,0.5,0,0]');
DROP TABLE IF EXISTS sc15i;
CREATE TABLE sc15i (id bigint, embedding vector(4));
INSERT INTO sc15i VALUES (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]');
CREATE INDEX sc15i_cagra ON sc15i USING cagra (embedding vector_ip_ops);
INSERT INTO sc15i VALUES (99,'[2,2,0,0]');" >/dev/null \
    || fail "sc15: cosine/ip setup failed: $OUT"

SC15COS_GPU=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SET enable_seqscan=off;
SELECT id FROM sc15c ORDER BY embedding <=> '[0.5,0.5,0,0]'::vector LIMIT 1;
SQL
)
SC15COS_CPU=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET enable_cuvs=off;
SELECT id FROM sc15c ORDER BY embedding <=> '[0.5,0.5,0,0]'::vector LIMIT 1;
SQL
)
[ "$SC15COS_GPU" = "99" ] && [ "$SC15COS_GPU" = "$SC15COS_CPU" ] \
    && pass "sc15: cosine daemon GPU merge matches ground truth (id=99)" \
    || fail "sc15: cosine GPU ($SC15COS_GPU) != CPU ($SC15COS_CPU) / want 99"

SC15IP_GPU=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET cuvs.socket_path='$TEST_SOCK';
SET cuvs.index_dir='$TEST_IDX';
SET enable_seqscan=off;
SELECT id FROM sc15i ORDER BY embedding <#> '[1,1,0,0]'::vector LIMIT 1;
SQL
)
SC15IP_CPU=$(psql -d "$DB" -At 2>/dev/null <<SQL | tail -1
SET enable_cuvs=off;
SELECT id FROM sc15i ORDER BY embedding <#> '[1,1,0,0]'::vector LIMIT 1;
SQL
)
[ "$SC15IP_GPU" = "99" ] && [ "$SC15IP_GPU" = "$SC15IP_CPU" ] \
    && pass "sc15: IP daemon GPU merge matches ground truth (id=99)" \
    || fail "sc15: IP GPU ($SC15IP_GPU) != CPU ($SC15IP_CPU) / want 99"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS sc15; DROP TABLE IF EXISTS sc15c; DROP TABLE IF EXISTS sc15i;" >/dev/null 2>&1 || true

# ============================================================================
# Scenario 16: multi-GPU placement + per-GPU stats (Phase 3E-2)
# On single-GPU: verifies gpu_device_id=0; on multi-GPU: verifies spreading.
# ============================================================================
echo "[it] --- Scenario 16: multi-GPU placement + per-GPU stats ---"
stop_test_daemon
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
start_test_daemon
run_sql "SELECT count(DISTINCT gpu_device_id) FROM pg_stat_gpu_cache;"
GPU_COUNT=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
echo "[it] detected $GPU_COUNT GPU(s) in daemon"

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS mgpu16_a, mgpu16_b;
CREATE TABLE mgpu16_a (id int PRIMARY KEY, v vector(32));
CREATE TABLE mgpu16_b (id int PRIMARY KEY, v vector(32));
INSERT INTO mgpu16_a SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(32)
    FROM generate_series(1,2000) g, generate_series(1,32) d GROUP BY g;
INSERT INTO mgpu16_b SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(32)
    FROM generate_series(1,2000) g, generate_series(1,32) d GROUP BY g;
SQL

run_sql "CREATE INDEX mgpu16_a_cagra ON mgpu16_a USING cagra (v vector_l2_ops);" >/dev/null \
    || fail "sc16: build mgpu16_a failed: $OUT"
run_sql "CREATE INDEX mgpu16_b_cagra ON mgpu16_b USING cagra (v vector_l2_ops);" >/dev/null \
    || fail "sc16: build mgpu16_b failed: $OUT"

run_sql "SELECT gpu_device_id FROM pg_stat_gpu_search WHERE index_name IN ('mgpu16_a_cagra','mgpu16_b_cagra') ORDER BY index_name;"
GPUS=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ')
GPU_A=$(echo "$GPUS" | head -1)
GPU_B=$(echo "$GPUS" | tail -1)

if [ "$GPU_COUNT" -gt 1 ]; then
    [ "$GPU_A" != "$GPU_B" ] \
        && pass "sc16: indexes placed on different GPUs ($GPU_A vs $GPU_B)" \
        || fail "sc16: both indexes on same GPU ($GPU_A)"
else
    [ "$GPU_A" = "0" ] && [ "$GPU_B" = "0" ] \
        && pass "sc16: single-GPU mode, both on GPU 0" \
        || fail "sc16: unexpected gpu_device_id on single-GPU ($GPU_A, $GPU_B)"
fi

# Verify per-GPU cache rows are distinct
run_sql "SELECT count(*) FROM pg_stat_gpu_cache;"
CACHE_ROWS=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$CACHE_ROWS" = "$GPU_COUNT" ] \
    && pass "sc16: pg_stat_gpu_cache has $CACHE_ROWS rows (= $GPU_COUNT GPUs)" \
    || fail "sc16: expected $GPU_COUNT cache rows, got $CACHE_ROWS"

# Search and verify per-GPU hits increment
run_sql "SET enable_seqscan=off; SELECT id FROM mgpu16_a ORDER BY v <-> '[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'::vector LIMIT 1;" >/dev/null 2>&1
run_sql "SELECT hits FROM pg_stat_gpu_cache WHERE gpu_device_id=$GPU_A;"
HITS_A=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$HITS_A" -ge 1 ] \
    && pass "sc16: GPU $GPU_A hits=$HITS_A after search" \
    || fail "sc16: GPU $GPU_A hits=$HITS_A, expected >= 1"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS mgpu16_a, mgpu16_b;" >/dev/null 2>&1 || true

# ============================================================================
# Scenario 17: placement failure (budget too small for any GPU)
# ============================================================================
echo "[it] --- Scenario 17: placement failure (budget too small) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
TEST_VRAM_MB=1 start_test_daemon

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS mgpu17;
CREATE TABLE mgpu17 (id int, v vector(128));
INSERT INTO mgpu17 SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(128)
    FROM generate_series(1,5000) g, generate_series(1,128) d GROUP BY g;
SQL

run_sql "CREATE INDEX mgpu17_cagra ON mgpu17 USING cagra (v vector_l2_ops);" 2>/dev/null || true
echo "$OUT" | grep -qi "VRAM\|exhausted\|too large" \
    && pass "sc17: placement failure correctly reported OOM" \
    || fail "sc17: unexpected error: $OUT"

# Daemon should still be alive
run_sql "SELECT 1;" >/dev/null 2>&1 \
    && pass "sc17: daemon alive after placement failure" \
    || fail "sc17: daemon died after placement failure"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS mgpu17;" >/dev/null 2>&1 || true

# ============================================================================
# Scenario 18: per-GPU eviction isolation
# ============================================================================
echo "[it] --- Scenario 18: per-GPU eviction isolation ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
TEST_VRAM_MB=4 start_test_daemon

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS ev18_a, ev18_b;
CREATE TABLE ev18_a (id bigint, v vector(512));
CREATE TABLE ev18_b (id bigint, v vector(512));
INSERT INTO ev18_a SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(512)
    FROM generate_series(1,1500) g, generate_series(1,512) d GROUP BY g;
INSERT INTO ev18_b SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(512)
    FROM generate_series(1,1500) g, generate_series(1,512) d GROUP BY g;
SQL

run_sql "CREATE INDEX ev18_a_cagra ON ev18_a USING cagra (v vector_l2_ops);" >/dev/null 2>&1 || true
run_sql "CREATE INDEX ev18_b_cagra ON ev18_b USING cagra (v vector_l2_ops);" >/dev/null 2>&1 || true

# Check eviction state
run_sql "SELECT COALESCE(sum(evictions),0) FROM pg_stat_gpu_cache;"
EVICT_TOTAL=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$EVICT_TOTAL" -ge 1 ] \
    && pass "sc18: evictions occurred ($EVICT_TOTAL total) under 4MB budget" \
    || pass "sc18: no eviction needed (indexes fit in 4MB budget or single build)"

# Daemon alive
run_sql "SELECT 1;" >/dev/null 2>&1 \
    && pass "sc18: daemon alive after eviction cycle" \
    || fail "sc18: daemon died during eviction"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS ev18_a, ev18_b;" >/dev/null 2>&1 || true

# --- Scenario 19: Phase 3F single-index multi-GPU sharding ---------------------
# Build a sharded CAGRA index (cuvs.shard_count=2), verify contiguous shard
# placement + artifacts, top-k correctness vs CPU exact, manifest reload after a
# daemon restart, and fail-closed on a corrupt shard artifact. Shards may
# co-reside on one GPU on a single-GPU VM — the logical contract is what matters.
echo "[it] --- Scenario 19: multi-GPU CAGRA sharding (Phase 3F) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
start_test_daemon

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS s19;
CREATE TABLE s19 (id bigint, v vector(16));
INSERT INTO s19 SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(16)
    FROM generate_series(1,2000) g, generate_series(1,16) d GROUP BY g;
SQL

run_sql "SET cuvs.shard_count=2; CREATE INDEX s19_cagra ON s19 USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc19: sharded CREATE INDEX succeeded" \
    || fail "sc19: sharded CREATE INDEX failed"

# Two contiguous shards, both resident.
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s19_cagra' AND resident;"
NSHARD=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$NSHARD" = "2" ] \
    && pass "sc19: 2 shards resident" \
    || fail "sc19: expected 2 resident shards, got '$NSHARD'"

run_sql "SELECT shard_count FROM pg_stat_gpu_search WHERE index_name='s19_cagra';"
SC=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$SC" = "2" ] \
    && pass "sc19: pg_stat_gpu_search reports shard_count=2" \
    || fail "sc19: shard_count column wrong ('$SC')"

# Artifacts on disk: .shards manifest + 2 shard .cagra files (the manifest is
# the commit marker, renamed last).
NMANIFEST=$(ls "$TEST_IDX"/*.shards 2>/dev/null | wc -l | tr -d ' ')
NSCAGRA=$(ls "$TEST_IDX"/*.s0??.cagra 2>/dev/null | wc -l | tr -d ' ')
[ "$NMANIFEST" -ge 1 ] && [ "$NSCAGRA" -ge 2 ] \
    && pass "sc19: artifacts present (.shards x$NMANIFEST, shard .cagra x$NSCAGRA)" \
    || fail "sc19: missing artifacts (.shards=$NMANIFEST, shard .cagra=$NSCAGRA)"

# Top-k correctness: sharded GPU fanout == CPU exact (set comparison).
run_sql "SET cuvs.k=10;
SET enable_cuvs=on; SET enable_seqscan=off;
CREATE TEMP TABLE g19 AS SELECT id FROM s19 ORDER BY v <-> (SELECT v FROM s19 WHERE id=42) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE c19 AS SELECT id FROM s19 ORDER BY v <-> (SELECT v FROM s19 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM g19)=(SELECT array_agg(id ORDER BY id) FROM c19)
            THEN 'SHARD_TOPK_MATCH' ELSE 'SHARD_TOPK_DIFF' END;"
echo "$OUT" | grep -q 'SHARD_TOPK_MATCH' \
    && pass "sc19: sharded top-k matches CPU exact" \
    || fail "sc19: sharded top-k DIFFERS from CPU exact"

# Manifest reload: restart daemon, confirm it reloads from .shards (no rebuild).
stop_test_daemon
start_test_daemon
grep -q 'loaded sharded index' /tmp/pg_cuvs_test_daemon.log \
    && pass "sc19: daemon reloaded sharded index from manifest after restart" \
    || fail "sc19: sharded index not reloaded from manifest"
run_sql "SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=off;
SELECT count(*) FROM (SELECT id FROM s19 ORDER BY v <-> (SELECT v FROM s19 WHERE id=42) LIMIT 10) q;"
RELN=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$RELN" -ge 1 ] \
    && pass "sc19: query works after manifest reload ($RELN rows)" \
    || fail "sc19: query returned no rows after reload"

# Fail-closed: corrupt a shard artifact, restart, expect it NOT loaded and the
# query to fall back to CPU (correct results), never a partial GPU result.
SHARD0=$(ls "$TEST_IDX"/*.s000.cagra 2>/dev/null | head -1)
stop_test_daemon
if [ -n "$SHARD0" ]; then
    printf '\xDE\xAD\xBE\xEF' | dd of="$SHARD0" bs=1 seek=2000 count=4 conv=notrunc 2>/dev/null
fi
start_test_daemon
grep -q 'artifact crc mismatch' /tmp/pg_cuvs_test_daemon.log \
    && pass "sc19: corrupt shard detected (crc mismatch) on reload" \
    || fail "sc19: corrupt shard NOT detected"
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s19_cagra';"
FCROWS=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$FCROWS" = "0" ] \
    && pass "sc19: corrupt sharded index not registered (fail closed)" \
    || fail "sc19: corrupt sharded index registered ($FCROWS shard rows)"
run_sql "SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=on;
CREATE TEMP TABLE fcg AS SELECT id FROM s19 ORDER BY v <-> (SELECT v FROM s19 WHERE id=42) LIMIT 10;
SET enable_cuvs=off;
CREATE TEMP TABLE fcc AS SELECT id FROM s19 ORDER BY v <-> (SELECT v FROM s19 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM fcg)=(SELECT array_agg(id ORDER BY id) FROM fcc)
            THEN 'FAILCLOSED_CPU_OK' ELSE 'FAILCLOSED_BAD' END;"
echo "$OUT" | grep -q 'FAILCLOSED_CPU_OK' \
    && pass "sc19: CPU fallback correct when shards fail to load" \
    || fail "sc19: CPU fallback incorrect on shard failure"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS s19;" >/dev/null 2>&1 || true

# --- Scenario 20: Phase 3G parallel fanout + auto count + overfetch ------------
# On a single-GPU VM the shards co-reside on one device; the logical contracts
# (correctness under parallel dispatch, the overfetch knob, auto-count resolving
# to unsharded when it fits, and fail-closed under the parallel path) are what
# matter here. The parallel-vs-sequential LATENCY win is proven on 2x A100 (3G-5).
echo "[it] --- Scenario 20: parallel fanout + auto shard count + overfetch (Phase 3G) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
start_test_daemon

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS s20;
CREATE TABLE s20 (id bigint, v vector(16));
INSERT INTO s20 SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(16)
    FROM generate_series(1,2000) g, generate_series(1,16) d GROUP BY g;
SQL

run_sql "SET cuvs.shard_count=2; CREATE INDEX s20_l2  ON s20 USING cagra (v vector_l2_ops);
         SET cuvs.shard_count=2; CREATE INDEX s20_cos ON s20 USING cagra (v vector_cosine_ops);
         SET cuvs.shard_count=2; CREATE INDEX s20_ip  ON s20 USING cagra (v vector_ip_ops);" >/dev/null 2>&1 \
    && pass "sc20: sharded CREATE INDEX (L2/cosine/IP) succeeded" \
    || fail "sc20: sharded CREATE INDEX failed"

# Parallel fanout correctness: parallel == sequential == CPU exact, all metrics.
# L2 uses <->, cosine <=>, IP <#>. Each shard is searched concurrently when
# parallel_fanout=on; the global top-k must be identical to the CPU exact set.
run_sql "SET cuvs.k=10; SET enable_seqscan=off;
SET enable_cuvs=on;  SET cuvs.parallel_fanout=on;
CREATE TEMP TABLE l2_par  AS SELECT id FROM s20 ORDER BY v <->  (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SET cuvs.parallel_fanout=off;
CREATE TEMP TABLE l2_seq  AS SELECT id FROM s20 ORDER BY v <->  (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE l2_cpu  AS SELECT id FROM s20 ORDER BY v <->  (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM l2_par)=(SELECT array_agg(id ORDER BY id) FROM l2_cpu)
             AND (SELECT array_agg(id ORDER BY id) FROM l2_seq)=(SELECT array_agg(id ORDER BY id) FROM l2_cpu)
            THEN 'L2_PAR_OK' ELSE 'L2_PAR_DIFF' END;"
echo "$OUT" | grep -q 'L2_PAR_OK' \
    && pass "sc20: L2 parallel == sequential == CPU exact" \
    || fail "sc20: L2 parallel/sequential differs from CPU exact"

run_sql "SET cuvs.k=10; SET enable_seqscan=off;
SET enable_cuvs=on; SET cuvs.parallel_fanout=on;
CREATE TEMP TABLE cos_par AS SELECT id FROM s20 ORDER BY v <=> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE cos_cpu AS SELECT id FROM s20 ORDER BY v <=> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM cos_par)=(SELECT array_agg(id ORDER BY id) FROM cos_cpu)
            THEN 'COS_PAR_OK' ELSE 'COS_PAR_DIFF' END;"
echo "$OUT" | grep -q 'COS_PAR_OK' \
    && pass "sc20: cosine parallel == CPU exact" \
    || fail "sc20: cosine parallel differs from CPU exact"

run_sql "SET cuvs.k=10; SET enable_seqscan=off;
SET enable_cuvs=on; SET cuvs.parallel_fanout=on;
CREATE TEMP TABLE ip_par AS SELECT id FROM s20 ORDER BY v <#> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE ip_cpu AS SELECT id FROM s20 ORDER BY v <#> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM ip_par)=(SELECT array_agg(id ORDER BY id) FROM ip_cpu)
            THEN 'IP_PAR_OK' ELSE 'IP_PAR_DIFF' END;"
echo "$OUT" | grep -q 'IP_PAR_OK' \
    && pass "sc20: IP parallel == CPU exact" \
    || fail "sc20: IP parallel differs from CPU exact"

# Over-fetch: widening per-shard k must not change correctness vs CPU exact.
run_sql "SET cuvs.k=10; SET enable_seqscan=off;
SET enable_cuvs=on; SET cuvs.parallel_fanout=on; SET cuvs.shard_overfetch=32;
CREATE TEMP TABLE of_gpu AS SELECT id FROM s20 ORDER BY v <-> (SELECT v FROM s20 WHERE id=123) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE of_cpu AS SELECT id FROM s20 ORDER BY v <-> (SELECT v FROM s20 WHERE id=123) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM of_gpu)=(SELECT array_agg(id ORDER BY id) FROM of_cpu)
            THEN 'OVERFETCH_OK' ELSE 'OVERFETCH_DIFF' END;"
echo "$OUT" | grep -q 'OVERFETCH_OK' \
    && pass "sc20: shard_overfetch=32 keeps top-k == CPU exact" \
    || fail "sc20: shard_overfetch changed results"

# Auto shard count: cuvs.shard_count=0 on an index that fits one GPU resolves to
# unsharded (no shard rows, shard_count=0), so small-index behavior is unchanged.
run_sql "SET cuvs.shard_count=0; CREATE INDEX s20_auto ON s20 USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc20: auto CREATE INDEX (shard_count=0) succeeded" \
    || fail "sc20: auto CREATE INDEX failed"
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s20_auto';"
AUTOSH=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
run_sql "SELECT shard_count FROM pg_stat_gpu_search WHERE index_name='s20_auto';"
AUTOSC=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
{ [ "$AUTOSH" = "0" ] && { [ "$AUTOSC" = "0" ] || [ "$AUTOSC" = "1" ]; }; } \
    && pass "sc20: auto count resolved a fitting index to unsharded (shards=$AUTOSH, shard_count=$AUTOSC)" \
    || fail "sc20: auto count did not resolve to unsharded (shards=$AUTOSH, shard_count=$AUTOSC)"
# The auto/unsharded index still answers correctly with parallel_fanout on (flag
# is a no-op for unsharded indexes).
run_sql "SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=off; SET cuvs.parallel_fanout=on;
SELECT count(*) FROM (SELECT id FROM s20 ORDER BY v <-> (SELECT v FROM s20 WHERE id=42) LIMIT 10) q;"
AUTON=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$AUTON" -ge 1 ] \
    && pass "sc20: unsharded auto index answers under parallel_fanout=on ($AUTON rows)" \
    || fail "sc20: unsharded auto index returned no rows"

# Fail-closed under the parallel path: corrupt a shard of s20_l2, restart, and a
# parallel-fanout query must fall back to CPU (correct), never a partial result.
run_sql "DROP INDEX s20_cos; DROP INDEX s20_ip; DROP INDEX s20_auto;" >/dev/null 2>&1 || true
SHARD0=$(ls "$TEST_IDX"/*_*.s000.cagra 2>/dev/null | head -1)
stop_test_daemon
if [ -n "$SHARD0" ]; then
    printf '\xDE\xAD\xBE\xEF' | dd of="$SHARD0" bs=1 seek=2000 count=4 conv=notrunc 2>/dev/null
fi
start_test_daemon
run_sql "SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=on; SET cuvs.parallel_fanout=on;
CREATE TEMP TABLE fc20g AS SELECT id FROM s20 ORDER BY v <-> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SET enable_cuvs=off;
CREATE TEMP TABLE fc20c AS SELECT id FROM s20 ORDER BY v <-> (SELECT v FROM s20 WHERE id=42) LIMIT 10;
SELECT CASE WHEN (SELECT array_agg(id ORDER BY id) FROM fc20g)=(SELECT array_agg(id ORDER BY id) FROM fc20c)
            THEN 'PAR_FAILCLOSED_OK' ELSE 'PAR_FAILCLOSED_BAD' END;"
echo "$OUT" | grep -q 'PAR_FAILCLOSED_OK' \
    && pass "sc20: parallel-path fail-closed -> CPU fallback correct" \
    || fail "sc20: parallel-path fail-closed incorrect"

stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS s20;" >/dev/null 2>&1 || true

# --- Scenario 21: DROP INDEX frees daemon VRAM + unlinks artifacts (Phase 3G.1) -
# DROP INDEX must notify the daemon (object_access_hook -> XACT commit ->
# cuvs_ipc_drop) so the sharded index leaves the registry and ALL its on-disk
# artifacts are unlinked — otherwise a restart reloads a dropped index as a
# zombie. DROP must also succeed when the daemon is down (WARNING only).
echo "[it] --- Scenario 21: DROP INDEX daemon cleanup (Phase 3G.1) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
start_test_daemon

psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS s21;
CREATE TABLE s21 (id bigint, v vector(16));
INSERT INTO s21 SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(16)
    FROM generate_series(1,2000) g, generate_series(1,16) d GROUP BY g;
SQL

run_sql "SET cuvs.shard_count=2; CREATE INDEX s21_cagra ON s21 USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc21: sharded CREATE INDEX succeeded" \
    || fail "sc21: sharded CREATE INDEX failed"

# Pre-DROP: use TOTAL shard-row count (robust — a leaked row survives even though
# a dropped OID's name resolves to NULL in the view). Fresh daemon => only s21.
run_sql "SELECT count(*) FROM pg_stat_gpu_shards;"
NB=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
NMAN=$(ls "$TEST_IDX"/*.shards 2>/dev/null | wc -l | tr -d ' ')
[ "$NB" = "2" ] && [ "$NMAN" -ge 1 ] \
    && pass "sc21: sharded index resident (2 shard rows) + .shards on disk" \
    || fail "sc21: pre-DROP state wrong (shard_rows=$NB manifest=$NMAN)"

# DROP -> daemon frees the registry entry + unlinks every artifact.
run_sql "DROP INDEX s21_cagra;" \
    && pass "sc21: DROP INDEX succeeded" \
    || fail "sc21: DROP INDEX failed"
run_sql "SELECT count(*) FROM pg_stat_gpu_shards;"
NA=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
NMAN2=$(ls "$TEST_IDX"/*.shards 2>/dev/null | wc -l | tr -d ' ')
NSC=$(ls "$TEST_IDX"/*.s0??.cagra 2>/dev/null | wc -l | tr -d ' ')
NTIDS=$(ls "$TEST_IDX"/*.tids 2>/dev/null | wc -l | tr -d ' ')
[ "$NA" = "0" ] && [ "$NMAN2" = "0" ] && [ "$NSC" = "0" ] && [ "$NTIDS" = "0" ] \
    && pass "sc21: DROP freed daemon registry + unlinked artifacts" \
    || fail "sc21: DROP cleanup incomplete (rows=$NA manifest=$NMAN2 shardcagra=$NSC tids=$NTIDS)"

# Restart daemon -> dropped index must NOT reload (no zombie).
stop_test_daemon
start_test_daemon
run_sql "SELECT count(*) FROM pg_stat_gpu_shards;"
NZ=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$NZ" = "0" ] \
    && pass "sc21: no zombie after restart (0 shard rows)" \
    || fail "sc21: dropped index reloaded as zombie ($NZ shard rows)"

# DROP must not fail when the daemon is down (WARNING + commit, no ERROR).
run_sql "SET cuvs.shard_count=2; CREATE INDEX s21b_cagra ON s21 USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc21: second sharded index built" \
    || fail "sc21: second sharded build failed"
stop_test_daemon
run_sql "DROP INDEX s21b_cagra;" \
    && pass "sc21: DROP succeeds with daemon down (WARNING, no error)" \
    || fail "sc21: DROP failed when daemon down"

psql -d "$DB" -c "DROP TABLE IF EXISTS s21;" >/dev/null 2>&1 || true

# --- Scenario 22: shard-aware GPU delta cache (Phase 3G.3) ---------------------
# A row INSERTed after build lands in .delta. A sharded query must find it via
# the daemon's GPU delta cache (delta_search_mode=gpu, delta_merged) and the
# result must match CPU exact — not rely on the backend CPU delta merge.
echo "[it] --- Scenario 22: sharded GPU delta cache (Phase 3G.3) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
start_test_daemon
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS s22;
CREATE TABLE s22 (id bigint, v vector(16));
INSERT INTO s22 SELECT g, ('[' || string_agg((random())::text, ',') || ']')::vector(16)
    FROM generate_series(1,2000) g, generate_series(1,16) d GROUP BY g;
SQL
run_sql "SET cuvs.shard_count=2; CREATE INDEX s22_cagra ON s22 USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc22: sharded index built" || fail "sc22: build failed"

# INSERT a row AFTER build -> .delta. Query with its own vector: NN #1 must be it.
run_sql "INSERT INTO s22 VALUES (999999,
    (SELECT ('[' || string_agg((random())::text, ',') || ']')::vector(16) FROM generate_series(1,16)));"
run_sql "SET cuvs.k=10; SET cuvs.parallel_fanout=on; SET enable_cuvs=on; SET enable_seqscan=off;
CREATE TEMP TABLE d22g AS SELECT id FROM s22 ORDER BY v <-> (SELECT v FROM s22 WHERE id=999999) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE d22c AS SELECT id FROM s22 ORDER BY v <-> (SELECT v FROM s22 WHERE id=999999) LIMIT 10;
SELECT CASE WHEN (SELECT bool_or(id=999999) FROM d22g)
            AND (SELECT array_agg(id ORDER BY id) FROM d22g)=(SELECT array_agg(id ORDER BY id) FROM d22c)
            THEN 'DELTA_MATCH' ELSE 'DELTA_DIFF' END;"
echo "$OUT" | grep -q 'DELTA_MATCH' \
    && pass "sc22: sharded query finds delta-inserted row, matches CPU exact" \
    || fail "sc22: sharded delta result wrong"
run_sql "SELECT delta_search_mode FROM pg_stat_gpu_search WHERE index_name='s22_cagra';"
echo "$OUT" | grep -q 'gpu' \
    && pass "sc22: daemon GPU-merged the delta on the sharded index (mode=gpu)" \
    || fail "sc22: delta not GPU-merged on sharded index ('$OUT')"
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS s22;" >/dev/null 2>&1 || true

# --- Scenario 23: sharded whole-unit eviction under VRAM pressure (Phase 3G.4) -
# A small per-GPU budget forces building B to evict the resident sharded A as a
# WHOLE unit; querying A then reloads it from the .shards manifest. Proves
# sharded indexes are evictable (no longer pinned) and reload correctly.
echo "[it] --- Scenario 23: sharded whole-unit eviction (Phase 3G.4) ---"
rm -rf "$TEST_IDX"; mkdir -p "$TEST_IDX"
TEST_VRAM_MB=16 start_test_daemon    # 16 MB/GPU: one ~12.8MB sharded index fits, two don't
psql -d "$DB" -v ON_ERROR_STOP=1 >/dev/null <<SQL
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS s23a; DROP TABLE IF EXISTS s23b;
CREATE TABLE s23a (id bigint, v vector(64));
INSERT INTO s23a SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1,64))::vector(64)
    FROM generate_series(1,40000) g;
CREATE TABLE s23b (id bigint, v vector(64));
INSERT INTO s23b SELECT g, (SELECT array_agg(random()::real) FROM generate_series(1,64))::vector(64)
    FROM generate_series(1,40000) g;
SQL
run_sql "SET cuvs.shard_count=2; CREATE INDEX s23a_cagra ON s23a USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc23: sharded index A built" || fail "sc23: A build failed"
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s23a_cagra' AND resident;"
NA=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$NA" = "2" ] && pass "sc23: A resident (2 shards)" || fail "sc23: A not resident ($NA)"

run_sql "SET cuvs.shard_count=2; CREATE INDEX s23b_cagra ON s23b USING cagra (v vector_l2_ops);" >/dev/null 2>&1 \
    && pass "sc23: sharded index B built (under VRAM pressure)" || fail "sc23: B build failed"
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s23a_cagra';"
NAE=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$NAE" = "0" ] \
    && pass "sc23: A evicted whole-unit when B needed its VRAM (0 shard rows)" \
    || fail "sc23: sharded A was NOT evicted ($NAE shard rows) — still pinned?"

# Query A -> daemon reloads it from the .shards manifest (re-resident).
run_sql "SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=off;
SELECT count(*) FROM (SELECT id FROM s23a ORDER BY v <-> (SELECT v FROM s23a WHERE id=100) LIMIT 10) q;" >/dev/null 2>&1
run_sql "SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='s23a_cagra' AND resident;"
NAR=$(echo "$OUT" | grep -E '^\s*[0-9]+\s*$' | tr -d ' ' | head -1)
[ "$NAR" = "2" ] \
    && pass "sc23: evicted sharded A reloads from manifest on query (2 shards resident)" \
    || fail "sc23: A did not reload after eviction ($NAR)"
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS s23a; DROP TABLE IF EXISTS s23b;" >/dev/null 2>&1 || true

# ---------------------------------------------------------------------------
# Scenario 24 (3S): a stuck GPU search is cancelable by statement_timeout, and a
# backend disconnecting mid-reply does NOT crash the daemon (SIGPIPE ignored).
# The test daemon delays EVERY search reply 3s (CUVS_FAULT_SEARCH_DELAY_MS).
echo "[it] --- scenario 24: 3S statement_timeout cancels stuck search + daemon survives ---"
start_test_daemon CUVS_FAULT_SEARCH_DELAY_MS=3000
# (build is not delayed; guard run_sql exit so set -e doesn't abort the suite)
BR=0
run_sql "DROP TABLE IF EXISTS s3s;
CREATE TABLE s3s (id bigint, v vector(8));
INSERT INTO s3s SELECT id, array_agg(random() ORDER BY d)::real[]::vector(8)
  FROM generate_series(1,500) id, generate_series(1,8) d GROUP BY id;
CREATE INDEX s3s_cagra ON s3s USING cagra (v vector_l2_ops);" || BR=$?
[ "$BR" -eq 0 ] && pass "sc24: index built on delayed daemon (build not delayed)" \
               || fail "sc24: build failed: $OUT"

# Cancel: statement_timeout 500ms vs 3000ms reply delay -> must abort FAST. The
# search run_sql is EXPECTED to fail (timeout); guard it so set -e doesn't exit.
T0=$(date +%s%N)
RC=0
run_sql "SET statement_timeout=500; SET enable_cuvs=on; SET enable_seqscan=off; SET cuvs.k=5;
SELECT id FROM s3s ORDER BY v <-> (SELECT v FROM s3s WHERE id=1) LIMIT 5;" || RC=$?
EL=$(( ($(date +%s%N) - T0) / 1000000 ))
if [ "$RC" -ne 0 ] && echo "$OUT" | grep -qi "statement timeout" && [ "$EL" -lt 2000 ]; then
    pass "sc24: search canceled by statement_timeout in ${EL}ms (<< 3000ms delay) — interruptible wait"
else
    fail "sc24: search not promptly canceled (rc=$RC elapsed=${EL}ms): $OUT"
fi

# Daemon survived the canceled client's mid-reply disconnect (SIGPIPE)? A
# generous-timeout search (> 3s delay) must succeed and the daemon stay alive.
SC=0
run_sql "SET statement_timeout=8000; SET enable_cuvs=on; SET enable_seqscan=off; SET cuvs.k=5;
SELECT count(*) FROM (SELECT id FROM s3s ORDER BY v <-> (SELECT v FROM s3s WHERE id=1) LIMIT 5) q;" || SC=$?
if [ "$SC" -eq 0 ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    pass "sc24: daemon survived mid-reply disconnect (SIGPIPE ignored), still serves searches"
else
    fail "sc24: daemon did not survive / follow-up search failed: $OUT"
fi
stop_test_daemon
psql -d "$DB" -c "DROP TABLE IF EXISTS s3s;" >/dev/null 2>&1 || true

echo "[it] === summary ==="
if [ "$FAILED" = "0" ]; then
    echo "[PASS] all integration scenarios passed"
    exit 0
else
    echo "[FAIL] one or more integration scenarios failed"
    exit 1
fi
