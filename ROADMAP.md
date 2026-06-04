# pg_cuvs 구현 로드맵

상세 스펙은 [design/PLAN.md](design/PLAN.md), 설계 결정은 [design/DECISIONS.md](design/DECISIONS.md) 참조.

---

## 현재 상태

### 완료

| Phase | 내용 |
|-------|------|
| 1, 1.5, 2, 2.1 | Proof of mechanism, test hardening, production ready single-node |
| 3E/3F/3G | Multi-GPU sharding (placement, parallel fanout, auto shard count, DROP cleanup, snapshot, delta cache, eviction) |
| 3I | GPU Build Accelerator — CAGRA→pgvector HNSW export (`pg_cuvs_build_hnsw`), CPU HNSW fallback, GPU-less dump/restore |
| 3K | `CREATE INDEX ... USING pg_cuvs_hnsw` DDL 전환 + `source` optional(heap에서 ephemeral CAGRA 빌드) + metric 선검증. `pg_cuvs_build_hnsw()` deprecate. installcheck 8/8 (ADR-038/041) |
| 3H-full | 운영 runbook 4종 추가 (capacity-planning, replica-bootstrap, release-upgrade, benchmark-runbook) + 기존 3H-light. TBD: streaming 물리복제 / cross-version upgrade 검증 |
| 3B | DiskANN/NVMe cold tier — **NO-GO** (cuVS 26.04 PQFlash 미완성, 재검토 조건: 1B+ 수요 또는 cuVS stable) |

### 미완료

| Phase | 내용 | Wave |
|-------|------|------|
| 3L | GPU brute force 검색 모드 (`cuvs.search_mode='brute_force'`) | 2 |
| 3M | 배치 검색 API (`pg_cuvs_batch_search`) | 2 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 2 |
| 3A | Pending delta / delta exact search | 3 |
| 4A-1 | CAGRA 빌드 double memcpy 제거 | 4 |
| 4A-2 | parallel maintenance workers | 4 |
| 3C | GCS artifact snapshot (본체) | 5 |
| 3D | Replica async warmup | 5 |

---

## 구현 순서

### Wave 1 — 릴리스 전 확정 (병렬 진행 가능)

#### 3K — `CREATE INDEX ... USING pg_cuvs_hnsw` DDL 전환
**왜 먼저**: `pg_cuvs_build_hnsw()` 함수 호출을 릴리스에 그대로 내보내면 이후 deprecate 비용이 커짐. DDL 표면이 확정되어야 문서도 그 기준으로 작성 가능.

구현 항목:
- `pg_cuvs_hnsw` AM handler 등록
- `ambuild()`에서 WITH 절 파라미터(`source`, `mode`) 파싱 + 기존 빌드 로직 실행
- `amvalidate()`에서 source CAGRA 호환성 검증
- 기존 `pg_cuvs_build_hnsw()` deprecate/제거
- pgvector 호환 매트릭스 CI 테스트

완료 기준:
- `CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops) WITH (source = 'my_cagra', mode = 'nsw')`
- `pg_indexes` 카탈로그 노출, `DROP INDEX`/`REINDEX`/`pg_dump`/`pg_restore` 동작

스펙: [design/PLAN.md — Phase 3K](design/PLAN.md) | ADR-038

---

#### 3H-full — 운영 runbook 완성
**왜 먼저**: 3H-light(4종) 완료. 릴리스 전 운영 문서 완성 필요.

추가 항목:
- replica bootstrap / true multi-node 운영 runbook
- capacity-planning 수치 (벤치마크 의존 항목 채우기)
- release upgrade runbook (0.1.0 → 다음 버전)
- benchmark runbook 갱신

스펙: [design/PLAN.md — Phase 3H](design/PLAN.md) | 기존 playbooks: `docs/playbooks/`

---

#### Release hardening — TOAST storage 감지 NOTICE + best practice 문서
**왜**: CAGRA 빌드 시 TOAST decompression이 heap scan 오버헤드의 50-60%를 차지. 벡터 전용 테이블에서 PLAIN storage를 쓰면 ~25-35% 절감. 구현 비용 최소(syscache 읽기 + ereport).

구현 항목:
- `cuvs_ambuild()`에서 indexed column의 `attstorage`가 EXTENDED('x')인지 syscache로 확인 → NOTICE 출력
- `docs/best-practices.md`에 벡터 전용 테이블 + PLAIN storage 권장 스키마 패턴 문서화
- pg_cuvs가 storage를 강제 변경하지 않음 (사용자 선택 존중)

