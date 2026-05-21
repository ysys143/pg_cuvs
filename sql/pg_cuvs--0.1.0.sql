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
-- Operator classes — reuse pgvector's operators, expose them via
-- our AM so users can write USING cagra (embedding vector_cosine_ops).
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING cagra AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING cagra AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);
