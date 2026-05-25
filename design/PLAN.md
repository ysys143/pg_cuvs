# pg_cuvs 개발 계획

## 프로젝트 정체성

pg_cuvs는 단순한 PostgreSQL용 GPU ANN 확장이 아니다. PostgreSQL을 제어 평면으로 유지하면서, 비용이 크고 병렬화 효과가 큰 벡터 후보 생성만 GPU sidecar로 오프로딩하는 미들웨어 프로젝트다.

핵심 원칙:
- SQL 인터페이스와 트랜잭션 시맨틱은 PostgreSQL이 유지한다.
- pgvector의 `vector` 타입, 연산자, opclass 표면을 최대한 재사용한다.
- GPU는 heap tuple을 직접 소유하지 않는다. GPU는 TID 후보와 distance를 반환하고, MVCC visibility, ACL, join, filter, heap recheck는 PostgreSQL이 수행한다.
- PostgreSQL backend는 CUDA context를 만들지 않는다. `pg_cuvs_server` sidecar가 단일 CUDA context와 VRAM-resident index를 소유한다.
- `CREATE INDEX USING cagra`가 성공했다면 VRAM build와 disk persistence가 모두 성공한 상태여야 한다.

---

## 현재 구현 기준

2026-05-26 기준 코드베이스는 Phase 1 proof-of-mechanism과 Phase 1.5 hardening을 통과한 상태다. sidecar, CAGRA build/search, persistence, durable DDL, restart reload, failure injection, large benchmark, crash guard regression이 모두 들어갔다. 예전 단계 구분에서 sidecar와 CAGRA를 후속 작업으로 보던 표현은 더 이상 현재 상태를 설명하지 않는다.

구현된 주요 표면:
- PostgreSQL C extension과 PGXS/nvcc build split.
- `cagra` index access method handler.
- pgvector `vector_l2_ops`, `vector_cosine_ops`, `vector_ip_ops` for `USING cagra`.
- `enable_cuvs`, `cuvs.socket_path`, `cuvs.index_dir`, `cuvs.circuit_breaker_threshold`.
- UDS + `shm_open` IPC.
- `pg_cuvs_server` sidecar daemon.
- CAGRA build/search wrapper.
- CAGRA serialize/deserialize와 `.tids` sidecar file.
- process-local circuit breaker.
- VRAM budget check와 LRU eviction foundation.
- GCP GPU VM remote build/test workflow.
- `shared_preload_libraries = 'pg_cuvs'` postinstall setting으로 cold backend planning dlopen 비용 제거.
- Phase 1.5 unit/integration/e2e/large benchmark/playbook baseline.
- scan lifecycle hardening: repeated scan, lateral rescan, cursor, multi ORDER BY crash regression.
- adversarial edge-case regression suite: `test/sql/edge_cases.sql` is included in `REGRESS = smoke cpu_fallback edge_cases`.
- post-hardening test expansion: integration/unit/e2e coverage for multi-opclass and crash guards.

현재 남은 핵심 debt:
- `cuvs_ambuild()`가 전체 corpus를 backend memory에 모은 뒤 daemon으로 넘긴다. 대형 테이블 build에서 backend RSS가 커진다.
- `aminsert`, `ambulkdelete`가 아직 구현되지 않았다. `amvacuumcleanup`은 VACUUM crash 방지 stub이며 freshness 복구를 수행하지 않는다.
- search `k`가 고정값이고 SQL `LIMIT`과 연결되지 않는다.
- metric 판별이 strategy number 휴리스틱에 의존한다.
- `CREATE INDEX` on `n_vecs < 2`는 cuVS CAGRA SIGABRT 방지를 위해 명시적으로 실패한다. 이를 영구 계약으로 둘지, tiny index CPU fallback/placeholder 정책으로 바꿀지 결정이 필요하다.
- planner cost model은 아직 rows/dim/k/residency를 충분히 반영하지 않는다.
- `pg_stat_gpu_search` 같은 SQL 관측 표면은 아직 없다. 현재는 process-local `pg_cuvs_last_search_*`와 로그/벤치마크로 간접 확인한다.
- write/delete 후 CAGRA index freshness 계약이 아직 운영 가능한 수준으로 정리되지 않았다.

---

## 단계 게이트

### Phase 1 — Proof of Mechanism

