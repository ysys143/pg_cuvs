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
| 3O | Pre-filter ANN — CAGRA-first BITSET prefilter (ADR-048). daemon 빌드 타임에 `rev_tids[]`(sorted)+`rev_item_ids[]` 역방향 맵 구성. 쿼리 타임에 필터 TID → item_id 이진탐색 → GPU BITSET. `handle_search` 3O 경로: CAGRA prefilter 우선(`cuvs_cagra_search_filtered`, approx/graph-based), 실패 시 BF prefilter fallback(`cuvs_bf_search_filtered`, exact). `use_prefilter` IPC 플래그. `cuvs.filter_auto_threshold` GUC(기본 0.05). `last_search_mode=4`(cagra_prefilter)/3(bf_prefilter). PR #36(BF prefilter), #37(CAGRA-first). installcheck 19/19 + isolation 2/2 GREEN. **비고**: 역방향 맵이 `finish_build_commit` fresh-build 분기에서 미빌드돼, 갓 빌드된 인덱스(첫 CREATE INDEX/eviction 후 재빌드)에서 prefilter가 데몬 재시작/in-place REINDEX 전까지 조용히 D-wedge로 강등되던 false-done을 PR #39에서 근본 수정(공유 헬퍼 1줄로 양 빌드 경로 커버). 원인: 완료 증거(`mode=4`)가 비대표적 상태(재시작 후 load 경로)에서만 수집됨 + `filter_comparison`이 mode를 assert 안 함. mode assertion 추가로 재발 방지(ADR-064 stream BF 작업 중 발견) |
| 3P | IVF-PQ AM — `CREATE INDEX USING ivfpq`, reloptions(`n_lists`/`pq_bits`/`pq_dim`), `cuvs.ivfpq_n_probes` GUC(default 64). PQ codes 내부 저장으로 VRAM 10–100× 절감. `.tids`+`.ivfpq` 사이드카. `default_version` 0.2.0으로 상향. installcheck 20/20 + isolation 2/2 GREEN (ADR-049) |
| 3Q | CAGRA Streaming Updates — `cuvsCagraExtend`(INSERT) + `cuvsCagraMerge`+cuvsFilter(DELETE/컴팩션) 실시간 인덱스 업데이트, .delta 경로 대체. VACUUM tombstone 연동(`cuvs_amvacuumcleanup`) 포함. INSERT/DELETE/UPDATE e2e · .delta 미생성 · vram_bytes 갱신 Scenario 6-8 PASS. installcheck 21/21 + isolation 2/2 GREEN (ADR-051) |
| 4C | Background Compaction + CONCURRENTLY 정합성 — PG bgworker auto-REINDEX + 4 GUC(`cuvs.auto_compact` 외 3종) + `pg_stat_gpu_search`에 `extend_count`/`compact_count`/`last_compact_at` 관측성 컬럼 추가. REINDEX CONCURRENTLY+DELETE isolation 테스트(3/3 GREEN). extend_count→compact_count 갱신 e2e(auto_compact.sql). installcheck 22/22 + isolation 3/3 GREEN (ADR-050) |
| 3C / 3D | GCS artifact snapshot + replica async warmup — manifest/checksum/version 기반 GCS upload(빌드 후 detached)/download(warmup cold-miss). unsharded + sharded(3G.2) 양쪽. fail-closed: corrupt(SHA256)/heap-incompat(relfilenode hard-reject)/cuVS-version(매니페스트 버전 게이트). 3D warmup 풀·cold 등록·cache-miss requeue·`pg_stat_gpu_search` warmup 컬럼. **인증**: A100에서 실 ephemeral GCS 버킷 round-trip(`make gpu-test-objstore`) — 업로드·warmup 하이드레이션 recall 일치·3종 fail-closed reject·버킷 생성/파괴 클린. installcheck 25/25 + isolation 3/3 GREEN (ADR-013/ADR-066). **비고**: 본체는 3F/3G 작업 중 배선됐으나 SSOT가 "미완료"로 뒤처진 reverse false-done이었음 — 실 GCS 검증 부재 + 매니페스트 빈틈(version 스탬프·base_generation) 보강 후 인증. emulator 기반 CI 회귀는 후속(트리거) |

