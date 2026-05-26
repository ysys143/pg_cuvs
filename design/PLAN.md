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

2026-05-26 기준 코드베이스는 Phase 1 proof-of-mechanism, Phase 1.5 hardening, 그리고 **제품 로드맵 Phase 2 — Production Ready Single-Node의 코어**를 통과한 상태다. sidecar, CAGRA build/search, persistence, durable DDL, restart reload, failure injection, large benchmark, crash guard regression에 더해 Phase 2 코어가 들어갔다: `pg_stat_gpu_search`/`pg_stat_gpu_cache` 관측성, `cuvs.k` LIMIT-k, opclass 기반 L2/Cosine/IP metric, k 기반 cost model, write→stale CPU fallback(+`.stale` 영속, REINDEX 해소), 빌드 메모리 가드(`cuvs.max_build_mem_mb` 0=auto), VRAM tiered cache(evict-to-fit + reload). exit 기준 감사는 `docs/phase2-exit-criteria.md`. 잔여(다음 단계): build streaming/mmap, delta correction(Phase 3), 자동 backend RSS 기록.

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
- write/delete staleness contract: `aminsert`/`ambulkdelete` mark stale, `.stale` persists it, CPU fallback protects correctness, `REINDEX` clears it.
- CAGRA build memory guard: `cuvs.max_build_mem_mb` and `cuvs.build_mem_safety_ratio`.

현재 남은 핵심 debt:
- `cuvs_ambuild()`는 여전히 전체 corpus를 backend memory에 모은 뒤 daemon으로 넘긴다. Phase 2는 `cuvs.max_build_mem_mb` fail-fast guard까지 닫았고, streaming/mmap handoff는 후속 작업이다.
- SQL `LIMIT`을 index AM에서 직접 읽지는 못한다. Phase 2는 pgvector `hnsw.ef_search`와 같은 모델로 `cuvs.k` GUC를 도입했고, `pg_stat_gpu_search.requested_k/returned_k`로 검증한다.
- write 후 CAGRA는 `.stale` sidecar로 stale 상태를 보존하고 CPU fallback한다. stale 상태에서도 GPU path를 유지하는 pending-delta/delta exact search는 Phase 3 필수 기능이다.
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

이 절의 Phase 2는 제품 로드맵 단계다. 목표는 단일 PostgreSQL instance + GPU sidecar 조합을 "데모"가 아니라 운영 가능한 단일 노드 엔진으로 만드는 것이었다. 2026-05-26 기준 **single-node core는 완료**했다. 완료 판정과 증거는 `docs/phase2-exit-criteria.md`, feature별 테스트 매핑은 `docs/phase2-test-matrix.md`를 따른다.

#### 완료된 workstream

- **Step 0 — Entry gate**: Phase 1.5 baseline(`make test-unit`, GPU regression/e2e/daemon tests, 1M×1536 benchmark, edge cases)을 기준선으로 고정.
- **Step 1 — Observability**: daemon-backed `pg_stat_gpu_search` 구현. search/error counts, last status/error, p50/p95/p99 wall-clock latency, `requested_k`/`returned_k`, stale/stale_since를 SQL에서 확인한다. daemon down은 empty result set으로 처리한다.
- **Step 2 — Executor correctness**: fixed `k=100` 제거. PostgreSQL index AM이 SQL `LIMIT`을 직접 읽을 수 없으므로 pgvector `hnsw.ef_search`와 같은 모델의 `cuvs.k` GUC를 사용한다. opclass/opfamily identity 기반으로 L2/Cosine/IP metric을 build/search에 전달한다.
- **Step 3 — Planner/cost model**: per-tuple 중심 cost를 k-dominant model로 교체하고, plan-shape regression으로 GPU path 선택과 `enable_cuvs=off` 회피를 잠갔다. JIT 설정은 변경하지 않았다.
- **Step 4 — Write/delete/staleness**: `aminsert`와 `ambulkdelete`가 daemon에 stale mark를 보낸다. `.stale` sidecar로 daemon restart 후에도 stale 상태를 보존한다. stale CAGRA는 GPU search를 하지 않고 CPU fallback하며, `REINDEX` 성공 시 fresh로 복구한다.
- **Step 5 — Large build memory guard**: `cuvs.max_build_mem_mb`를 추가했다. 기본 `0`은 auto mode(`MemAvailable * cuvs.build_mem_safety_ratio`), 양수는 operator hard cap이다. preflight/runtime guard가 oversized build를 clear ERROR로 fail-fast 시킨다.
- **Step 6 — VRAM/NVMe tiered cache**: load/reload 경로가 evict-to-fit을 사용하고, `ensure_vram` progress guard로 무한 루프를 피한다. daemon-global `pg_stat_gpu_cache`가 hits/misses/evictions/reloads/persist_failures/resident_count/VRAM budget을 노출한다.
- **Step 7 — Test matrix**: unit/regress/integration/e2e coverage를 `docs/phase2-test-matrix.md`에 정리했다. Integration scenarios 8-12가 observability, metric/k, staleness, build memory cap, cache eviction/reload를 검증한다.
- **Step 8 — Exit criteria audit**: `docs/phase2-exit-criteria.md`의 10개 기준은 모두 `MET` 또는 `MET (deviation)`이다.