목표: PostgreSQL query pipeline에서 GPU 후보 생성이 실제로 동작함을 증명한다.

완료 기준:
- `CREATE EXTENSION pg_cuvs`가 성공한다.
- `CREATE INDEX USING cagra`가 daemon을 통해 CAGRA index를 build한다.
- GPU search가 TID 후보를 반환하고 PostgreSQL heap recheck로 결과를 반환한다.
- daemon 장애 시 read query는 CPU path로 fallback한다.
- GCP L4 VM에서 smoke/regression/e2e acceptance criteria가 통과한다.

상태: 기능 증명은 통과했으나, durability 정책은 Phase 1.5에서 운영 계약으로 재정의한다.

---

### Phase 1.5 — Test & Ops Hardening

목표: Phase 2 기능 확장 전에 durability, failure mode, test coverage, playbook을 고정한다. 이 단계는 "새 기능"보다 "현재 계약을 깨지 않게 만드는 기준선"이다.

#### 1. Durable CAGRA DDL 계약

`CREATE INDEX USING cagra` 성공 조건:
- daemon에 연결 가능해야 한다.
- VRAM CAGRA build가 성공해야 한다.
- `.cagra` serialization이 성공해야 한다.
- `.tids` persistence가 성공해야 한다.
- tmp file write, `fsync(file)`, atomic `rename`, `fsync(dir)`까지 성공해야 한다.

실패 정책:
- build/persistence 중 하나라도 실패하면 `CREATE INDEX`는 `ERROR`로 실패하고 PostgreSQL catalog 변경은 롤백되어야 한다.
- SELECT search path에서는 daemon unavailable, OOM, GPU error를 CPU fallback으로 처리할 수 있다.
- DDL path와 query path의 실패 정책을 분리하기 위해 IPC status code를 세분화한다.

필수 구현:
- `CUVS_STATUS_UNAVAILABLE`, `CUVS_STATUS_BUILD_FAILED`, `CUVS_STATUS_PERSIST_FAILED` 등 status 분리.
- `handle_build`에서 새 index를 build/persist 완료하기 전 기존 resident index를 삭제하지 않는다.
- 새 build가 완전히 성공한 뒤 registry entry를 atomic swap한다.
- 실패 시 새 handle, TID array, mmap, tmp files를 cleanup한다.
- persisted `.cagra`/`.tids` pair가 둘 다 유효하지 않으면 startup load 대상에서 제외한다.
- `.tids`는 versioned + checksummed header(magic/version/n_vecs/dim/metric/crc32)를 갖는다. 기존 headerless `.tids` 파일은 invalid로 거부되므로 pre-1.0 on-disk index는 모두 REINDEX 해야 한다.

#### 2. Unit Test Coverage

대상:
- circuit breaker state transition.
- TID encode/decode.
- suffix parsing과 persisted pair discovery.
- tmp path, rename, cleanup helper.
- status code mapping.
- IPC frame serialization boundary.
- test-only failure injection hooks.

목표:
- 순수 C helper 영역은 80-90% 이상 coverage를 목표로 한다.
- CUDA/cuVS 내부는 line coverage보다 boundary tests와 GPU e2e로 검증한다.

#### 3. Integration Test Coverage

대상:
- extension load, GUC, AM/opclass registration.
- daemon unavailable 상태의 DDL failure와 SELECT fallback 분리.
- fake daemon 또는 test daemon을 통한 status별 `ambuild` 분기.
- persistence failure injection: serialize failure, `.tids` open/write/fsync/rename failure.
- daemon restart 후 persisted index reload.

#### 4. GPU E2E Coverage

GCP GPU VM 전용 시나리오:
- clean build/install/test.
- real daemon start/stop/restart.
- `CREATE INDEX USING cagra` creates valid `.cagra` + `.tids`.
- nearest-neighbor query returns expected rows.
- daemon restart reloads index without heap rebuild.
- daemon down during SELECT falls back to CPU.
- persistence failure makes `CREATE INDEX` fail.
- VRAM budget exhaustion follows the documented policy.

Makefile target 방향:
- `gpu-test-unit`
- `gpu-test-regress`
- `gpu-test-daemon`
- `gpu-test-e2e`
- `gpu-test-all`

#### 5. Large Dataset Benchmark & JIT Decision Gate

