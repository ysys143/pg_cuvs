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

**2026-05-30~31 Competitive Benchmark 결과 (BENCHMARK_CROSSOVER.md §11~12)**:
- 1M×384 pilot: CAGRA가 HNSW 대비 build 13×, latency 11× 우위. crossover ≈ N 10K~100K.
- 50M×384 competitive: HNSW(p50=13ms,QPS=546), vchordrq(p50=49ms,QPS=152,recall=0.9991),
  DiskANN(2GB cache TIMEOUT), CAGRA(2×A100-40GB VRAM 부족 FAILED).
- **Phase 3B go/no-go**: NO-GO. cuVS PQFlash 경로 미완성, DiskANN 50M에서 실용 불가.
  pg_cuvs = GPU VRAM hot tier 포지셔닝 고정. 재검토 조건: 1B+ 수요 확인 또는 cuVS stable.
  (design/PHASE_3B_DECISION.md, ADR-026 참조)
- **다음 우선순위 (ADR-027)**: cost model recalibration(완료) → Phase 3I/3J(완료, ADR-037) →
  3H-full playbooks → release hardening. 신규 무관 기능 추가 금지.

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
- index_dir 해석(ADR-045): 위 게이트들의 `<index_dir>`는 세션 GUC만이 아니라 **인덱스 `index_dir` reloption(있으면) > `cuvs.index_dir` GUC > `$PGDATA/cuvs_indexes`** 3단계로 해석된다. 빌드 디렉터리를 `pg_class.reloptions`에 self-describe하므로, 빌드 세션과 다른(또는 미설정) GUC를 가진 별도 연결도 동일 디렉터리에서 아티팩트를 찾아 cross-session seqscan 폴백을 막는다. 플래너 게이트는 Oid만 가지므로 `try_index_open(oid, NoLock)`으로 rd_options를 읽는다 — 플래너가 이미 락을 보유하므로 refcount-only relcache lookup(IPC/CUDA 없음). reloption 부재 시 기존 GUC 경로와 byte-identical. 검증: regress `reloption_dir`(11/11) + no-GUC 연결 cross-session Index Scan 실증.

Phase 2.1 완료 기준(전부 충족):
- INSERT/UPDATE 후 stale marker가 있는 상태에서 같은 query가 empty result가 아니라 CPU ground truth와 같은 결과를 반환한다.
- DELETE+VACUUM stale path도 empty result가 아니라 CPU ground truth 또는 명시적 ERROR/fallback 정책을 따른다.
- daemon restart 후 `.stale`이 살아 있는 index도 fresh GPU로 오인되지 않고 CPU path로 재라우팅된다.
- `docs/phase2-exit-criteria.md`의 criterion #5를 `MET`으로 되돌릴 수 있다.

---

### Phase 3 — Scale Out / Large Index Storage

목표: Phase 2의 single-node CAGRA core를 운영 workload와 대규모 저장소로 확장한다. Phase 3는 하나의 거대한 milestone이 아니라, 각각 독립적으로 검증 가능한 subphase로 진행한다.

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
- **3A-3 delta controls/stats**: `cuvs.delta_search`(enum GUC: `auto`(기본)|`cpu`|`gpu`)와 `pg_stat_gpu_search` delta columns(`delta_rows`, `delta_generation`, `delta_vram_bytes`, `delta_merged_count`, `delta_search_mode`)로 delta 경로를 관측·강제할 수 있다. (GUC는 `DefineCustomEnumVariable`이라 `SET cuvs.delta_search='cpu'` 형태의 문자열 라벨을 받는다 — 잘못된 라벨은 `invalid value` ERROR. SRF 출력 컬럼 `delta_search_mode`는 마지막 검색이 실제 쓴 모드(gpu/cpu/none)로 GUC와 별개.)
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

**완료 (2026-06-06, ADR-047)**: 메커니즘은 2026-05 WIP로 기구현·정확했으나 위 완료 기준이 요구하는 검증이 비어 미완 표기됐던 것을, 본 세션에서 회귀+격리+e2e로 certify(false-done 역방향 해소). 검증 산출물: `test/sql/pending_delta.sql`(cap fail-closed·Sc15 GPU delta cache built·tri-mode·max_delta_rows=0), `test/sql/delta_recall.sql`(delete-drift recall — over-fetch fix red→green), `test/specs/delta_tombstone_snapshot.spec`·`delta_interleaving.spec`(pg_isolation_regress: 동시 DELETE 가시성·미커밋 delta 격리), `infra/scripts/delta-restart-e2e.sh`(valid delta restart 생존 + corrupt delta fail-closed), `test/unit/test_cuvs_util.c::test_tombstone_format`. VM(A100/PG16): installcheck 15/15 + isolation 2/2 GREEN, e2e PASS. 발견·수정: delete-drift 임계 아래 + top-k 집중 삭제에서 recall<LIMIT → `cuvs_gettuple` tombstone-aware over-fetch(`k += min(n_tomb, cuvs_k)`).

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
- 단일 physical CAGRA index를 여러 GPU shard로 자동 분할하는 internal sub-index sharding은 3E MVP가 아니라 별도 Phase 3F다.
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

3E-1 verified evidence:
- multi-GPU VM에서 두 physical CAGRA indexes가 서로 다른 GPUs에 resident함을 확인했다: `mgpu_a_cagra -> GPU 0`, `mgpu_b_cagra -> GPU 1`.
- `pg_stat_gpu_search`는 per-index `gpu_device_id`를 노출하고, `pg_stat_gpu_cache`는 GPU별 `resident_count`/VRAM usage를 별도 row로 반환한다.
- 이 검증은 index-level placement의 acceptance이며, partitioned logical-table integration과 degraded/failure policy는 3E 후속 subphase에 남긴다.

3E-2 Placement and degraded policy:
- GPU unavailable, VRAM pressure, placement failure, reload failure를 구분한다.
- index placement 실패 시 DDL은 명확한 ERROR, SELECT는 CPU fallback 또는 circuit-breaker reroute로 닫는다.
- GPU별 eviction은 다른 GPU의 resident indexes에 영향을 주지 않아야 한다.
- `cuvs.max_vram_mb`는 global-only가 아니라 per-device budget으로 해석하거나 별도 per-GPU override를 둔다.

3E-2 verified evidence:
- single-GPU VM: unit tests 120/120, integration 18 scenarios all pass.
- single-GPU VM: 1MB budget placement failure reports OOM while daemon stays alive.
- single-GPU VM: 4MB budget eviction-isolation scenario triggers `evictions=1` and daemon stays alive.
- multi-GPU VM: per-GPU cache counters are independent (`hits=1` on GPU 0 and `hits=1` on GPU 1 after targeted searches).
- multi-GPU VM: index spreading remains verified with physical indexes resident on GPU 0 and GPU 1.

3E-3 Partitioned logical-table integration:
- 하나의 parent table에 대한 query가 child partition CAGRA indexes를 사용하고, 각 physical index가 3E runtime에 의해 GPU별로 분산 배치되는 recipe를 문서화/검증한다.
- global top-k correctness는 `enable_cuvs=off` CPU exact result와 비교한다.
- single-GPU VM에서는 planner shape와 correctness만 검증하고, 실제 GPU별 placement는 multi-GPU VM에서 acceptance로 검증한다.

3E-3 checklist:
- hash/range/list partition 중 최소 하나의 권장 DDL recipe를 `infra/scripts/` 또는 `docs/recipes/`에 둔다.
- parent table query만 사용한다. 사용자가 child partition을 직접 query해야 하는 recipe는 완료로 보지 않는다.
- 각 child partition에 physical CAGRA index를 생성하고, `pg_stat_gpu_search.index_name/gpu_device_id/resident`로 배치를 확인한다.
- parent query의 global top-k 결과를 `enable_cuvs=off` CPU exact query와 비교한다.
- planner shape를 `EXPLAIN (COSTS OFF)`로 기록한다. PostgreSQL이 partition append/merge를 어떻게 구성하는지 문서에 남긴다.
- multi-GPU VM에서는 child partition indexes가 2개 이상 GPU에 분산 resident해야 한다.

3E-3 verified evidence:
- `infra/scripts/multigpu-partition-recipe.sql` provides the parent-table query recipe and correctness check; `infra/scripts/benchmark-multigpu.sh` provides the benchmark harness.
- multi-GPU VM hash partition distribution was balanced: p0 10023 rows, p1 9977 rows.
- child CAGRA indexes were distributed across GPUs: `recipe_p0_cagra -> GPU 1`, `recipe_p1_cagra -> GPU 0`.
- parent-table query planned as `Limit -> Merge Append -> Index Scan using recipe_p0_cagra / recipe_p1_cagra`, with no direct child-partition query required.
- global top-k matched CPU exact (`enable_cuvs=off`) exactly: `{42,1157,1342,3219,13538,14144,14689,16814,17103,19806}`.
- per-GPU stats moved independently during the parent-table query: GPU 0 hits=2, GPU 1 hits=2.

3E-4 Multi-GPU hardware acceptance:
- ephemeral 4/8-GPU VM 또는 DGX/HGX급 노드에서 검증한다.
- 여러 physical CAGRA indexes가 서로 다른 GPUs에 resident함을 stats와 daemon log로 확인한다.
- concurrent searches가 GPU별로 dispatch되고, per-GPU VRAM/cache counters가 독립적으로 움직이는지 검증한다.

3E-4 checklist:
- daemon startup log와 SQL stats에서 usable GPU count가 기대값과 일치한다.
- 최소 2개 이상의 CAGRA indexes가 서로 다른 GPUs에 resident한다.
- `pg_stat_gpu_cache`가 GPU별 row를 반환하고, `resident_count`, `vram_used_mb`, `evictions`, `reloads`, `search_count`가 device별로 독립적으로 갱신된다.
- VRAM pressure 테스트에서 target GPU의 LRU만 evict되고 다른 GPU의 resident indexes는 유지된다.
- concurrent search benchmark가 GPU별 dispatch를 실제로 발생시킨다. 단순 sequential smoke만으로는 3E-4 완료로 보지 않는다.
- GPU unavailable 또는 placement failure 시나리오가 3E-2 정책과 같은 결과를 낸다.
- multi-GPU VM은 검증 후 stop/delete하거나 비용 상태를 명시적으로 보고한다.

3E-4 verified evidence:
- multi-GPU VM detected GPU 0 and GPU 1 at daemon startup.
- 2+ CAGRA indexes were resident on different GPUs.
- `pg_stat_gpu_cache` returned independent per-GPU counters; targeted searches incremented hits on each GPU separately.
- concurrent benchmark passed: 805 TPS, 8052 transactions, with GPU 0 and GPU 1 each recording +8052 hits.
- VRAM pressure eviction isolation passed: GPU 0 evictions=1 while GPU 1 evictions=0.
- `--gpu-devices` restriction passed: GPU 0 only, one cache row exposed.
- multi-GPU VM was stopped after validation (`TERMINATED`).

Phase 3E 완료 기준:
- daemon-level multi-GPU runtime이 여러 CUDA devices를 관리하고, physical CAGRA indexes를 GPU별로 배치/검색할 수 있다.
- GPU별 VRAM budget과 eviction policy가 서로 간섭하지 않는다.
- GPU unavailable / placement failure / VRAM pressure에 대한 degraded mode가 명확하다.
- partitioned parent table recipe는 하나의 logical table query로 보이면서 여러 physical CAGRA indexes를 multi-GPU runtime에 태우는 방식을 검증한다.

Phase 3E status: MVP complete. 3E-1 placement, 3E-2 degraded/placement policy,
3E-3 partitioned logical-table integration, and 3E-4 hardware acceptance passed.
True single-index multi-GPU CAGRA sharding is split out as Phase 3F.

