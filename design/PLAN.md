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

2026-05-26 기준 코드베이스는 Phase 1 proof-of-mechanism, Phase 1.5 hardening, 그리고 **제품 로드맵 Phase 2 — Production Ready Single-Node의 코어** 대부분을 통과한 상태다. sidecar, CAGRA build/search, persistence, durable DDL, restart reload, failure injection, large benchmark, crash guard regression에 더해 Phase 2 코어가 들어갔다: `pg_stat_gpu_search`/`pg_stat_gpu_cache` 관측성, `cuvs.k` LIMIT-k, opclass 기반 L2/Cosine/IP metric, k 기반 cost model, write→stale marker(+`.stale` 영속, REINDEX 해소), 빌드 메모리 가드(`cuvs.max_build_mem_mb` 0=auto), VRAM tiered cache(evict-to-fit + reload). 2026-05-26 재검증에서 stale runtime path가 실제 CPU fallback이 아니라 gettuple `return false`에 따른 empty result임이 드러났고, Phase 2.1 hotfix로 닫았다(`cuvsamcostestimate`가 `.stale` sidecar를 stat해 stale 인덱스를 seqscan/CPU로 라우팅). exit 기준 감사는 `docs/phase2-exit-criteria.md`. 잔여(다음 단계): build streaming/mmap, delta correction(Phase 3), 자동 backend RSS 기록.

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
- `pg_stat_gpu_search` and `pg_stat_gpu_cache` SQL observability views.
- `cuvs.k`, opclass/opfamily metric identity, and k-dominant cost model.
- write/delete staleness contract: `aminsert`/`ambulkdelete` mark stale, `.stale` persists it, `REINDEX` clears it. 자동 CPU fallback은 Phase 2.1에서 costestimate stale gate로 배선됐다.
- CAGRA build memory guard: `cuvs.max_build_mem_mb` and `cuvs.build_mem_safety_ratio`.

현재 남은 핵심 debt:
- `cuvs_ambuild()`는 여전히 전체 corpus를 backend memory에 모은 뒤 daemon으로 넘긴다. Phase 2는 `cuvs.max_build_mem_mb` fail-fast guard까지 닫았고, streaming/mmap handoff는 후속 작업이다.
- SQL `LIMIT`을 index AM에서 직접 읽지는 못한다. Phase 2는 pgvector `hnsw.ef_search`와 같은 모델로 `cuvs.k` GUC를 도입했고, `pg_stat_gpu_search.requested_k/returned_k`로 검증한다.
- write 후 CAGRA는 `.stale` sidecar로 stale 상태를 보존하고, Phase 2.1이 `cuvsamcostestimate`에서 그 sidecar를 stat해 stale 인덱스를 seqscan/CPU로 reroute한다(gettuple `return false`의 empty result 절벽 해소). stale 상태에서도 GPU path를 유지하는 pending-delta/delta exact search는 Phase 3 필수 기능이다.
- `CREATE INDEX` on `n_vecs < 2`는 cuVS CAGRA SIGABRT 방지를 위해 명시적으로 실패한다. 이를 영구 계약으로 둘지, tiny index CPU fallback/placeholder 정책으로 바꿀지 결정이 필요하다.
- `pg_stat_gpu_search`는 per-index wall-clock latency와 상태를 제공하지만 `gpu_kernel_us`/`ipc_us`/`cpu_recheck_us` split은 아직 없다.
- 10M-scale benchmark와 automatic peak backend RSS 기록은 VM capacity/cross-process PID tracking 문제로 deferred 상태다.

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
- `2542c8e` — Product Phase 2 Step 1: `pg_stat_gpu_search` daemon-backed observability view 추가.
- `a8e46d6` — stat view integration assertion fix.

주의: `cannot find -lcuvs` 형태의 integration build failure는 conda `cuvs_dev` 미활성화로 `CUVS_PREFIX`가 비어 `-L/lib` fallback이 생긴 환경 문제로 분류한다. 코드 회귀 여부는 conda 활성화 상태의 성공 run을 기준으로 판단한다.

---

### Phase 2 — Production Ready Single-Node