Phase 1.5에서 대규모 데이터 테스트를 먼저 수행한다. 목적은 Phase 2 기능 검증이 아니라 운영 기준선을 고정하는 것이다. 특히 JIT threshold는 추정값으로 정하지 않고, 대규모 데이터와 현실적인 cost model에서 측정한 뒤 결정한다.

Dataset tiers:
- Small: 10K vectors.
- Medium: 1M vectors.
- Large: 10M vectors.
- Stress: configured VRAM budget 또는 physical VRAM 한계 근처.

Dimensions:
- 384.
- 768.
- 1536.

Query k:
- 10.
- 100.
- 1000.

측정 항목:
- `CREATE INDEX USING cagra` build time.
- peak backend RSS during `cuvs_ambuild()`.
- daemon RSS and GPU VRAM.
- `.cagra` and `.tids` artifact size.
- daemon restart reload time.
- cold backend planning time.
- warm backend planning time.
- execution latency p50/p95/p99.
- fallback count and fallback reason.
- `EXPLAIN (ANALYZE)`의 `JIT:` section 발생 여부.
- `nvidia-smi` 기준 backend별 CUDA context 생성 여부.

JIT 결정 규칙:
- `shared_preload_libraries = 'pg_cuvs'`는 이미 실험으로 확정된 필수 설정이다.
- JIT threshold는 Phase 1.5에서 자동 적용하지 않는다.
- 대규모 데이터 또는 Phase 2 cost model 확장 후 `JIT:`가 발생하고 latency가 튀는 경우에만 threshold sweep을 수행한다.
- threshold sweep 후보는 기본값, 1e6, 1e7, 1e8 등으로 두고, vector-search p95/p99가 튀지 않는 가장 낮은 값을 선택한다.
- mixed analytical workload가 있으면 JIT 이득을 별도 측정하고 전역 설정 여부를 다시 판단한다.

#### 6. Operational Safety

Phase 2 전에 정리할 항목:
- daemon signal handler는 flag만 set한다. CUDA serialize, mutex, file I/O는 main loop의 graceful shutdown path에서 수행한다.
- planner cost path에서 CUDA runtime을 직접 touch하지 않는다. daemon status cache 또는 conservative cost로 대체한다. (구현 완료: `cuvsamcostestimate`에서 `cuvs_gpu_available()` 제거.)
- 첫 쿼리 planning 비용은 `shared_preload_libraries = 'pg_cuvs'`로 제거한다. libcuvs.so(812MB)를 postmaster가 한 번 dlopen하고 백엔드가 fork로 상속 → 95ms -> 0.4ms. `make gpu-postinstall`이 설정. (ADR-018.)
- JIT 설정은 대규모 benchmark와 threshold sweep 없이 변경하지 않는다.
- `fprintf(stderr)`는 log level macro로 정리한다: ERROR, WARN, INFO, DEBUG.
- `PG_CUVS_DEBUG`는 hot-path trace 전용으로 유지한다.

#### 7. Playbook

`docs/playbooks/`에 다음 문서를 둔다.

- `gpu-vm-build-and-test.md`
- `large-dataset-benchmark.md`
- `jit-threshold-sweep.md`
- `daemon-restart-recovery.md`
- `create-index-failure-diagnosis.md`
- `persistence-corruption-recovery.md`
- `vram-oom-fallback.md`
- `rollback-and-cleanup.md`

각 playbook 형식:
- 증상
- 확인 명령
- 원인 분기
- 복구 절차
- 검증 명령
- escalation 기준

Phase 1.5 완료 기준:
- durability DDL 계약이 구현되어 GPU VM e2e로 검증된다.
- failure injection으로 주요 실패 경로를 재현할 수 있다.
- 대규모 데이터 benchmark가 최소 1M/1536d와 VRAM budget stress case를 포함해 실행된다.
- JIT threshold는 측정 결과가 필요성을 보인 경우에만 결정된다.
- regression/e2e target이 문서화된 명령 하나로 실행된다.
- 운영 playbook이 최소 복구 흐름을 포함한다.

#### 완료 현황 (2026-05-25)

Phase 1.5는 GPU VM(A100 40GB)에서 검증 완료했다.