#### Phase 3F — True Multi-GPU CAGRA Sharding

3F-0 Design lock (확정 결정, ADR-021):
- SW MVP 먼저, HW acceptance(3F-6)는 별도 게이트.
- MVP fanout은 sequential(기존 `g_index_mutex` 직렬화 유지). parallel fanout은 follow-up.
- shard count는 explicit `cuvs.shard_count=N`. 기본값 `0`/`1`은 기존 unsharded 동작을 byte-identical하게 유지하고 `N>=2`에서만 sharding 활성화. auto shard count는 follow-up.
- shard 분할은 build-order contiguous range. clustering 기반 assignment는 현재 로드맵에서 제외한다.
- fail-closed: shard 하나라도 실패면 partial 금지(SELECT CPU fallback, DDL ERROR).
- 3F sub-phase 진행 순서: 3F-1 artifact+build → 3F-2 daemon runtime+reload → 3F-3 sequential fanout search+merge → 3F-4 fail-closed+delta/tombstone → 3F-5 SW MVP verification → 3F-6 multi-GPU hardware acceptance(완료 게이트) → 3F-7 follow-up(parallel fanout, auto count, daemon-side shard delta cache, per-shard k+slop, partial degraded mode, object snapshot manifest 확장).

목표:
- 단일 non-partitioned table / 단일 logical CAGRA index를 내부적으로 N개 shard로 나누고, 하나의 query를 여러 GPUs에 fanout한 뒤 daemon에서 global top-k를 merge한다.
- 사용자 SQL/DDL surface는 기존과 같다: `CREATE INDEX ... USING cagra (...)`와 parent/child partition 직접 query 없이 일반 table query를 유지한다.
- Phase 3E의 index-level multi-GPU runtime은 기반으로 사용하지만, Phase 3F는 별도 artifact/runtime/query-merge 기능으로 취급한다.

Shard model:
- 첫 구현은 build-order contiguous ranges로 shard한다. 즉 global `.tids` 배열의 `[start,end)` range가 shard 하나에 대응한다.
- Vector clustering 기반 shard assignment는 제외한다. 이 작업은 대규모 데이터에 대해 k-means/assignment를 반복 수행해야 하므로 단순 full scan보다 비싸고, 기본 all-shard fanout 구조에서는 shard pruning 이득도 거의 없다. segment-level incremental rebuild 같은 별도 인덱스 아키텍처를 도입할 때만 다시 검토한다.
- 각 shard는 독립 cuVS CAGRA artifact이며 하나의 GPU에 resident한다. shard 하나가 여러 GPUs에 걸치지 않고, logical index가 여러 shard/GPU를 소유한다.

Artifact contract:
- legacy unsharded index는 현재처럼 `<db>_<index>.cagra` + `<db>_<index>.tids`를 유지한다.
- sharded index는 global `<db>_<index>.tids`를 generation/TID source of truth로 유지하고, `<db>_<index>.shards` manifest를 commit marker로 추가한다.
- shard artifacts는 예: `<db>_<index>.s000.cagra`, `<db>_<index>.s001.cagra` 형태를 사용한다.
- `.shards` manifest는 shard count, shard id, global TID start offset, n_vecs, dim, metric, assigned gpu, artifact checksum, base `.tids` CRC를 포함한다.
- durable DDL contract는 all-or-nothing이다. 모든 shard `.cagra.tmp`, global `.tids.tmp`, `.shards.tmp`를 fsync한 뒤 final rename하고, `.shards` manifest rename을 마지막 commit marker로 삼는다. startup은 manifest가 없거나 shard/checksum이 맞지 않으면 logical sharded index를 load하지 않는다.

Daemon/runtime design:
- `IndexEntry`는 unsharded entry와 sharded logical entry를 구분한다. sharded logical entry는 `ShardEntry[]`를 소유한다.
- `ShardEntry`는 shard id, `CuvsCagraIndex` handle, global TID offset, n_vecs, gpu_device_id, vram_bytes, search/error counters, last status를 가진다.
- 3F build는 explicit `cuvs.shard_count=N`으로 shard count를 결정한다. auto VRAM-based shard count는 Phase 3G에서 다룬다.
- placement는 3E-1/3E-2의 VRAM-aware policy를 shard 단위로 적용한다. 한 logical index의 shards는 가능한 한 여러 GPUs에 spread하고, 특정 GPU restriction(`--gpu-devices`)을 존중한다.
- search는 one IPC request per logical index를 유지한다. daemon이 shard별 CAGRA search를 병렬 실행하고, metric-aware comparator로 global top-k를 merge해서 기존 reply format(TIDs + distances)을 반환한다.
- per-shard request k는 최소 `k`로 시작한다. 그래야 global top-k가 한 shard에 몰리는 경우도 후보 부족으로 놓치지 않는다. 추후 `cuvs.shard_overfetch`로 `k + slop`을 조정할 수 있다.
- shard 하나라도 unavailable/reload 실패/VRAM failure이면 partial ANN result를 반환하지 않는다. 기본 정책은 SELECT CPU fallback, DDL/REINDEX ERROR다. partial degraded recall은 별도 opt-in GUC 없이는 허용하지 않는다.

Delta/tombstone compatibility:
- `.delta` generation은 global `.tids` CRC에 계속 묶는다.
- 1차 구현은 sharded base search와 backend CPU delta merge fallback을 유지한다. daemon-side GPU delta cache를 shard별로 split하는 것은 후속 최적화다.
- `.tombstone`은 global TID 기준으로 유지하고 backend snapshot-aware filtering을 계속 적용한다.
- REINDEX는 global `.tids`, `.shards`, shard `.cagra` artifacts, `.delta`, `.tombstone`을 같은 generation 경계로 정리한다.

Observability:
- `pg_stat_gpu_search`는 logical index row를 유지한다. unsharded index는 `gpu_device_id`를 표시하고, sharded index는 `gpu_device_id`를 NULL/unknown으로 두거나 대표값 없이 `shard_count`를 노출한다.
- 새 view `pg_stat_gpu_shards`를 추가해 `index_oid`, `index_name`, `shard_id`, `gpu_device_id`, `n_vecs`, `resident`, `vram_used_mb`, `search_count`, `error_count`, `last_status`를 노출한다.
- `pg_stat_gpu_cache`는 기존 per-GPU counters를 유지하며 shard resident count도 resident_count에 포함한다.

Phase 3F 완료 기준:
- single non-partitioned table + single logical CAGRA index에서 `cuvs.shard_count=2`로 build하면 두 shard가 GPU 0/1에 분산 resident한다.
- parent/partition 없이 일반 table query가 daemon shard fanout을 사용하고, top-k result가 `enable_cuvs=off` CPU exact와 일치한다.
- concurrent benchmark에서 GPU 0/1 shard search counters가 모두 증가한다.
- daemon restart 후 `.shards` manifest와 shard artifacts로 rebuild 없이 reload한다.
- shard artifact 하나가 손상되면 partial result가 아니라 CPU fallback 또는 ERROR로 fail closed한다.
- delta/tombstone이 존재하는 상태에서도 INSERT/UPDATE/DELETE correctness contract를 유지한다.

3F execution plan:
- **3F-0 Design lock**: SW MVP 먼저, hardware acceptance 별도. Sequential fanout 먼저, parallel fanout 후속. Explicit `cuvs.shard_count=N` 먼저, auto shard count 후속. 기본값 0/1은 기존 unsharded 동작을 유지한다.
- **3F-1 Sharded artifact + build MVP**: `cuvs.shard_count` GUC, global `.tids`, `.shards` manifest, shard `.sNNN.cagra` artifacts, build-order contiguous range split, all-or-nothing durable DDL을 구현한다.
- **3F-2 Daemon runtime + reload**: `IndexEntry`를 unsharded/sharded로 구분하고 `ShardEntry[]`를 추가한다. startup reload는 legacy `.cagra`와 sharded `.shards` manifest를 모두 지원한다. `pg_stat_gpu_shards`와 logical-row `shard_count` 관측성을 추가한다.
- **3F-3 Sequential fanout search + merge**: 한 IPC search는 logical index 하나를 요청하고, daemon은 shard들을 순차 검색한 뒤 metric-aware global top-k를 merge한다. 기존 reply format을 유지한다.
- **3F-4 Fail-closed + delta/tombstone correctness**: missing/corrupt/reload-failed shard는 partial result가 아니라 CPU fallback/DDL ERROR로 닫는다. `.delta`와 `.tombstone`은 global `.tids` CRC 기준으로 유지하고 snapshot-aware filtering을 보존한다.
- **3F-5 SW MVP verification**: single non-partitioned table에서 `SET cuvs.shard_count=2`로 shard artifacts, manifest, restart reload, top-k CPU exact match, corrupt shard fail-closed, delta/tombstone correctness를 검증한다.
- **3F-6 Multi-GPU hardware acceptance**: 2x A100 또는 동급 multi-GPU VM에서 shard 0/1이 GPU 0/1에 분산 resident하고, concurrent benchmark에서 shard별 counters가 증가하며, restart/corruption/fail-closed가 유지됨을 검증한다. 검증 후 VM stop/delete와 비용 상태를 보고한다.
- **3F-7 Boundary**: parallel fanout, auto VRAM-based shard count, daemon-side shard-aware GPU delta cache, per-shard adaptive `k + slop`, optional degraded partial recall mode, object snapshot manifest extension for shard artifacts는 3F MVP 범위 밖이다. Vector-clustering shard assignment는 3G로 넘기지 않고 제외한다.

3F-1~3F-5 verified evidence (2026-05-28, single-GPU A100 VM):
- artifact format unit tests green: `cuvs_shards_write/read` round-trip + 8 rejection cases (bad magic/version, shard_count 0/>MAX, body crc, non-contiguous offsets, sum != n_vecs, out-of-order shard_id, truncated body); plus `cuvs_parse_index_filename` rejects `.sNNN.cagra`. `make test-unit`: 150 passed, 0 failed.
- `SET cuvs.shard_count=2` build wrote `<db>_<idx>.tids` (16032 B) + `.shards` manifest (120 B = 40 header + 2×40 records) + `.s000.cagra` + `.s001.cagra`, no legacy `.cagra`. Daemon log: `built sharded index ... (2000 vecs, 2 shards)`, shard 0/1 contiguous ranges `[0,1000)` / `[1000,2000)`.
- `pg_stat_gpu_shards` reported 2 resident shards with per-shard GPU/offset/n_vecs; `pg_stat_gpu_search.shard_count=2` with logical `gpu_device_id` NULL.
- top-k correctness: sharded GPU fanout == `enable_cuvs=off` CPU exact, both as a set and in exact distance order across multiple query vectors (qid=42/100/500/777/1500). Results mixed ids from both shard ranges, proving cross-shard merge + global TID mapping.
- daemon restart reloaded the sharded index from the `.shards` manifest with no rebuild (`loaded sharded index ...`), and post-reload queries stayed correct (`RELOAD_TOPK_MATCH`).
- fail-closed: corrupting one shard `.cagra` was caught on reload (`shard 0 artifact crc mismatch ... skip`); the logical index was not registered (0 `pg_stat_gpu_shards` rows) and the query fell back to CPU with correct results (`FAILCLOSED_CPU_CORRECT`).
- delta/tombstone: INSERT into a sharded table was found via backend CPU delta merge (`DELTA_MATCH`); DELETE excluded the dead TID via snapshot-aware tombstone filtering (same global-TID, backend-side path as unsharded; tombstone result-shortening below k is the pre-existing universal behavior, not sharding-specific).
- no regressions: PG regression suite (smoke/cpu_fallback/edge_cases) 3/3, full fault-injection integration suite incl. new Scenario 19, and e2e durability smoke all green. `shard_count<=1` default path is byte-identical to pre-3F behavior.

