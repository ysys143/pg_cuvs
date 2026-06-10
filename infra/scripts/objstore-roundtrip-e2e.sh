#!/bin/bash
# objstore-roundtrip-e2e.sh — Phase 3C real GCS snapshot round-trip + fail-closed
# certification (ADR-013). Runs on the GPU VM:
#   make gpu-test-objstore
#
# What it proves (the gap that compile + no-regression could never close):
#   1. UPLOAD     — a CREATE INDEX with cuvs.snapshot_uri set pushes
#                   index.cagra + index.tids + manifest.json to a real GCS bucket,
#                   and the manifest carries the corrected contract fields.
#   2. DOWNLOAD   — a node with the heap + .relfilenode sidecar but NO local
#                   .cagra/.tids hydrates from GCS via the warmup path (Phase 3D)
#                   and returns the SAME exact top-k as before the wipe.
#   3. FAIL-CLOSED (완료 기준 b/d) — each must REJECT and leave the index unloaded:
#        (a) corrupt artifact   -> SHA256 mismatch
#        (b) heap relfilenode    -> heap-incompat hard reject
#        (c) cuVS version stamp  -> version-compat gate (ADR-013)
#
# Uses a DEDICATED daemon (production binary) on a TEST socket + index dir with
# --snapshot-uri pointed at an EPHEMERAL bucket created at start and destroyed on
# exit (trap). The production pg-cuvs-server unit is stopped for the run and
# restarted at cleanup. Real GCS auth = the VM service-account instance-metadata
# token (same identity gcloud uses here), so no IAM plumbing.
#
# Guard order in cuvs_objstore_download(): relfilenode -> OID -> cuVS version ->
# (download) SHA256. The fail-closed cases below each target one guard.

set -e

DB=postgres
SOCK=/tmp/.s.pg_cuvs_objstore
IDX=/tmp/cuvs_indexes_objstore
BIN=/usr/lib/postgresql/16/bin/pg_cuvs_server   # production daemon (real GCS client)
LOG=/tmp/pg_cuvs_objstore_daemon.log
CLUSTER=ci-roundtrip
QUERY='[0.5,0.5,0.5,0.5,0.5,0.5,0.5,0.5]'
BUCKET="gs://pg-cuvs-snap-test-$(date +%s)-$RANDOM"
DAEMON_PID=""
FAILED=0

command -v gcloud >/dev/null || export PATH="$PATH:/snap/bin"

pass() { echo "[PASS] $1"; }
fail() { echo "[FAIL] $1"; FAILED=1; }

cleanup() {
    echo "[obj] cleanup"
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2>/dev/null || true
    rm -f "$SOCK"; rm -rf "$IDX"
    psql -d "$DB" -c "DROP TABLE IF EXISTS obj_items;" >/dev/null 2>&1 || true
    echo "[obj] destroy ephemeral bucket $BUCKET"
    gcloud storage rm --recursive "$BUCKET" >/dev/null 2>&1 || true
    echo "[obj] restart production pg-cuvs-server"
    sudo systemctl start pg-cuvs-server 2>/dev/null || true
}
trap cleanup EXIT

# ---- daemon helpers -------------------------------------------------------
start_daemon() {
    echo "[obj] start daemon (snapshot_uri=$BUCKET cluster=$CLUSTER)"
    "$BIN" --socket "$SOCK" --index-dir "$IDX" --max-vram-mb 38000 \
           --snapshot-uri "$BUCKET" --cluster-id "$CLUSTER" \
           >"$LOG" 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 60); do
        [ -S "$SOCK" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || { echo "[obj] daemon died:"; cat "$LOG"; return 1; }
        sleep 0.5
    done
    [ -S "$SOCK" ] || { echo "[obj] socket never appeared:"; cat "$LOG"; return 1; }
    chmod 666 "$SOCK" 2>/dev/null || true
    chmod 0777 "$IDX" 2>/dev/null || true
}
stop_daemon() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && wait "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=""; rm -f "$SOCK"
}