- **#1 Durable DDL**: `CUVS_STATUS_BUILD_FAILED`/`PERSIST_FAILED` 분리, `.tids` versioned+crc32 header(magic `TIDS`/version/n_vecs/dim/metric/crc32/reserved), startup load 시 pair 검증(legacy headerless 거부), idx_tmp fsync 실패를 fatal로, 실패 시 handle/tids/tmp cleanup. fault injection으로 serialize/tids-write/rename/registry-full 실패 경로를 재현해 모두 `ERROR` + catalog rollback 확인.
- **#2 Unit test**: PG/CUDA-free helper(`src/cuvs_util.{h,c}`) 단위테스트 `make test-unit` 73 assertions green. circuit breaker, TID encode/decode, filename parsing, status mapping, crc32, `.tids` header round-trip + 거부 경로(magic/version/n_vecs/truncate/crc/reserved) 포함.
- **#3 Integration**: `gpu-test-daemon` 5개 시나리오(daemon-down, serialize-fault, tids-fault, clean build, registry-full eviction-save) 전부 PASS. PG regression(`smoke`, `cpu_fallback`)은 deterministic query로 고정해 `make installcheck` 2/2 통과(expected output 커밋됨).
- **#4 GPU E2E**: `make gpu-e2e`(build -> daemon restart -> reload -> stable 결과 + versioned `.tids` 검증) PASS. Makefile target `gpu-test-{unit,regress,daemon,e2e,all}` 제공.
- **#5 Large benchmark**: `make gpu-bench`(기본 10K×384) / `make gpu-bench-1m`(1M×1536). 1M×1536 측정: build 70.8s, `.cagra` 6.4GB, +6.1GB VRAM, cold planning 0.75ms / warm 0.065ms, exec p50/p95/p99 = 3.6/4.1/4.2ms, fallbacks 0, CUDA context는 데몬 1개뿐(백엔드 0). **`JIT:` section은 10K×384와 1M×1536 모두 미발생** → JIT 미조정 결정 유효(threshold sweep 불필요). (ADR-018, ADR-019.)
- **#6 Ops safety**: signal handler flag-only + `graceful_shutdown`(`sigaction`, no `SA_RESTART`), log level macro(ERROR/WARN/INFO 무조건, DEBUG는 `PG_CUVS_DEBUG` gate). cost-path CUDA 제거와 shared_preload는 기존 완료.
- **#7 Playbook**: `docs/playbooks/` 8종 작성.

추가로 1M gate run이 IPC durability 버그를 드러냈다: 대규모 build의 reply 수신은 수 분간 `recv()`를 블록하는데 `send_all`/`recv_all`이 `EINTR`을 재시도하지 않아, 백엔드에 전달된 시그널이 IPC를 중단시켜 데몬은 build/persist에 성공했는데 `CREATE INDEX`만 실패(catalog rollback)하고 6.4GB orphan을 남겼다. `EINTR` 재시도로 수정. (ADR-020.)

#### 후속 hardening 현황 (2026-05-26)

Phase 1.5 완료 후에도 crash guard와 regression coverage가 추가로 들어갔다. 여기서 말하는 "Phase 2"는 일부 작업 실행계획의 하위 단계명이며, 아래의 제품 로드맵 **Phase 2 — Production Ready Single-Node**와 구분한다.

- `b341e7d` — `edge_cases` regression suite 추가와 scan bug 3건 수정. `Makefile`의 `REGRESS`는 `smoke cpu_fallback edge_cases`다.
- `cb91b60` — multi-opclass와 crash guard 대상의 integration/unit/e2e coverage 확장. 정상 GPU VM 환경에서는 integration 24/24 PASS와 e2e PASS가 기준이다.
- `0d30414` — dim mismatch를 cuVS 호출 전에 reject해 daemon SIGABRT를 방지.
- `7fac5d3` — `n_vecs < 2` CAGRA build를 reject해 daemon SIGABRT를 방지.
- `fe0f997` — fixed sleep 대신 UDS polling으로 daemon readiness check를 안정화.

주의: `cannot find -lcuvs` 형태의 integration build failure는 conda `cuvs_dev` 미활성화로 `CUVS_PREFIX`가 비어 `-L/lib` fallback이 생긴 환경 문제로 분류한다. 코드 회귀 여부는 conda 활성화 상태의 성공 run을 기준으로 판단한다.

---

### Phase 2 — Production Ready Single-Node