3F-6 verified evidence (2026-05-28, 2x A100 `a2-highgpu-2g` VM `pg-cuvs-dev-mgpu`, us-central1-f):
- daemon detected GPU 0 and GPU 1 at startup (both A100-SXM4-40GB, warmed up).
- single logical CAGRA index with `cuvs.shard_count=2` over 4000 vecs placed **shard 0 -> GPU 0, shard 1 -> GPU 1** (`pg_stat_gpu_shards` distinct_gpus=2, contiguous ranges `[0,2000)`/`[2000,4000)`) — true single-index multi-GPU sharding on real hardware.
- top-k correctness on multi-GPU: sharded fanout == `enable_cuvs=off` CPU exact (`MGPU_TOPK_MATCH`); one query incremented `search_count` on both shard 0 (GPU 0) and shard 1 (GPU 1).
- concurrent benchmark (`pgbench -j4 -c8 -T10` against the single sharded table): 925 transactions, 0 failed, ~92 TPS; per-shard `search_count` rose +925 on BOTH shards, proving every query dispatched to both GPUs under 8 concurrent clients (real concurrent multi-GPU dispatch, not sequential smoke).
- multi-GPU restart reload from the `.shards` manifest restored shard 0 -> GPU 0 / shard 1 -> GPU 1 with no rebuild.
- multi-GPU fail-closed: corrupting the GPU-1 shard artifact was caught on reload (`shard 1 artifact crc mismatch ... skip`), the logical index was not registered (0 `pg_stat_gpu_shards` rows), and the query fell back to CPU with correct results (`MGPU_FAILCLOSED_CPU_OK`).
- VM stopped after acceptance (`TERMINATED`); machine type `a2-highgpu-2g` (2x A100 40GB, ~$7.35/hr on-demand), ~30 min runtime ≈ $3-4.

Phase 3F status: **COMPLETE**. 3F-1..3F-5 (SW MVP) and 3F-6 (2x A100 hardware acceptance) passed. A single non-partitioned `CREATE INDEX ... USING cagra` with `cuvs.shard_count>=2` now builds, reloads, and serves a single logical index sharded across multiple physical GPUs with global top-k merge, fail-closed durability, and delta/tombstone correctness. The 3F-7 productization items (parallel fanout, auto shard count, over-fetch, daemon-side shard delta cache, object snapshot extension, etc.) move to Phase 3G/follow-up. Vector-clustering shard assignment is explicitly excluded unless a future incremental segment-rebuild architecture justifies it.

#### Phase 3G — True Sharding Optimization / Productization

목표:
- Phase 3F의 sequential/manual-shard true sharding MVP를 운영 가능한 고성능 기능으로 확장한다.
- 3F가 correctness/durability/fail-closed를 닫는 단계라면, 3G는 latency(parallel fanout), automation(auto shard count), recall 방어(over-fetch)를 닫는 단계다.
- 3G **본범위는 Core productization 3종**으로 고정한다(ADR-022). delta-cache/snapshot/partial-recall은 follow-up으로 분리한다. vector-clustering shard assignment는 제외한다.

3G-0 lock (결정):
- 본범위 = 3G-1 parallel fanout + 3G-2 auto VRAM-based shard count + 3G-3 `cuvs.shard_overfetch`.
- HW gate는 3G 끝(3G-5)에 둔다 — 단일 GPU SW 검증 후 2x A100 한 번의 유료 run으로 닫는다(3F-6 방식).
- `cuvs.shard_count` 의미: `0` = **auto**(데몬이 VRAM로 derive, 한 GPU에 들어가면 1/unsharded로 resolve → 작은 index 동작 불변), `1` = 강제 unsharded, `N>=2` = 강제 N(3F 동작).
- `cuvs.parallel_fanout`(bool, default on): sharded search per-query 토글. A/B + kill switch.
- `cuvs.shard_overfetch`(int, default 0): shard별 `k + overfetch`, global merge top-k 유지. default 0 = 3F byte-identical.
- follow-up(3G 범위 밖): shard-aware GPU delta cache, object-snapshot manifest extension, degraded partial recall.
- excluded: vector-clustering shard assignment. Clustering large enough to need sharding is itself an expensive out-of-core/distributed job, often more expensive than the scans it tries to optimize. It is not justified for all-shard fanout; revisit only if pg_cuvs introduces incremental cluster/segment rebuilds.

3G 핵심 재사용(재작성 금지):
- multi-source merge: `delta_cand_cmp` + `g_merge_metric` + `qsort`(`pg_cuvs_server.c`, 3F sharded merge에서 사용). parallel fanout은 dispatch만 바꾸고 merge는 동일.
- placement helper: `estimate_vram_bytes`, `pick_gpu_for_index`, `total_vram_used`, `n_usable_gpus`/`usable_gpu`, `g_max_vram_per_gpu[]`. auto count는 이 위의 arithmetic.
- non-evictable sharded entry(`gpu_device_id=0xFFFFFFFF`): shard handle + `tids` 가 search 수명 동안 안정적 → lock-free window의 근거.
- 관측: `pg_stat_gpu_search` p50/p95/p99 + `shard_count` 재사용(새 스키마 없음).

3G execution plan:
- **3G-0 Design lock**: scope/HW gate/GUC 의미를 ADR-022와 본 절에 고정.
- **3G-1 Parallel fanout**: (a) within-query thread-per-shard dispatch(mutex 유지, `sum -> max(shard_i)`), (b) **safe-by-construction lock-free window** — mutex 안에서 shard descriptor + `tids` + metric을 스냅샷하고 GPU dispatch+join만 mutex 밖에서 실행한 뒤, 다시 lock을 잡고 oid로 entry를 re-find해 counters/collect/merge/stats를 갱신한다(unlock 이후 `IndexEntry*` 재사용 금지; collect+qsort merge는 `g_merge_metric` 직렬성 유지를 위해 re-lock 구간에서). inflight refcount/deferred-free는 도입하지 않는다 — sharded non-evictable + REINDEX의 PG AccessExclusiveLock 직렬화로 free-during-search race가 없기 때문이며, 이 불변식이 깨지는 future feature가 들어오면 재도입한다(ADR-022). `cuvs.parallel_fanout=off`는 기존 sequential 경로 유지. cuVS resource pool은 per-device mutex라 multi-device 동시 dispatch 안전(`cudaSetDevice`는 thread-local).
- **3G-2 Auto VRAM shard count**: 순수 helper `cuvs_auto_shard_count(n_vecs, dim, per_gpu_budget, n_gpus, max_shards)`를 `cuvs_util`에 추가(unit-test). build dispatch에서 `cmd->shard_count==0`이면 derive(>=2 → `build_sharded`, 1 → unsharded, 0/neg → fail-closed build error). `1`/`N>=2`는 강제.
- **3G-3 `cuvs.shard_overfetch`**: GUC + `CuvsCmdFrame` 필드. fanout에서 shard별 `sk = min(shard.n_vecs, k + overfetch)`, merge는 global top-k.
- **3G-4 SW verification(단일 GPU)**: unit(`cuvs_auto_shard_count`), integration Scenario 20(parallel==sequential==CPU exact L2/cosine/IP, auto count resolve, overfetch correctness, parallel 경로 fail-closed, default 경로 byte-identical), e2e reload.
- **3G-5 Multi-GPU hardware acceptance(완료 게이트)**: 2x A100에서 auto count가 GPU0/1로 `>=2` shard 분산, `pgbench-shard.sql` A/B로 parallel p50 < sequential p50, recall preserved, reload + corrupt-shard fail-closed. VM stop + 비용 보고. 증거는 본 문서에 기록.

Phase 3G 완료 기준:
- parallel fanout이 2x A100에서 sequential fanout 대비 latency를 개선하고(병렬 p50 < 직렬 p50) correctness/recall을 유지한다.
- `cuvs.shard_count=0` auto가 작은 index는 1(unsharded), 단일 GPU budget 초과 index는 `>=2`로 resolve하고, 전부 안 들어가면 fail-closed build error로 처리한다. explicit `1`/`N>=2` override는 유지된다.
- `cuvs.shard_overfetch`가 correctness를 유지하면서 shard별 요청 k를 늘린다(default 0 = 3F byte-identical).
- parallel 경로에서도 corrupt/missing shard는 partial 없이 fail-closed(SELECT CPU fallback / DDL ERROR)한다.
- default 경로(`shard_count` unset/1, overfetch 0, parallel on이지만 unsharded)는 3F와 동작 동일.
- follow-up 3종(shard delta cache, snapshot manifest ext, degraded partial recall)은 명시적으로 3G 범위 밖으로 분리한다.
- vector-clustering shard assignment은 후속 범위가 아니라 제외 항목이다. 비용은 큰데 3G의 all-shard fanout/CPU global merge 목표에는 직접 기여하지 않는다.

3G-1~3G-4 verified evidence (2026-05-28, single-GPU A100 VM):
- unit `make test-unit`: 162 passed, 0 failed (incl. 새 `cuvs_auto_shard_count` 12 케이스: fits→1, needs 2/4, n_gpus/max_shards cap, n_vecs/2 floor, cannot-fit→0).
- PG regression(smoke/cpu_fallback/edge_cases) 3/3, full integration suite(Scenario 1-20, 새 Scenario 20 포함), e2e durability 모두 green.
- Scenario 20(단일 GPU, shards co-resident): parallel==sequential==CPU exact(L2/cosine/IP), `cuvs.shard_overfetch=32` correctness 유지, `cuvs.shard_count=0` auto가 fitting index를 unsharded(1)로 resolve, parallel 경로 corrupt-shard fail-closed→CPU. `shard_count<=1`/overfetch 0 default 경로는 3F와 byte-identical.

3G-5 verified evidence (2026-05-28, 2x A100 `a2-highgpu-2g` VM `pg-cuvs-dev-mgpu`, us-central1-f):
- **auto shard count + GPU spread**: 600K vecs dim 64 (VRAM est 192 MB) under `--max-vram-mb 128`(per-GPU) with `cuvs.shard_count=0` → 데몬 로그 `auto shard count: 600000 vecs dim 64 -> 2 shard(s)`, **shard 0 → GPU 0, shard 1 → GPU 1**(`pg_stat_gpu_shards` distinct_gpus=2, contiguous ranges `[0,300000)`/`[300000,600000)`).
- **correctness/recall**: clustered 데이터(2048 clusters)에서 sharded parallel top-k == `enable_cuvs=off` CPU exact, recall@10 = **30/30 (1.0)** across 3 queries; query self-vector always #1. (uniform-random 데이터는 CAGRA가 unsharded에서도 recall ~0이라 ANN 한계이지 sharding 결함 아님 — unsharded/sharded 동일 동작 확인.)
- **parallel == sequential**: `cuvs.parallel_fanout` on/off 결과 set 완전 일치(10/10) — 병렬 dispatch가 결과를 바꾸지 않음.
- **counters 양쪽 GPU**: 모든 query가 shard 0(GPU 0)·shard 1(GPU 1) `search_count`를 동일하게 증가(예: 800/800).
- **latency parallel < sequential**: q400 query set 1200 searches A/B(restart로 stats reset), 데몬 측정 평균 latency SEQ **1492 µs** vs PAR **1053 µs**(~30% 감소, sum→max). 주의: `pg_stat_gpu_search.p50`은 log2 버킷이라 둘 다 [1024,2048)→2048로 양자화되어 구분 불가 → 정밀한 `avg_latency_us`(=total/count)로 비교함.
- **fail-closed(parallel 경로)**: shard 0 `.s000.cagra` 손상 후 reload → `shard 0 artifact crc mismatch ... skip`, logical index 미등록(`pg_stat_gpu_shards` 0 rows), 쿼리는 partial 없이 fail-closed(“cagra index not loaded ... retry will use CPU” ERROR + REINDEX HINT; breaker/`enable_cuvs=off`/REINDEX로 CPU 복구). NOT_FOUND/breaker는 기존 계약이며 3G fanout 변경과 무관.
- VM stop 후 `TERMINATED` 확인; machine type `a2-highgpu-2g`(2x A100 40GB, ~$7.35/hr on-demand), ~25분 runtime ≈ $3.

