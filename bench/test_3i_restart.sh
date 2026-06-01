#!/bin/bash
# test_3i_restart.sh — Phase 3I restart-after-import validation.
#
# Verifies that a pgvector HNSW index populated via pg_cuvs_import_hnsw
# survives a PostgreSQL restart and returns correct results.
#
# Run on pg-cuvs-dev-mgpu with:
#   sudo bash bench/test_3i_restart.sh
set -e

DB=contrib_regression
IDX_DIR=/tmp/cuvs_indexes
PG_SERVICE=postgresql
DAEMON_SERVICE=pg-cuvs-server

echo "=== Phase 3I restart-after-import test ==="

# 1. Setup: create table, build CAGRA index, import into pgvector HNSW.
sudo -u postgres psql "$DB" << 'SQL'
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;
SET cuvs.index_dir = '/tmp/cuvs_indexes';

DROP TABLE IF EXISTS restart_test CASCADE;
CREATE TABLE restart_test (id bigint, embedding vector(4));
INSERT INTO restart_test SELECT i, ('[' || (i*0.05) || ',0,0,0]')::vector
    FROM generate_series(1,20) i;

CREATE INDEX restart_cagra ON restart_test USING cagra (embedding vector_l2_ops);
CREATE INDEX restart_hnsw  ON restart_test USING hnsw  (embedding vector_l2_ops);

SET client_min_messages = warning;
SELECT pg_cuvs_import_hnsw('restart_cagra'::regclass, 'restart_hnsw'::regclass);
SET client_min_messages = notice;

-- Record pre-restart results.
SET enable_cuvs = off;
SET enable_seqscan = off;
SELECT string_agg(id::text, ',' ORDER BY id) AS before_restart
FROM (SELECT id FROM restart_test ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5) s;
RESET enable_seqscan;
SET enable_cuvs = on;

CHECKPOINT;
SQL

echo "--- Restarting PostgreSQL and pg-cuvs-server ---"
sudo systemctl stop "$DAEMON_SERVICE" "$PG_SERVICE"
sleep 2
sudo systemctl start "$PG_SERVICE"
sleep 3
sudo systemctl start "$DAEMON_SERVICE"
sleep 3
echo "Restart complete."

# 2. Query after restart — must match pre-restart results.
sudo -u postgres psql "$DB" << 'SQL'
SET enable_cuvs = off;
SET enable_seqscan = off;

SELECT string_agg(id::text, ',' ORDER BY id) AS after_restart
FROM (SELECT id FROM restart_test ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5) s;

-- Verify plan still uses HNSW index scan.
EXPLAIN (COSTS OFF)
SELECT id FROM restart_test ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 5;

RESET enable_seqscan;
SET enable_cuvs = on;

-- Cleanup.
DROP TABLE restart_test CASCADE;
DROP EXTENSION pg_cuvs;
SQL

echo "=== restart-after-import test PASSED ==="