이 절의 Phase 2는 제품 로드맵 단계다. 위 후속 hardening의 "Phase 2(edge_cases/scan fixes)"는 이미 완료됐지만, `pg_stat_gpu_search`, LIMIT-k, metric identity, write/staleness, large-build memory 모델은 아직 이 제품 로드맵 Phase 2의 남은 작업이다.

목표: 단일 PostgreSQL instance + GPU sidecar 조합을 "데모"가 아니라 운영 가능한 단일 노드 엔진으로 만든다. Phase 2의 중심은 새 ANN 알고리즘이 아니라 **관측성, planner/executor 정확도, write/staleness 계약, 대형 build 안정성**이다. DiskANN/Vamana는 Phase 2 말미의 설계/준비 항목이며, 기본 CAGRA 운영 계약이 닫히기 전에는 앞당기지 않는다.

Phase 2는 아래 순서로 진행한다. 앞 단계 산출물이 뒤 단계의 검증 도구가 되므로 순서를 바꾸지 않는다.

#### 0. Entry Gate: Phase 1.5 Baseline Freeze

Phase 2 시작 전 기준선을 고정한다.

필수 확인:
- `make test-unit` green.
- GPU VM에서 `make gpu-test-all` green.
- `make gpu-bench-1m` 또는 동등한 1M×1536 benchmark green.
- `test/sql/edge_cases.sql` regression green.
- `design/PLAN.md`와 `design/DECISIONS.md`가 현재 제한사항을 숨기지 않는다.

기준선에서 명시적으로 받아들이는 현재 동작:
- `n_vecs < 2` CAGRA build는 cuVS SIGABRT 방지를 위해 `ERROR`로 실패한다.
- SQL `LIMIT`과 GPU `k`는 아직 연결되지 않고, hard cap은 `k=100`이다.
- metric은 strategy-number heuristic에 의존한다.
- write/delete 후 CAGRA index freshness는 보장하지 않는다.

#### 1. Observability First: `pg_stat_gpu_search`

Phase 2의 첫 기능은 `pg_stat_gpu_search`다. 이후 LIMIT-k, cost model, fallback, cache, write/staleness를 모두 이 뷰로 검증한다. 로그와 `pg_cuvs_last_search_*`만으로 Phase 2 기능을 쌓지 않는다.

MVP 구현:
- daemon memory에 per-index stats를 둔다.
- IPC에 `STATS` op를 추가한다.
- extension SQL function이 daemon stats를 조회한다.
- SQL view `pg_stat_gpu_search`를 제공한다.
- daemon unavailable 상태에서도 view query가 PostgreSQL backend를 죽이지 않고 빈 결과 또는 명확한 ERROR를 반환한다.

초기 컬럼:
- `database_oid`
- `index_oid`
- `index_name`
- `calls`
- `success_calls`
- `fallback_calls`
- `error_calls`
- `rows_returned`
- `requested_k`
- `returned_k`
- `avg_latency_us`
- `p50_latency_us`
- `p95_latency_us`
- `p99_latency_us`
- `gpu_kernel_us`
- `ipc_us`
- `cpu_recheck_us`
- `vram_cache_hits`
- `vram_cache_misses`
- `reload_count`
- `last_status`
- `last_error`
- `last_fallback_reason`
- `last_search_at`

검증:
- clean GPU search, daemon-down fallback, cache reload, dim mismatch rejection, build failure가 stats에 반영된다.
- `edge_cases.sql`가 stats view 존재와 basic counter 증가를 확인한다.
- benchmark summary는 기존 log parsing 대신 stats view 값을 우선 사용한다.

후속:
- PostgreSQL native shared stats 또는 `pgstat` integration은 MVP 이후 검토한다. Phase 2 MVP에서는 daemon-owned stats가 source of truth다.

#### 2. Executor Correctness: LIMIT-k, OrderBy, Metric

목표는 "빠르지만 임의로 100개만 가져오는 인덱스"를 끝내고 SQL 의미에 맞는 검색 계약을 만드는 것이다.

