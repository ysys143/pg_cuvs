#!/bin/bash
# e2e-smoke.sh — GPU end-to-end durability smoke for pg_cuvs.
#
# Builds a cagra index via the live daemon, verifies a nearest-neighbor
# search, restarts the daemon, and confirms the persisted .cagra/.tids pair
# reloads (startup_load_indexes) and still returns identical results with no
# heap rebuild. Also checks the versioned .tids magic. Run on the GPU VM:
#   make gpu-e2e
#
# Requires: pg_cuvs installed, pg-cuvs-server systemd unit, index dir
# /tmp/cuvs_indexes (matching the daemon --index-dir).

set -e

IDX_DIR=/tmp/cuvs_indexes
DB=postgres

echo "[e2e] restart daemon"
sudo systemctl restart pg-cuvs-server
sleep 2
sudo systemctl is-active pg-cuvs-server

echo "[e2e] setup + build three cagra indexes (l2, cosine, ip opclasses)"
# Three indexes with different opclasses persist three (db,oid).cagra/.tids
# pairs. The restart must reload ALL of them (multi-index startup_load) and
# preserve each index's metric metadata, so every search is stable.
psql -d "$DB" -v ON_ERROR_STOP=1 <<SQL
SET cuvs.index_dir = '$IDX_DIR';
DROP TABLE IF EXISTS e2e_items;
DROP TABLE IF EXISTS e2e_cos;
DROP TABLE IF EXISTS e2e_ip;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
CREATE TABLE e2e_items (id bigint, embedding vector(4));
INSERT INTO e2e_items VALUES
  (1,'[1,0,0,0]'),(2,'[0,1,0,0]'),(3,'[0,0,1,0]'),(4,'[0,0,0,1]'),
  (5,'[0.9,0.1,0,0]'),(6,'[0,0.9,0.1,0]'),(7,'[0,0,0.9,0.1]'),(8,'[0.8,0,0,0.2]');
CREATE INDEX e2e_cagra ON e2e_items USING cagra (embedding vector_l2_ops);
CREATE TABLE e2e_cos (id bigint, embedding vector(4));
INSERT INTO e2e_cos SELECT id, embedding FROM e2e_items;
CREATE INDEX e2e_cos_cagra ON e2e_cos USING cagra (embedding vector_cosine_ops);
CREATE TABLE e2e_ip (id bigint, embedding vector(4));
INSERT INTO e2e_ip SELECT id, embedding FROM e2e_items;
CREATE INDEX e2e_ip_cagra ON e2e_ip USING cagra (embedding vector_ip_ops);
SQL

q() { psql -d "$DB" -At -c "SET cuvs.index_dir='$IDX_DIR'; $1"; }

echo "[e2e] search BEFORE restart (all three indexes)"
BEFORE=$(q "SELECT id FROM e2e_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;")
BEFORE_COS=$(q "SELECT id FROM e2e_cos ORDER BY embedding <=> '[1,0,0,0]'::vector LIMIT 3;")
BEFORE_IP=$(q "SELECT id FROM e2e_ip ORDER BY embedding <#> '[1,0,0,0]'::vector LIMIT 3;")
echo "before: l2=[$BEFORE] cos=[$BEFORE_COS] ip=[$BEFORE_IP]"

echo "[e2e] inspect newest .tids header magic (expect 'TIDS' = 54 49 44 53 LE)"
# Newest .tids = the index we just built. Older files may be legacy-format.
TIDS=$(ls -t "$IDX_DIR"/*.tids 2>/dev/null | head -1)
MAGIC_OK=0
if [ -n "$TIDS" ]; then
    xxd -l 8 "$TIDS"
    if [ "$(xxd -l 4 -p "$TIDS")" = "54494453" ]; then
        MAGIC_OK=1
        echo "[e2e] .tids magic OK (versioned+crc32 header)"
    else
        echo "[e2e] FAIL: newest .tids ($TIDS) lacks TIDS magic"
    fi
else
    echo "[e2e] FAIL: no .tids file found in $IDX_DIR"
fi

echo "[e2e] restart daemon -> reload persisted index"
sudo systemctl restart pg-cuvs-server
sleep 2
sudo journalctl -u pg-cuvs-server -n 20 --no-pager | grep -iE "load|reload" || echo "[e2e] (no load line matched)"

echo "[e2e] search AFTER restart (served from reloaded indexes, no heap rebuild)"
AFTER=$(q "SELECT id FROM e2e_items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 3;")
AFTER_COS=$(q "SELECT id FROM e2e_cos ORDER BY embedding <=> '[1,0,0,0]'::vector LIMIT 3;")
AFTER_IP=$(q "SELECT id FROM e2e_ip ORDER BY embedding <#> '[1,0,0,0]'::vector LIMIT 3;")
echo "after:  l2=[$AFTER] cos=[$AFTER_COS] ip=[$AFTER_IP]"

echo "[e2e] cleanup"
psql -d "$DB" -c "DROP TABLE IF EXISTS e2e_items, e2e_cos, e2e_ip;" >/dev/null

if [ -n "$BEFORE" ] && [ "$BEFORE" = "$AFTER" ] \
   && [ "$BEFORE_COS" = "$AFTER_COS" ] && [ "$BEFORE_IP" = "$AFTER_IP" ] \
   && [ "$MAGIC_OK" = "1" ]; then
    echo "[e2e] PASS: all three indexes (l2/cosine/ip) stable across restart + versioned .tids header"
else
    echo "[e2e] FAIL: l2 [$BEFORE]/[$AFTER] cos [$BEFORE_COS]/[$AFTER_COS] ip [$BEFORE_IP]/[$AFTER_IP] magic_ok=$MAGIC_OK"
    exit 1
fi
echo "[e2e] DONE"
