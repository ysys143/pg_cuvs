-- pg_cuvs upgrade: 0.3.0 → 0.4.0
-- ADR-073: standalone `flat` access method — GPU exact brute-force index
-- (FAISS IndexFlat-style). Builds a resident flat vector store (.vectors), no
-- graph. recall=1.0. ALWAYS brute-force regardless of cuvs.search_mode.
--
-- Usage:
--   ALTER EXTENSION pg_cuvs UPDATE TO '0.4.0';
--   CREATE INDEX idx ON items USING flat (embedding vector_l2_ops)
--     WITH (precision = 'float16');
--   SELECT * FROM items ORDER BY embedding <-> '[...]' LIMIT 10;

\echo Use "ALTER EXTENSION pg_cuvs UPDATE TO ''0.4.0''" to load this file. \quit

-- ----------------------------------------------------------------
-- flat Access Method handler
-- ----------------------------------------------------------------
CREATE FUNCTION flatamhandler(internal)
RETURNS index_am_handler
AS '$libdir/pg_cuvs', 'flatamhandler'
LANGUAGE C;

CREATE ACCESS METHOD flat
TYPE INDEX
HANDLER flatamhandler;

COMMENT ON ACCESS METHOD flat IS
  'GPU exact brute-force index for pgvector (FAISS IndexFlat-style). Builds a '
  'resident flat vector store (.vectors), no graph; recall=1.0. Always exact '
  'brute-force regardless of cuvs.search_mode. Tuning: WITH (precision=''float16'').';

-- ----------------------------------------------------------------
-- Operator classes — reuse pgvector operators via the flat AM
-- ----------------------------------------------------------------
CREATE OPERATOR CLASS vector_l2_ops
DEFAULT FOR TYPE vector USING flat AS
    OPERATOR 1 <-> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_l2_squared_distance(vector, vector);

CREATE OPERATOR CLASS vector_cosine_ops
FOR TYPE vector USING flat AS
    OPERATOR 1 <=> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 cosine_distance(vector, vector);

CREATE OPERATOR CLASS vector_ip_ops
FOR TYPE vector USING flat AS
    OPERATOR 1 <#> (vector, vector) FOR ORDER BY float_ops,
    FUNCTION 1 vector_negative_inner_product(vector, vector);