#### 완료 기준과 deviation

- `pg_stat_gpu_search`로 per-index search/error/stale/wall-clock latency를, `pg_stat_gpu_cache`로 daemon-global cache counters를 SQL에서 관측한다.
- fixed `k=100`은 제거됐다. 단, SQL `LIMIT` 직접 추출이 아니라 `cuvs.k` GUC로 top-k를 제어한다.
- L2/Cosine/IP metric은 strategy-number heuristic이 아니라 opclass/opfamily identity에서 결정한다.
- repeated scan, parameterized query, transaction block, cursor, LATERAL rescan은 regression에 잠겼다.
- write/delete 후 stale index는 조용히 사용되지 않는다. stale이면 CPU fallback하고 `REINDEX`가 stale을 해소한다.
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

### Phase 3 — Scale Out / Large Index Storage

목표: Phase 2의 single-node CAGRA core를 운영 workload와 대규모 저장소로 확장한다. Phase 3는 하나의 거대한 milestone이 아니라, 각각 독립적으로 검증 가능한 5개 subphase로 진행한다.

#### Phase 3A — Pending Delta / Delta Exact Search

Phase 2의 stale fallback은 정합성을 지키지만, 쓰기가 발생한 순간 GPU path를 포기한다. 운영 workload에 INSERT/UPDATE가 조금이라도 섞이면 너무 보수적이므로 Phase 3에서는 pending-delta를 필수 기능으로 구현한다.

목표:
- 마지막 successful CAGRA build/REINDEX 이후의 INSERT/UPDATE를 pending-delta store에 보관한다.
- DELETE/UPDATE old version은 snapshot-aware tombstone metadata에 기록한다.
- query 시 base CAGRA search와 CPU-side pending-delta exact search를 함께 수행한다.
- base candidates, delta candidates, snapshot-aware tombstone filtering 결과를 over-fetch 기반으로 top-k merge한다.
- PostgreSQL heap recheck/MVCC는 마지막 방어선으로 유지한다.

검색 흐름:
```
query vector
  -> base CAGRA search over immutable graph with k + slop
  -> CPU exact search over pending-delta rows visible to the current snapshot
  -> apply snapshot-aware tombstone filtering
  -> metric-compatible merge/re-rank to k + slop candidates
  -> PostgreSQL heap recheck / MVCC visibility
  -> if visible rows < k and more candidates may exist, retry with larger slop or CPU fallback
```

MVP scope decision:
- Phase 3A MVP의 delta exact search는 **backend CPU-side**로 수행한다.
- 이유: daemon GPU-side delta는 per-insert IPC, daemon-owned vector memory, restart durability, VRAM pressure를 동시에 키운다. Phase 3A의 목적은 write-after-build 정합성 회복이며, delta 크기가 작을 때 CPU exact가 가장 좁은 구현이다.
- GPU-side/batched daemon delta는 Phase 3A MVP가 아니라 후속 최적화다.
- delta가 비용 한계 또는 정합성 한계를 넘으면 stale GPU를 억지로 쓰지 않고 CPU fallback한다.