Phase 3G status: **COMPLETE**. 3G-1 parallel fanout(safe-by-construction lock-free window), 3G-2 auto VRAM shard count, 3G-3 `cuvs.shard_overfetch`까지 SW 검증(3G-4)과 2x A100 hardware acceptance(3G-5)를 통과했다. sharded `CREATE INDEX`가 GPU별로 동시에 fanout되어 sequential 대비 per-query latency를 낮추고, `cuvs.shard_count=0`이 VRAM 기준으로 shard 수를 자동 결정하며, recall/fail-closed/delta·tombstone 계약을 유지한다. follow-up(shard-aware GPU delta cache, object-snapshot manifest 확장, degraded partial recall)은 별도 phase로 분리한다.

##### Phase 3G.1 — Sharded index DROP cleanup (hotfix)

3G-5에서 드러난 누수 대응: `DROP INDEX`가 데몬에 통지하지 않아 dropped sharded index가 VRAM/disk에 잔존(non-evictable + restart 시 zombie reload). 결정/설계는 ADR-023.
- **DROP-notify only**: backend `object_access_hook`이 cagra index DROP을 수집, `XACT_EVENT_COMMIT`에서 `cuvs_ipc_drop`(`CUVS_OP_DROP_INDEX`) 발사. 데몬 `handle_drop`이 free + registry compact + 모든 sidecar(`.cagra/.tids/.shards/.sNNN.cagra/.delta/.tombstone/.stale/.relfilenode`) unlink. DROP은 데몬-down에도 실패하지 않음(WARNING).
- sharded는 여전히 non-evictable → 3G-1 lock-free invariant 유지(inflight 불필요).
- **3G.1b(분리)**: whole-index eviction(VRAM pressure 회수). sharded를 evictable로 만들면 inflight refcount/deferred-free 재도입 필요(ADR-022/023). policy: logical-index whole-unit eviction, save_index 스킵(durable), dirty 시 fail-closed.
- 검증(단일 GPU): DROP 후 `pg_stat_gpu_shards` 0 rows + VRAM 회수 + restart 후 zombie 없음 + 데몬-down DROP 성공. integration Scenario 21.

##### Phase 3G.2/3G.3/3G.4 — snapshot · delta cache · eviction (ADR-024)

3G follow-up 3종을 닫아 Phase 3G 전체 완료. 설계는 ADR-024.
- **3G.2 sharded snapshot/warmup**: `.tids`+`.shards`+N `.sNNN.cagra`를 한 set으로 GCS snapshot(`CuvsManifest`에 `shard_count`+`.shards` sha; `cuvs_objstore_upload_sharded` + download 분기). warmup은 atomic `load_index_sharded`로 partial-hot 미노출. heap mismatch/corrupt shard fail-closed. `.delta/.tombstone/.stale` 제외. **GCS transfer round-trip은 bucket 부재로 자동 검증 불가**(3C/3D 동일) — compile + no-regression + download-후-load(Scenario 19)로 커버.
- **3G.3 shard-aware delta cache**: 글로벌 `.delta`를 shard 0 GPU(`delta_gpu_of`)의 brute-force cache로 올려 fanout merge에 통합 → `delta_merged=1`(GPU 병합), 실패 시 backend CPU merge fallback. Scenario 22(mode=gpu + CPU exact 일치).
- **3G.4 sharded eviction**: VRAM pressure에서 sharded index를 whole-unit evict(`free_index_shards`, save 스킵), `IndexEntry.inflight` refcount로 lock-free fanout 보호(eviction이 in-flight를 skip). Scenario 23(evict + manifest reload).

Phase 3G status: **COMPLETE (core + follow-up)** — 3G-1..3G-5 core + 3G.1 DROP cleanup + 3G.2/3.3/3.4 follow-up까지 구현/검증(3G.2 GCS transfer만 bucket 의존으로 미자동화). degraded partial recall은 명시적 비채택(fail-closed-only 유지), vector-clustering shard assignment는 제외 항목.

#### Phase 3H — Operational Playbooks / Runbooks

목표:
- Phase 3A-3G의 실제 운영 표면이 고정된 뒤 최종 운영 playbook을 작성한다.

구현 항목:
- replica bootstrap / instance replacement runbook.
- object storage permission failure, corrupt manifest, heap compatibility mismatch 대응.
- async warmup/cache hydration 진단.
- multi-GPU runtime warmup, per-GPU VRAM pressure, placement failure/degraded mode 복구.
- true sharding 운영: `.shards` manifest validation, missing/corrupt shard artifact, shard reload failure, `pg_stat_gpu_shards` 진단, shard-level restart recovery.
- true sharding 성능: sequential/parallel fanout 비교, shard count sizing, per-shard over-fetch tuning.
- capacity planning: VRAM, NVMe, object storage artifact size, delta growth.
- on-call triage: stats view, daemon logs, PostgreSQL warnings/errors, GCS audit/logging 확인 순서.

Phase 3H 완료 기준:
- Phase 3A-3G의 기능/관측성 표면을 기준으로 `docs/playbooks/`에 최종 운영 runbook을 작성한다.
- 각 playbook은 최소 하나의 검증된 GPU VM 또는 replica 시나리오를 근거로 한다.
- Phase 1.5 playbook은 baseline 문서로 유지하고, Phase 3 playbook은 multi-node/multi-GPU 운영 절차를 별도 문서로 둔다.

**3H-light 완료(2026-05-28)**: DiskANN/벤치마크 전에 baseline을 고정. 기존 Phase
1.5/2 playbook 8종에 더해 Phase 3 표면을 커버하는 4종을 `docs/playbooks/`에 추가하고
`docs/playbooks/README.md` 색인을 작성했다(6절 형식, 이번 세션 검증 시나리오 근거):
- `gpu-vm-lifecycle.md` — VM start/stop/reset, ephemeral IP, **stop/start 후 NVIDIA
  driver mismatch → reset**, GCS용 SA 부착, mgpu start/stop+비용.
- `multi-gpu-sharding-ops.md` — shard_count(0/1/N), `pg_stat_gpu_shards` 배치,
  parallel_fanout(+p50 log2-bucket→avg 사용), shard_overfetch, 손상 shard fail-closed→
  REINDEX, whole-unit eviction+reload.
- `gcs-snapshot-ops.md` — SA/IAM/bucket, upload→download round-trip 검증, sharded 복원
  (`.relfilenode` warmup), heap relfilenode 거부. (검증 중 발견·수정한 PUT→POST/JSON
  공백 버그도 명시.)
- `drop-and-write-path-diagnosis.md` — DROP 데몬 정리(+재시작 zombie 검증), delta/
  tombstone/stale, sharded GPU delta cache(delta_search_mode).
- **3H-full(이후)**: replica bootstrap(true multi-node), DiskANN 운영, capacity-planning
  수치(벤치마크 의존), release upgrade runbook, benchmark runbook 갱신은 3B/벤치마크
  이후로 분리.

#### Phase 3I — CAGRA-to-HNSW: GPU Build Accelerator

목표: GPU로 빠르게 CAGRA 그래프를 빌드한 뒤, HNSW 포맷으로 변환해 GPU 없이 서빙할 수 있게 한다.
"GPU를 인덱스 빌드 가속기로만 쓰고, 검색은 CPU로" 하는 운영 모드를 지원한다.

**배경**: cuVS는 `from_cagra()` API로 CAGRA 그래프를 HNSW 포맷으로 변환할 수 있다.
RAPIDS 공식 문서도 이 "GPU build + CPU HNSW search" 패턴을 권장 시나리오로 소개한다.
두 sub-phase로 진행한다.

**운영 포지셔닝**: 온프렘/프라이빗 RAG 시스템은 embedding model serving, reranker,
batch embedding을 위해 이미 GPU 서버를 운영하는 경우가 많다. 이 GPU가 벡터 DB와 같은
리소스 풀 또는 가까운 데이터 경로에 있으면, GPU를 검색 서빙에 상시 점유시키지 않고
인덱스 빌드/재빌드에만 빌려 쓰는 모델이 현실적이다. Phase 3I의 목적은 이 모델을
PostgreSQL/pgvector 운영 표면 안에 넣는 것이다.

```text
embedding / batch GPU pool
        │
        ├─ build window: CAGRA build + HNSW export/import
        │
        └─ serve window: standard pgvector HNSW on CPU
```

따라서 3I는 "GPU 검색 DB로 갈아타라"가 아니라, "기존 pgvector HNSW 검색 경로는 유지하되
가장 느린 build/rebuild 단계만 주변 GPU로 가속하라"는 기능이다. 실패하거나 GPU를 회수해도
검색 경로는 pgvector CPU index로 남는다.

---

#### Phase 3I-1 — cuVS HNSW CPU 서빙 모드

pg_cuvs가 cuVS HNSW artifact를 직접 직렬화/로드해 GPU 없이 CPU HNSW 검색을 제공한다.
pgvector 의존 없이 pg_cuvs 자체 AM으로 처리한다.

구현 항목:
- `CREATE INDEX USING cagra` 빌드 후 `from_cagra()` 호출로 cuVS HNSW graph 생성.
- cuVS HNSW를 `cuvs.index_dir`에 `.hnsw` sidecar로 직렬화 (`cuvs::serialize` API).
- daemon이 GPU 없는 환경 또는 `cuvs.cpu_hnsw_fallback=on` 시 `.hnsw`를 로드해
  cuVS HNSW CPU search API로 쿼리를 처리한다.
- `pg_stat_gpu_search`에 `search_mode` 컬럼 추가: `gpu_cagra | cpu_hnsw | cpu_fallback`.
- GUC: `cuvs.cpu_hnsw_fallback` (bool, default off) — GPU 불가 시 cuVS HNSW로 자동 전환.

완료 기준:
- GPU VM에서 `CREATE INDEX USING cagra` → GPU CAGRA build + cuVS HNSW 직렬화 성공.
- GPU 비활성화 후 동일 인덱스에 SELECT → cuVS CPU HNSW search로 올바른 결과 반환.
- recall vs pgvector HNSW e2e 검증 (동일 데이터, recall ≥ 0.95).
- `pg_stat_gpu_search.search_mode = 'cpu_hnsw'` 확인.

3I-1 완료 현황 (2026-06-02):
- `cuvs.cpu_hnsw_fallback=on` 시 CAGRA build 후 `.hnsw` sidecar 조건부 생성 (ADR-032).
- daemon이 `.hnsw` sidecar를 로드해 cuVS CPU HNSW search 수행.
- `search_mode` 컬럼 구현 완료 (`gpu_cagra | cpu_hnsw | cpu_fallback`, `pg_stat_gpu_search` + daemon `last_search_mode`).

---

#### Phase 3I-2 — cuVS HNSW → pgvector 어댑터

cuVS HNSW graph를 pgvector HNSW 내부 페이지 포맷으로 변환한다.
결과는 표준 `USING hnsw` 인덱스로 등록돼 pgvector가 읽는다.

