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

2026-05-25 기준 코드베이스는 Phase 1 proof-of-mechanism을 넘어 sidecar, CAGRA, persistence PoC가 들어간 상태다. 예전 단계 구분에서 sidecar와 CAGRA를 후속 작업으로 보던 표현은 더 이상 현재 상태를 설명하지 않는다.

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

현재 남은 핵심 debt:
- `CREATE INDEX` durability 계약 정리 중: VRAM build와 disk persistence가 모두 성공해야 DDL success.
- `cuvs_ambuild()`가 전체 corpus를 backend memory에 모은 뒤 daemon으로 넘긴다.
- `aminsert`, `ambulkdelete`, `amvacuumcleanup`이 아직 구현되지 않았다.
- search `k`가 고정값이고 SQL `LIMIT`과 연결되지 않는다.
- metric 판별이 strategy number 휴리스틱에 의존한다.
- planner cost 함수가 CUDA runtime을 직접 touch할 수 있다.
- daemon signal handling이 signal-safe하지 않다.
- 운영용 log level, stats, playbook이 부족하다.

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

#### 5. Operational Safety

Phase 2 전에 정리할 항목:
- daemon signal handler는 flag만 set한다. CUDA serialize, mutex, file I/O는 main loop의 graceful shutdown path에서 수행한다.
- planner cost path에서 CUDA runtime을 직접 touch하지 않는다. daemon status cache 또는 conservative cost로 대체한다. (구현 완료: `cuvsamcostestimate`에서 `cuvs_gpu_available()` 제거.)
- 첫 쿼리 planning 비용은 `shared_preload_libraries = 'pg_cuvs'`로 제거한다. libcuvs.so(812MB)를 postmaster가 한 번 dlopen하고 백엔드가 fork로 상속 → 95ms -> 0.4ms. `make gpu-postinstall`이 설정. (ADR-018.)
- `fprintf(stderr)`는 log level macro로 정리한다: ERROR, WARN, INFO, DEBUG.
- `PG_CUVS_DEBUG`는 hot-path trace 전용으로 유지한다.

#### 6. Playbook

`docs/playbooks/`에 다음 문서를 둔다.

- `gpu-vm-build-and-test.md`
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
- regression/e2e target이 문서화된 명령 하나로 실행된다.
- 운영 playbook이 최소 복구 흐름을 포함한다.

---

### Phase 2 — Production Ready Single-Node

목표: 단일 PostgreSQL instance + GPU sidecar 조합을 운영 가능한 수준으로 만든다. Phase 2의 우선순위는 DiskANN보다 관측성, planner/executor 정확도, write/staleness 계약이다.

#### 1. Observability: `pg_stat_gpu_search`

Phase 2 초반에 추가한다. 이후 planner, fallback, VRAM cache, DiskANN 작업을 디버깅하는 기반이다.

MVP 구현:
- stats source of truth는 daemon memory에 둔다.
- extension function이 IPC `STATUS` 또는 `STATS` op로 daemon stats를 조회한다.
- SQL view `pg_stat_gpu_search`를 제공한다.

초기 컬럼 후보:
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
- `p95_latency_us`
- `gpu_kernel_us`
- `ipc_us`
- `cpu_recheck_us`
- `vram_cache_hits`
- `vram_cache_misses`
- `reload_count`
- `last_status`
- `last_error`
- `last_search_at`

후속:
- PostgreSQL native shared stats 또는 `pgstat` integration은 MVP 이후 검토한다.

#### 2. Planner / Executor Correctness

필수 항목:
- SQL `LIMIT`을 GPU top-k로 전달하고 고정 `k=100`을 제거한다.
- opclass/operator OID 기반으로 L2/Cosine/IP metric을 판별한다.
- cost model을 rows, dim, k, daemon availability, index residency 기준으로 개선한다.
- daemon status cache를 사용해 planner에서 CUDA runtime을 직접 호출하지 않는다.
- `pg_stat_gpu_search`에 `requested_k`, `returned_k`, fallback reason을 기록한다.

#### 3. Write / Staleness Policy

CAGRA는 정적 graph index이므로 write path는 명시적 계약이 필요하다.

구현 방향:
- `aminsert`, `ambulkdelete`, `amvacuumcleanup`을 채운다.
- INSERT/UPDATE/DELETE 발생 시 pending-delta 또는 stale marker를 기록한다.
- threshold 초과 시 WARNING을 내고 operator에게 VACUUM/REINDEX를 안내한다.
- VACUUM 또는 REINDEX에서 현재 heap 상태로 rebuild하고 resident index를 atomic swap한다.

MVP 선택지:
- write 발생 후 cagra index를 stale로 표시하고 query는 CPU fallback.
- 이후 pending-delta 보정과 lazy rebuild로 확장.

#### 4. VRAM / NVMe Tiered Cache

구현 항목:
- VRAM resident index LRU eviction을 production path로 안정화한다.
- evicted index는 persisted artifact에서 reload한다.
- `cuvs.max_vram_mb`를 query/build/load 모두에 일관 적용한다.
- reload latency와 hit/miss를 `pg_stat_gpu_search`에 기록한다.

#### 5. Large Build Memory Pressure

현재 `cuvs_ambuild()`는 모든 벡터를 backend memory에 모은 뒤 daemon으로 전달한다. Phase 2에서는 최소한 한도와 실패 정책을 명확히 하고, 가능하면 streaming handoff로 개선한다.

후보:
- chunked shared memory streaming.
- daemon-side staging file.
- mmap 기반 temp corpus.
- shard build foundation.

Phase 2 완료 기준:
- 10M급 vector workload에서 OOM 없이 build/search/fallback/reload가 동작한다.
- write/staleness 상태가 SQL과 log에서 명확히 관측된다.
- `pg_stat_gpu_search`가 GPU 사용량, fallback, latency, cache hit/miss를 보여준다.
- daemon restart와 index reload가 playbook 없이도 자동 검증된다.

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
