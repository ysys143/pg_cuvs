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
| Release-hardening | 빌드-time 권고 emit 3종 — TOAST NOTICE(고차원 toastable→PLAIN 권고), index_dir이 $PGDATA 하위면 WARNING(basebackup 비대), pgvector 0.5–0.8 밖이면 WARNING(HNSW 포맷 drift). `docs/best-practices.md`. installcheck GREEN(release_hardening) + 수동 e2e (ADR-043/ADR-013/ADR-038) |
| 3A | Pending delta / delta exact search — INSERT/UPDATE `.delta` append(false stale 없음) + CPU/GPU 병합, snapshot-aware `.tombstone`, tri-mode `cuvs.delta_search`(int 0/1/2), delta cap fail-closed, tombstone-aware over-fetch로 delete-drift recall 보존. installcheck 15/15 + isolation 2/2 GREEN + restart e2e PASS (ADR-047). **비고**: 메커니즘은 3F/3G·phase3a WIP(2026-05)로 구현됐으나 완료 기준(회귀/격리 검증) 미충족으로 미완 표기됐던 것을 본 세션에서 검증·certify(false-done 역방향 해소) |

### 미완료

| Phase | 내용 | 트랙 |
|-------|------|------|
| 4A-1 | CAGRA 빌드 double memcpy 제거 (**quick win**: ~2-5s, 난이도 낮음, 4A-2 enabler) | 릴리스 후 기능 |
| 4A-2 | parallel maintenance workers — heap scan/detoast 병렬화 (~8-12s/~10-14%, 난이도 중간) | 릴리스 후 기능 |
| 3C / 3D | GCS artifact snapshot 본체 / Replica async warmup | 트리거 (multi-node 수요) |
| fallback 관측성 · circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 | 트리거 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실이고, Phase 코드(`3A`/`3K` 등)가 안정적 식별자다. 살아있는 시퀀스에는 **미완 순차 작업만 트랙 이름으로** 둔다(번호 `Wave N`을 붙이지 않는다 — 완료할 때마다 renumber하면 같은 번호가 다른 걸 가리켜 꼬이므로). 완료 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 항목은 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다.

### 릴리스 후 기능 (순차)

> **3A Pending Delta는 완료**(완료 표 참조). streaming write(INSERT/UPDATE/DELETE) 후 REINDEX 없이 GPU+delta 병합으로 정합한 top-k를 반환한다. 3L `CuvsBfIndex`를 3A-2 GPU delta cache가 재사용. 상세 스펙·검증은 [design/PLAN.md — Phase 3A](design/PLAN.md), 결정은 ADR-047. 순차 경로는 4A부터다.

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

순차 경로(릴리스 후 기능)와 섞지 않는다. 각 항목은 트리거가 충족될 때 순차 트랙으로 승격한다.

### 분산 운영 — 3C/3D (트리거: multi-node 수요)

#### 3C → 3D — GCS Snapshot + Replica Async Warmup
**왜 나중에**: 단일 노드 워크로드가 충분히 커버되고 multi-node 수요가 생기는 시점에 진입.

- **3C**: manifest/checksum/version 기반 GCS upload/download 전체 경로 (3G.2 sharded snapshot과 통합)
- **3D**: background warmup thread pool, cold entry 등록, cache miss CPU fallback

스펙: [design/PLAN.md — Phase 3C, 3D](design/PLAN.md) | ADR-013

### 운영 하드닝 잔여 (트리거별)

2026-06-05 스펙 무결성 감사(`docs/spec-audit-2026-06-05.md`) + AI council 논의에서 누적. **운영자 대면 절차의 단일 산출물은 `design/OPS_GPU_PLAYBOOK.md`**. release급 emit(TOAST NOTICE / index_dir WARNING / pgvector 가드)은 Release-hardening으로 **완료됨**(완료 표 참조). 아래는 트리거 대기분.

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
