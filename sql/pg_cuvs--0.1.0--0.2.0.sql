-- pg_cuvs upgrade: 0.1.0 → 0.2.0
-- Phase 3P: IVF-PQ access method (GPU Product Quantization; 10-100x VRAM vs CAGRA)
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.2.0';
--
-- After upgrade, create an IVF-PQ index:
--   CREATE INDEX idx ON items USING ivfpq (embedding vector_l2_ops)
--     WITH (n_lists = 1024);
--   SET cuvs.ivfpq_n_probes = 64;
--   SELECT * FROM items ORDER BY embedding <-> '[...]' LIMIT 10;

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.2.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- IVF-PQ Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION ivfpqamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'ivfpqamhandler'
LANGUAGE C;

CREATE ACCESS METHOD ivfpq
TYPE INDEX
HANDLER ivfpqamhandler;

COMMENT ON ACCESS METHOD ivfpq IS
  'GPU IVF-PQ index for pgvector. 10-100x less VRAM than cagra via Product '
  'Quantization. Tuning: WITH (n_lists=1024, pq_bits=8, pq_dim=0). '
  'Search recall: SET cuvs.ivfpq_n_probes = 64 (higher = better recall).';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the ivfpq AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING ivfpq AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);