# Run SQL with the test socket/index GUCs; capture combined output into $OUT.
run_sql() {
    OUT=$(psql -d "$DB" -v ON_ERROR_STOP=1 2>&1 <<SQL
SET cuvs.socket_path = '$SOCK';
SET cuvs.index_dir = '$IDX';
$1
SQL
)
    return $?
}
# Run a single SELECT (tuples-only) and echo just the scalar value (last line).
scalar_sql() {
    psql -d "$DB" -tA -v ON_ERROR_STOP=1 2>/dev/null <<SQL | grep -v '^SET$' | tail -1
SET cuvs.socket_path = '$SOCK';
SET cuvs.index_dir = '$IDX';
SET cuvs.search_mode = brute_force;
SET cuvs.k = 10;
SET max_parallel_workers_per_gather = 0;
$1
SQL
}

# Drop the local artifacts but KEEP the .relfilenode sidecar (the cold-
# registration key) so the daemon must hydrate from GCS.
wipe_local() { rm -f "$IDX"/*.cagra "$IDX"/*.tids "$IDX"/*.vectors "$IDX"/*.shards 2>/dev/null || true; }

wait_gcs() {  # $1=glob  $2=timeout_s
    local i; for i in $(seq 1 "${2:-40}"); do gcloud storage ls "$1" >/dev/null 2>&1 && return 0; sleep 1; done; return 1
}
wait_local_cagra() {  # $1=timeout_s
    local i; for i in $(seq 1 "${1:-40}"); do ls "$IDX"/*.cagra >/dev/null 2>&1 && return 0; sleep 1; done; return 1
}

# ===========================================================================
echo "[obj] create ephemeral bucket $BUCKET"
gcloud storage buckets create "$BUCKET" --location=US >/dev/null 2>&1 \
    || { echo "[obj] FATAL: bucket create failed (SA needs storage.buckets.create)"; exit 1; }

echo "[obj] stop production daemon for the duration (frees GPU; distinct socket anyway)"
sudo systemctl stop pg-cuvs-server 2>/dev/null || true
rm -rf "$IDX"; mkdir -p "$IDX"; chmod 0777 "$IDX"

# ---- Phase 1: build -> upload --------------------------------------------
start_daemon
run_sql "
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
DROP TABLE IF EXISTS obj_items;
CREATE TABLE obj_items (id int, v vector(8));
SELECT setseed(0.31);
INSERT INTO obj_items
SELECT g, array_agg(round((random())::numeric, 5) ORDER BY d)::real[]::vector(8)
FROM generate_series(1, 300) g, generate_series(1, 8) d
GROUP BY g;
SET cuvs.search_mode = brute_force;
SET cuvs.k = 10;
SET max_parallel_workers_per_gather = 0;
CREATE INDEX obj_cagra ON obj_items USING cagra (v vector_l2_ops);
" || { echo "[obj] build SQL failed: $OUT"; exit 1; }

GROUND=$(scalar_sql "SELECT string_agg(id::text, ',' ORDER BY v <-> '$QUERY')
                     FROM (SELECT id, v FROM obj_items ORDER BY v <-> '$QUERY' LIMIT 10) s;")
echo "[obj] ground-truth top10 ids: $GROUND"

if wait_gcs "$BUCKET/**/manifest.json" 40 && \
   wait_gcs "$BUCKET/**/index.cagra" 20 && \
   wait_gcs "$BUCKET/**/index.tids" 20; then
    pass "1. upload: manifest + index.cagra + index.tids present in GCS"
else
    fail "1. upload: artifacts missing from GCS"; echo "--- daemon log ---"; tail -30 "$LOG"
fi

MJSON=$(gcloud storage cat "$(gcloud storage ls "$BUCKET/**/latest/manifest.json" 2>/dev/null | head -1)" 2>/dev/null || true)
echo "[obj] latest manifest:"; echo "$MJSON"
if echo "$MJSON" | grep -q '"cuvs_version"' && \
   ! echo "$MJSON" | grep -q '"pg_cuvs_version": "0.1.0"' && \
   ! echo "$MJSON" | grep -q '"base_generation": 0,'; then
    pass "1b. manifest: cuvs_version present, pg_cuvs_version!=0.1.0, base_generation!=0"
else
    fail "1b. manifest contract fields wrong"
fi

# ---- Phase 2: wipe local -> warmup download -> exact recall ---------------
stop_daemon
wipe_local
echo "[obj] local dir after wipe (must retain .relfilenode):"; ls -1 "$IDX"
start_daemon
if wait_local_cagra 40; then
    pass "2a. download: warmup re-materialised local .cagra from GCS"
else
    fail "2a. download: .cagra never re-materialised"; tail -30 "$LOG"
fi
GOT=$(scalar_sql "SELECT string_agg(id::text, ',' ORDER BY v <-> '$QUERY')
                  FROM (SELECT id, v FROM obj_items ORDER BY v <-> '$QUERY' LIMIT 10) s;")
if [ -n "$GROUND" ] && [ "$GOT" = "$GROUND" ]; then
    pass "2b. recall: hydrated top-10 == ground truth ($GOT)"
else
    fail "2b. recall mismatch: got=[$GOT] expected=[$GROUND]"
fi

# ---- Phase 3a: corrupt artifact -> SHA reject -----------------------------
# relfilenode + OID + version all match, so the guard falls through to SHA verify.
stop_daemon
CAGRA_OBJ=$(gcloud storage ls "$BUCKET/**/index.cagra" 2>/dev/null | grep -v latest | head -1)
printf 'CORRUPTED-NOT-A-CAGRA' | gcloud storage cp - "$CAGRA_OBJ" >/dev/null 2>&1
wipe_local
start_daemon
sleep 6
if ! ls "$IDX"/*.cagra >/dev/null 2>&1 && grep -qi "SHA256 MISMATCH" "$LOG"; then
    pass "3a. corrupt artifact: SHA256 mismatch -> rejected, no local .cagra"
else
    fail "3a. corrupt artifact NOT rejected"; grep -i "objstore\|sha256\|mismatch" "$LOG" | tail -10
fi

# ---- Phase 3b: heap relfilenode mismatch -> hard reject -------------------
# A bogus local .relfilenode disagrees with the manifest's; relfilenode is the
# FIRST guard, so it rejects before download (the GCS .cagra being corrupt from
# 3a is irrelevant — the guard fires earlier).
stop_daemon
RFN=$(ls "$IDX"/*.relfilenode 2>/dev/null | head -1)
rm -f "$RFN"; echo "999999 999999" > "$RFN"   # rm+recreate: sidecar is postgres-owned
wipe_local
start_daemon
sleep 6
if ! ls "$IDX"/*.cagra >/dev/null 2>&1 && grep -qi "HEAP COMPAT MISMATCH" "$LOG"; then
    pass "3b. relfilenode mismatch: heap-incompat hard reject, no local .cagra"
else
    fail "3b. relfilenode mismatch NOT rejected"; grep -i "objstore\|relfilenode\|compat" "$LOG" | tail -10
fi

# ---- Phase 3c: cuVS version stamp mismatch -> version gate reject ---------
# Restore a correct local .relfilenode (from the manifest) so only the cuVS
# version differs, then tamper the GCS manifest's cuvs_version.
stop_daemon
LATEST_MANI=$(gcloud storage ls "$BUCKET/**/latest/manifest.json" 2>/dev/null | head -1)
gcloud storage cat "$LATEST_MANI" 2>/dev/null > /tmp/obj_mani.json
GOOD_RFN=$(grep -o '"relfilenode": [0-9]*' /tmp/obj_mani.json | grep -o '[0-9]*')
GOOD_TOID=$(grep -o '"table_oid": [0-9]*' /tmp/obj_mani.json | grep -o '[0-9]*')
rm -f "$RFN"; echo "$GOOD_RFN $GOOD_TOID" > "$RFN"
sed 's/"cuvs_version": "[^"]*"/"cuvs_version": "99.99-BOGUS"/' /tmp/obj_mani.json > /tmp/obj_mani_bad.json
gcloud storage cp /tmp/obj_mani_bad.json "$LATEST_MANI" >/dev/null 2>&1
wipe_local
start_daemon
sleep 6
if ! ls "$IDX"/*.cagra >/dev/null 2>&1 && grep -qi "cuVS VERSION MISMATCH" "$LOG"; then
    pass "3c. cuVS version mismatch: version gate reject, no local .cagra"
else
    fail "3c. cuVS version mismatch NOT rejected"; grep -i "objstore\|version" "$LOG" | tail -10
fi

# ===========================================================================
stop_daemon
if [ "$FAILED" -eq 0 ]; then
    echo "[obj] ALL PASS — Phase 3C GCS round-trip + fail-closed certified"
else
    echo "[obj] FAILURES present"
fi
exit $FAILED
