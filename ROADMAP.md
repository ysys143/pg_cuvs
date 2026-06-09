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
| 3S | statement_timeout / 취소 전파 — backend reply 대기를 `recv_all_interruptible`(poll + wait 콜백)로 인터럽트 가능하게: `statement_timeout`/cancel이 걸린 GPU 검색을 ~544ms에 끊음(이전 무기한). 데몬 `SIGPIPE` 무시로 client mid-reply disconnect에서 생존(기존 잠재 크래시 버그도 해소). `CUVS_OP_CANCEL` 미도입(소켓 close로 충분). integration sc24 + installcheck 17/17 GREEN (ADR-053) |
| D | Exact filtered BF (D-wedge) — **전체 완료** (ADR-063). Option B(`cuvs_filtered_knn` SRF bigint[]+tid[] 오버로드, 4x overfetch, NULL→unfiltered fallback) + Option A(Custom Scan hook `cuvs.filtered_knn_hook`). IPC `CuvsCmdFrame` filter_shm_key 확장, daemon binary-search post-filter. 잔여 4항목 완료: (1) `tid[]` 타입-안전 wrapper SQL 오버로드, (2) `ExplainCustomScan` 콜백으로 EXPLAIN ANALYZE GPU IPC latency 노출, (3) `.delta` + tombstone 통합(`cuvs_merge_delta_filtered` / `cuvs_apply_tombstones_filtered`), (4) selectivity×correlation 2D 실험으로 `cuvs.filter_auto_threshold=0.05` 근거 실측(`docs/filter-threshold-experiment.md`). installcheck 19/19 + isolation 2/2 GREEN |
| 3O | Pre-filter ANN — CAGRA-first BITSET prefilter (ADR-048). daemon 빌드 타임에 `rev_tids[]`(sorted)+`rev_item_ids[]` 역방향 맵 구성. 쿼리 타임에 필터 TID → item_id 이진탐색 → GPU BITSET. `handle_search` 3O 경로: CAGRA prefilter 우선(`cuvs_cagra_search_filtered`, approx/graph-based), 실패 시 BF prefilter fallback(`cuvs_bf_search_filtered`, exact). `use_prefilter` IPC 플래그. `cuvs.filter_auto_threshold` GUC(기본 0.05). `last_search_mode=4`(cagra_prefilter)/3(bf_prefilter). PR #36(BF prefilter), #37(CAGRA-first). installcheck 19/19 + isolation 2/2 GREEN |
| 3P | IVF-PQ AM — `CREATE INDEX USING ivfpq`, reloptions(`n_lists`/`pq_bits`/`pq_dim`), `cuvs.ivfpq_n_probes` GUC(default 64). PQ codes 내부 저장으로 VRAM 10–100× 절감. `.tids`+`.ivfpq` 사이드카. `default_version` 0.2.0으로 상향. installcheck 20/20 + isolation 2/2 GREEN (ADR-049) |
| 3Q | CAGRA Streaming Updates — `cuvsCagraExtend`(INSERT) + `cuvsCagraMerge`+cuvsFilter(DELETE/컴팩션) 실시간 인덱스 업데이트, .delta 경로 대체. VACUUM tombstone 연동(`cuvs_amvacuumcleanup`) 포함. INSERT/DELETE/UPDATE e2e · .delta 미생성 · vram_bytes 갱신 Scenario 6-8 PASS. installcheck 21/21 + isolation 2/2 GREEN (ADR-051) |
| 4C | Background Compaction + CONCURRENTLY 정합성 — PG bgworker auto-REINDEX + 4 GUC(`cuvs.auto_compact` 외 3종) + `pg_stat_gpu_search`에 `extend_count`/`compact_count`/`last_compact_at` 관측성 컬럼 추가. REINDEX CONCURRENTLY+DELETE isolation 테스트(3/3 GREEN). extend_count→compact_count 갱신 e2e(auto_compact.sql). installcheck 22/22 + isolation 3/3 GREEN (ADR-050) |

### 미완료

> **전략 재평가 (2026-06-07, ADR-061 / [design/STRATEGY_NOTES.md](design/STRATEGY_NOTES.md))**: 경쟁 데이터(VectorChord가 32억 벡터를 CPU+NVMe 월 $12k로 서빙)가 "규모=GPU" 전제를 깸. pg_cuvs 표적 = **쿼리당-비용 지배 세그먼트**(온라인 RAG, 멀티테넌트). 신규 1순위 후보 = **exact filtered brute-force**(D) — cuVS `cuvsBruteForceSearch` + BITSET prefilter 네이티브 지원 확인됨. 3O는 D의 post-filter를 BITSET pre-filter로 확장하는 순차 작업으로 재정의(대체 아님). 3P(아래)는 "규모 핵심" → "VRAM working-set 천장"으로 격하.

