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
| 3K | `CREATE INDEX ... USING pg_cuvs_hnsw` DDL 전환 + `source` optional(heap에서 ephemeral CAGRA 빌드) + metric 선검증. `pg_cuvs_build_hnsw()` deprecate. installcheck GREEN (ADR-038/041) |
| 3L | GPU brute force 검색 모드 (`.vectors` sidecar, `search_mode`/`bf_precision` GUC, micro-batching, sharded BF). installcheck GREEN, recall@10=1.0 (ADR-039) |
| 3M | 배치 검색 API (`CUVS_OP_SEARCH_BATCH`, `pg_cuvs_batch_search` SRF, CAGRA/BF/sharded 지원). installcheck GREEN, Q×K top-k 일치 (ADR-040) |
| 3H-full | 운영 runbook 4종 추가 (capacity-planning, replica-bootstrap, release-upgrade, benchmark-runbook) + 기존 3H-light. TBD: streaming 물리복제 / cross-version upgrade 검증 |
| 3B | DiskANN/NVMe cold tier — **NO-GO** (cuVS 26.04 PQFlash 미완성, 재검토 조건: 1B+ 수요 또는 cuVS stable) |
| 하드닝 | `index_dir` reloption — cross-session seqscan 폴백 근절. 인덱스가 빌드 디렉터리를 `pg_class.reloptions`에 self-describe (reloption > 세션 GUC > `$PGDATA` 3단계). installcheck GREEN, no-GUC 연결에서 Index Scan 실증 (ADR-045) |
| 하드닝 | orphan artifact GC (`pg_cuvs_gc_orphans(do_delete)`) — 데몬-down DROP / DROP DATABASE / 재시작 좀비 재로드로 인한 VRAM+디스크 누수 근절. backend가 `index_dir`을 `pg_index`/`pg_database`와 대조(daemon은 sidecar라 카탈로그 불가). dry-run 기본. installcheck GREEN(gc_orphans) + 데몬-down e2e 검증. ADR-009 정정 반영 (ADR-046) |
| 4-preflight | 연산 지역성 프로파일링 완료 (A100/PG16, N=1M dim=1024). 검색 GPU:IPC≈2:1(GPU-bound), 빌드 GPU 82%/backend 18%, export buffer-mgr 39%, TOAST vs PLAIN 8%. 측정 근거로 4A 하향. `docs/profiling-results.md` (ADR-044) |

### 미완료

| Phase | 내용 | 트랙 |
|-------|------|------|
| Release-hardening | TOAST NOTICE + index_dir WARNING + pgvector 가드 + best-practices | 릴리스 전 확정 |
| 3A | Pending delta / delta exact search | 릴리스 후 기능 |
| 4A-1 | CAGRA 빌드 double memcpy 제거 (**quick win**: ~2-5s, 난이도 낮음, 4A-2 enabler) | 릴리스 후 기능 |
| 4A-2 | parallel maintenance workers — heap scan/detoast 병렬화 (~8-12s/~10-14%, 난이도 중간) | 릴리스 후 기능 |
| 3C / 3D | GCS artifact snapshot 본체 / Replica async warmup | 트리거 (multi-node 수요) |
| fallback 관측성 · circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 | 트리거 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실. 아래 번호 Wave는 **미완 순차 작업만** 담는다 — 완료된 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 기반 항목은 번호 없는 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다. 이로써 "번호 = 실행 순서"를 유지한다(완료/추가 때마다 꼬이던 문제 근절).

### Wave 1 — 릴리스 전 확정

#### Release-hardening — ambuild/build-time emit 묶음 + best-practices
**왜 먼저**: 릴리스 게이트. 셋 다 동일한 ambuild/build-time emit(syscache 읽기 + ereport)이라 한 묶음으로 처리. 측정·설계는 끝났고 구현 비용 최소.

