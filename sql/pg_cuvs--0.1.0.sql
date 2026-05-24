-- pg_cuvs--0.1.0.sql
-- Loaded by CREATE EXTENSION pg_cuvs; (requires pgvector)
\echo Use "CREATE EXTENSION pg_cuvs" to load this file. \quit

-- ----------------------------------------------------------------
-- Index Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION cuvsamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'cuvsamhandler'
LANGUAGE C;

CREATE ACCESS METHOD cagra
TYPE INDEX
HANDLER cuvsamhandler;

COMMENT ON ACCESS METHOD cagra IS
  'GPU-accelerated CAGRA index for pgvector vector type (pg_cuvs)';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the cagra AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING cagra AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);

-- ----------------------------------------------------------------
-- pg_cuvs_reset_circuit(index_name text)
-- Re-enables GPU routing after circuit breaker trips (FALLBACK-04).
-- ----------------------------------------------------------------
CREATE FUNCTION pg_cuvs_reset_circuit(index_oid regclass)
RETURNS void
AS '$libdir/pg_cuvs', 'pg_cuvs_reset_circuit'
LANGUAGE C STRICT;

COMMENT ON FUNCTION pg_cuvs_reset_circuit(regclass) IS
  'Reset circuit breaker for a cagra index to re-enable GPU routing. '
  'Use after repeated GPU errors have been resolved. '
  'Example: SELECT pg_cuvs_reset_circuit(''my_schema.cagra_idx''::regclass);';