### 미완료

> **전략 재평가 (2026-06-07, ADR-061 / [design/STRATEGY_NOTES.md](design/STRATEGY_NOTES.md))**: 경쟁 데이터(VectorChord가 32억 벡터를 CPU+NVMe 월 $12k로 서빙)가 "규모=GPU" 전제를 깸. pg_cuvs 표적 = **쿼리당-비용 지배 세그먼트**(온라인 RAG, 멀티테넌트). 신규 1순위 후보 = **exact filtered brute-force**(D) — cuVS `cuvsBruteForceSearch` + BITSET prefilter 네이티브 지원 확인됨. 3O는 D의 post-filter를 BITSET pre-filter로 확장하는 순차 작업으로 재정의(대체 아님). 3P(아래)는 "규모 핵심" → "VRAM working-set 천장"으로 격하.

| Phase | 내용 | 트랙 |
|-------|------|------|
| fallback 관측성 · circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 | 트리거 |
| ~~MAX_INDEXES 상향/동적화 + 런타임-축출 auto-reload~~ [OK] | 다중 테넌트 파티션 온라인-스케일 선결. 측정·근거 ADR-061 / STRATEGY_NOTES §G | **완료** (ADR-068): 레지스트리를 *하드월*→*소프트 LRU 캡*으로 전환 — `--max-indexes`(기본 1024, was 64, calloc) + `load_index` 슬롯-확보 eviction(auto-reload 배선) + build 경로 graceful defer. `gpu-test-maxidx`: cap 4에 10테넌트 빌드 ERROR 0 + 전 테넌트 쿼리 GPU reload(evict=16/reload=10). installcheck 26/26 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |
| fp16 입력 | float16 벡터 입력으로 VRAM ~50% 절감 — `WITH (precision=fp16)` reloption, cuVS C API 지원 확인 필요 | 트리거 (cuVS fp16 지원 확인) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실이고, Phase 코드(`3A`/`3K` 등)가 안정적 식별자다. 살아있는 시퀀스에는 **미완 순차 작업만 트랙 이름으로** 둔다(번호 `Wave N`을 붙이지 않는다 — 완료할 때마다 renumber하면 같은 번호가 다른 걸 가리켜 꼬이므로). 완료 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 항목은 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다.

### 릴리스 후 기능 (순차)