**제약**: pgvector HNSW는 PostgreSQL 인덱스 페이지에 저장된다. 외부에서 빌드한 그래프를
임포트하려면 pgvector의 내부 페이지 포맷(HnswPage, HnswElementData, 레벨 구조)을
정확히 재현해야 한다. pgvector 버전 종속성이 생긴다.

구현 항목:
- pgvector HNSW 페이지 포맷 역공학 (hnswbuild.c, hnswutils.c 분석).
- cuVS HNSW node-neighbor 배열 + `.tids` → pgvector HnswElement + TID 매핑 변환기.
- `smgr`/`GenericXLogRegister` 경로로 PostgreSQL 인덱스 페이지 직접 기록하는 bulk writer.
- `CREATE INDEX USING cagra WITH (export_hnsw=on)` 또는 별도 함수
  `pg_cuvs_export_hnsw(index_oid)` 인터페이스.
- pgvector AM entry point에 index를 등록해 `USING hnsw`로 쿼리 가능하게 함.
- 레벨 구조 처리: cuVS HNSW M/ef_construction 파라미터를 pgvector 호환 값으로 맞춤.

완료 기준:
- GPU VM에서 `CREATE INDEX USING cagra WITH (export_hnsw=on)` 실행.
- `\d table` 에 `USING hnsw` 인덱스가 나타남.
- `SELECT ... ORDER BY v <-> query LIMIT 10` 이 pgvector HNSW path로 실행됨.
- recall ≥ 0.95 e2e 검증.
- GPU 없는 별도 VM에서 pg_dump/restore 후 pgvector HNSW 쿼리 성공.

**알려진 위험**:
- pgvector HNSW 페이지 포맷이 버전마다 바뀔 수 있어 pgvector 최소 버전을 명시해야 함.
- 레벨 구조가 다르면 recall 저하 가능 — `ef_construction` 파라미터 튜닝 필요.
- 변환 중 fsync/WAL 처리를 올바르게 해야 crash-safe.

3I-2 구현 현황 (2026-06-02, ADR-031b/036/037):
- **ADR-031b (AccessExclusiveLock 수정)**: `pg_cuvs_import_hnsw`가 `ExclusiveLock`으로 target을 열어 truncate+재작성 도중 concurrent SELECT가 반쯤 지워진 페이지를 읽을 수 있던 correctness 버그 수정. `AccessExclusiveLock`으로 변경. (`src/hnsw_export.c`)
- **ADR-036 (pg_cuvs_import_cagra)**: hnswlib `.hnsw` 중간 파일 없이 CAGRA adjacency를 IPC로 직접 pgvector 페이지로 변환. import_hnsw 대비 전체 1.18× 빠름 (119s vs 140.7s, Cohere 1024d 1M). flat NSW 구조(level 0만), recall=0.9963.
- **ADR-037 (pg_cuvs_build_hnsw 통합 API)**: `pg_cuvs_import_cagra`를 `pg_cuvs_build_hnsw(cagra_oid, mode DEFAULT 'nsw') RETURNS regclass`로 통합. `INDEX_CREATE_SKIP_BUILD`로 285s CPU pgvector 빌드 제거. pgvector catalog entry만 생성.
- 4-way 모드: nsw(117s/2.4×, 권장), hnsw(144s), hnswlib(139s/2.0×, 권장), hnswlib_file(151s).
- recall@10 단일 측정: nsw=0.9963, hnswlib=0.9962 (Cohere 1024d, N=1M).
- 테스트 스위트 7/7 PASS (build_hnsw, build_hnsw_edge, metrics 포함).
- **ef-recall pareto 완료 (2026-06-02)**: nsw/hnsw/hnswlib 세 모드 recall curve 동일 확인.
  nsw 권장 default 유지 근거 확보. (bench/results/ef_recall_sweep.csv, ADR-037)

---

Phase 3I 전체 완료 기준:
- 3I-1: GPU 없는 환경에서 cuVS CPU HNSW 서빙 동작, e2e recall 검증.
- 3I-2: pgvector와 호환되는 HNSW 인덱스 export, GPU 없는 VM에서 pg_dump/restore 검증.
- 각 sub-phase는 독립적으로 릴리스 가능하다.

Phase 3I status: **COMPLETE** (2026-06-02).
3I-1: cpu_hnsw_fallback GUC + .hnsw sidecar 조건부 생성 구현. search_mode 컬럼 구현 완료.
3I-2: pg_cuvs_build_hnsw 4-mode 구현, 7/7 tests PASS, ef-recall pareto 검증(nsw 권장 default 확인), GPU-less VM dump/restore PASS (gpu_free_test에 pgvector만 설치 후 HNSW 쿼리 정확).

---

#### Phase 3K — CREATE INDEX ... USING pg_cuvs_hnsw DDL 문법 전환

목표: `pg_cuvs_build_hnsw()` SQL 함수 호출 방식을 표준 `CREATE INDEX` DDL 문법으로 전환해 PostgreSQL 인덱스 관리 표준 경로와 통합한다.

배경:
- 현재 CAGRA→HNSW 변환은 `SELECT pg_cuvs_build_hnsw('my_cagra'::regclass, 'nsw')` 함수 호출이다.
- 이 방식은 `pg_indexes` 카탈로그에 노출되지 않고, `pg_dump`/`pg_restore`에서 자동 지원되지 않으며, `DROP INDEX`/`REINDEX`가 자연스럽지 않다.

결정 (ADR-038):
- `pg_cuvs_hnsw` 커스텀 Access Method를 등록한다.
- `CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops) WITH (source = 'my_cagra', mode = 'nsw')` 형태의 표준 DDL을 사용한다.
- `ambuild()` 안에서 현재 `pg_cuvs_build_hnsw()`의 로직(source CAGRA 찾기 -> `INDEX_CREATE_SKIP_BUILD`로 pgvector HNSW 껍데기 생성 -> CAGRA 그래프를 pgvector 페이지로 변환)을 수행한다.

구현 항목:
- `pg_cuvs_hnsw` AM handler 등록 (`CREATE ACCESS METHOD pg_cuvs_hnsw TYPE INDEX HANDLER pg_cuvs_hnsw_handler`).
- `ambuild()`에서 `WITH` 절 파라미터(`source`, `mode`, `ef_construction` 등)를 파싱하고 기존 빌드 로직을 실행.
- `amvalidate()`에서 source CAGRA 인덱스 존재 여부와 dimension/metric 호환성을 검증.
- pgvector 호환성: 버전 고정(pgvector >= 0.7.0) + CI 호환 매트릭스 테스트.
- 기존 `pg_cuvs_build_hnsw()` SQL 함수를 deprecate 또는 제거.

Phase 3K 완료 기준:
- `CREATE INDEX ... USING pg_cuvs_hnsw` DDL이 CAGRA source에서 pgvector HNSW 인덱스를 생성한다.
- 생성된 인덱스가 `pg_indexes` 카탈로그에 정상 노출된다.
- `DROP INDEX`와 `REINDEX`가 자연스럽게 동작한다.
- `pg_dump`/`pg_restore`로 인덱스 정의가 자동 보존된다.
- recall/성능이 기존 함수 호출 방식과 동등하다.
- CI에서 pgvector 호환 매트릭스가 검증된다.

Phase 3K status: **COMPLETE** (2026-06-04, installcheck 8/8).
- `pg_cuvs_hnsw` AM: pgvector hnsw `IndexAmRoutine`을 복사해 read 경로를 위임하고 `ambuild`/`amoptions`만 override (ADR-038). opclass는 pgvector 0.8.0 hnsw support proc 미러링.
- `source` 선택적 (ADR-041): 생략 시 heap에서 ephemeral CAGRA를 빌드 → 변환 → drop. 한 DDL로 완결되고 REINDEX가 self-contained. source 명시 시 기존 CAGRA를 재사용(GPU 검색용으로 보존).
- `pg_cuvs_build_hnsw()`는 deprecated (런타임 NOTICE). 0.1.0이 미릴리스라 0.2.0 bump 없이 base `sql/pg_cuvs--0.1.0.sql`에 포함.

---

#### Phase 3M — 배치 검색 API

목표: Q개 쿼리를 단일 IPC 요청으로 묶어 데몬에 전송하고, 한 번의 GPU dispatch로 Q×K 결과를 반환하는 `pg_cuvs_batch_search` SQL 함수를 추가한다. (ADR-040)

배경:
- 현재 IPC는 Q=1 단일 쿼리 구조라 벤치마크 GT 생성, RAG 멀티청크 검색 등 배치성 workload에서 IPC 왕복 Q회 + GPU kernel launch Q회가 누적된다.
- cuVS CAGRA/BF search API는 이미 Q×dim 행렬 입력을 지원하므로 데몬 변경이 최소화된다.

구현 항목:
- `CUVS_OP_SEARCH_BATCH` opcode 추가. 기존 `CUVS_OP_SEARCH`(Q=1) 경로는 변경 없이 유지.
- request shm: Q×dim float32 행렬. reply shm: Q×K (tid, distance) 행렬.
- `pg_cuvs_batch_search(rel regclass, queries vector[], k int) RETURNS TABLE(query_idx int, ctid tid, distance float4)` SRF 구현.
- heap recheck / MVCC visibility를 함수 내부에서 처리.
- CAGRA / BF / sharded 경로 모두 지원.

Phase 3M 완료 기준:
- `pg_cuvs_batch_search`로 Q개 쿼리를 단일 IPC 왕복으로 처리한다.
- 각 query_idx의 결과가 단일 쿼리 API와 동일한 top-K를 반환한다.
- Q=1000, dim=1024 기준 단일 쿼리 반복 대비 throughput이 유의미하게 향상된다.
- BF mode(Phase 3L)와 sharded index(Phase 3F/3G)에서도 동작한다.
- 기존 단일 쿼리 경로는 변경 없이 동작한다.

---

#### Phase 3L — GPU Brute Force 검색 모드 사용자 노출

목표: 내부 delta cache 전용으로만 쓰이던 `cuvs_brute_force_search` / `CuvsBfIndex`를 사용자가 직접 사용할 수 있는 GPU exact search 경로로 노출한다. (ADR-039)

배경:
- `USING cagra` ANN 검색은 recall < 1.0일 수 있다. 소규모 데이터 또는 정확도가 중요한 workload에서 exact GPU search가 필요하다.
- 벤치마크 ground truth 생성 시 seqscan 대비 GPU BF가 훨씬 빠르다 (N=1M 기준 seqscan 수십 초 vs GPU BF 수백 ms).
- `cuvs_brute_force_search` 인프라가 이미 구현되어 있어 구현 비용이 낮다.

구현 항목:
- `cuvs.search_mode` GUC 추가 (`'cagra'` default / `'brute_force'`).
- `CREATE INDEX USING cagra` 빌드 시 raw vector matrix를 `.vectors` sidecar로 직렬화 (versioned header, fsync+rename durable).
- daemon `IndexEntry`에 `main_bf_idx` (`CuvsBfIndex`) 추가; startup 시 `.vectors` sidecar 존재하면 로드.
- IPC search request에 `search_mode` 필드 추가; daemon이 분기해 `cuvs_brute_force_search` 또는 기존 CAGRA 경로 호출.
- sharded index: shard별 벡터 범위를 각 shard의 `CuvsBfIndex`에 올려 기존 fanout 경로에서 BF 검색.
- `pg_stat_gpu_cache`에 `bf_vram_bytes` 노출.
- `pg_stat_gpu_search.search_mode`가 `'brute_force'`를 반환.

VRAM 영향:
- 1M×384 float32 기준 약 1.5 GB 추가 (CAGRA 그래프와 별도).
- `.vectors` sidecar가 없으면 BF 검색 요청 시 ERROR.

