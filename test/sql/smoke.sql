-- smoke.sql — basic load test for pg_cuvs.
-- Verifies the extension installs, the AM is registered, and the operator
-- classes exist. Does NOT execute a GPU search yet (Phase 1 in progress).

\set ON_ERROR_STOP on

CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION IF NOT EXISTS pg_cuvs;

-- Access method registered?
SELECT amname FROM pg_am WHERE amname = 'cagra';

-- Operator classes registered for our AM?
SELECT opcname FROM pg_opclass o
JOIN pg_am a ON a.oid = o.opcmethod
WHERE a.amname = 'cagra'
ORDER BY opcname;

-- enable_cuvs GUC exists?
SHOW enable_cuvs;

-- Cleanup
DROP EXTENSION pg_cuvs;