스펙: ADR-043 | [design/PLAN.md — Phase 4A 장기 항목](design/PLAN.md)

---

### Wave 2 — 신규 기능, 리스크 낮음 (순차)

#### 3L — GPU Brute Force 검색 모드
**왜**: `cuvs_brute_force_search` / `CuvsBfIndex` 인프라가 이미 구현됨. 구현 비용 낮음. 3M의 BF 배치 경로 전제. **3A-2(GPU delta cache)가 동일한 `CuvsBfIndex` 인프라를 재사용하므로 3A 전에 먼저 완성하는 것이 효율적.**

구현 항목:
- `CREATE INDEX USING cagra` 시 `.vectors` sidecar 직렬화 (versioned header, fsync+rename)
- `IndexEntry.main_bf_idx` + daemon startup 로드
- `cuvs.search_mode` GUC (`'cagra'` default / `'brute_force'`)
- `cuvs.bf_precision` GUC (`'float32'` / `'float16'`) — float16으로 VRAM 절반, QPS 35% 향상
- `cuvs.bf_batch_wait_us` 마이크로배칭 (Q=1 → Q=N dispatch로 throughput 향상)
- `cuvsamcostestimate` BF 분기 추가
- `pg_stat_gpu_cache.bf_vram_bytes`, `bf_precision`

완료 기준:
- `SET cuvs.search_mode = 'brute_force'` → recall@10 = 1.0 (seqscan GT와 일치)
- sharded index에서도 정확한 top-k 반환
- `.vectors` sidecar 없을 때 명확한 ERROR

스펙: [design/PLAN.md — Phase 3L](design/PLAN.md) | ADR-039 | 참고: `docs/bruteforce-acceleration-lessons.md`

---

#### 3M — 배치 검색 API
**왜**: 3L 완료 후. IPC Q>1 확장이 3A-2 GPU delta cache와도 인프라를 공유. GT 생성/RAG 멀티청크 등 실용 시나리오 명확.

구현 항목:
- `CUVS_OP_SEARCH_BATCH` opcode (기존 `CUVS_OP_SEARCH` Q=1 경로 변경 없음)
- request shm: Q×dim float32 / reply shm: Q×K (tid, distance)
- `pg_cuvs_batch_search(rel regclass, queries vector[], k int) RETURNS TABLE(query_idx int, ctid tid, distance float4)` SRF
- heap recheck / MVCC 내부 처리
- CAGRA / BF / sharded 경로 지원

완료 기준:
- Q개 쿼리를 단일 IPC 왕복으로 처리
- query_idx별 결과가 단일 쿼리 API와 동일한 top-K
- Q=1000, dim=1024 기준 단일 쿼리 반복 대비 throughput 유의미하게 향상

스펙: [design/PLAN.md — Phase 3M](design/PLAN.md) | ADR-040

---

#### 3N — OFFSET-aware K 자동 조정
**왜**: ORM pagination(Django, Rails, Spring Data, SQLAlchemy)이 `LIMIT K OFFSET N` 문법을 사용하는데 현재 pg_cuvs는 OFFSET을 인식하지 못함. DDL(3K) 이후 DML까지 "PostgreSQL 인덱스답게" 동작하는 범위를 확장.

구현 항목:
- `cuvsamcostestimate()` 또는 `cuvs_beginscan()`에서 Plan의 LIMIT+OFFSET 감지
- IPC의 K를 `offset + limit`으로 계산해 전달 (daemon 변경 없음)
- `cuvs_gettuple()`에서 앞 offset개 skip 후 반환
- `offset > cuvs.max_offset_warning`(기본 1000) 시 NOTICE 경고
- regression test: OFFSET 0/10/100 결과 일관성

완료 기준:
- `SELECT ... ORDER BY ... <-> ... LIMIT 10 OFFSET N`이 CAGRA/BF/sharded 경로에서 정상 동작
- OFFSET 0일 때 기존 동작 동일 (regression 없음)
- Django/SQLAlchemy의 `.offset().limit()` 패턴이 GPU 인덱스를 사용함을 `EXPLAIN`으로 확인

스펙: [design/PLAN.md — Phase 3N](design/PLAN.md) | ADR-042

---

### Wave 3 — Pending Delta (순차)

RAG/검색 시스템은 문서가 계속 추가/수정된다. INSERT 하나만 해도 GPU path가 완전히 포기되는 현재 구조로는 streaming write가 있는 모든 워크로드에서 pg_cuvs를 실운용할 수 없다. **3L에서 완성한 `CuvsBfIndex` 인프라를 3A-2 GPU delta cache가 직접 재사용한다.**

