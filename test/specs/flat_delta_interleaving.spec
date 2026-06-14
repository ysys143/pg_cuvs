# flat_delta_interleaving.spec — flat (A1) variant of delta_interleaving.
# ADR-074: verify cross-session pending-delta visibility through the flat path.
# flat's aminsert appends to the same non-transactional `.delta` sidecar, so an
# uncommitted INSERT's vector lands in `.delta` immediately; MVCC/heap recheck
# must hide it from another snapshot until commit, then reveal it via the merge.
#
# Determinism: id 200 at the unique extremum '[7,7,7,7]' → deterministic nearest
# (or empty) IFF visible.

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE isof2 (id bigint, embedding vector(4));
    INSERT INTO isof2 SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    CREATE INDEX isof2_flat ON isof2 USING flat (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE isof2;
    DROP EXTENSION pg_cuvs;
}

session w
setup      { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step w_begin   { BEGIN; }
step w_insert  { INSERT INTO isof2 VALUES (200, '[7,7,7,7]'); }
step w_commit  { COMMIT; }

session r
setup      { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
step r_pre  { SELECT count(*) AS sees_200
              FROM (SELECT id FROM isof2 ORDER BY embedding <-> '[7,7,7,7]'::vector LIMIT 1) s
              WHERE id = 200; }
step r_post { SELECT id FROM isof2 ORDER BY embedding <-> '[7,7,7,7]'::vector LIMIT 1; }

permutation w_begin w_insert r_pre w_commit r_post