Phase 3L 완료 기준:
- `SET cuvs.search_mode = 'brute_force'`로 `USING cagra` 인덱스에 GPU exact search가 동작한다.
- recall@10 = 1.0 (pgvector seqscan ground truth와 일치).
- `pg_stat_gpu_search.search_mode = 'brute_force'` 확인.
- `cuvs.search_mode = 'cagra'` default 경로는 기존과 byte-identical.
- sharded index에서도 BF 검색이 정확한 top-k를 반환한다.
- `.vectors` sidecar 없을 때 BF 요청 시 명확한 ERROR.

---

#### Phase 3N — OFFSET-aware K 자동 조정

목표: `SELECT ... ORDER BY embedding <-> query LIMIT K OFFSET N`이 GPU CAGRA/BF/sharded 경로에서 정상 동작하도록, pg_cuvs가 OFFSET을 감지해 내부 K를 `offset + limit`으로 자동 조정한다. ORM(Django, Rails, Spring Data, SQLAlchemy) pagination 호환성을 확보한다. (ADR-042)

배경:
- 현재 pg_cuvs는 LIMIT만 지원하고 OFFSET을 인식하지 못한다. OFFSET을 쓰면 executor가 seqscan fallback하거나 결과가 잘려나온다.
- 벡터 인덱스(CAGRA, HNSW, BF)는 "이전 검색의 N번째부터 이어서 탐색"이 구조적으로 불가능하므로, `offset + limit`개를 한 번에 검색해 앞부분을 drop하는 것이 유일한 현실적 경로다(Qdrant 동일 방식).

구현 항목:
- `cuvsamcostestimate()` 또는 `cuvs_beginscan()`에서 PG Plan 노드의 LIMIT+OFFSET 감지. `IndexScanDesc`에 offset 값을 저장한다.
- IPC search 메시지의 K 값을 `offset + limit`으로 계산해 전달한다(별도 offset 필드 추가 대신 K 자체를 확장하는 방식이 daemon 변경 최소).
- daemon은 K=offset+limit으로 CAGRA/BF 검색을 수행하고 전체 결과를 반환한다(daemon 변경 없음).
- backend `cuvs_gettuple()`에서 앞 offset개를 skip한 뒤 나머지를 반환한다.
- `offset > cuvs.max_offset_warning`(기본 1000) 시 NOTICE를 남겨 keyset pagination 전환을 권고한다.
- regression test: OFFSET 0/10/100에서 결과가 단일 `LIMIT (offset+limit)` 쿼리의 뒤쪽 slice와 일치하는지 검증.

Phase 3N 완료 기준:
- `SELECT ... ORDER BY embedding <-> query LIMIT 10 OFFSET N`이 CAGRA/BF/sharded 경로에서 정상 동작한다.
- OFFSET 0일 때 기존 동작과 byte-identical(regression 없음).
- `pg_stat_gpu_search.requested_k`에 `offset + limit` 값이 반영된다.
- Django QuerySet slicing(`qs[100:110]`), SQLAlchemy `.offset(100).limit(10)` 패턴으로 GPU 인덱스가 사용됨을 `EXPLAIN`으로 확인한다.
- large offset(>1000) 시 NOTICE 경고가 발생한다.

#### fp16 입력 벡터 (트리거: cuVS C API fp16 지원 확인)

`CREATE INDEX ... WITH (precision=fp16)` reloption 추가. float32 대비 VRAM ~50% 절감. 동일 예산에서 인덱스 크기 2× 향상.

전제: `cuvsCagraBuild` dataset 인자에 `CUDA_R_16F` dtype 전달 가능 여부 VM 헤더 검증 필요.

완료 기준:
- cuVS C API fp16 지원 확인 + recall 저하 < 0.5% 실측.
- N=1M dim=1024 기준 VRAM 사용량 float32 대비 ≥45% 절감 확인.
- 기존 test suite PASS.

스펙: ADR-054

---

#### EXPLAIN ANALYZE GPU 타이밍 (트리거: 명시 진단 수요)

PG custom scan node의 `ExplainCustomScan` 콜백에서 GPU kernel time / IPC latency를 `EXPLAIN ANALYZE` output에 주입. daemon 응답 프레임에 `gpu_kernel_us`, `ipc_roundtrip_us` 추가.

완료 기준:
- `EXPLAIN (ANALYZE)` output에 `GPU kernel: Xms, IPC: Yms` 행 표시 확인.
- 기존 test suite PASS.

스펙: ADR-055

---

#### VACUUM 연동 tombstone 정리 (트리거: 3Q 완료)

`ambulkdelete` hook에서 tombstone 비율이 `cuvs.compact_on_delete_ratio`를 초과하면 `CUVS_OP_COMPACT` 호출. PG autovacuum 스케줄을 재활용해 별도 bgworker 불필요.

4C(full REINDEX bgworker)와 동일 `CUVS_OP_COMPACT` 공유 — 병행 가능. `ambulkdelete`가 동기 호출이므로 threshold 설계 또는 비동기 dispatch 검토 필요.

완료 기준: autovacuum 실행 후 tombstone 자동 정리 e2e 확인. 기존 test suite PASS.

스펙: ADR-056

---

#### Subtransaction-aware DROP 수집 (트리거: SAVEPOINT+DROP 실측 문제)

`RegisterSubXactCallback` + `SUBXACT_EVENT_ABORT_SUB`로 롤백된 SAVEPOINT 내 DROP OID를 `cuvs_pending_drops`에서 제거. 각 엔트리에 `SubTransactionId` 태깅 필요.

현재 한계: `SAVEPOINT s; DROP INDEX; ROLLBACK TO s; COMMIT;` 시 살아있는 인덱스의 artifact가 조기 삭제 → stale 처리 → REINDEX 복구. ADR-023 commit-only 단순화의 알려진 한계로 의도적 보류.

완료 기준: SAVEPOINT 롤백 후 인덱스 artifact 보존 확인, 기존 test suite PASS.

스펙: ADR-057

---

Phase 3 전체 완료 기준:
- Phase 3A-3N의 subphase 완료 기준을 모두 만족한다.
- 각 subphase는 독립적으로 중단/릴리스 가능하며, 다음 subphase 미완료가 이전 subphase의 정합성을 깨지 않는다.

---

## Phase 4 — 인제스트 성능 가속

Phase 3J(direct CAGRA→pgvector) 완료 후 측정된 두 가지 병목에 대한 개선 로드맵.
실측 기준: N=1M, dim=1024, Cohere Wikipedia, A100-40GB.

### 4-preflight — 연산 지역성 프로파일링

목표: Phase 4A/4B 최적화 착수 전에 빌드/검색/export 경로의 latency split을 실측해 최적화 우선순위를 정량 근거로 잡는다. 현재 ADR-034(빌드 오버헤드), ADR-035(page write 병목), ADR-043(TOAST 비용)의 근거가 코드 분석 기반 추정이므로 실측으로 검증/보정한다. (ADR-044)

실행 순서: **4-pre-1 → 4-pre-2 → 4-pre-3 → 4-pre-4**.

구현 항목:

**4-pre-1 — nsys daemon 프로파일링** (검색 + 빌드):
- `nsys profile --trace=cuda,nvtx,osrt`로 `pg_cuvs_server` 데몬 프로세스를 프로파일링한다.
- GPU 커널 실행 시간, CUDA memcpy H2D/D2H, 커널 launch overhead, shm mmap 읽기/쓰기, socket send/recv를 측정한다.

**4-pre-2 — perf backend 빌드 프로파일링**:
- `perf record -g -e cache-misses,LLC-load-misses` 로 PG backend(`CREATE INDEX USING cagra`)를 프로파일링한다.
- heap scan → detoast → memcpy → shm write 각 구간의 cache miss 핫스팟을 식별한다.
- TOAST(EXTENDED) vs PLAIN에서 `perf stat` 비교 → ADR-043 실증 검증에 cache miss 수치 추가.

**4-pre-3 — pg_stat_io + perf probe buffer manager 측정**:
- `pg_stat_io`(PG16+)와 `perf probe`로 `write_elem_page` 경로의 buffer pool lookup/lock/WAL 비용을 측정한다.
- ADR-035의 "buffer manager 제약" 거부 근거를 정량화한다.

**4-pre-4 — 결과 문서화 + 우선순위 재검증**:
- `docs/profiling-results.md`에 측정 환경/방법/결과를 아카이브한다.
- ADR-044에 분해 수치를 기록하고, Phase 4A/4B 최적화 우선순위를 재검증한다.

4-preflight 완료 기준:
- 검색 경로: `ipc_us` / `gpu_kernel_us` / `memcpy_us` 분해 수치 기록.
- 빌드 경로: `heap_scan_us` / `detoast_us` / `memcpy_us` / `shm_write_us` / `gpu_build_us` 분해 수치 기록.
- export 경로: `page_write_us` / `buffer_mgr_overhead_us` / `wal_us` 분해 수치 기록.
- TOAST vs PLAIN cache miss 수치 비교가 ADR-043에 반영됨.
- 4A/4B 우선순위가 실측 근거로 확정 또는 재조정됨.

---

### 4A — CAGRA 빌드 PostgreSQL 오버헤드 감소

