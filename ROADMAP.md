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
| 3A | Pending delta / delta exact search — INSERT/UPDATE `.delta` append(false stale 없음) + CPU/GPU 병합, snapshot-aware `.tombstone`, tri-mode `cuvs.delta_search`(enum auto|cpu|gpu), delta cap fail-closed, tombstone-aware over-fetch로 delete-drift recall 보존. installcheck 15/15 + isolation 2/2 GREEN + restart e2e PASS (ADR-047). **비고**: 메커니즘은 3F/3G·phase3a WIP(2026-05)로 구현됐으나 완료 기준(회귀/격리 검증) 미충족으로 미완 표기됐던 것을 본 세션에서 검증·certify(false-done 역방향 해소) |
| 4A | 빌드 오버헤드(raw cuVS 대비) 최소화 — **ADR-057** memfd+SCM_RIGHTS corpus(누수-안전·무복사, peak RSS −32%), **ADR-058** parallel maintenance workers(분산스캔), **ADR-059** 데몬 multi-partial direct H2D(리더 merge 복사 제거). backend 오버헤드 ~6.3s(단일)→~3.7s(병렬); wall-clock은 GPU floor(~33s) 지배라 marginal — 가치는 north-star(backend 제거율). PLAIN 권고도 ADR-044 실측 ~8%로 보정. self-NN 단일==병렬(multi-partial) 5/5 + installcheck 15/15 + iso 2/2 GREEN, /dev/shm 고아 0. `docs/profiling-results.md` §7/8/9 (#20, ADR-057/058/059) |
| 3R | CAGRA 빌드 파라미터 reloption — `graph_degree`/`intermediate_graph_degree`/`build_algo`(auto\|ivf_pq\|nn_descent) per-index reloption으로 노출, recall↔speed↔VRAM 튜닝. cuVS 26.04 `graph_build_params` variant 매핑. DDL validator + `intermediate >= graph_degree` fail-closed. 파라미터 실적용 실증(`.cagra` adjacency Δ = n×Δgd×4 정확) + installcheck 16/16(`build_params`) + iso 2/2 GREEN (ADR-052) |

### 미완료

| Phase | 내용 | 트랙 |
|-------|------|------|
| 3O | Pre-filter ANN — WHERE 조건을 cuVS bitvector mask로 daemon에 전달, 고선택성 필터 GPU 품질 향상. **보류(2026-06-07, 접근 미정)**: PG AM이 비-인덱스-컬럼 qual을 안 넘겨 원안(bitvector)은 custom-scan+멀티세션·고위험; 저비용 대안=iterative over-fetch(접근 A). 실수요 시 A부터 (ADR-048) | 릴리스 후 기능(보류) |
| 3S | statement_timeout / 취소 전파 — UDS recv timeout + CHECK_FOR_INTERRUPTS + CUVS_OP_CANCEL, 연결 고갈 방지 | 릴리스 후 기능 |
| 3P | IVF-PQ — 새 AM `USING ivfpq` (product quantization, VRAM 10–100× 절감, 100M+ 대용량) | 릴리스 후 기능 |
| 3Q | CAGRA Streaming Updates — `cuvsCagraExtend`(INSERT) + `cuvsCagraMerge`+cuvsFilter(DELETE/컴팩션) 실시간 인덱스 업데이트, .delta 경로 대체 | 릴리스 후 기능 |
| 4C | Background Compaction + CONCURRENTLY 정합성 — PG bgworker auto-REINDEX + DELETE 정합 검증 | 릴리스 후 기능 |
| 3C / 3D | GCS artifact snapshot 본체 / Replica async warmup | 트리거 (multi-node 수요) |
| fallback 관측성 · circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 | 트리거 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |
| fp16 입력 | float16 벡터 입력으로 VRAM ~50% 절감 — `WITH (precision=fp16)` reloption, cuVS C API 지원 확인 필요 | 트리거 (cuVS fp16 지원 확인) |
| EXPLAIN GPU 타이밍 | `EXPLAIN ANALYZE`에 GPU kernel time / IPC latency 노출, 쿼리별 병목 진단 | 트리거 (명시 진단 수요) |
| VACUUM tombstone 연동 | autovacuum 시 `CUVS_OP_COMPACT` 자동 트리거 — 별도 bgworker 없이 PG 스케줄 재활용 (3Q 의존) | 트리거 (3Q 완료) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실이고, Phase 코드(`3A`/`3K` 등)가 안정적 식별자다. 살아있는 시퀀스에는 **미완 순차 작업만 트랙 이름으로** 둔다(번호 `Wave N`을 붙이지 않는다 — 완료할 때마다 renumber하면 같은 번호가 다른 걸 가리켜 꼬이므로). 완료 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 항목은 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다.

### 릴리스 후 기능 (순차)

> **3A Pending Delta는 완료**(완료 표 참조). streaming write(INSERT/UPDATE/DELETE) 후 REINDEX 없이 GPU+delta 병합으로 정합한 top-k를 반환한다. 3L `CuvsBfIndex`를 3A-2 GPU delta cache가 재사용. 상세 스펙·검증은 [design/PLAN.md — Phase 3A](design/PLAN.md), 결정은 ADR-047. **4A(빌드 오버헤드)·3R(빌드 파라미터 reloption)도 완료**(완료 표 참조; 4A=ADR-057/058/059, 3R=ADR-052) — **3O(Pre-filter ANN)는 보류**(접근 미정, ADR-048) — 순차 경로는 3S부터다.

> **3O 보류 (2026-06-07)**: 착수 직전 코드 확인에서 PG 인덱스 AM이 비-인덱스-컬럼 WHERE qual을 AM에 안 넘긴다는 핵심 장벽 발견. 원안(WHERE→bitvector→daemon, 접근 B)은 custom scan/planner hook + cuVS bitset API 필요 → 멀티세션·고위험. 저비용 대안 접근 A(iterative over-fetch)가 사용자 문제를 ~1세션에 해결. 실수요 미확인이라 보류, 재개 시 접근 A 우선. 분석·결정은 ADR-048, [design/PLAN.md — Phase 3O](design/PLAN.md).

#### 3S — statement_timeout / 취소 전파
**왜**: 다음 순차(3O 보류). PG cancel이 daemon IPC로 전파되지 않아 GPU 검색이 걸리면 statement_timeout 이후에도 backend가 대기. 연결 고갈 방지 필수.

구현 항목:
- UDS recv에 `poll(fd, ipc_timeout_ms)` 루프 + `CHECK_FOR_INTERRUPTS()` 감지
- 취소 시 daemon에 `CUVS_OP_CANCEL` 전송 후 오류 반환
- GUC `cuvs.ipc_timeout_ms` (기본 5000ms)
- daemon: cancel op 수신 시 진행 중 검색 중단 후 연결 상태 초기화

완료 기준: `statement_timeout = '1s'` 설정 후 느린 GPU 검색에서 timeout 확인, 연결 정상 반환 확인, 기존 test suite PASS

스펙: [design/PLAN.md — Phase 3S](design/PLAN.md) | ADR-053

#### 3P — IVF-PQ (추가 cuVS 알고리즘)
**왜**: 3O 완료 후. CAGRA는 VRAM에 float32 전체 보유 필요 — 대용량(100M+) 환경에서 비실용적. IVF-PQ로 VRAM 10–100× 절감.

구현 항목:
- 새 AM handler `pg_cuvs_ivfpq_handler` 등록 (`CREATE INDEX USING ivfpq`)
- reloption: `n_lists`, `pq_bits`, `pq_dim`
- GUC: `cuvs.ivfpq_n_probes`
- daemon: `CUVS_OP_BUILD_IVFPQ`, `CUVS_OP_SEARCH_IVFPQ` op 추가

완료 기준: N=1M build 성공, recall@10 ≥ 0.90, VRAM CAGRA 대비 10× 절감 실측, 기존 test suite PASS

스펙: [design/PLAN.md — Phase 3P](design/PLAN.md) | ADR-049

#### 3Q — CAGRA Streaming Updates
**왜**: 3P 완료 후. cuVS 26.04 C API(`cuvsCagraExtend`, `cuvsCagraMerge`, `cuvsFilter`)로 INSERT/DELETE를 .delta 파일 없이 VRAM 내에서 직접 처리. delta 누적에 따른 search-time 병합 비용을 제거하고 recall 유지.

구현 항목:
- `CUVS_OP_EXTEND`: IPC op + daemon `cuvsCagraExtend` + disk serialize(내구성)
- `CUVS_OP_COMPACT`: `cuvsCagraMerge(filter=tombstone bitvector)` → new index atomic swap → old VRAM 해제
- GUC `cuvs.extend_chunk_size` (`max_chunk_size` 제어, 0=auto)
- VRAM 예산 갱신 (extend grow 반영)
- `aminsert` → `CUVS_OP_EXTEND` 전환, 3A .delta 경로 deprecated

완료 기준: INSERT/DELETE/UPDATE e2e 검증(recall@10 동일성), .delta 미생성 확인, 기존 test suite PASS

스펙: [design/PLAN.md — Phase 3Q](design/PLAN.md) | ADR-051

#### 4C — Background Compaction + CREATE INDEX CONCURRENTLY 정합성
**왜**: 3P 완료 후. delta 수동 REINDEX 운용 부담 제거. CONCURRENTLY DELETE 정합성 검증은 4C 착수 전 선행 필수.

구현 항목:
- 4C-0 선행: REINDEX CONCURRENTLY 동작 검증 + DELETE concurrent build isolation 테스트
- `cuvs_compaction_worker` bgworker 등록
- GUC: `cuvs.auto_compact`, `cuvs.auto_compact_check_interval`, `cuvs.auto_compact_threshold`
- `pg_stat_gpu_search`에 `last_compact_at`, `compact_count` 컬럼 추가

완료 기준: auto_compact=on에서 delta 초과 인덱스 자동 REINDEX e2e 검증, CONCURRENTLY DELETE isolation GREEN, 기존 test suite PASS

스펙: [design/PLAN.md — Phase 4C](design/PLAN.md) | ADR-050

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

#### fp16 입력 벡터 (트리거: cuVS C API fp16 지원 확인)
**효과**: float16 입력으로 VRAM ~50% 절감. `WITH (precision=fp16)` reloption.
**전제**: VM에서 `cuvsCagraBuild`에 `CUDA_R_16F` dtype 전달 가능 여부 확인 필요.
**트리거**: cuVS C API fp16 지원 확인 + recall 저하 < 0.5% 실측.

스펙: [design/PLAN.md — fp16 입력 벡터](design/PLAN.md) | ADR-054

#### EXPLAIN ANALYZE GPU 타이밍 (트리거: 명시 진단 수요)
**효과**: `EXPLAIN ANALYZE`에 GPU kernel time / IPC latency 노출. 쿼리별 병목 진단 가능.
**구현**: daemon 응답 프레임에 `gpu_kernel_us`/`ipc_roundtrip_us` 추가, custom scan `ExplainCustomScan` 콜백에서 주입.
**트리거**: 프로덕션 배포 후 쿼리별 GPU latency 분해 수요 명시.

스펙: [design/PLAN.md — EXPLAIN GPU 타이밍](design/PLAN.md) | ADR-055

#### VACUUM 연동 tombstone 정리 (트리거: 3Q 완료)
**효과**: autovacuum 시 `CUVS_OP_COMPACT` 자동 트리거 — 별도 bgworker 없이 PG 스케줄 재활용. 4C와 동일 COMPACT op 공유.
**트리거**: 3Q 완료 + autovacuum 중 tombstone 지연 정리가 실측 문제로 확인.

스펙: [design/PLAN.md — VACUUM tombstone 연동](design/PLAN.md) | ADR-056

---

## Phase 4B 현황

`import_hnsw` 페이지 write 병목 — **현상 유지** (UNLOGGED ~28s).
병렬 page write / Bulk WAL은 PG buffer manager 제약으로 단기 제외.
재검토 조건: PG 코어에 bulk page write API 추가 또는 `ReadBuffer` concurrent extension 완화.

스펙: [design/PLAN.md — Phase 4B](design/PLAN.md) | ADR-035
