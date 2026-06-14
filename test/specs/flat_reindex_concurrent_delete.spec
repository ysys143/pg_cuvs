# flat_reindex_concurrent_delete.spec — flat (A1) variant of reindex_concurrent_delete.
# ADR-074: a deleted vector must not ghost in search results across REINDEX +
# concurrent DELETE+VACUUM on a `flat` index.
#
# Difference from the cagra spec: NO pg_cuvs_compact step. compact is cagra-only
# (handle_compact requires e->handle, which is NULL for flat) — flat has no graph
# to merge in place; its compaction IS a rebuild via REINDEX. So flat's lifecycle
# is DELETE → VACUUM (tombstone) → REINDEX (rebuild .vectors without dead rows).
#
# 결정론: id=99는 '[99,0,0,0]' — base cluster(거리 ~1-20)와 멀어, 삭제 후 쿼리는
# 항상 base cluster를 반환(id=99 ghost이면 실패).

setup
{
    SET cuvs.index_dir = '/tmp/cuvs_indexes';
    CREATE EXTENSION IF NOT EXISTS vector;
    CREATE EXTENSION IF NOT EXISTS pg_cuvs;
    CREATE TABLE isof3 (id bigint, embedding vector(4));
    INSERT INTO isof3 SELECT g, ('['||g||',0,0,0]')::vector FROM generate_series(1,20) g;
    INSERT INTO isof3 VALUES (99, '[99,0,0,0]');
    CREATE INDEX isof3_flat ON isof3 USING flat (embedding vector_l2_ops);
}

teardown
{
    DROP TABLE isof3;
    DROP EXTENSION pg_cuvs;
}

session s1
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; }
step s1_del     { DELETE FROM isof3 WHERE id = 99; }
step s1_vac     { VACUUM isof3; }
step s1_reindex { REINDEX INDEX CONCURRENTLY isof3_flat; }

session s3
setup     { SET cuvs.index_dir = '/tmp/cuvs_indexes'; SET enable_seqscan = off; }
step s3_read { SELECT id <> 99 AS no_ghost
               FROM isof3 ORDER BY embedding <-> '[99,0,0,0]'::vector LIMIT 1; }

# perm A: DELETE 선행 → REINDEX가 dead row를 빌드에서 제외
permutation s1_del s1_vac s1_reindex s3_read
# perm B: REINDEX 선행 → DELETE+VACUUM 후 tombstone 필터 + heap recheck 경로
permutation s1_reindex s1_del s1_vac s3_read
