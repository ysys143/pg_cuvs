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
| circuit breaker 전역화 · SQL latency split | 운영 하드닝 잔여 (fallback 관측성은 PR #43 완료) | 트리거 |
| ~~MAX_INDEXES 상향/동적화 + 런타임-축출 auto-reload~~ [OK] | 다중 테넌트 파티션 온라인-스케일 선결. 측정·근거 ADR-061 / STRATEGY_NOTES §G | **완료** (ADR-068): 레지스트리를 *하드월*→*소프트 LRU 캡*으로 전환 — `--max-indexes`(기본 1024, was 64, calloc) + `load_index` 슬롯-확보 eviction(auto-reload 배선) + build 경로 graceful defer. `gpu-test-maxidx`: cap 4에 10테넌트 빌드 ERROR 0 + 전 테넌트 쿼리 GPU reload(evict=16/reload=10). installcheck 26/26 |
| 3N | OFFSET-aware K 자동 조정 (ORM pagination 호환) | 트리거 (ORM 요구) |
| fp16 CAGRA reloption | BF용 fp16은 **3L `cuvs.bf_precision` 완료·출고**(`cuvs_wrapper.cu` device fp16 사본, BF VRAM 절반). 잔여 = CAGRA `WITH (precision=fp16)` reloption 표면만. cuVS C API fp16은 BF에서 실증돼 **트리거 충족** | 트리거 (수요 시 즉시 착수) |

---

## 구현 순서

> **원칙(2026-06-06)**: '완료' 표가 완료의 단일 진실이고, Phase 코드(`3A`/`3K` 등)가 안정적 식별자다. 살아있는 시퀀스에는 **미완 순차 작업만 트랙 이름으로** 둔다(번호 `Wave N`을 붙이지 않는다 — 완료할 때마다 renumber하면 같은 번호가 다른 걸 가리켜 꼬이므로). 완료 Phase는 시퀀스에서 제외(상세는 완료 표 + `design/PLAN.md`). **분산·운영 하드닝 등 조건/트리거 항목은 '트리거 기반 백로그'로 분리**해 순차 경로와 섞지 않는다.

### 릴리스 후 기능 (순차)

> **3A Pending Delta는 완료**(완료 표 참조). streaming write(INSERT/UPDATE/DELETE) 후 REINDEX 없이 GPU+delta 병합으로 정합한 top-k를 반환한다. 3L `CuvsBfIndex`를 3A-2 GPU delta cache가 재사용. 상세 스펙·검증은 [design/PLAN.md — Phase 3A](design/PLAN.md), 결정은 ADR-047. **4A(빌드 오버헤드)·3R(빌드 파라미터 reloption)도 완료**(완료 표 참조; 4A=ADR-057/058/059, 3R=ADR-052), **3S(취소 전파)도 완료**(ADR-053), **D(exact filtered BF)도 완료**(ADR-063, 잔여 4항목 포함), **3O(CAGRA-first BITSET prefilter)도 완료**(ADR-048, PR #36/#37), **3Q(CAGRA Streaming Updates)도 완료**(ADR-051, installcheck 21/21), **4C(Background Compaction)도 완료**(ADR-050, installcheck 22/22 + isolation 3/3), **3C/3D(GCS snapshot + replica async warmup)도 완료·인증**(ADR-013/ADR-066, 실 GCS round-trip `make gpu-test-objstore`, installcheck 25/25 + isolation 3/3) — 기능 순차 경로 완료. **repo 공개 전 운영 하드닝 3종(fallback 관측성=PR #43 · VRAM budget 강제=ADR-065 해소 · OOM 후 재사용=PR #42)도 완료**. **MAX_INDEXES 하드월도 해소**(ADR-068, PR #45 — 소프트 LRU 캡 `--max-indexes` 기본 1024 + 슬롯-확보 auto-reload; PR #50 Tier-1 evict/reload 가드). **CI 2-tier도 구현·검증 완료**(ADR-067, PR #46–48/#50 — Tier 1 매 PR 자동 + Tier 2 UI 버튼 실 A100 26/26). README도 현재화 완료(Install/Requirements/Compatibility/Quickstart/Usage). 라이선스는 **PostgreSQL License**로 확정. **다음 순차 작업: 릴리스 준비 — `BENCHMARK.md` 공개 · 문서 정합성/현행화 · 운영 플레이북 완성**(아래 "릴리스 준비 — 문서·운영 정비" 절; "에코시스템 진입 계획" 전제조건 참조).

### 자원 거버넌스 하드닝 — 확정 버그 3개 (ADR-069, PR #54)

> 자원 거버넌스 감사 + 적대적 리뷰 2라운드에서 **정책과 무관하게 코드로 확정된 버그 3개**(+IVF-PQ eviction 부수 발견). PR #54에서 전부 출고 — **Tier-1 GREEN 26/26, 데몬 ASAN 빌드**. 큰 거버넌스 항목은 트리거 백로그로 분리.

- **[OK] VRAM 회계 누락** — `total_vram_used`가 unsharded `main_bf_vram_bytes` / sharded `shards[].bf_vram_bytes` 미합산 → eviction 과약정 → OOM. (IVF-PQ `ivfpq_vram_bytes`는 `vram_bytes`와 중복이라 비-합산.) **완료**: `vram_accounting.sql`.
- **[OK] 빌드 락 starvation** — `handle_build`/`build_sharded`가 `g_index_mutex`를 GPU 빌드 내내 보유 → 검색/통계/드롭 블록. **완료**: reservation-counter(`g_pending_build_vram`)로 GPU 빌드 구간 언락(양 경로); `build_lock.sql`. 동시성(starvation 부재)·`build_sharded` 멀티GPU는 Tier-2.
- **[OK] 빌드 OOM evict-retry** — OOM 신호(`cuvs_last_build_was_oom`)+`inject_build_oom`(opcode 20)+evict 후 1회 재시도. **완료**: `build_oom.sql`(ASAN 무크래시).
- **[OK] (부수) IVF-PQ eviction 크래시** — `evict_lru→save_index→cuvs_cagra_serialize(NULL handle)` SEGV(IVF-PQ는 `handle==NULL`). 기존 잠복(IVF-PQ 인덱스 evict 불가); #3 retry가 ASAN으로 노출. **완료**: `evict_lru` IVF-PQ 분기(save 없이 free+reload) + `save_index` NULL 방어 + Tier-1 데몬 ASAN 상시.
- **[OK] 병렬빌드(handle_build_multi)에 #2/#3 적용** — ADR-058/059 병렬 경로(대형·OOM 빈발)는 단일 경로와 별개라 미적용이었음. **완료**: 동일 reservation/unlock + OOM evict-retry 적용. `build_multi_oom.sql`(강제 병렬 + OOM → evict + retry, 데몬 로그 `[handle_build_multi]` 확인). #2/#3은 이제 세 빌드 경로 전부 커버.

대상: `src/pg_cuvs_server.c` · `src/cuvs_wrapper.{cu,h}` · `src/cuvs_wrapper_shim_cpu.c` · `src/pg_cuvs.c` · `.github/workflows/ci.yml`(ASAN) · `test/sql/{vram_accounting,build_lock,build_oom,build_multi_oom}.sql` | ADR-069

### 릴리스 준비 — 문서·운영 정비 (순차)

> repo가 PUBLIC이 된 지금, 외부 사용자·기여자·운영자가 **현행 제품을 ADR 발굴 없이** 이해·운용할 수 있어야 한다. 현 문서는 ADR 69개(`DECISIONS.md` 214KB) + `PLAN.md`(1523줄) + 분산 design/docs로 **역사적 근거·작업메모 누적**에 가까워, 현재 제품의 기능·아키텍처·적용 기법·고려사항을 일목요연하게 볼 단일 reference가 없다(README가 유일 개요).

> **입력 자료**: [`docs/levers-and-governance.md`](docs/levers-and-governance.md) — 레버 카탈로그(GUC 34/reloption 11/데몬플래그 9, 소스 추출) + 표준 PG 레버 거버넌스(ADR-069 운영자 버전) + 세션 학습(PR#54) + **문서화 감사 결과(§5: 드리프트·미설명 레버·backport TODO 체크리스트)**. 아래 "문서 정합성/현행화"·"운영 플레이북"·"References"가 이 문서를 승격·소비한다.

- **문서 정합성/현행화 (current-state reference 정비)**
  - `ARCHITECTURE.md`(신규): 현행 컴포넌트(확장 `.so` / sidecar 데몬 / shmem IPC), 데이터·제어 흐름, 인덱스 생애주기(build→serialize→load→evict→reload), VRAM 자기-회계, 멀티-GPU 샤딩, GCS 스냅샷.
  - 기능/능력 reference: 현존 검색 모드·인덱스 AM(cagra/ivfpq/hnsw/BF)·GUC·reloption 일람 통합(현재 PLAN/ADR/README 분산).
  - 적용 기법·고려사항 요약: 비자명 엔지니어링 — BITSET 극성 규약, rev-map prefilter(3O), fail-closed 계약, VRAM 자기-회계(ADR-065), delta/tombstone 병합, CPU-reference shim CI, false-done 방지 원칙.
  - 문서 맵: ADR/PLAN = "역사적 근거", reference = "현행 SSOT"로 명확히 구분. drift(`SPEC.md` 등) reconcile.
  - **완료 기준**: 외부 기여자/사용자가 reference 문서군만으로 "무엇을·어떻게·왜"를 ADR 발굴 없이 파악. 문서 맵이 현행 vs 역사를 구분.

- **운영 플레이북 완성 (`design/OPS_GPU_PLAYBOOK.md` 단일화)**
  - 현황: OPS_GPU_PLAYBOOK(331줄)은 GPU 튜닝 + MIG만 다룸. `docs/playbooks/`에 3종(replica-bootstrap·capacity-planning·benchmark-runbook); **release-upgrade 런북 부재**.
  - 작업: 운영 생애주기 전반 단일화 — 기동·모니터링(어느 `pg_stat_gpu_*` 뷰·임계값), 장애모드·복구(데몬 다운·VRAM OOM·fallback 급증·eviction 폭주), 업그레이드/롤백, 백업/복구(GCS 스냅샷), 스케일링·캐파, 인시던트 대응. 흩어진 런북을 OPS_GPU_PLAYBOOK로 연결.
  - **완료 기준**: 신규 운영자가 플레이북만으로 배포→모니터→장애대응→업그레이드 수행 가능; 각 절차에 실 명령·뷰·임계값 포함.

- **가이드 사이트 발행 (GitHub Pages, MkDocs)**
  - 레퍼런스: [PG-Strom 문서](https://heterodb.github.io/pg-strom/)(MkDocs + RTD 테마, GitHub Pages, **동일 PostgreSQL License·같은 PG-GPU 카테고리** → IA 모델: Home/Install/Tutorial/Advanced Features/References/Release Notes) + [cuVS integrations](https://docs.rapids.ai/api/cuvs/stable/integrations/)(Faiss/Milvus/Lucene/Kinetica 등재; **PostgreSQL/DB 확장 전무 → pg_cuvs 첫 등재 기회**, 등재엔 dedicated 페이지+링크 필요).
  - 작업: MkDocs(Material 또는 RTD 테마) + GitHub Actions로 Pages 발행(`ysys143.github.io/pg_cuvs`). IA는 PG-Strom 미러 — Home(개요) / Install(설치·버전매트릭스) / Tutorial(quickstart·필터검색·멀티테넌트) / Features(검색 모드·인덱스 AM·GUC·reloption) / References(SQL 함수·뷰·기법 요약) / Operations(플레이북) / Release Notes.
  - **위 두 항목(문서 현행화·플레이북)의 단일 렌더 표면** — ARCHITECTURE·기능 reference·기법 요약·플레이북을 사이트가 렌더(중복 산출물 안 만듦, single source). cuVS integrations 등재용 dedicated 페이지(pg_cuvs ← cuVS) 준비 → **에코시스템 진입 단계 3**(cuVS 문서/README 링크 요청) 산출물.
  - **완료 기준**: Pages URL 라이브 + PG-Strom 수준 IA + cuVS integrations PR에 링크할 dedicated 페이지 존재.

대상: `design/OPS_GPU_PLAYBOOK.md` · `docs/playbooks/` · (신규) `ARCHITECTURE.md` + 문서 맵 · (신규) `mkdocs.yml` + Pages 배포 워크플로

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
| **host RAM cgroup 가이드** | ADR-069. memfd/shm 코퍼스·데몬 배열은 PG 회계 밖 → `maintenance_work_mem`/`temp_file_limit`로 강제 불가(category error). 진짜 천장은 OS/cgroup(`systemd MemoryMax=`, `RLIMIT_AS`). | 문서(OPS 플레이북에 cgroup/슬라이스 설정 + RSS 모니터링) | 멀티테넌트 프로덕션 배포 시 |
| **scratch-aware VRAM admission** | ADR-069. `estimate_vram_bytes`가 CAGRA 빌드 scratch(IVF-PQ intermediate ~3-10x) 미포함 → 사전 admission이 빌드 OOM을 못 막음(버그 #3 reactive retry로 1차 완화). | 코드(intermediate/graph degree 기반 동적 추정 또는 RMM pool cap) | 대규모 빌드 OOM 실측 시 |
| **백엔드 아티팩트 스탬프(timeline/system_identifier)** | ADR-069. 외부 아티팩트가 WAL/복제 밖 → standby/PITR에서 timeline 발산 시 stale 결과 위험. 데몬은 pg_control/timeline 못 읽음 → 백엔드가 `.tids` 헤더 스탬프 + plan-time 검증해야 fail-closed 가능. | 코드(사이드카 포맷 + 백엔드 검증) | replica/PITR 정합성 요구 시 |
| **corpus → BufFile 옵션** | ADR-069. 코퍼스를 PG `BufFile` temp 파일로 옮기면 `temp_file_limit`이 *진짜로* 적용(디스크 백킹 트레이드오프). 메모리 제약 환경 대안. | 코드(corpus tier에 BufFile 추가) | host RAM 제약 환경 수요 시 |
| **daemon host-bytes cap + evict-on-host-pressure** | ADR-069. resident host 배열(`rev_tids`/`rev_item_ids`/`tids` ~20B/vec)이 개수 LRU(`g_max_indexes`)로만 제한 → host RAM은 인덱스 크기 비례 누적. | 코드(host-bytes 카운터 + host 압박 시 eviction) | 데몬 host RSS 누적 실측 시 |
| ~~**빌드 락 동시성 Tier-2 검증**~~ [OK] | ADR-069. #2 unlock의 starvation-부재(동시 검색 비차단) 검증. | **완료** (A100, 2026-06-11): 6.97s GPU 빌드 중 동시 검색 25회 각 50–110ms(블록 없음). installcheck 30/30 + isolation 3/3. |
| **`build_sharded` 멀티GPU 검증** | ADR-069. 샤드 빌드의 reservation/eviction은 2+ GPU 필요(dev VM은 단일 A100). | Tier-2 멀티GPU VM | 멀티GPU 배포/회귀 의심 시 |

스펙: `docs/spec-audit-2026-06-05.md` | ADR-011 / ADR-017 / ADR-069 | `design/OPS_GPU_PLAYBOOK.md`

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
| ~~GitHub repo 공개~~ [OK] | **PUBLIC 공개됨**. 라이선스: **PostgreSQL License** (확정 — `LICENSE`·README 일치; 이전 표의 "Apache 2.0" 표기는 실제 파일과 불일치였어 정정) |
| 재현 가능한 벤치마크 공개 | `BENCHMARK.md` — 핵심은 **overhead characterization**: GPU가 distance computation을 제거하면 IPC / PG heap fetch가 새 병목이 된다는 것을 latency 분해로 실증. pgvector(CPU HNSW) 대비 QPS/latency 비교는 부수. selectivity sweep은 멀티테넌트 filtered search 효과 지지 실험으로 포함(논문 중심 아님) |
| 외부 사용자용 가이드 | README 현재화 완료. **가이드 사이트 발행**으로 격상(GitHub Pages/MkDocs, PG-Strom IA 미러) — "릴리스 준비 — 문서·운영 정비" 절 |
| ~~CI — GPU 테스트 전략~~ [OK] | **구현·검증 완료** (ADR-067, [design/CI_STRATEGY.md](design/CI_STRATEGY.md)): 2-tier — **Tier 1** `ci.yml`(CPU-reference shim `cuvs_wrapper.h` 경계 대체, hosted ubuntu, 매 PR 자동·무료; plumbing·계약·mode·recall + filter_comparison·MAX_INDEXES evict/reload 가드) + **Tier 2** `gpu.yml`(UI 버튼 `workflow_dispatch`, WIF 키리스 GCP 인증, self-hosted A100, 실 installcheck 26/26 검증). PR #46–48, #50. 잔여: emulator CI 회귀(트리거). |

### 진입 단계

| 단계 | 목표 | 전제 조건 | 타이밍 |
|------|------|-----------|--------|
| 1 | repo 공개 [OK] + 벤치마크 공개 | 없음 | repo 공개됨 · `BENCHMARK.md`만 잔여 |
| 2 | cuvs-bench backend PR | 3Q 완료 [OK] | 즉시 착수 가능 |
| 3 | [cuVS integrations](https://docs.rapids.ai/api/cuvs/stable/integrations/) 등재 + cuVS README 링크 (현재 Faiss/Milvus/Lucene/Kinetica — PG 확장 전무 → 선점) | 2단계 merge + 가이드 사이트 dedicated 페이지 | 2단계 후 |
| 4 | NVIDIA 채널 노출 | 3단계 등재 | 3단계 후 |

**현황**: PostgreSQL 관련 언급이 cuVS 생태계에 전무 — 선점 기회. 현재 공식 통합: Milvus, Faiss, Elasticsearch(진행 중), Kinetica.