필수 구현:
- SQL `LIMIT`을 GPU top-k로 전달한다.
- fixed `k=100`을 제거한다.
- LIMIT이 없거나 추출 불가능한 경우의 bounded default를 문서화한다.
- multi ORDER BY, NULL query vector, parameterized query vector, cursor, LATERAL rescan에서 crash-free 상태를 유지한다.
- opclass/operator OID 기반으로 L2/Cosine/IP metric을 판별한다.
- build-time metric과 search-time metric이 불일치하면 GPU search를 수행하지 않고 명확히 ERROR 또는 fallback 처리한다.
- `pg_stat_gpu_search.requested_k`, `returned_k`, `last_fallback_reason`을 채운다.

정책 결정 필요:
- LIMIT이 매우 큰 경우 `k` 상한을 둘지 여부.
- `n_vecs < 2`를 계속 DDL ERROR로 둘지, tiny relation은 CPU-only marker 또는 placeholder artifact로 처리할지.
- metric mismatch를 DDL 단계에서 완전히 차단할지, query 단계 fallback으로 둘지.

검증:
- `LIMIT 1/10/100/1000`이 각각 daemon request k로 반영된다.
- `vector_l2_ops`, `vector_cosine_ops`, `vector_ip_ops`가 서로 다른 metric으로 build/search된다.
- parameterized query vector와 transaction block 안 repeated query가 segfault 없이 동작한다.
- recall benchmark가 `k`별로 기록된다.

#### 3. Planner / Cost Model

목표는 planner가 GPU path를 선택할 때 예측 가능하고, cost 증가가 JIT 같은 부작용을 만들 경우 실험으로 제어하는 것이다.

필수 구현:
- cost model 입력에 rows, dim, requested k, estimated selectivity, index residency를 반영한다.
- planner path에서 CUDA runtime을 직접 호출하지 않는다. readiness는 daemon status cache 또는 conservative assumption으로 처리한다.
- daemon unavailable, circuit breaker open, stale index, nonresident index 상태가 cost 또는 path enable 여부에 반영된다.
- `EXPLAIN`에서 GPU path 선택 이유를 추적할 수 있도록 debug/stat 표면을 둔다.

JIT 정책:
- Phase 1.5 결과상 10K×384와 1M×1536에서 `JIT:` section은 없었다.
- Phase 2 cost model 변경 후 `EXPLAIN (ANALYZE)`에 `JIT:`가 나타나고 p95/p99 latency variance가 커질 때만 `docs/playbooks/jit-threshold-sweep.md`를 실행한다.
- `jit = off` 전역 적용은 기본 정책이 아니다. threshold는 실험으로 결정한다.

검증:
- small/medium/large table에서 planner 선택이 benchmark 결과와 크게 어긋나지 않는다.
- JIT 발생 여부가 benchmark artifact에 기록된다.
- cost 변경 전후 recall/latency/fallback이 같은 run ID로 비교 가능하다.

#### 4. Write / Delete / Staleness Contract

CAGRA는 정적 graph index이므로 PostgreSQL write path와의 계약을 명확히 해야 한다. Phase 2에서 "쓰기 후에도 조용히 오래된 결과를 반환"하는 상태는 허용하지 않는다.

MVP 계약:
- `aminsert`를 구현해 indexed table에 INSERT/UPDATE가 발생했음을 감지한다.
- `ambulkdelete`를 구현해 DELETE/VACUUM 경로에서 stale marker를 갱신한다.
- `amvacuumcleanup`은 단순 stub에서 벗어나 stale 상태와 rebuild 필요성을 보고한다.
- stale index는 GPU search를 하지 않고 CPU fallback하거나 명확한 ERROR를 낸다. 기본은 read availability를 위해 CPU fallback.
- stale 상태, stale row count estimate, last write LSN 또는 timestamp를 `pg_stat_gpu_search`에 노출한다.
- `REINDEX`는 현재 heap snapshot 기준으로 fresh artifact를 build/persist하고 daemon resident entry를 atomic swap한다.

후속 확장:
- pending-delta table 또는 small in-memory delta exact search.
- stale ratio threshold 이하에서는 CAGRA result + delta correction.
- autovacuum hook 또는 background worker lazy rebuild.

검증:
- INSERT 후 GPU stale marker가 켜지고 query가 CPU fallback한다.
- DELETE/VACUUM 후 stale 상태가 관측된다.
- REINDEX 후 stale marker가 해제되고 GPU path가 재개된다.
- catalog rollback, daemon restart, stale marker persistence가 서로 충돌하지 않는다.

