# pg_cuvs Best Practices

릴리스 운영 시 권장 설정. 각 항목은 빌드 시점 NOTICE/WARNING으로도 안내된다(강제 변경 없음).

## 1. 벡터 컬럼 storage: 고차원은 PLAIN 권장 (ADR-043)

pgvector `vector` 타입의 기본 storage는 **EXTERNAL**이다. 차원이 커서 행이 TOAST 임계(~2KB, 약 dim≥500)를 넘으면 값이 out-of-line으로 저장되고, CAGRA 빌드의 heap scan마다 **detoast 비용**(빌드 heap scan의 ~25-35%)을 낸다.

벡터 전용 테이블(다른 큰 컬럼 없음)이라면 **PLAIN storage**를 권장한다:

```sql
CREATE TABLE items (id bigint, embedding vector(1024));
ALTER TABLE items ALTER COLUMN embedding SET STORAGE PLAIN;
-- 기존 데이터가 있으면 재작성 필요:
VACUUM FULL items;
-- 이후 인덱스 빌드/REINDEX
CREATE INDEX ON items USING cagra (embedding vector_l2_ops);
```

- 빌드 시 컬럼이 TOAST-able이고 해당 차원에서 toast되면 NOTICE가 출력된다.
- 작은 차원(행이 inline 유지)은 detoast 비용이 없어 NOTICE를 내지 않는다.
- 측정 근거: `docs/profiling-results.md`(4-preflight), ADR-043. EXTENDED/EXTERNAL vs PLAIN 빌드 차이 실측.

주의: 행이 페이지(8KB)에 안 들어갈 만큼 크면 PLAIN이 불가하다. 매우 고차원(예: dim>2000 float4)은 TOAST가 불가피할 수 있다.

## 2. index_dir 배치: $PGDATA 밖, 같은 로컬 볼륨 (OBJSTORE-03)

CAGRA artifact는 재생성 가능한 수 GB 파생 데이터다. **지역성(빠른 로컬 디스크)** 과 **백업 멤버십($PGDATA 트리 안/밖)** 은 직교한다:

- 기본 `index_dir` = `$PGDATA/cuvs_indexes`는 **PGDATA 트리 안** → `pg_basebackup`이 통째로 복사(백업 비대 + standby 신규 프로비저닝 시 쓸모없는 GB 전송/ RTO 악화). 데이터 오염은 아니다(relfilenode fail-closed).
- 권장: **같은 NVMe의 형제 디렉터리**(PGDATA 트리 밖)에 둔다 → 지역성 유지 + base backup에서 제외.

```bash
# 예: PGDATA가 /var/lib/postgresql/16/main 일 때
sudo install -d -o postgres -g postgres -m 0700 /var/lib/postgresql/cuvs_indexes
```
```sql
ALTER SYSTEM SET cuvs.index_dir = '/var/lib/postgresql/cuvs_indexes';
SELECT pg_reload_conf();
-- 또는 per-index: CREATE INDEX ... WITH (index_dir = '/var/lib/postgresql/cuvs_indexes')
```

- 데몬(`pg-cuvs-server`)의 `--index-dir`도 동일 경로로 맞춘다.
- `index_dir`이 `$PGDATA` 하위로 해석되면 빌드 시 WARNING이 출력된다.
- 복원/운영 절차: `design/OPS_GPU_PLAYBOOK.md` §6. orphan 정리는 `pg_cuvs_gc_orphans(true)`(ADR-046).

## 3. pgvector 버전: 0.5.x–0.8.x (HNSW export)

`pg_cuvs_build_hnsw` / `USING pg_cuvs_hnsw`는 pgvector on-disk HNSW 페이지(`HNSW_VERSION=1`, pgvector 0.5.0~0.8.x에서 안정)를 직접 쓴다. 이 범위 밖 pgvector에서는 on-disk 포맷이 달라져 export된 인덱스를 읽지 못할 수 있다.

- 설치된 pgvector가 범위 밖이면 HNSW 빌드 시 WARNING이 출력된다.
- pgvector 메이저 포맷 bump 시 `src/hnsw_export.c`의 `PGV_HNSW_VERSION`/`PGV_HNSW_MAGIC` 상수와 호환성을 재검증할 것.