구현 항목:
- pending-delta 저장소: backend-visible local/sidecar metadata MVP. daemon-owned GPU delta는 제외한다.
- delta entry schema: TID, vector, op type, base generation, write generation, xmin/xmax 또는 snapshot 비교에 필요한 metadata.
- tombstone metadata: base CAGRA에 남아 있을 수 있는 deleted/updated-old TID를 전역 삭제하지 않는다. 현재 query snapshot에서 invisible한 경우에만 제거한다.
- persistent stale/generation marker: `.stale` 또는 generation sidecar는 delta artifact와 독립적으로 유지한다. delta가 base stale을 보정할 수 있는 상태일 때도 daemon restart 후 fail-open하지 않아야 한다.
- merge algorithm: base result와 delta exact result의 metric-compatible distance ordering을 보장한다. L2/cosine/IP 각각의 정렬 방향, normalization, tie-break를 명시하고 pgvector ground truth로 검증한다.
- over-fetch policy: base CAGRA와 delta exact 모두 `k + slop`을 대상으로 merge한다. heap recheck 후 visible rows가 k 미만이면 slop 확대 또는 CPU fallback한다.
- write capture batching: `COPY`/bulk INSERT가 per-row IPC storm을 만들지 않도록 batch capture 또는 threshold-then-CPU-fallback 정책을 둔다.
- threshold/backpressure policy: `cuvs.rebuild_threshold` 또는 `cuvs.max_delta_rows` 초과 시 GPU+delta path를 중지하고 CPU fallback한다. background rebuild는 Phase 3A MVP 범위 밖이며, 자동 실행 주체가 생기기 전까지는 `REINDEX` 권고만 한다.
- stats: `pg_stat_gpu_search`에 `delta_count`, `tombstone_count`, `delta_search_us`, `delta_merged`, `stale_but_corrected` 계열 컬럼 추가.
- rebuild compaction: successful REINDEX 후 pending-delta/tombstone을 비우고 새 base generation으로 교체.

정합성 원칙:
- pending-delta가 없으면 INSERT된 새 벡터가 CAGRA 후보에 절대 나오지 않으므로 stale GPU search는 틀릴 수 있다.
- pending-delta exact search가 들어오기 전까지 Phase 2의 CPU fallback 정책을 유지한다.
- delta merge가 실패하거나 delta artifact가 손상되면 GPU path를 사용하지 않고 CPU fallback한다.
- Phase 3 MVP의 delta/tombstone은 source-of-truth가 아니라 derived artifact다. WAL과 같은 durability 계약을 흉내 내지 않는다.
- daemon restart 후 delta/tombstone artifact가 없거나 손상되면 stale GPU path를 쓰지 않고 CPU fallback한다.
- `.stale`/generation marker는 delta보다 오래 살아야 한다. delta가 유실돼도 base CAGRA가 fresh로 오인되면 안 된다.
- 전역 tombstone으로 UPDATE old TID를 제거하면 오래된 snapshot에서 visible한 tuple을 누락할 수 있으므로 금지한다.

PostgreSQL transaction/MVCC caveat:
- index AM write callback은 transaction commit 전에 호출될 수 있다.
- aborted transaction의 tuple TID가 delta/tombstone에 남을 수 있으므로 heap recheck/MVCC visibility가 최종 필터다.
- heap recheck는 후보를 제거할 수만 있고, merge에서 이미 빠진 후보를 되살릴 수 없다. 따라서 tombstone filtering은 snapshot-aware여야 하고, over-fetch/fallback이 필요하다.
- rollback을 완벽히 추적하는 WAL-like delta는 Phase 3A MVP 범위가 아니다.
- cleanup policy는 명시해야 한다: VACUUM/REINDEX/build generation 전환 시 delta/tombstone을 정리하고, cleanup 실패 시 CPU fallback한다.