#### 5. Large Build Memory Pressure

현재 `cuvs_ambuild()`는 모든 벡터를 backend memory에 모은 뒤 daemon으로 넘긴다. Phase 2에서는 대형 테이블 build가 backend RSS로 무너지는 구조를 고친다.

1차 목표:
- build 전 estimated corpus bytes를 계산한다.
- backend memory 한도를 넘을 것으로 예상되면 명확한 ERROR와 playbook 링크를 제공한다.
- benchmark가 peak backend RSS를 계속 기록한다.

2차 목표:
- chunked shared memory streaming 또는 mmap staging으로 daemon에 corpus를 넘긴다.
- daemon-side staging file은 tmp + fsync + rename 규칙을 따른다.
- build 실패 시 staging artifact와 partial index artifact를 정리한다.
- 향후 shard build와 multi-GPU fanout이 가능한 frame format을 선택한다.

검증:
- 10M급 또는 VM 한계에 가까운 dataset에서 backend RSS가 선형 폭증하지 않는다.
- build 중 signal/EINTR/restart failure path가 orphan artifact를 남기지 않는다.
- streaming path와 기존 small build path가 같은 `.cagra`/`.tids` validation을 통과한다.

#### 6. VRAM / NVMe Tiered Cache

Phase 1.5에서 LRU/eviction foundation과 persistence durability는 마련됐다. Phase 2에서는 이를 운영 계약으로 올린다.

필수 구현:
- `cuvs.max_vram_mb`를 build/load/search/reload 경로에 일관 적용한다.
- VRAM resident index LRU eviction을 production path로 고정한다.
- evicted index reload는 persisted artifact pair validation을 반드시 통과해야 한다.
- reload latency, hit/miss, eviction count, persist failure를 `pg_stat_gpu_search`에 기록한다.
- registry-full, eviction-save failure, reload corruption이 user-visible status로 분리된다.

검증:
- VRAM budget below working set에서 cache hit/miss와 reload가 예측 가능하게 발생한다.
- eviction 실패는 slot을 잘못 free하지 않는다.
- corrupted `.cagra`/`.tids` pair는 load되지 않고 fallback/ERROR 정책을 따른다.

#### 7. Phase 2 Test Matrix

Phase 2 기능은 구현과 동시에 테스트를 추가한다.

Unit:
- LIMIT-k extraction helper.
- opclass/operator metric mapping.
- cost model pure helper.
- stats aggregation/histogram.
- stale marker state transition.
- build-size estimate and memory cap.

Integration:
- `pg_stat_gpu_search` view.
- daemon unavailable stats behavior.
- status/fallback reason mapping.
- INSERT/DELETE/VACUUM/REINDEX freshness.
- metric mismatch and dim mismatch.
- `n_vecs < 2` current policy.

GPU E2E:
- 1M×1536 baseline 유지.
- LIMIT-k sweep.
- opclass metric sweep.
- VRAM budget eviction/reload.
- stale fallback and REINDEX recovery.
- streaming build 또는 memory cap behavior.

Benchmark:
- 10K, 1M, 10M where feasible.
- dimensions 384, 768, 1536.
- k 1, 10, 100, 1000.
- p50/p95/p99, recall, build time, reload time, backend RSS, daemon RSS, VRAM, JIT section, fallback reason.

#### 8. Phase 2 Exit Criteria

Phase 2는 다음 조건을 모두 만족해야 완료로 본다.

- `pg_stat_gpu_search`가 GPU success/fallback/error/cache/stale/latency를 SQL에서 보여준다.
- `LIMIT`이 daemon top-k request에 반영되고 fixed `k=100`은 사라진다.
- L2/Cosine/IP metric이 opclass/operator identity에서 결정된다.
- repeated scan, parameterized query, transaction block, cursor, LATERAL rescan이 crash-free로 regression에 잠긴다.
- write/delete 후 stale index가 조용히 사용되지 않는다.
- REINDEX가 stale 상태를 해소하고 durable artifact/resident index를 atomic하게 교체한다.
- 10M급 또는 VM 한계에 가까운 large build에서 OOM/failure 정책이 명확히 동작한다.
- VRAM budget 초과 시 eviction/reload/fallback이 stats와 로그로 관측된다.
- JIT 설정은 실험 근거 없이 전역 변경되지 않는다.
- 모든 Phase 1.5 regression이 계속 green이다.