> **3A Pending Delta는 완료**(완료 표 참조). streaming write(INSERT/UPDATE/DELETE) 후 REINDEX 없이 GPU+delta 병합으로 정합한 top-k를 반환한다. 3L `CuvsBfIndex`를 3A-2 GPU delta cache가 재사용. 상세 스펙·검증은 [design/PLAN.md — Phase 3A](design/PLAN.md), 결정은 ADR-047. **4A(빌드 오버헤드)·3R(빌드 파라미터 reloption)도 완료**(완료 표 참조; 4A=ADR-057/058/059, 3R=ADR-052), **3S(취소 전파)도 완료**(ADR-053), **D(exact filtered BF)도 완료**(ADR-063, 잔여 4항목 포함), **3O(CAGRA-first BITSET prefilter)도 완료**(ADR-048, PR #36/#37), **3Q(CAGRA Streaming Updates)도 완료**(ADR-051, installcheck 21/21), **4C(Background Compaction)도 완료**(ADR-050, installcheck 22/22 + isolation 3/3), **3C/3D(GCS snapshot + replica async warmup)도 완료·인증**(ADR-013/ADR-066, 실 GCS round-trip `make gpu-test-objstore`, installcheck 25/25 + isolation 3/3) — 기능 순차 경로 완료. **repo 공개 전 운영 하드닝 3종(fallback 관측성=PR #43 · VRAM budget 강제=ADR-065 해소 · OOM 후 재사용=PR #42)도 완료**. **MAX_INDEXES 하드월도 해소**(ADR-068, PR — 소프트 LRU 캡 `--max-indexes` 기본 1024 + 슬롯-확보 auto-reload). **다음 순차 작업: 릴리스 준비**(README 정비 · `BENCHMARK.md` 공개 · CI GPU 전략 확정=ADR-067 — "에코시스템 진입 계획" 전제조건 참조). **릴리스 후 순차: 엄밀 벤치마크 + 코스트모델 보정**(ADR-069, [design/BENCHMARK_PROTOCOL.md](design/BENCHMARK_PROTOCOL.md) — Stage A 물리 실측 → Stage B regret 보정 루프 → 동결 → 필터/증분/Pareto 스위트. 논문 트랙 R1–R5는 별도 일정).

---

## 트리거 기반 백로그 (번호 없음 — 조건 충족 시 승격)

순차 경로(릴리스 후 기능)와 섞지 않는다. 각 항목은 트리거가 충족될 때 순차 트랙으로 승격한다.

### 분산 운영 — 3C/3D (완료·인증 — 완료 표 참조)

> **완료 (2026-06-10, ADR-013/ADR-066)**: 아래 스펙은 모두 구현·인증됐다. 실 GCS round-trip + fail-closed가 A100에서 `make gpu-test-objstore`로 검증됐고(installcheck 25/25 + isolation 3/3), SSOT는 완료 표로 이전됐다. 이 절은 원 스펙 기록으로 보존한다. 잔여 후속(트리거): emulator 기반 CI 회귀(`STORAGE_EMULATOR_HOST` + fake-gcs-server), S3 provider.

#### 3C → 3D — GCS Snapshot + Replica Async Warmup
**왜 repo 공개 전에**: read replica는 production PostgreSQL 운용의 기본 패턴. 3C/3D 없이 공개하면 외부 사용자가 replica 설정에서 첫 번째 벽에 부딪힌다 — GCS snapshot 없이 각 replica가 REINDEX를 독립 실행해야 하고(GPU 빌드 비용 × 노드 수), 3D 없이 cold-start QPS가 무너진다.

- **3C**: manifest/checksum/version 기반 GCS upload/download 전체 경로. **3F/3G sharded 아티팩트 필수 지원**: unsharded(`.cagra`/`.tids`/`.vectors`)와 sharded(`.shards` manifest + `.s000.cagra`/`.s001.cagra`/...) 양쪽 모두 처리해야 함 — sharded 인덱스 사용자의 replica warmup이 3C에 의존. 한쪽만 구현하면 3F/3G를 사용하는 환경에서 replica 설정 불가
- **3D**: background warmup thread pool, cold entry 등록, cache miss CPU fallback. sharded 인덱스의 경우 shard별 warmup 순서 및 부분 warmup 중 fallback 처리 포함
- **CI 주의**: 3C sharded 경로 테스트는 multi-GPU 환경 필요 (3F-6과 동일 조건). single-GPU mock CI로는 sharded snapshot 경로 커버 불가 — GPU CI 전략 확정 시 반영 필요

스펙: [design/PLAN.md — Phase 3C, 3D](design/PLAN.md) | ADR-013

### 운영 하드닝 잔여 (트리거별)

2026-06-05 스펙 무결성 감사(`docs/spec-audit-2026-06-05.md`) + AI council 논의에서 누적. **운영자 대면 절차의 단일 산출물은 `design/OPS_GPU_PLAYBOOK.md`**. release급 emit(TOAST NOTICE / index_dir WARNING / pgvector 가드)은 Release-hardening으로 **완료됨**(완료 표 참조). 아래는 트리거 대기분.

| 항목 | 근거 | 성격 | 트리거 |
|------|------|------|--------|
| ~~**fallback 관측성**~~ [OK] | 감사 #2. `pg_stat_gpu_search`에 fallback 카운터 부재 — 쿼리가 조용히 CPU로 새는지 SQL로 알 수 없음(fallback은 backend plan-time 결정이라 데몬 미도달). | 코드(backend shmem 카운터 → view) | **완료** (PR #43): 신규 `pg_stat_gpu_fallback` 뷰 + 백엔드 shmem 카운터(`cuvsamcostestimate`가 사유별 기록), `fallback_stat` 테스트, installcheck 26/26 |
| ~~**VRAM budget 강제**~~ [OK] | `set_vram_budget(0)` 기본 무제한 + raw `cudaMemGetInfo` 신뢰 불가(CUDA async mempool 캐싱). | 코드(기본값 재검토 + mempool-aware, ADR-065) | **완료** (ADR-065 해소): 기본 budget = 총 VRAM의 90%(데몬 자체 회계로 강제, raw 조회 불요) + `cudaMemPoolGetAttribute` 기반 free 보정(best-effort). `gpu-test-vram` 36418/40465 MB PASS. **동적 sizing은 별개 follow-up** |
| ~~**OOM 후 인덱스 재사용 미검증**~~ [OK] | CAGRA extend OOM 시 `_pr.poison()` 이후 재빌드 없이 동일 인덱스 쿼리 동작 불명. | 테스트(OOM 복구 → 재빌드 없이 쿼리) | **완료** (PR #42): `extend_cuda_oom`에 resident CAGRA 그래프 무결성(원본값 NN, tie-robust) + GPU-served assert 추가 |
| **circuit breaker 전역화** | 감사 #5. breaker가 백엔드 프로세스-로컬(shared 아님) — 동시 연결에서 GPU 장애 시 전역 보호 안 됨(백엔드당 상한은 있음). | 코드(shared-memory breaker 상태) | 3C/3D 완료(프로덕션 배포 시점) |
| **SQL latency split** | 감사 #1. `pg_stat_gpu_search`에 GPU/IPC/recheck 분해 미노출. | 코드(데몬 계측 + IPC + SQL 컬럼) | 명시 요청 시(ADR-044가 외부 측정 완료, SQL 노출 한계가치 낮음) |
| **delta 누적 성능 저하 관측성** | brute-force 머지가 O(n_delta)라 delta 누적 시 검색 성능 저하. 현재 SQL로 "지금 REINDEX 해야 하나" 판단 기준 없음 — `pg_stat_gpu_search`에 delta 누적 경보 signal 미노출. | 코드(`pg_stat_gpu_search`에 `delta_ratio` 또는 `delta_warn` 컬럼 추가) | delta 누적 운영 문제 실측 시 |
| **자동 티어링 없음** | VRAM 압박 시 CAGRA → IVF-PQ / HNSW 자동 강등 없음. LRU 축출(3E/3F/3G)은 인덱스를 디스크로 내리지만 포맷 변환은 수동 — 운영자가 직접 `CREATE INDEX USING ivfpq` 또는 `pg_cuvs_build_hnsw()` 재실행 필요. VRAM 초과 시 `안전하게 실패`는 보장하나 `자동 품질 강등`은 미구현. | 코드(VRAM 임계값 도달 시 daemon이 대상 인덱스를 IVF-PQ로 자동 변환 또는 HNSW export 트리거) | VRAM 관리 자동화 명시 수요 시 (3P + 3I 완료가 선결) |

스펙: `docs/spec-audit-2026-06-05.md` | ADR-011 / ADR-017 | `design/OPS_GPU_PLAYBOOK.md`

### 기타

#### Streaming BF — sidecar-gather 경로 — **1차 구현 완료** (ADR-064)
**왜**: VRAM을 초과하는 데이터셋에서 고선택성 필터 쿼리를 GPU로 처리하는 경로. 현재 GPU 검색은 인덱스 전체가 VRAM에 상주해야 동작 — VRAM 초과 시 OOM 또는 multi-GPU sharding만 가능.
**구현**: `CUVS_OP_SEARCH_STREAM_BF` + `handle_search_stream_bf()`. 3O 역방향 맵(`heapTID → item_id`) 재활용 → 필터 통과 벡터만 `.vectors` sidecar에서 `pread`로 gather(전체 상주 없음) → `cuvs.stream_bf_chunk_vectors` 단위 청크 GPU BF → host running top-k 머지. `last_search_mode=6`(stream_bf). 자동 전환 GUC `cuvs.stream_bf_selectivity_threshold`(기본 0.0=off; 미만이면 stream, 3O보다 우선, 사이드카 부재 시 3O 폴백). 검증: `stream_bf_recall.sql`(강제 경로 + CPU exact 일치 + parity). installcheck 25/25 + isolation 3/3 GREEN.
**적소**: 고선택성 필터(필터 통과 비율 낮음) + VRAM 초과 데이터. 저선택성은 gather 비용이 이득을 상쇄.
**제약**: 청크 크기 = 정확도 무관(running 머지는 임의 청킹에 exact) → GPU 잔여 조회 불필요, `cudaMemGetInfo` 미사용(ADR-065 준수). 고정 cap GUC.
**후속(트리거)**: (1) mempool-aware 청크 auto-sizing(ADR-065 follow-up), (2) selectivity×Q 자동 라우팅(현재 수동 GUC), (3) VRAM 초과 대규모 스케일 실측(회귀 CI 밖).

스펙: ADR-064

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
| ~~**3C/3D 완료**~~ [OK] | GCS artifact snapshot + replica async warmup — **완료·인증** (ADR-013/ADR-066, `make gpu-test-objstore`). 잔여: emulator CI 회귀(트리거) |
| GitHub repo 공개 | public release. 라이선스: **Apache 2.0** (확정) |
| 재현 가능한 벤치마크 공개 | `BENCHMARK.md` — 핵심은 **overhead characterization**: GPU가 distance computation을 제거하면 IPC / PG heap fetch가 새 병목이 된다는 것을 latency 분해로 실증. pgvector(CPU HNSW) 대비 QPS/latency 비교는 부수. selectivity sweep은 멀티테넌트 filtered search 효과 지지 실험으로 포함(논문 중심 아님) |
| 외부 사용자용 설치 가이드 | README 정비 (설치, quick start, CUDA/cuVS 버전 매트릭스) |
| CI — GPU 테스트 전략 | **전략 확정** (ADR-067, 스펙 [design/CI_STRATEGY.md](design/CI_STRATEGY.md)): 2-tier — Tier 1 CPU-reference shim(`cuvs_wrapper.h` 경계 대체, hosted ubuntu, 매 PR 자동, 무료, plumbing·계약·mode·recall 검증) + Tier 2 실 A100 installcheck(self-hosted, 사용자 on-demand `/gpu-test`). 구현 미착수(shim TU + workflow 2종). |

### 진입 단계

| 단계 | 목표 | 전제 조건 | 타이밍 |
|------|------|-----------|--------|
| 1 | repo 공개 + 벤치마크 공개 | 없음 | 즉시 가능 |
| 2 | cuvs-bench backend PR | 3Q 완료 [OK] | 즉시 착수 가능 |
| 3 | cuVS 문서/README 링크 요청 | 2단계 merge | 2단계 후 |
| 4 | NVIDIA 채널 노출 | 3단계 등재 | 3단계 후 |

**현황**: PostgreSQL 관련 언급이 cuVS 생태계에 전무 — 선점 기회. 현재 공식 통합: Milvus, Faiss, Elasticsearch(진행 중), Kinetica.