이 절의 Phase 2는 제품 로드맵 단계다. 목표는 단일 PostgreSQL instance + GPU sidecar 조합을 "데모"가 아니라 운영 가능한 단일 노드 엔진으로 만드는 것이었다. 2026-05-26 기준 **single-node core 완료**(stale 자동 CPU fallback은 Phase 2.1에서 마저 배선). 완료 판정과 증거는 `docs/phase2-exit-criteria.md`, feature별 테스트 매핑은 `docs/phase2-test-matrix.md`를 따른다.

#### 완료된 workstream

- **Step 0 — Entry gate**: Phase 1.5 baseline(`make test-unit`, GPU regression/e2e/daemon tests, 1M×1536 benchmark, edge cases)을 기준선으로 고정.
- **Step 1 — Observability**: daemon-backed `pg_stat_gpu_search` 구현. search/error counts, last status/error, p50/p95/p99 wall-clock latency, `requested_k`/`returned_k`, stale/stale_since를 SQL에서 확인한다. daemon down은 empty result set으로 처리한다.
- **Step 2 — Executor correctness**: fixed `k=100` 제거. PostgreSQL index AM이 SQL `LIMIT`을 직접 읽을 수 없으므로 pgvector `hnsw.ef_search`와 같은 모델의 `cuvs.k` GUC를 사용한다. opclass/opfamily identity 기반으로 L2/Cosine/IP metric을 build/search에 전달한다.
- **Step 3 — Planner/cost model**: per-tuple 중심 cost를 k-dominant model로 교체하고, plan-shape regression으로 GPU path 선택과 `enable_cuvs=off` 회피를 잠갔다. JIT 설정은 변경하지 않았다.
- **Step 4 — Write/delete/staleness**: `aminsert`와 `ambulkdelete`가 daemon에 stale mark를 보낸다. `.stale` sidecar로 daemon restart 후에도 stale 상태를 보존한다. `REINDEX` 성공 시 fresh로 복구한다. 재검증 결과 stale CAGRA query가 empty result가 되던 문제는 Phase 2.1(costestimate stale gate)이 닫았다.
- **Step 5 — Large build memory guard**: `cuvs.max_build_mem_mb`를 추가했다. 기본 `0`은 auto mode(`MemAvailable * cuvs.build_mem_safety_ratio`), 양수는 operator hard cap이다. preflight/runtime guard가 oversized build를 clear ERROR로 fail-fast 시킨다.
- **Step 6 — VRAM/NVMe tiered cache**: load/reload 경로가 evict-to-fit을 사용하고, `ensure_vram` progress guard로 무한 루프를 피한다. daemon-global `pg_stat_gpu_cache`가 hits/misses/evictions/reloads/persist_failures/resident_count/VRAM budget을 노출한다.
- **Step 7 — Test matrix**: unit/regress/integration/e2e coverage를 `docs/phase2-test-matrix.md`에 정리했다. Integration scenarios 8-12가 observability, metric/k, staleness, build memory cap, cache eviction/reload를 검증한다.
- **Step 8 — Exit criteria audit**: `docs/phase2-exit-criteria.md`의 10개 기준 전부 `MET`(stale fallback 기준 #5는 Phase 2.1 hotfix 후 MET 복구).

#### 완료 기준과 deviation

- `pg_stat_gpu_search`로 per-index search/error/stale/wall-clock latency를, `pg_stat_gpu_cache`로 daemon-global cache counters를 SQL에서 관측한다.
- fixed `k=100`은 제거됐다. 단, SQL `LIMIT` 직접 추출이 아니라 `cuvs.k` GUC로 top-k를 제어한다.
- L2/Cosine/IP metric은 strategy-number heuristic이 아니라 opclass/opfamily identity에서 결정한다.
- repeated scan, parameterized query, transaction block, cursor, LATERAL rescan은 regression에 잠겼다.
- write/delete 후 stale index는 조용히 fresh GPU path로 사용되지 않고, Phase 2.1의 plan-time reroute(costestimate `.stale` gate)로 seqscan/CPU 정답을 반환한다.
- large build는 streaming이 아니라 memory cap 기반 fail-fast로 처리한다.
- VRAM budget 초과는 eviction/reload/fallback으로 처리하고 cache stats에서 관측한다.
- JIT 설정은 실험 근거 없이 전역 변경하지 않는다.

#### 의도적 deferral

- build streaming / mmap staging: oversized build는 현재 ERROR로 실패한다.
- pending-delta / delta exact search: Phase 3 필수 기능이다.
- automatic peak backend RSS 기록: cross-process PID tracking이 필요하다.
- 10M-scale benchmark: VM capacity가 허용될 때 실행한다.
- `gpu_kernel_us` / `ipc_us` / `cpu_recheck_us` latency split: 현재는 daemon wall-clock latency 하나만 기록한다.

---

### Phase 2.1 — Stale Fallback Hotfix

상태: 완료(2026-05-26). STALE IPC status가 gettuple에서 WARNING 후 `return false`를 반환해 "index scan 종료"로 보이고 planner 재라우팅이 없어, stale query가 정답 CPU path 대신 empty result가 되던 문제를 닫았다.

수정 내역:
- `cuvsamcostestimate`가 `cuvs_index_is_stale()`로 `.stale` sidecar(`<index_dir>/<db>_<idx>.stale`)를 `stat()`해 stale 인덱스의 cost를 `1e9`로 올린다 — CUDA/IPC 없는 순수 local stat이라 planner 금지 규칙을 지킨다. 이로써 stale 인덱스는 seqscan+sort(또는 pgvector CPU path)로 라우팅된다. STALE을 circuit breaker에 섞지 않은 이유: stale은 daemon failure가 아니라 per-index data freshness 상태라 별도 stale gate가 더 명확하다.
- gettuple STALE branch WARNING 문구를 실제 동작에 맞춰 "went stale mid-scan; returning no rows (retry replans to CPU)"로 바꿨다 — costestimate gate가 정상 경로를 막으므로 이 branch는 plan→execute 사이 동시 write로 stale이 된 TOCTOU race에서만 도달한다.
- delete-drift backstop: `aminsert`와 달리 DELETE는 index AM에 per-row 훅이 없고 `ambulkdelete`(VACUUM)가 유일한 신호인데, VACUUM은 dead-page <2% bypass·`vacuum_index_cleanup=off`·wraparound failsafe에서 ambulkdelete를 건너뛰어 `.stale`을 안 남긴다. 이때 graph에 남은 죽은 TID는 heap recheck가 걸러내므로 wrong-answer는 아니지만 recall이 조용히 깎인다. `cuvsamcostestimate`가 `.tids` 헤더의 build count(IPC 없이 32B header read)와 planner live-row 추정을 비교해 삭제 비율이 `cuvs.max_stale_fraction`(기본 0.10, 1.0=비활성)을 넘으면 CPU로 reroute한다. 정상 VACUUM 경로는 bypass가 drift를 ~2%로 묶고 `cuvs.k` over-fetch가 흡수하므로, 이 gate는 ambulkdelete가 억제되는 케이스의 backstop + 운영자 tunable이다.
- 검증: regress edge_cases는 write 후 `enable_cuvs=on`에서 stale 인덱스가 새 row를 CPU로 반환함을 단언한다. Integration Sc 10은 100k-row 테이블에서 fresh일 때 planner가 cagra를 고르고, INSERT/REINDEX/DELETE+VACUUM에서 EXPLAIN이 Seq Scan↔cagra로 정확히 전환되며 정답(id=5, not empty)을 반환함을, 그리고 `vacuum_index_cleanup=off`로 binary 플래그를 억제한 채 20% 삭제 시 drift gate가 reroute하고 `max_stale_fraction=1.0`이면 다시 cagra로 돌아옴(tunable)을 단언한다.

Phase 2.1 완료 기준(전부 충족):
- INSERT/UPDATE 후 stale marker가 있는 상태에서 같은 query가 empty result가 아니라 CPU ground truth와 같은 결과를 반환한다.
- DELETE+VACUUM stale path도 empty result가 아니라 CPU ground truth 또는 명시적 ERROR/fallback 정책을 따른다.
- daemon restart 후 `.stale`이 살아 있는 index도 fresh GPU로 오인되지 않고 CPU path로 재라우팅된다.
- `docs/phase2-exit-criteria.md`의 criterion #5를 `MET`으로 되돌릴 수 있다.

---

### Phase 3 — Scale Out / Large Index Storage

목표: Phase 2의 single-node CAGRA core를 운영 workload와 대규모 저장소로 확장한다. Phase 3는 하나의 거대한 milestone이 아니라, 각각 독립적으로 검증 가능한 5개 subphase로 진행한다.

#### Phase 3A — Pending Delta / Delta Exact Search

Phase 2의 stale fallback은 정합성을 지키지만, 쓰기가 발생한 순간 GPU path를 포기한다. 운영 workload에 INSERT/UPDATE가 조금이라도 섞이면 너무 보수적이므로 Phase 3에서는 pending-delta를 필수 기능으로 구현한다.

Phase 3A policy split:
- `.delta` = INSERT/UPDATE new version을 보정하는 append-only derived artifact.
- `.stale` = delta로 보정할 수 없어 CPU reroute가 필요한 상태.
- INSERT/UPDATE가 delta append에 성공하면 `.stale`로 표시하지 않고 daemon `MARK_STALE`도 보내지 않는다. base CAGRA는 계속 검색 가능해야 backend가 base candidates + CPU delta candidates를 merge할 수 있다.
- delta append 실패, delta 손상, generation mismatch, threshold 초과는 `.stale` 또는 plan-time gate로 CPU reroute한다.
- DELETE/VACUUM은 이번 increment에서 기존 Phase 2.1 정책을 유지한다. `ambulkdelete`는 `.stale` mark를 남기고 CPU reroute하며, DELETE tombstone/snapshot-aware old-version correction은 후속 단계다.

Phase 3A entry gate:
- Phase 2.1 stale fallback hotfix가 완료되어야 한다. delta correction이 불가능하거나 손상된 경우 CPU fallback이 실제로 동작해야 하며, gettuple `return false` 기반 empty result를 fallback으로 간주하지 않는다.

목표:
- 마지막 successful CAGRA build/REINDEX 이후의 INSERT/UPDATE를 pending-delta store에 보관한다.
- DELETE/UPDATE old version tombstone은 CPU MVP 범위에 넣지 않는다. UPDATE의 new version은 delta에 기록하고, old/dead TID는 heap recheck, over-fetch, delete-drift gate, 기존 VACUUM stale path로 방어한다.
- query 시 base CAGRA search와 CPU-side pending-delta exact search를 함께 수행한다.
- base candidates와 delta candidates를 over-fetch 기반으로 top-k merge한다.
- PostgreSQL heap recheck/MVCC는 마지막 방어선으로 유지한다.

검색 흐름:
```
query vector
  -> base CAGRA search over immutable graph with k + slop
  -> CPU exact search over pending-delta rows visible to the current snapshot
  -> metric-compatible merge/re-rank to k + slop candidates
  -> PostgreSQL heap recheck / MVCC visibility
  -> if visible rows < k and more candidates may exist, retry with larger slop or CPU fallback
```

Delivered scope:
- **3A-1 CPU-exact MVP**: backend-side `.delta` append + CPU exact merge로 INSERT/UPDATE new version을 보정한다.
- **3A-2 GPU delta cache core**: daemon이 `.delta`를 generation/mtime 기준 lazy reload하고, resident GPU brute-force delta cache를 base CAGRA 결과와 merge한다. IPC reply의 `delta_merged` flag로 backend가 CPU merge fallback을 생략할지 결정한다.
- **3A-3 delta controls/stats**: `cuvs.delta_search=auto|cpu|gpu`와 `pg_stat_gpu_search` delta columns(`delta_rows`, `delta_generation`, `delta_vram_bytes`, `delta_merged_count`, `delta_search_mode`)로 delta 경로를 관측·강제할 수 있다.
- **3A-4 tombstone/cleanup**: `.tombstone` sidecar와 snapshot-aware backend filtering으로 DELETE/UPDATE-old dead TID를 보정한다. `ambulkdelete`는 dead TID를 tombstone으로 기록하고, tombstone append 실패·cap 초과·unusable 상태에서만 stale fallback으로 닫힌다.
- daemon-side GPU delta cache는 성능 경로이고, backend CPU merge/tombstone filter는 daemon이 merge하지 못한 경우의 correctness fallback이다.

구현 항목:
- pending-delta 저장소: backend-visible `.delta` sidecar. daemon은 같은 artifact를 읽어 resident GPU brute-force cache를 구성한다.
- delta entry schema: TID, vector, op type(INSERT/UPDATE-new), base generation, write generation, xmin 또는 heap recheck에 필요한 최소 metadata.
- `aminsert` policy: delta append 성공 시 daemon `MARK_STALE`을 보내지 않는다. delta append 실패 시 기존 stale path로 fail closed한다.
- persistent generation marker: delta artifact가 어떤 base CAGRA generation을 보정하는지 확인한다. generation mismatch는 GPU+delta를 사용하지 않고 CPU reroute한다.
- merge algorithm: base result와 delta exact result의 metric-compatible distance ordering을 보장한다. L2/cosine/IP 각각의 정렬 방향, normalization, tie-break를 명시하고 pgvector ground truth로 검증한다.
- over-fetch policy: base CAGRA와 delta exact 모두 `k + slop`을 대상으로 merge한다. heap recheck 후 visible rows가 k 미만이면 slop 확대 또는 CPU fallback한다.
- write capture batching: `COPY`/bulk INSERT가 per-row IPC storm을 만들지 않도록 batch capture 또는 threshold-then-CPU-fallback 정책을 둔다.
- threshold/backpressure policy: `cuvs.rebuild_threshold` 또는 `cuvs.max_delta_rows` 초과 시 GPU+delta path를 중지하고 CPU fallback한다. background rebuild는 Phase 3A MVP 범위 밖이며, 자동 실행 주체가 생기기 전까지는 `REINDEX` 권고만 한다.
- stats: `pg_stat_gpu_search`는 delta rows/generation/VRAM/search mode/merge count를 노출한다.
- rebuild compaction: successful REINDEX 후 pending-delta를 비우고 새 base generation으로 교체한다.

정합성 원칙:
- pending-delta가 없으면 INSERT된 새 벡터가 CAGRA 후보에 절대 나오지 않으므로 stale GPU search는 틀릴 수 있다.
- pending-delta exact search가 들어오기 전까지 Phase 2의 CPU fallback 정책을 유지한다.
- delta merge가 실패하거나 delta artifact가 손상되면 GPU path를 사용하지 않고 CPU fallback한다.
- Phase 3A MVP의 delta는 source-of-truth가 아니라 derived artifact다. WAL과 같은 durability 계약을 흉내 내지 않는다.
- daemon restart 후 delta artifact가 없거나 손상되면 base CAGRA를 fresh+complete로 오인하지 않고 CPU fallback한다.
- `.stale`은 더 이상 모든 INSERT/UPDATE의 기본 결과가 아니다. valid delta가 있는 INSERT/UPDATE는 corrected-not-stale 상태이고, `.stale`은 delta/tombstone failure, cap 초과, unsafe 상태의 fail-closed marker다.
- tombstone filtering은 snapshot-aware여야 한다. 전역 tombstone으로 UPDATE old TID를 제거하면 오래된 snapshot에서 visible한 tuple을 누락할 수 있으므로 금지한다.

PostgreSQL transaction/MVCC caveat:
- index AM write callback은 transaction commit 전에 호출될 수 있다.
- aborted transaction의 tuple TID가 delta에 남을 수 있으므로 heap recheck/MVCC visibility가 최종 필터다.
- heap recheck는 후보를 제거할 수만 있고, merge에서 이미 빠진 후보를 되살릴 수 없다. 따라서 tombstone filtering은 heap recheck 전 후보 단계에서 snapshot-aware로 적용한다.
- rollback을 완벽히 추적하는 WAL-like delta는 Phase 3A MVP 범위가 아니다.
- cleanup policy는 명시해야 한다: REINDEX/build generation 전환 시 delta/tombstone을 정리하고, cleanup 실패 시 CPU fallback한다.

검증:
- INSERT 후 REINDEX 전에도 새 row가 GPU+delta merged top-k에 포함된다.
- UPDATE 후 new vector delta가 보이고, old/dead base TID는 snapshot-aware tombstone filter로 처리된다.
- DELETE/VACUUM은 tombstone path를 사용하며, tombstone append 실패·cap 초과·unusable 상태에서만 `.stale` CPU reroute로 fallback한다.
- delta threshold 초과 시 GPU+delta path가 중지되고 CPU reroute 또는 REINDEX 권고가 발생한다.
- daemon restart 후 delta가 유실되어도 generation/delta validity gate 때문에 incomplete GPU 결과를 서빙하지 않는다.
- aborted transaction delta가 가까운 distance를 갖더라도 heap recheck 후 k개 recall이 유지되거나 CPU fallback한다.
- L2/cosine/IP 각각에서 GPU delta merge 결과가 pgvector CPU ground truth와 top-k가 일치한다. Integration Sc 15는 daemon log assertion으로 resident GPU delta cache가 실제 build됐음을 확인한다.
- random INSERT/UPDATE/DELETE/query interleaving property test를 추가한다.
- bulk INSERT/COPY에서 delta append와 threshold/backpressure 정책이 daemon IPC storm 없이 동작한다.

Phase 3A 완료 기준:
- write-heavy workload에서 stale CPU fallback 없이 base CAGRA + pending-delta exact search로 정합한 top-k를 반환한다.
- INSERT/UPDATE, aborted transaction, daemon restart에서 틀린 GPU 결과를 반환하지 않는다.
- DELETE/VACUUM은 tombstone correction을 기본 경로로 사용하고, tombstone이 안전하지 않을 때 `.stale` CPU reroute로 닫힌다.
- delta 손상 또는 cleanup 실패는 CPU fallback으로 닫힌다.
- over-fetch recall, restart fail-closed, metric-specific merge가 regression/integration/property test로 검증된다.

Phase 3A follow-ups:
- random INSERT/UPDATE/DELETE/query interleaving property test를 더 넓은 데이터 크기와 transaction snapshot 조합으로 확장한다.
- delta/tombstone cap과 REINDEX 권고를 운영자가 보기 쉬운 alert/playbook 항목으로 묶는다.

#### Phase 3B — DiskANN / Vamana Local NVMe

목표:
- VRAM resident CAGRA 한계를 넘는 index를 local NVMe 기반으로 검색한다.
- CAGRA는 hot set, DiskANN/Vamana는 cold/large set으로 역할을 분리한다.

구현 항목:
- `USING diskann` AM 추가.
- cuVS Vamana GPU build + CPU/NVMe search.
- local manifest/checksum으로 DiskANN artifact integrity를 검증한다.
- 같은 column에 hot/cold index가 공존하는 tiered search를 구현한다.
- tiered search는 metric identity와 top-k merge ordering을 보장한다.

Phase 3B 완료 기준:
- 100M+ vector 또는 VRAM을 초과하는 index를 단일 VRAM resident 전제 없이 검색한다.
- local NVMe DiskANN/Vamana search가 CAGRA-only path와 동일 metric semantics를 유지한다.
- hot CAGRA + cold DiskANN tiered search가 동작한다.

#### Phase 3C — Artifact Manifest / Object Storage-backed Immutable Index Snapshots

구현 항목:
- local `cuvs.index_dir` artifact를 object storage snapshot으로 확장한다.
- object storage snapshot은 **heap/table 배포 수단이 아니다**. PostgreSQL heap은 physical replication, basebackup/WAL archive, managed replica, logical replication, dump/restore 같은 PostgreSQL 메커니즘이 책임진다.
- pg_cuvs snapshot은 heap-compatible PostgreSQL node가 이미 존재할 때, GPU/NVMe index rebuild 시간을 줄이는 derived index artifact cache다.
- MVP provider는 GCS로 시작하고, provider-neutral manifest를 유지한다. S3는 후속 provider로 추가 가능해야 한다.
- 경로 예: `gs://<bucket>/pg_cuvs/<cluster_id>/<database_oid>/<index_oid>/<version>/`
- manifest, checksum, version을 둬 partial upload/corrupt upload/stale upload를 감지한다.
- local NVMe는 cache, object storage는 재사용 가능한 artifact store로 취급한다.
- index artifact는 WAL 대상이 아닌 derived data로 유지한다.

Manifest contract:
- `database_oid`
- `table_oid`
- `index_oid`
- heap compatibility identity: `relfilenode` 또는 build source identity, plus any snapshot/checkpoint marker needed to prove the local heap can use the TID mapping
- `base_generation`
- metric, dimension, vector count
- artifact paths와 checksums
- build timestamp
- pg_cuvs version과 cuVS version
- stale/delta compatibility marker
- replica compatibility marker: target node must already have a compatible PostgreSQL heap; otherwise the artifact must not be loaded

Phase 3C 완료 기준:
- manifest-backed GCS snapshot upload/download가 성공한다.
- partial upload, corrupt artifact, stale manifest, missing local cache가 감지되고 복구 또는 CPU fallback으로 닫힌다.
- snapshot artifact는 PostgreSQL WAL source-of-truth가 아니라 재생성 가능한 derived data로 유지된다.
- heap-incompatible node는 artifact를 로드하지 않고 REINDEX 또는 PostgreSQL restore/replication 절차를 요구한다.

#### Phase 3D — Replica / Multi-node Loading + Async Warmup

Delivered scope:
- daemon startup no longer blocks on GCS hydration. Missing local artifacts are registered as cold entries, the UDS socket opens, and background warmup workers download/load artifacts progressively.
- fixed-size warmup thread pool with bounded queue and `--warmup-threads` / `cuvs.warmup_threads`.
- query-time cache miss handling: if a cold artifact is requested before warmup completes, the daemon records the miss, queues or requeues warmup, and the backend falls back to CPU until the index becomes hot.
- `pg_stat_gpu_search` exposes warmup state, last warmup timestamp, warmup duration, download count, and cache miss count for hot and cold entries.

구현 항목:
- primary에서 build 후 object storage upload.
- read replica는 heap scan rebuild 없이 object storage에서 download/load.
- catalog OID, relfilenode 변화, manifest version mapping을 관리한다.
- daemon startup 시 metadata만 scan하고 cold index를 background prefetch한다.
- NVMe cache miss 시 object storage download 후 VRAM promotion한다.
- warmup 상태와 miss reason을 stats view에 노출한다.

Phase 3D 완료 기준:
- 새 daemon 또는 read replica가 heap rebuild 없이 object storage snapshot에서 index를 복구한다.
- startup은 metadata scan으로 빠르게 시작하고, cold artifact는 background warmup으로 적재한다.
- warmup/cache miss/download/reload 상태가 stats view에 노출된다.

#### Phase 3E — Multi-GPU / Sharding

Scope decision:
- Phase 3E의 본체는 PostgreSQL partitioning recipe가 아니라 **daemon-level multi-GPU runtime**이다.
- PostgreSQL partitioning은 하나의 logical parent table을 여러 physical CAGRA indexes로 나누어 multi-GPU runtime을 활용하는 integration recipe일 뿐이다.
- 단일 physical CAGRA index를 여러 GPU shard로 자동 분할하는 internal sub-index sharding은 3E MVP가 아니라 optional/future subphase다.
- 이미 완료된 Phase 3A/3C/3D 번호는 유지한다. 3E 내부 subphase만 분리한다.

3E-0 Architecture decision:
- PG-Strom 조사 결과를 반영한다: partitioning과 multi-GPU는 별개다. PG-Strom의 partition pushdown은 PostgreSQL child relation 최적화이고, multi-GPU device support는 별도 runtime/scheduler 기능이다.
- pg_cuvs도 partitioning을 multi-GPU의 구현체로 착각하지 않는다. multi-GPU는 daemon의 GPU context, index placement, dispatch, stats 계층에서 구현한다.

3E-1 Daemon-level multi-GPU runtime:
- daemon startup에서 모든 CUDA device를 발견하고 device별 cuVS/CUDA context와 worker/stream 상태를 초기화한다.
- `IndexEntry`를 단일 전역 VRAM registry에서 per-GPU resident registry로 확장한다.
- build/load/warmup 시 physical index artifact를 하나의 GPU에 배치한다. 기본 placement는 VRAM-aware이며, round-robin은 tie-breaker로만 쓴다.
- search request는 index가 resident한 GPU context로 dispatch한다.
- GPU별 VRAM budget, resident index count, search count, evictions, reloads, failures를 추적한다.
- planner/backend가 CUDA를 직접 touch하지 않는 기존 계약을 유지한다.

3E-2 Placement and degraded policy:
- GPU unavailable, VRAM pressure, placement failure, reload failure를 구분한다.
- index placement 실패 시 DDL은 명확한 ERROR, SELECT는 CPU fallback 또는 circuit-breaker reroute로 닫는다.
- GPU별 eviction은 다른 GPU의 resident indexes에 영향을 주지 않아야 한다.
- `cuvs.max_vram_mb`는 global-only가 아니라 per-device budget으로 해석하거나 별도 per-GPU override를 둔다.

3E-3 Partitioned logical-table integration:
- 하나의 parent table에 대한 query가 child partition CAGRA indexes를 사용하고, 각 physical index가 3E runtime에 의해 GPU별로 분산 배치되는 recipe를 문서화/검증한다.
- global top-k correctness는 `enable_cuvs=off` CPU exact result와 비교한다.
- single-GPU VM에서는 planner shape와 correctness만 검증하고, 실제 GPU별 placement는 multi-GPU VM에서 acceptance로 검증한다.

3E-4 Multi-GPU hardware acceptance:
- ephemeral 4/8-GPU VM 또는 DGX/HGX급 노드에서 검증한다.
- 여러 physical CAGRA indexes가 서로 다른 GPUs에 resident함을 stats와 daemon log로 확인한다.
- concurrent searches가 GPU별로 dispatch되고, per-GPU VRAM/cache counters가 독립적으로 움직이는지 검증한다.

3E-5 Optional/future internal sub-index sharding:
- 단일 non-partitioned table / 단일 logical CAGRA index를 여러 GPU shard로 나누는 설계는 별도 고급 기능으로 둔다.
- 이 기능은 CAGRA graph split, per-shard over-fetch, daemon fanout, top-k merge, recall policy를 새로 요구하므로 3E MVP 완료 기준에 넣지 않는다.

Phase 3E 완료 기준:
- daemon-level multi-GPU runtime이 여러 CUDA devices를 관리하고, physical CAGRA indexes를 GPU별로 배치/검색할 수 있다.
- GPU별 VRAM budget과 eviction policy가 서로 간섭하지 않는다.
- GPU unavailable / placement failure / VRAM pressure에 대한 degraded mode가 명확하다.
- partitioned parent table recipe는 하나의 logical table query로 보이면서 여러 physical CAGRA indexes를 multi-GPU runtime에 태우는 방식을 검증한다.
- internal sub-index sharding은 명시적으로 future work로 남긴다.

#### Phase 3F — Operational Playbooks / Runbooks

목표:
- Phase 3A-3E의 실제 운영 표면이 고정된 뒤 최종 운영 playbook을 작성한다. Phase 3D 중간 상태에서 replica/object-storage playbook을 자세히 만들면 3E multi-GPU/sharding 도입 후 절차가 다시 바뀌므로, Phase 3 최종 playbook은 3E 완료 이후로 둔다.

구현 항목:
- replica bootstrap / instance replacement runbook.
- object storage permission failure, corrupt manifest, heap compatibility mismatch 대응.
- async warmup/cache hydration 진단.
- multi-GPU runtime warmup, per-GPU VRAM pressure, placement failure/degraded mode 복구.
- capacity planning: VRAM, NVMe, object storage artifact size, delta growth.
- on-call triage: stats view, daemon logs, PostgreSQL warnings/errors, GCS audit/logging 확인 순서.

Phase 3F 완료 기준:
- Phase 3A-3E의 기능/관측성 표면을 기준으로 `docs/playbooks/`에 최종 운영 runbook을 작성한다.
- 각 playbook은 최소 하나의 검증된 GPU VM 또는 replica 시나리오를 근거로 한다.
- Phase 1.5 playbook은 baseline 문서로 유지하고, Phase 3 playbook은 multi-node/multi-GPU 운영 절차를 별도 문서로 둔다.

Phase 3 전체 완료 기준:
- Phase 3A-3F의 subphase 완료 기준을 모두 만족한다.
- 각 subphase는 독립적으로 중단/릴리스 가능하며, 다음 subphase 미완료가 이전 subphase의 정합성을 깨지 않는다.

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
| write/stale index | delta-corrected INSERT/UPDATE, stale marker fallback, VACUUM/REINDEX 계약 |
| large build memory pressure | chunked streaming, mmap staging, shard build |
| artifact corruption | tmp+fsync+rename, manifest/checksum, pair validation |

---

## pg_cuvs vs 대안

| | pg_cuvs | pgvectorscale | pg_lance | Milvus/Qdrant |
|---|---|---|---|---|
| 실행 모델 | PG + GPU sidecar | PG in-process | PG + LanceDB | 전용 서버 |
| SQL/트랜잭션 | 완전 지원 목표 | 완전 지원 | 제한적 | 없음 |
| GPU 가속 | CAGRA/Vamana build | 없음 | 없음 | 일부 |
| 데이터 규모 | VRAM hot + NVMe/object storage cold | NVMe DiskANN | S3/Lance | 외부 cluster |
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