---

### Phase 3 — Scale Out / Large Index Storage

목표: 단일 노드 VRAM resident CAGRA의 한계를 넘어 NVMe, S3, replica, multi-GPU로 확장한다.

#### 1. DiskANN / Vamana

구현 항목:
- `USING diskann` AM 추가.
- cuVS Vamana GPU build + CPU/NVMe search.
- CAGRA는 hot set, DiskANN은 cold set으로 역할 분리.
- 같은 column에 hot/cold index가 공존하는 tiered search를 구현한다.

#### 2. S3-backed Immutable Index Snapshots

구현 항목:
- local `cuvs.index_dir` artifact를 S3 snapshot으로 확장한다.
- 경로: `s3://<bucket>/pg_cuvs/<cluster_id>/<database_oid>/<index_oid>/<version>/`
- manifest, checksum, version을 둬 partial upload를 감지한다.
- local NVMe는 cache, S3는 재사용 가능한 artifact store로 취급한다.
- index artifact는 WAL 대상이 아닌 derived data로 유지한다.

#### 3. Replica / Multi-node Loading

구현 항목:
- primary에서 build 후 S3 upload.
- read replica는 heap scan rebuild 없이 S3에서 download/load.
- catalog OID, relfilenode 변화, manifest version mapping을 관리한다.

#### 4. Async Prefetch / Warmup

구현 항목:
- daemon startup 시 metadata만 scan하고 hot index를 background prefetch한다.
- NVMe cache miss 시 S3 download 후 VRAM promotion한다.
- warmup 상태와 miss reason을 stats view에 노출한다.

#### 5. Multi-GPU / Sharding

구현 항목:
- shard 단위 index build/search.
- GPU assignment와 VRAM budget per device.
- query fanout 후 top-k merge.
- PCIe/NUMA topology 고려.

Phase 3 완료 기준:
- 100M+ vector를 단일 VRAM resident 전제로 두지 않고 검색한다.
- S3 snapshot에서 새 daemon 또는 read replica가 index를 복구한다.
- hot CAGRA + cold DiskANN tiered search가 동작한다.
- partial upload, stale manifest, missing local cache의 복구 경로가 있다.

---

## 기술 위험 및 대응

| 위험 | 대응 |
|------|------|
| CUDA context 폭증 | sidecar daemon이 단일 context를 소유 |
| DDL durability 위반 | VRAM build + disk persistence 모두 성공해야 `CREATE INDEX` 성공 |
| GPU OOM | preflight, LRU eviction, query fallback, DDL error 분리 |
| daemon 장애 | SELECT는 CPU fallback, DDL은 ERROR |
| planner에서 CUDA touch | daemon status cache 또는 conservative cost |
| signal unsafe shutdown | signal handler는 flag만 set, main loop에서 graceful shutdown |
| write/stale index | stale marker, fallback, lazy rebuild, VACUUM/REINDEX 계약 |
| large build memory pressure | chunked streaming, mmap staging, shard build |
| artifact corruption | tmp+fsync+rename, manifest/checksum, pair validation |

---

## pg_cuvs vs 대안

| | pg_cuvs | pgvectorscale | pg_lance | Milvus/Qdrant |
|---|---|---|---|---|
| 실행 모델 | PG + GPU sidecar | PG in-process | PG + LanceDB | 전용 서버 |
| SQL/트랜잭션 | 완전 지원 목표 | 완전 지원 | 제한적 | 없음 |
| GPU 가속 | CAGRA/Vamana build | 없음 | 없음 | 일부 |
| 데이터 규모 | VRAM hot + NVMe/S3 cold | NVMe DiskANN | S3/Lance | 외부 cluster |
| 운영 복잡도 | 중간 | 낮음 | 낮음 | 높음 |
| 타겟 워크로드 | Operational vector search | Operational vector search | Analytics | Dedicated vector DB |

---

## 참고 자료

- [RAPIDS cuVS 문서](https://docs.rapids.ai/api/cuvs/stable/)
- [pgvectorscale](https://github.com/timescale/pgvectorscale)
- [PG-Strom](https://github.com/heterodb/pg-strom)
- `design/SPEC.md`
- `design/DECISIONS.md`
- `.claude/skills/gpu-vm-provision/`