| Phase | 내용 | 트랙 |
|-------|------|------|
| 3C / 3D | GCS artifact snapshot 본체 / Replica async warmup | 트리거 (multi-node 수요) |
| fallback 관측성 · circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 | 트리거 |
| MAX_INDEXES 상향/동적화 + 런타임-축출 auto-reload | 다중 테넌트 파티션 온라인-스케일 선결 — 현 `MAX_INDEXES=64`가 하드월(>64 파티션 축출 시 ERROR+REINDEX, auto-reload 미배선). 측정·근거 ADR-061 / STRATEGY_NOTES §G | 트리거 (수백+ 테넌트 수요) |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |
| fp16 입력 | float16 벡터 입력으로 VRAM ~50% 절감 — `WITH (precision=fp16)` reloption, cuVS C API 지원 확인 필요 | 트리거 (cuVS fp16 지원 확인) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실이고, Phase 코드(`3A`/`3K` 등)가 안정적 식별자다. 살아있는 시퀀스에는 **미완 순차 작업만 트랙 이름으로** 둔다(번호 `Wave N`을 붙이지 않는다 — 완료할 때마다 renumber하면 같은 번호가 다른 걸 가리켜 꼬이므로). 완료 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 항목은 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다.

### 릴리스 후 기능 (순차)

> **3A Pending Delta는 완료**(완료 표 참조). streaming write(INSERT/UPDATE/DELETE) 후 REINDEX 없이 GPU+delta 병합으로 정합한 top-k를 반환한다. 3L `CuvsBfIndex`를 3A-2 GPU delta cache가 재사용. 상세 스펙·검증은 [design/PLAN.md — Phase 3A](design/PLAN.md), 결정은 ADR-047. **4A(빌드 오버헤드)·3R(빌드 파라미터 reloption)도 완료**(완료 표 참조; 4A=ADR-057/058/059, 3R=ADR-052), **3S(취소 전파)도 완료**(ADR-053), **D(exact filtered BF)도 완료**(ADR-063, 잔여 4항목 포함), **3O(CAGRA-first BITSET prefilter)도 완료**(ADR-048, PR #36/#37), **3Q(CAGRA Streaming Updates)도 완료**(ADR-051, installcheck 21/21), **4C(Background Compaction)도 완료**(ADR-050, installcheck 22/22 + isolation 3/3) — 릴리스 후 기능 순차 경로 완료. 다음 순차 작업은 트리거 기반 백로그 참조.

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

#### EXPLAIN ANALYZE GPU 타이밍 (D 잔여로 승격)
**상태**: D 잔여로 이동. 스펙: [design/PLAN.md — EXPLAIN GPU 타이밍](design/PLAN.md) | ADR-055

#### VACUUM 연동 tombstone 정리 — 완료 (3Q 포함)
`cuvs_amvacuumcleanup`에서 tombstone 존재 시 `CUVS_OP_COMPACT` 자동 호출로 구현됨. 3Q 완료 기준에 포함(Scenario 5 PASS). 별도 트리거 불필요.

스펙: [design/PLAN.md — VACUUM tombstone 연동](design/PLAN.md) | ADR-056

---

## Phase 4B 현황

`import_hnsw` 페이지 write 병목 — **현상 유지** (UNLOGGED ~28s).
병렬 page write / Bulk WAL은 PG buffer manager 제약으로 단기 제외.
재검토 조건: PG 코어에 bulk page write API 추가 또는 `ReadBuffer` concurrent extension 완화.

스펙: [design/PLAN.md — Phase 4B](design/PLAN.md) | ADR-035

---

## cuVS 에코시스템 진입 계획

상세: [docs/ecosystem-strategy.md](docs/ecosystem-strategy.md) | ADR-062

### 전제 조건 (진입 전 필수)

| 항목 | 필요 작업 |
|------|-----------|
| GitHub repo 공개 | public release + 라이선스 확인 |
| 재현 가능한 벤치마크 공개 | `BENCHMARK.md` (pgvector vs pg_cuvs 핵심 수치). **논문화 시 수준 상향**: selectivity × correlation 2축 체계적 sweep(논문 수준 = 복수 dataset × 다단계 selectivity × correlation 유형별) — filter_comparison.sql 확장 형태로 구축, D 잔여 또는 3O 완료 기준에 포함 |
| 외부 사용자용 설치 가이드 | README 정비 (설치, quick start) |
| 기본 CI | GitHub Actions 최소 구성 |

### 진입 단계

| 단계 | 목표 | 전제 조건 | 타이밍 |
|------|------|-----------|--------|
| 1 | repo 공개 + 벤치마크 공개 | 없음 | 즉시 가능 |
| 2 | cuvs-bench backend PR | 3Q 완료 [OK] | 즉시 착수 가능 |
| 3 | cuVS 문서/README 링크 요청 | 2단계 merge | 2단계 후 |
| 4 | NVIDIA 채널 노출 | 3단계 등재 | 3단계 후 |

**현황**: PostgreSQL 관련 언급이 cuVS 생태계에 전무 — 선점 기회. 현재 공식 통합: Milvus, Faiss, Elasticsearch(진행 중), Kinetica.