#### 3A-1 — CPU-exact MVP
- `aminsert`에서 `.delta` sidecar에 (TID, vector, generation) append — 성공 시 `MARK_STALE` 전송 안 함
- `.delta` append 실패 시 기존 stale path로 fail-closed
- query 시 base CAGRA(k+slop) + CPU exact search over delta rows + top-k merge
- generation mismatch 시 CPU reroute
- `cuvs.max_delta_rows` 초과 시 GPU+delta 중단 + REINDEX 권고
- REINDEX 후 `.delta` compaction

#### 3A-2 — GPU BF delta cache
- daemon이 `.delta`를 generation/mtime 기준 lazy reload → `CuvsBfIndex`로 로드 (3L 인프라 재사용)
- base CAGRA search + GPU BF delta search → daemon 내 merge → `delta_merged` flag
- `delta_merged=1`이면 backend CPU merge 생략

#### 3A-3 — delta controls/stats
- `cuvs.delta_search=auto|cpu|gpu` GUC
- `pg_stat_gpu_search` delta 컬럼 (`delta_rows`, `delta_generation`, `delta_vram_bytes`, `delta_search_mode`)

#### 3A-4 — tombstone/cleanup
- `.tombstone` sidecar + snapshot-aware dead TID filtering
- `ambulkdelete`가 dead TID를 tombstone으로 기록
- tombstone cap 초과/unusable 시에만 `.stale` CPU reroute

완료 기준:
- INSERT/UPDATE 후 REINDEX 없이 GPU+delta merged top-k가 pgvector ground truth와 일치
- DELETE/VACUUM은 tombstone correction 기본 경로 사용
- daemon restart 후 delta 유실 시 incomplete GPU 결과 서빙 안 함

스펙: [design/PLAN.md — Phase 3A](design/PLAN.md)

---

### Wave 4 — 빌드 성능 (순차)

#### 4A-1 — CAGRA 빌드 double memcpy 제거
**왜**: 4A-1의 shm 직접 할당이 4A-2 worker buffer와 결합해야 double memcpy 완전 제거.

구현 항목:
- `cuvs_ambuild()`에서 scan 전 `shm_open` + `ftruncate` + `mmap`
- `grow_build_buffers()` `realloc` → `ftruncate` + `mremap` 교체
- `shm_write_build_payload()` memcpy×2 제거
- `shm_open` 실패 시 heap 경로 degraded fallback + WARNING

완료 기준: N=1M dim=1024 Cohere A100 기준 build ≤ 50s (현재 55s)

스펙: [design/PLAN.md — Phase 4A](design/PLAN.md) | ADR-034 §4A-1

---

#### 4A-2 — parallel maintenance workers
**왜**: 4A-1 완료 후. worker별 buffer가 shm 위에 올라가야 double memcpy 경로가 완전히 제거됨.

구현 항목:
- `table_index_build_scan()` parallel 인자 전달 (구현 시 PG API 조사 후 결정)
- worker별 독립 `CuvsBuildState` + leader merge (memcpy 연접, 정렬 불필요)
- `max_parallel_maintenance_workers` GUC 읽기 (0이면 기존 단일 프로세스)

완료 기준: workers=4 기준 build ≤ 35s (현재 55s)

스펙: [design/PLAN.md — Phase 4A](design/PLAN.md) | ADR-034 §4A-2

---

### Wave 5 — 분산 운영 (replica 수요 확인 후)

#### 3C → 3D — GCS Snapshot + Replica Async Warmup
**왜 나중에**: 단일 노드 워크로드가 충분히 커버되고 multi-node 수요가 생기는 시점에 진입.

- **3C**: manifest/checksum/version 기반 GCS upload/download 전체 경로 (3G.2 sharded snapshot과 통합)
- **3D**: background warmup thread pool, cold entry 등록, cache miss CPU fallback

스펙: [design/PLAN.md — Phase 3C, 3D](design/PLAN.md) | ADR-013

---

## Phase 4B 현황

`import_hnsw` 페이지 write 병목 — **현상 유지** (UNLOGGED ~28s).
병렬 page write / Bulk WAL은 PG buffer manager 제약으로 단기 제외.
재검토 조건: PG 코어에 bulk page write API 추가 또는 `ReadBuffer` concurrent extension 완화.

스펙: [design/PLAN.md — Phase 4B](design/PLAN.md) | ADR-035
