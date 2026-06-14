# flat_tombstone_snapshot.spec — flat (A1) variant of delta_tombstone_snapshot.
# ADR-074: A1 reuses cagra's gettuple/delta/tombstone/recheck machinery, so MVCC
# visibility should be identical — but the cagra isolation specs never exercised
# `USING flat`. This closes that gap for the DELETE-visibility-across-snapshots
# property: a committed DELETE is filtered for a NEW snapshot (heap recheck) but
# remains visible to an OLDER REPEATABLE READ snapshot opened before the delete.
#
# Determinism: id 42 at the unique far extremum '[9,9,9,9]' (distance 0 to an
# equal probe) → deterministic nearest IFF visible. enable_seqscan off forces the
# flat GPU-BF index path (not a CPU seqscan).

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE isof (id bigint, embedding vector(4));
    INSERT INTO isof SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    INSERT INTO isof VALUES (42, '[9,9,9,9]');
    CREATE INDEX isof_flat ON isof USING flat (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE isof;
    DROP EXTENSION pg_cuvs;
}

session s1
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off;
            BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1_snap  { SELECT id FROM isof ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }
step s1_again { SELECT id FROM isof ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }
step s1_commit { COMMIT; }

session s2
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s2_del { DELETE FROM isof WHERE id = 42; }
step s2_vac { VACUUM isof; }

session s3
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
step s3_read { SELECT id FROM isof ORDER BY embedding <-> '[9,9,9,9]'::vector LIMIT 1; }

permutation s1_snap s2_del s2_vac s1_again s1_commit s3_read