검증:
- INSERT 후 REINDEX 전에도 새 row가 GPU+delta merged top-k에 포함된다.
- UPDATE는 old snapshot에서 old TID가 계속 보이고, new snapshot에서 new vector delta가 보인다.
- DELETE된 base TID는 해당 snapshot에서 invisible할 때만 merge 단계에서 제거된다.
- delta threshold 초과 시 rebuild 권고/trigger가 발생한다.
- daemon restart 후 delta/tombstone이 유실되어도 persistent stale/generation marker 때문에 stale GPU를 fresh로 서빙하지 않는다.
- aborted transaction delta가 가까운 distance를 갖더라도 heap recheck 후 k개 recall이 유지되거나 CPU fallback한다.
- L2/cosine/IP 각각에서 pgvector CPU ground truth와 top-k가 일치한다.
- random INSERT/UPDATE/DELETE/query interleaving property test를 추가한다.
- bulk INSERT/COPY에서 delta threshold/backpressure 정책이 per-row IPC storm 없이 동작한다.

Phase 3A 완료 기준:
- write-heavy workload에서 stale CPU fallback 없이 base CAGRA + pending-delta exact search로 정합한 top-k를 반환한다.
- aborted transaction, UPDATE, DELETE, VACUUM, daemon restart에서 틀린 GPU 결과를 반환하지 않는다.
- delta/tombstone 손상 또는 cleanup 실패는 CPU fallback으로 닫힌다.
- old snapshot visibility, over-fetch recall, restart fail-closed, metric-specific merge가 regression/integration/property test로 검증된다.

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

#### Phase 3C — Artifact Manifest / S3-backed Immutable Index Snapshots

구현 항목:
- local `cuvs.index_dir` artifact를 S3 snapshot으로 확장한다.
- 경로: `s3://<bucket>/pg_cuvs/<cluster_id>/<database_oid>/<index_oid>/<version>/`
- manifest, checksum, version을 둬 partial upload/corrupt upload/stale upload를 감지한다.
- local NVMe는 cache, S3는 재사용 가능한 artifact store로 취급한다.
- index artifact는 WAL 대상이 아닌 derived data로 유지한다.

Manifest contract:
- `database_oid`
- `index_oid`
- `relfilenode` 또는 build source identity
- `base_generation`
- metric, dimension, vector count
- artifact paths와 checksums
- build timestamp
- pg_cuvs version과 cuVS version
- stale/delta compatibility marker

Phase 3C 완료 기준:
- manifest-backed S3 snapshot upload/download가 성공한다.
- partial upload, corrupt artifact, stale manifest, missing local cache가 감지되고 복구 또는 CPU fallback으로 닫힌다.
- snapshot artifact는 PostgreSQL WAL source-of-truth가 아니라 재생성 가능한 derived data로 유지된다.

#### Phase 3D — Replica / Multi-node Loading + Async Warmup

구현 항목:
- primary에서 build 후 S3 upload.
- read replica는 heap scan rebuild 없이 S3에서 download/load.
- catalog OID, relfilenode 변화, manifest version mapping을 관리한다.
- daemon startup 시 metadata만 scan하고 hot index를 background prefetch한다.
- NVMe cache miss 시 S3 download 후 VRAM promotion한다.
- warmup 상태와 miss reason을 stats view에 노출한다.

Phase 3D 완료 기준:
- 새 daemon 또는 read replica가 heap rebuild 없이 S3 snapshot에서 index를 복구한다.
- startup은 metadata scan으로 빠르게 시작하고, hot artifact는 background warmup으로 적재한다.
- warmup/cache miss/download/reload 상태가 stats view에 노출된다.

#### Phase 3E — Multi-GPU / Sharding

구현 항목:
- shard 단위 index build/search.
- GPU assignment와 VRAM budget per device.
- query fanout 후 top-k merge.
- PCIe/NUMA topology 고려.

Phase 3E 완료 기준:
- multi-GPU shard fanout과 top-k merge가 metric-compatible하게 동작한다.
- GPU별 VRAM budget과 eviction policy가 서로 간섭하지 않는다.
- single-GPU fallback 또는 degraded mode가 명확하다.

Phase 3 전체 완료 기준:
- Phase 3A-3E의 subphase 완료 기준을 모두 만족한다.
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