구현 항목:
- **TOAST storage NOTICE** (ADR-043): `cuvs_ambuild()`에서 indexed column의 `attstorage`가 EXTENDED('x')면 NOTICE — 벡터 전용 테이블은 PLAIN으로 ~25-35% 빌드 절감(강제 변경 X). 실측 표는 4-preflight 산출물(`docs/profiling-results.md`)·ADR-043 사용.
- **index_dir WARNING** (감사 #6 / OBJSTORE-03): resolved `index_dir`이 `$PGDATA` 트리 하위면 1회 WARNING — `pg_basebackup` 비대/standby 프로비저닝 비용. 같은 NVMe 형제 디렉터리 권장(지역성↔백업 직교).
- **pgvector 버전 가드 WARNING** (3K 잔여): 설치 pgvector 버전을 known-good 범위(0.5.0~0.8.x, HNSW_VERSION=1)와 대조 → 벗어나면 WARNING(on-disk 포맷 drift 위험). GPU 매트릭스 CI 대신 저비용 가드.
- `docs/best-practices.md`: 벡터 전용 테이블 + PLAIN storage + index_dir 배치 권장 스키마 패턴 문서화.

완료 기준:
- EXTENDED 컬럼에 CAGRA 빌드 시 NOTICE, `$PGDATA` 하위 index_dir에 WARNING, 범위 밖 pgvector에 WARNING (installcheck로 검증).
- `docs/best-practices.md` 작성.

스펙: ADR-043 / ADR-013(OBJSTORE-03) / ADR-038(3K 잔여) | `design/OPS_GPU_PLAYBOOK.md`

---

### Wave 2 — 릴리스 후 기능 (순차)

RAG/검색 시스템은 문서가 계속 추가/수정된다. INSERT 하나만 해도 GPU path가 완전히 포기되는 현재 구조로는 streaming write가 있는 모든 워크로드에서 pg_cuvs를 실운용할 수 없다. **3L에서 완성한 `CuvsBfIndex` 인프라를 3A-2 GPU delta cache가 직접 재사용한다.**

#### 3A — Pending Delta

##### 3A-1 — CPU-exact MVP
- `aminsert`에서 `.delta` sidecar에 (TID, vector, generation) append — 성공 시 `MARK_STALE` 전송 안 함
- `.delta` append 실패 시 기존 stale path로 fail-closed
- query 시 base CAGRA(k+slop) + CPU exact search over delta rows + top-k merge
- generation mismatch 시 CPU reroute
- `cuvs.max_delta_rows` 초과 시 GPU+delta 중단 + REINDEX 권고
- REINDEX 후 `.delta` compaction

##### 3A-2 — GPU BF delta cache
- daemon이 `.delta`를 generation/mtime 기준 lazy reload → `CuvsBfIndex`로 로드 (3L 인프라 재사용)
- base CAGRA search + GPU BF delta search → daemon 내 merge → `delta_merged` flag
- `delta_merged=1`이면 backend CPU merge 생략

##### 3A-3 — delta controls/stats
- `cuvs.delta_search=auto|cpu|gpu` GUC
- `pg_stat_gpu_search` delta 컬럼 (`delta_rows`, `delta_generation`, `delta_vram_bytes`, `delta_search_mode`)

##### 3A-4 — tombstone/cleanup
- `.tombstone` sidecar + snapshot-aware dead TID filtering
- `ambulkdelete`가 dead TID를 tombstone으로 기록
- tombstone cap 초과/unusable 시에만 `.stale` CPU reroute

완료 기준:
- INSERT/UPDATE 후 REINDEX 없이 GPU+delta merged top-k가 pgvector ground truth와 일치
- DELETE/VACUUM은 tombstone correction 기본 경로 사용
- daemon restart 후 delta 유실 시 incomplete GPU 결과 서빙 안 함

스펙: [design/PLAN.md — Phase 3A](design/PLAN.md)

---

#### 4A — 빌드 성능 (4-preflight 측정으로 범위·기대치 재조정)

> **2026-06-05 4-preflight 측정 결론**: 빌드 wall-clock 83.5s = backend(heap/detoast/memcpy/shm) ~15.5s(18%) **직렬 후** GPU CAGRA build ~68s(82%). ADR-034의 "PG overhead 45s" 추정은 틀렸고 backend는 ~15.5s가 천장이다.
> - **4A-1 (double memcpy)**: ~2-5s(~3-6%)지만 **난이도 낮음 → quick win**(memcpy ~1.7s + realloc page fault 완화). shm 직접 할당이 4A-2의 enabler라 먼저 착수.
> - **4A-2 (parallel workers)**: backend heap scan+detoast(~12s) 병렬화 → ~15.5s→~7s, **~8-12s(~10-14%) 절감**, 난이도 중간. 절대 이득 크나 작업량 많음.
> - 둘 다 빌드가 일회성(CREATE INDEX/REINDEX)이라 쿼리 경로 대비 **긴급도만 낮을 뿐 저가치 아님**. 빌드 속도가 우선이면 4A-1→4A-2.
> - **결합 효과 ("PG 오버헤드 제거율"로 평가)**: PLAIN(detoast 제거, 15.5s→8.7s) + 4A-1(realloc page fault + double memcpy 제거) + 4A-2(heap scan 분산)를 모두 적용하면 backend ~15.5s→~2-4s로 거의 소멸 → 빌드 83.5s→**~70-72s ≈ cuVS native의 ~95%**, MVCC/durability 유지한 채. 비율(17%)로 작아 보여도 절대 14.5s는 전부 제거 가능한 PG 오버헤드 → "Postgres 안전성 + cuVS native 속도".
> - **천장**: 어떤 4A도 빌드를 ~68s(GPU build) 밑으로 못 내린다. 그 이하는 cuVS build 파라미터(graph_degree 등) 또는 streaming(cuVS incremental API 부재) 필요. 빌드 가속이 실수요로 올라오면 **GPU build 파라미터 튜닝을 4A와 함께** 검토.
> 상세: `docs/profiling-results.md` §3·§6, ADR-044.

##### 4A-1 — CAGRA 빌드 double memcpy 제거
**왜**: 4A-1의 shm 직접 할당이 4A-2 worker buffer와 결합해야 double memcpy 완전 제거.

구현 항목:
- `cuvs_ambuild()`에서 scan 전 `shm_open` + `ftruncate` + `mmap`
- `grow_build_buffers()` `realloc` → `ftruncate` + `mremap` 교체
- `shm_write_build_payload()` memcpy×2 제거
- `shm_open` 실패 시 heap 경로 degraded fallback + WARNING

완료 기준: N=1M dim=1024 Cohere A100 기준 build ≤ 50s (현재 55s)

스펙: [design/PLAN.md — Phase 4A](design/PLAN.md) | ADR-034 §4A-1

##### 4A-2 — parallel maintenance workers
**왜**: 4A-1 완료 후. worker별 buffer가 shm 위에 올라가야 double memcpy 경로가 완전히 제거됨.

구현 항목:
- `table_index_build_scan()` parallel 인자 전달 (구현 시 PG API 조사 후 결정)
- worker별 독립 `CuvsBuildState` + leader merge (memcpy 연접, 정렬 불필요)
- `max_parallel_maintenance_workers` GUC 읽기 (0이면 기존 단일 프로세스)

완료 기준: workers=4 기준 build ≤ 35s (현재 55s)

스펙: [design/PLAN.md — Phase 4A](design/PLAN.md) | ADR-034 §4A-2

---

## 트리거 기반 백로그 (번호 없음 — 조건 충족 시 승격)

순차 릴리스 경로(Wave 1·2)와 섞지 않는다. 각 항목은 트리거가 충족될 때 Wave로 승격한다.

### 분산 운영 — 3C/3D (트리거: multi-node 수요)

#### 3C → 3D — GCS Snapshot + Replica Async Warmup
**왜 나중에**: 단일 노드 워크로드가 충분히 커버되고 multi-node 수요가 생기는 시점에 진입.

- **3C**: manifest/checksum/version 기반 GCS upload/download 전체 경로 (3G.2 sharded snapshot과 통합)
- **3D**: background warmup thread pool, cold entry 등록, cache miss CPU fallback

스펙: [design/PLAN.md — Phase 3C, 3D](design/PLAN.md) | ADR-013

### 운영 하드닝 잔여 (트리거별)

2026-06-05 스펙 무결성 감사(`docs/spec-audit-2026-06-05.md`) + AI council 논의에서 누적. **운영자 대면 절차의 단일 산출물은 `design/OPS_GPU_PLAYBOOK.md`**. release급 emit(TOAST NOTICE / index_dir WARNING / pgvector 가드)은 Wave 1로 승격됨. 아래는 트리거 대기분.

| 항목 | 근거 | 성격 | 트리거 |
|------|------|------|--------|
| **fallback 관측성** | 감사 #2. `pg_stat_gpu_search`에 fallback/success 카운터 부재 — 쿼리가 조용히 CPU로 새는지 SQL로 알 수 없음(fallback은 backend plan-time 결정이라 데몬 미도달). | 코드(backend per-index fallback 카운터 → view 노출) | 관측성/Phase 2 작업 시 동반 |
| **circuit breaker 전역화** | 감사 #5. breaker가 백엔드 프로세스-로컬(shared 아님) — 동시 연결에서 GPU 장애 시 전역 보호 안 됨(백엔드당 상한은 있음). | 코드(shared-memory breaker 상태) | 동시연결 GPU 장애 복원력 요구 시(프로덕션화) |
| **SQL latency split** | 감사 #1. `pg_stat_gpu_search`에 GPU/IPC/recheck 분해 미노출. | 코드(데몬 계측 + IPC + SQL 컬럼) | 명시 요청 시(ADR-044가 외부 측정 완료, SQL 노출 한계가치 낮음) |

스펙: `docs/spec-audit-2026-06-05.md` | ADR-011 / ADR-017 | `design/OPS_GPU_PLAYBOOK.md`

### 기타

#### 3N — OFFSET-aware K 자동 조정 (트리거: ORM pagination 요구)
**상태**: 보류 (low-value). 자체 분석상 임팩트 낮음 — PG executor의 `LIMIT/OFFSET`이 이미 동작하고, pg_cuvs는 K를 `offset + limit`으로 조정하면 됨(별도 API 불필요). top-K 사용 패턴(K=10~100)에서 offset pagination 수요가 드묾. ORM 호환이 명확한 요구로 올라오는 시점에 재검토. 구현 자체는 자명(저난이도)이라 필요 시 즉시 착수 가능.

스펙: [design/PLAN.md — Phase 3N](design/PLAN.md) | ADR-042

---

## Phase 4B 현황

`import_hnsw` 페이지 write 병목 — **현상 유지** (UNLOGGED ~28s).
병렬 page write / Bulk WAL은 PG buffer manager 제약으로 단기 제외.
재검토 조건: PG 코어에 bulk page write API 추가 또는 `ReadBuffer` concurrent extension 완화.

스펙: [design/PLAN.md — Phase 4B](design/PLAN.md) | ADR-035