**상태: 완료 (2026-06-07) — ADR-057 / ADR-058 / ADR-059** (#20 main 머지). 아래 원안(2026-05)은 ADR-044 프로파일링으로 전제가 보정됐고 구현 설계도 분기했다 — 완료 요약을 먼저 두고 원안은 이력으로 보존한다.

**완료 요약 (실측·검증)**:
- **ADR-057 — memfd + SCM_RIGHTS corpus**: 원안의 "shm 직접 할당"은 크래시 시 `/dev/shm` 고아 누수가 남아 사용자가 heap revert → **익명 memfd fd를 데몬에 전달**로 재설계(이름 없어 고아 구조적 불가). copy 오버헤드 ->0(데몬이 corpus 직접 mmap), peak RSS -32%.
- **ADR-058 — parallel maintenance workers**: 참가자별 named-shm partial -> 리더 memfd merge. 분산 스캔.
- **ADR-059 — 데몬 multi-partial direct H2D**: 리더 merge 복사(ADR-058 병목) 제거 — 데몬이 N partial을 device 행렬로 offset별 직접 H2D(`cuvs_cagra_build_multi`).
- **실측 (bench_500000 dim1024, A100/PG16)**: backend 오버헤드(total-GPU floor) **~6.3s(단일) -> ~3.7s(병렬)**. wall-clock은 GPU floor(~33s) 지배라 marginal — 가치는 north-star(raw cuVS 대비 backend 제거율). 정합: self-NN 단일==병렬(multi-partial) 5/5; installcheck 15/15 + iso 2/2 GREEN; `n_partials=0` 단일 경로 byte-identical; /dev/shm 고아 0. 상세: `docs/profiling-results.md` §7/8/9.

> **전제 보정 (ADR-044, 2026-06-05)**: 아래 원안의 "build ~55s / cuVS 직접 ~10s (45s 차이)"는 **틀렸다**. 실측은 GPU CAGRA build가 ~68s(82%)로 지배하고 backend(heap/detoast/memcpy/shm)는 ~15.5s(18%)다. 따라서 원안 완료 기준의 "<=50s / <=30-35s" wall-clock 목표는 **GPU 천장상 도달 불가**이며, 4A의 가치는 wall-clock이 아니라 **제거 가능한 backend 오버헤드(~15.5s) 비율**로 평가한다(north-star). PLAIN(detoast 제거)은 직교 보완 레버이고 권고 문구는 ADR-044 실측 ~8%로 보정됨(`docs/best-practices.md` §1).

--- 이하 원안 (2026-05 — 이력 보존, 상기 완료 요약·ADR-044 보정이 우선) ---

원안 추정(ADR-044가 역전): pg_cuvs CAGRA build ~55s / cuVS lib 직접 ~10s (45s 차이)

```
heap scan → varlena decode → malloc 버퍼 누적 → memcpy → shm → daemon → GPU → CAGRA
```

| 개선 방향 | 절감(원안 추정) | 난이도 | 상태 |
|-----------|------|--------|------|
| double memcpy 제거 | ~2-5s | 낮음 | [완료] ADR-057 (memfd corpus로 대체 구현 — 무복사) |
| parallel maintenance workers | ~10-20s | 중간 | [완료] ADR-058 (+ ADR-059 merge 복사 제거) |
| Streaming/pipeline | ~10-15s | 높음 | cuVS API 미제공, 장기 (3Q에서 일부 — cuvsCagraExtend) |
| heap scan 최적화 | ~15-25s | 높음 | PG internals, 장기 (ADR-034 후속 분석: 실현 불가) |
| 이진 벡터 저장 | ~5-10s | 높음 | 스키마 변경, 장기 (ADR-034 후속 분석: pg_cuvs 단독 불가; PLAIN은 사용자 스키마 레버 ADR-043) |

원안 단기 목표(ADR-044가 도달불가로 보정): double memcpy 제거 -> ~50s, parallel workers -> ~30-35s

실행 순서: **4A-1 → 4A-2**. 4A-1이 shm 직접 할당을 도입하므로, 4A-2의 worker별 buffer가 shm 위에 올라갈 수 있어 이중 복사 경로가 완전히 제거된다. 4A-2를 먼저 하면 worker별 heap buffer → shm memcpy가 여전히 남는다. *(실제로는 ADR-057이 simple shm을 memfd로 대체했고, 4A-2의 worker partial은 PG parallel worker가 leader fd를 못 받아 named-shm 강제 — ADR-058/059 참조.)*

구현 항목:

**4A-1 — double memcpy 제거** (ADR-034 §4A-1):
- `cuvs_ambuild()`에서 scan 시작 전 `shm_open` + 초기 `ftruncate`(preflight 추정치 기반) + `mmap` 수행.
- `grow_build_buffers()`의 `realloc`을 `ftruncate` + `mremap`(또는 `munmap` + 재`mmap`)으로 교체.
- `shm_write_build_payload()`의 `shm_open` + `mmap` + `memcpy` x 2를 제거. IPC frame에 shm 이름만 전달.
- `shm_open` 실패 시 기존 heap 경로로 degraded fallback + WARNING.

**4A-2 — parallel maintenance workers** (ADR-034 §4A-2):
- `table_index_build_scan()`에 `ParallelTableScanDesc`를 전달하거나 `table_parallel_index_build_scan()`을 사용(구현 시 PG API 조사 후 결정).
- worker별 독립 `CuvsBuildState`로 partial buffer 누적. scan 완료 후 leader가 memcpy 연접으로 merge.
- `max_parallel_maintenance_workers` GUC를 읽어 worker 수 결정. 0이면 기존 단일 프로세스 경로.
- `amcanparallel` 등 scan 병렬화 콜백은 등록하지 않음(build 병렬화는 `ambuild()` 내부에서 처리).

4A-1 완료 기준:
- 데이터셋: N=1M, dim=1024, Cohere Wikipedia. 하드웨어: A100-40GB.
- `CREATE INDEX USING cagra` build time이 기존 ~55s에서 ~50s 이하로 감소.
- `shm_open` 실패 시 heap 경로 fallback이 동작하고, 결과가 기존과 동일(recall, 정합성 차이 없음).
- 기존 test suite(`make gpu-test-all`) 전수 PASS. fallback 경로용 integration scenario 1건 추가.

4A-2 완료 기준:
- 동일 데이터셋/하드웨어.
- `max_parallel_maintenance_workers = 4` 설정 시 build time이 ~50s에서 ~30-35s로 감소.
- `max_parallel_maintenance_workers = 0`이면 기존 단일 프로세스 경로와 동작 동일(byte-identical artifact).
- worker별 merge 후 CAGRA build 결과(recall, TID mapping)가 단일 프로세스 build와 동일.
- 기존 test suite 전수 PASS. parallel build용 integration scenario 1건 추가(worker 수 >= 2에서 build + search 정합 확인).

> **완료 기준 정정 (실제 충족 방식)**: 위 wall-clock 목표(build "<=50s", "<=30-35s")는 ADR-044 실측(GPU floor ~33-68s 지배)으로 **도달 불가가 입증돼 폐기**됐다. 실제 certify는 (a) **정합** — self-NN 단일==병렬(multi-partial) 5/5, `max_parallel_maintenance_workers=0` 단일 경로 byte-identical, recall/TID 동일; (b) **무회귀** — installcheck 15/15 + isolation 2/2 GREEN; (c) **backend 오버헤드 실측 감소** — bench_500000 dim1024 ~6.3s(단일)->~3.7s(병렬, ADR-059 merge 제거); (d) **누수 안전** — /dev/shm 고아 0, 크래시 매트릭스(ADR-057)로 수행했다. 원안의 "shm_open 실패 -> heap fallback"은 3-tier corpus(memfd->named-shm->heap, ADR-057)로 일반화됐다.

장기 항목 분석 결과 (ADR-034 §heap scan 후속 분석, 2026-06-03):

heap scan + varlena decode ~15-20s는 PG 오버헤드 45s 중 가장 큰 단일 구간이다. 주 원인은 pgvector `vector` 타입이 varlena여서 dim >= 512인 벡터가 거의 모두 TOAST되고, `cuvs_build_callback()`이 매 tuple마다 `PG_DETOAST_DATUM`(palloc + decompress)을 호출하는 것이다. 검토한 5가지 가속 방안(PLAIN storage 강제, 별도 binary column, pgvector fixed-length 기여, raw page scan 커스터마이징, prefetch/streaming detoast) 중 pg_cuvs 단독으로 단기에 실현 가능한 것은 없다.

현실적 대응:
- parallel workers(4A-2)가 wall-clock 분산으로 유일한 단기 가속 수단이다.
- PLAIN storage(`ALTER TABLE ... SET STORAGE PLAIN`)는 사용자 선택이며, 빌드 성능 개선이 가능하나 일반 쿼리 성능 저하 트레이드오프가 있다. OPS_GPU_PLAYBOOK에 "빌드 성능 최적화 팁"으로 안내한다. *(이 단락의 "~45s PG 오버헤드"·"~25-35% 개선"은 ADR-044 프로파일링 이전 추정 — 실측은 backend ~15.5s, PLAIN 빌드 절감 ~8%. `docs/best-practices.md` §1 참조.)*
- 장기 모니터링 대상: pgvector fixed-length storage 지원 여부, PG 코어 TOAST prefetch/streaming API.

### 4B — import_hnsw 페이지 write 병목 감소

현재: ~57s (LOGGED) / ~28s (UNLOGGED) — ReadBuffer+PageInit+PageAddItem×2+WAL 순차 1M회

| 개선 방향 | 절감 | 난이도 | 상태 |
|-----------|------|--------|------|
| UNLOGGED 타겟 | ~28s | 낮음 | **완료** (ADR-033) |
| UNLOGGED + REINDEX 패턴 | 운영 패턴 | 낮음 | **완료** (ADR-035 문서화) |
| 병렬 페이지 write | ~15-25s | 높음 | **단기 제외** (ADR-035: PG buffer manager concurrent extension 제약) |
| Bulk WAL | ~10-15s | 높음 | **단기 제외** (ADR-035: log_newpage_range는 buffer-manager-through 경로와 비호환) |

**UNLOGGED + REINDEX 권장 패턴** (현재 가능):
```sql
-- 1. 빠른 import (WAL 없음, crash unsafe)
CREATE UNLOGGED INDEX t_hnsw ON t USING hnsw (...);
SELECT pg_cuvs_import_cagra('t_cagra'::regclass, 't_hnsw'::regclass);

-- 2. maintenance window 후 WAL-safe 전환
REINDEX INDEX t_hnsw;  -- pgvector 재빌드, LOGGED
```

4B 현황: 단기 개선 가능 항목(UNLOGGED, Phase 3J direct path)은 모두 적용 완료. 잔여 개선(병렬 page write, Bulk WAL)은 ADR-035에서 PG internals 의존도를 이유로 단기 제외로 결정했다. 따라서 4B의 현실적 목표는 현상 유지(UNLOGGED ~28s, LOGGED ~57s)이며, PG 코어에 인덱스 build용 bulk page write API가 추가되거나 `ReadBuffer` concurrent extension 제약이 완화되는 시점에 재검토한다.

재검토 조건:
- PG 코어에 `ReadBufferExtended`의 multi-block allocation 변형이 추가되는 경우.
- PG 코어에 buffer manager를 통한 `log_newpage_range` 연동 패턴이 정립되는 경우.
- `wal_compression`의 float 벡터 데이터 압축 효과가 실측으로 유의미하다고 확인되는 경우(별도 실측 필요).

### 4 — 완료 기준

검증 환경: N=1M, dim=1024, Cohere Wikipedia, A100-40GB. `bench/run_cohere.sh` 또는 동등한 harness로 실행하고 `bench/results/`에 결과를 기록한다.

| 목표 | 수치 | 검증 방법 |
|------|------|-----------|
| 4A-1 완료 | CAGRA build ≤ 50s (현재 55s) | `EXPLAIN (ANALYZE)` build time + shm fallback integration test |
| 4A-2 완료 | CAGRA build ≤ 35s (현재 55s, workers=4 기준) | 동일 + workers=0 동작 동일 확인 + parallel build integration test |
| 4B 현상 유지 | UNLOGGED import ~28s (변동 없음) | `EXPLAIN (ANALYZE)` import time, 기존 test suite regression 없음 |
| 종합 (4A-2 + 4B, UNLOGGED) | 전체 ≤ 65s (현재 96s, native 285s 대비 4.4×) | end-to-end: CAGRA build + pg_cuvs_build_hnsw(nsw) + UNLOGGED import |

---

## Phase 3R — CAGRA 빌드 파라미터 reloption

목표: `graph_degree`, `intermediate_graph_degree`, `build_algo`를 `CREATE INDEX ... WITH (...)` reloption으로 노출해 사용자가 recall↔build-time·VRAM 트레이드오프를 직접 제어할 수 있도록 한다. (ADR-052)

### 구현 항목

#### 1. reloption 추가

| reloption | 타입 | cuVS 기본값 | 범위 |
|-----------|------|-------------|------|
| `graph_degree` | int | 64 | 8–512 |
| `intermediate_graph_degree` | int | 128 | 8–1024 |
| `build_algo` | enum str | `IVF_PQ` | `IVF_PQ`, `NN_DESCENT` |

`relopt_kind` 등록 + `cuvs_relopts` 파싱에 추가.

#### 2. IPC 전달

`CuvsCmdFrame` 또는 별도 `CuvsIndexParams` struct에 3개 필드 추가. daemon의 `cuvsCagraIndexParams` 직접 설정 후 `cuvsCagraBuild` 호출.

#### 3. 기본값 처리

reloption 미지정(0/NULL) → cuVS 기본값 통과. 값 지정 시만 override.

### 완료 기준

- `WITH (graph_degree=32)` 빌드 시 cuVS params 반영 확인 (daemon 로그 또는 인덱스 메타 검증)
- `WITH (build_algo='NN_DESCENT')` 빌드 성공
- 기존 test suite(`make gpu-test-all`) 전수 PASS

스펙: ADR-052

---

## Phase 3O — Pre-filter ANN (필터 검색)

목표: WHERE 조건을 cuVS bitvector mask로 daemon에 전달해, GPU가 조건을 만족하는 벡터만 탐색한다. 고선택성 필터에서 GPU 후보 품질을 높이고 IPC·recheck 낭비를 줄인다. (ADR-048)

### 구현 항목

**3O-1 — IPC 프레임 확장**:
- `CuvsCmdFrame`에 `filter_shm_key[64]` 추가 (비트맵 shm 이름; 빈 문자열 = unfiltered).
- `CuvsBuildShm` 패턴과 동일하게 backend가 비트맵 shm 세그먼트를 생성·전달·소멸.

**3O-2 — backend 비트맵 생성**:
- `cuvs_beginscan` / `cuvs_rescan`에서 `scan->xs_recheck_itup` filter 조건을 heap TID 비트맵으로 평가.
- 비트맵 밀도가 `cuvs.prefilter_threshold` 이하이면 pre-filter 경로, 초과이면 기존 post-filter 경로.

**3O-3 — daemon filtered search**:
- `CUVS_OP_SEARCH`에서 `filter_shm_key`가 있으면 cuVS CAGRA filtered_search API 호출.
- cuVS bitvector mask 포맷으로 변환 후 `SearchParams::sample_filter` 전달.

**3O-4 — fallback 및 테스트**:
- 비트맵 shm 생성 실패 시 기존 unfiltered + post-filter 경로로 graceful degradation + WARNING.
- isolation test: concurrent filter + ANN 정합 확인. recall 비교(pre-filter vs post-filter, 동일 결과 보장).

### 완료 기준

- `SELECT ... WHERE category = $1 ORDER BY embedding <-> $2 LIMIT 10` 쿼리에서 pre-filter 경로 동작 확인 (`EXPLAIN` + `pg_stat_gpu_search.search_mode` 확인).
- recall@10 동일성: pre-filter와 post-filter가 동일한 top-k 반환 (deterministic 데이터셋 기준).
- 기존 test suite(`make gpu-test-all`) 전수 PASS.
- fallback 경로 integration 시나리오 1건 추가.

스펙: ADR-048

---

## Phase 3S — statement_timeout / 취소 전파

목표: PG의 `statement_timeout` 및 `pg_cancel_backend()`가 daemon IPC로 전파되도록 한다. GPU 검색이 걸려도 backend가 timeout 이후 무기한 대기하지 않아 연결 고갈을 방지한다. (ADR-053)

### 구현 항목

#### 1. backend 측

- `cuvs_ipc_search()` 내부 recv를 `poll(fd, cuvs.ipc_timeout_ms)` 루프로 교체.
- 루프 내 `CHECK_FOR_INTERRUPTS()` 호출 — PG cancel signal 즉시 감지.
- 취소 감지 또는 poll timeout 시:
  1. daemon에 `CUVS_OP_CANCEL` 전송 (best-effort, 실패 무시).
  2. `ereport(ERROR, ...)` 반환 — PG 트랜잭션 rollback.

#### 2. daemon 측

- `CUVS_OP_CANCEL` 수신 시 해당 연결의 진행 중 검색 중단.
- cuVS 검색 자체가 non-cancellable이면: worker thread join + 짧은 timeout 후 결과 폐기, 연결 상태 초기화.
- cancel 처리 후 `CUVS_STATUS_CANCELLED` 응답.

#### 3. GUC

| GUC | 기본값 | 설명 |
|-----|--------|------|
| `cuvs.ipc_timeout_ms` | 5000 | poll 상한 (ms). 0=무기한 대기(이전 동작). |

### 완료 기준

- `SET statement_timeout = '1s'` 후 인위적으로 느린 GPU 검색에서 1초 이내 오류 반환 확인
- 취소 후 연결 정상 재사용 확인 (연결 고갈 없음)
- `cuvs.ipc_timeout_ms=0` 시 이전 동작 유지 (regression 없음)
- 기존 test suite(`make gpu-test-all`) 전수 PASS

스펙: ADR-053

---

## Phase 3P — IVF-PQ 및 추가 cuVS 알고리즘

목표: `CREATE INDEX USING ivfpq`로 product quantization 기반 GPU 인덱스를 지원한다. CAGRA 대비 VRAM 10–100× 절감, 대용량(100M+) 데이터셋 대응. (ADR-049)

### 구현 항목

**3P-1 — 새 AM handler 등록**:
- `pg_cuvs_ivfpq_handler` 등록 (`CREATE ACCESS METHOD ivfpq TYPE INDEX HANDLER ...`).
- reloption: `n_lists`(IVF 클러스터 수, 기본 1024), `pq_bits`(코드워드 비트, 기본 8), `pq_dim`(서브공간 수, 기본 dim/2).
- GUC: `cuvs.ivfpq_n_probes`(탐색 클러스터 수, 기본 64 — recall/speed 트레이드오프).

**3P-2 — IPC op 추가**:
- `CUVS_OP_BUILD_IVFPQ`, `CUVS_OP_SEARCH_IVFPQ` 추가.
- build 경로: 동일 shm 코퍼스 전달 → daemon이 cuVS `IvfPq::build()` → serialize.
- search 경로: 동일 shm query 전달 → daemon이 cuVS `IvfPq::search()`.

**3P-3 — daemon IVF-PQ 경로**:
- `IndexEntry`에 `ivfpq` 타입 추가 (CAGRA handle과 별개).
- persist/load: cuVS IVF-PQ serialize/deserialize.
- VRAM budget 계산: PQ 압축 코드 크기 기준 (float32의 `pq_bits/32` 비율).

**3P-4 — 테스트 및 문서**:
- smoke: `CREATE INDEX USING ivfpq` + search recall@10 ≥ 0.90 (n_probes=64 기준).
- 기존 CAGRA 경로 회귀 없음.
- `docs/algorithm-selection-guide.md`: CAGRA vs IVF-PQ 선택 기준 문서화.

### 완료 기준

- N=1M, dim=1024 데이터셋에서 `CREATE INDEX USING ivfpq` build 성공.
- recall@10 ≥ 0.90 (CAGRA 대비 트레이드오프 허용).
- VRAM 사용량이 동일 데이터셋 CAGRA 대비 10× 이상 절감 실측.
- 기존 test suite(`make gpu-test-all`) 전수 PASS.

스펙: ADR-049

---

## Phase 3Q — CAGRA Streaming Updates

목표: cuVS 26.04 C API(`cuvsCagraExtend`, `cuvsCagraMerge`, `cuvsFilter`)를 활용해 INSERT/DELETE/UPDATE를 .delta 파일 없이 VRAM 내에서 직접 처리한다. delta 누적에 따른 search-time CPU/GPU 병합 비용을 제거하고, recall을 유지한 채 실시간 인덱스 업데이트를 지원한다. (ADR-051)

**전제 조건**: cuVS 26.04 이상 (VM 헤더 검증 완료, 2026-06-06).

### 구현 항목

#### 1. IPC 확장

| Op | 방향 | 설명 |
|----|------|------|
| `CUVS_OP_EXTEND` | backend → daemon | shm에 새 벡터+TID 기록, daemon이 `cuvsCagraExtend` 후 disk serialize |
| `CUVS_OP_COMPACT` | backend → daemon | `cuvsCagraMerge(filter=tombstone bitvector)` → 새 인덱스 atomic swap |

`CuvsCmdFrame`에 `extend_shm_key[64]`(EXTEND용) 추가.

#### 2. EXTEND 경로 (INSERT/UPDATE)

- `aminsert`: shm에 새 벡터 + TID 기록 → `CUVS_OP_EXTEND` IPC
- daemon: `cuvsCagraExtend(res, &params, new_dataset, index)` 호출
  - `params.max_chunk_size = cuvs.extend_chunk_size` (0=auto)
- daemon: 성공 후 disk serialize (`.cagra` + `.tids` 갱신) — 내구성 보장
- VRAM 예산 카운터 갱신 (extend로 인덱스 grow 반영)
- EXTEND 실패 시: 3A tombstone + delta append fallback + WARNING (graceful degradation)

#### 3. COMPACT 경로 (DELETE/tombstone 제거)

- `CUVS_OP_COMPACT` IPC: backend가 tombstone bitvector를 shm에 기록
- daemon: `cuvsCagraMerge(res, params, [index], 1, filter, output_index)` 호출
  - `filter` = tombstone bitvector (`cuvsFilter` wrapping)
- 새 인덱스 atomic swap → old VRAM 해제
- disk serialize (갱신된 `.cagra` + `.tids`)

#### 4. GUC

| GUC | 기본값 | 설명 |
|-----|--------|------|
| `cuvs.extend_chunk_size` | `0` (auto) | `cuvsCagraExtendParams.max_chunk_size` 직접 매핑 |
| `cuvs.compact_on_delete_ratio` | `0.1` | tombstone 비율이 이 값 초과 시 COMPACT 자동 트리거 권장 |

#### 5. 3A .delta 경로 deprecated

- 3Q 완료 후 `aminsert`의 delta file append 경로 제거
- tombstone 메커니즘은 유지 (COMPACT op의 filter 소스로 재활용)
- 3A `cuvs.delta_search` GUC: deprecated 표기 (EXTEND 경로에서 불필요)

### 완료 기준

- INSERT/DELETE/UPDATE e2e 검증: recall@10 동일성, .delta 파일 미생성 확인
- VRAM 예산 갱신 정확성: extend 후 `pg_stat_gpu_cache.vram_bytes` 반영 확인
- COMPACT 후 tombstone 비율 0 확인
- 기존 test suite(`make gpu-test-all`) 전수 PASS
- `cuvs.extend_chunk_size=0`(auto) 기준 N=100K INSERT p99 레이턴시 측정

스펙: ADR-051

---

## Phase 4C — Background Compaction + CREATE INDEX CONCURRENTLY 정합성

목표: delta 수동 REINDEX 운용 부담을 제거하고, CREATE INDEX CONCURRENTLY의 DELETE 정합성을 검증·보장한다. (ADR-050)

### 구현 항목

**4C-0 — REINDEX CONCURRENTLY 선행 검증** (착수 전 필수):
- `REINDEX INDEX CONCURRENTLY`가 pg_cuvs AM에서 올바르게 동작하는지 검증.
- DELETE가 섞인 concurrent build isolation 시나리오 추가 (`pg_isolation_regress`).
- 필요 시 `cuvs_ambuild` 시작 시점에 기존 delta/tombstone 명시적 무효화 경로 추가.

**4C-1 — background worker 등록**:
- `_PG_init`에서 `cuvs_compaction_worker` bgworker 등록.
- `cuvs.auto_compact = on|off` GUC (기본 off).
- `cuvs.auto_compact_check_interval` GUC (기본 60s, 폴링 주기).
- `cuvs.auto_compact_threshold` GUC (delta_rows 절대값 또는 base 대비 %, 기본 10%).

**4C-2 — compaction 실행 로직**:
- `pg_stat_gpu_search`에서 `delta_rows > threshold` 인덱스 식별.
- SPI 또는 별도 libpq connection으로 `REINDEX INDEX CONCURRENTLY <oid>` 실행.
- 실행 중 daemon restart / 인덱스 DROP 등 경쟁 조건 안전 처리.

**4C-3 — 관측성**:
- `pg_stat_gpu_search`에 `last_compact_at`(마지막 compaction epoch), `compact_count`(총 횟수) 컬럼 추가.
- compaction 실행·완료·실패를 PG log에 기록.

### 완료 기준

- `cuvs.auto_compact = on`에서 delta_rows > threshold인 인덱스가 자동 REINDEX됨 (e2e 검증).
- REINDEX 후 delta_rows = 0, search recall 유지.
- CREATE INDEX CONCURRENTLY + concurrent DELETE isolation 시나리오 GREEN.
- 기존 test suite(`make gpu-test-all`) 전수 PASS.

스펙: ADR-050

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
