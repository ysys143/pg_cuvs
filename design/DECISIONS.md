# pg_cuvs Architecture Decisions

날짜순으로 기록. 결정 이유와 트레이드오프를 함께 남긴다.

---

## ADR-001 — C 본체 + C++/CUDA 래퍼 분리

**날짜**: 2026-05-22

**문제**: PostgreSQL 헤더의 `float4`와 CUDA의 `float4` 타입이 충돌한다.

**결정**: PG 헤더를 쓰는 코드는 모두 `.c` (순수 C). CUDA/cuVS를 호출하는 코드는 `.cu`로 분리하고 `extern "C"` 인터페이스(`src/cuvs_wrapper.h`)로 연결.

**결과**:
- `src/pg_cuvs.c` — PG AM 핸들러, GUC, 코스트 모델
- `src/cuvs_wrapper.cu` — GPU 호출, nvcc로 컴파일
- `src/cuvs_wrapper.h` — 두 세계의 유일한 접점

**대안**: 전체를 C++로 작성. 거부 — PG 확장은 C로 작성하는 게 표준이며, CUDA 헤더와의 충돌이 더 자주 발생.

---

## ADR-002 — Sidecar Daemon (PG-Strom 스타일)

**날짜**: 2026-05-22

**상태**: 구현 완료. Phase 설명은 ADR-015/016의 최신 게이트가 대체한다.

**문제**: PG의 process-per-connection 모델에서 백엔드마다 CUDA 컨텍스트를 생성하면 VRAM이 빠르게 고갈된다.

**결정**: `pg_cuvs_server` 별도 데몬이 CUDA 컨텍스트를 단독 소유. 백엔드는 shared memory IPC로 요청만 던진다.

**결과**:
- 초기 구상은 Phase 1에서 in-process 호출로 동작을 검증하고 Phase 2에서 sidecar로 전환하는 것이었다.
- 2026-05-25 기준 실제 구현은 Phase 1 proof-of-mechanism 중 UDS + `shm_open` IPC와 `pg_cuvs_server` sidecar까지 진입했다.
- 이후 계획은 Phase 1.5에서 durability/test/ops hardening을 먼저 닫고 Phase 2로 넘어간다.
- GPU 데몬이 죽어도 PG는 살아있음 → CPU 경로(pgvector HNSW)로 계속 서비스

**대안**: pg_duckdb처럼 in-process 링크. 거부 — 연결당 컨텍스트 비용이 비현실적.

---

## ADR-003 — Cost Model 기반 자동 라우팅

**날짜**: 2026-05-22

**문제**: 데이터가 적을 때는 GPU IPC 오버헤드가 CPU SeqScan보다 비싸다. 사용자가 매번 선택하게 하면 안 된다.

**결정**: `cuvsamcostestimate`가 startup_cost=1000 (PCI-e + CUDA context overhead)를 반환. 행 수가 적으면 PG planner가 자동으로 HNSW를 선택. `enable_cuvs=off`로 수동 강제 가능.

**대안**: GUC 하나로 GPU on/off. 거부 — 같은 워크로드 안에서도 테이블 크기에 따라 최적 경로가 달라짐.

---

## ADR-004 — 로컬 맥 + GCP GPU VM 분리

**날짜**: 2026-05-22

**문제**: 개발 머신에 NVIDIA GPU가 없다.

**결정**: 로컬 맥은 코드 편집만. 빌드/테스트는 GCP L4 VM. Makefile의 `gpu-*` 타깃이 SSH로 원격 실행 + 결과를 `tee`로 로컬에 스트리밍.

**적용한 pg_aidb 교훈**:
- named volume 함정 → bind mount + 명시적 copy
- pipe truncation → `ssh -tt` + `tee` 실시간 스트리밍
- ad-hoc 명령 → 모든 GCP 실행을 make 타깃으로 캡슐화
- secrets → `.env.gpu` (gitignored)

**대안**: GPU 노트북 구매. 보류 — 클라우드 비용이 더 저렴하고 다양한 GPU 아키텍처 테스트가 가능.

---

## ADR-005 — 초기 Phase 1 brute-force 범위 결정

**날짜**: 2026-05-22

**상태**: 대체됨. CAGRA build/search/persistence PoC가 Phase 1 중 구현되었고, 최신 단계 관리는 ADR-015/016과 `design/PLAN.md`를 따른다.

**문제**: CAGRA 인덱스 빌드/persist 로직이 복잡한데, 먼저 GPU 호출 경로 자체를 검증하고 싶다.

**결정**: 초기 결정은 Phase 1을 `cuvs_brute_force_search`로 매 쿼리마다 코퍼스를 GPU에 올리는 exact search로 제한하고, CAGRA + persist + cache는 후속 단계에서 구현하는 것이었다.

**결과**: 이 결정은 구현 진행 중 superseded 되었다. 2026-05-25 기준 `src/cuvs_wrapper.cu`에는 CAGRA build/search/serialize/deserialize wrapper가 있으며, daemon path도 존재한다. 남은 쟁점은 "CAGRA를 할지 말지"가 아니라 DDL durability, test coverage, write/staleness, observability를 운영 계약으로 고정하는 것이다.

**대안**: 처음부터 CAGRA. 거부 — Phase 1의 목표는 "GPU 경로 살아있음"을 증명하는 것이지 성능이 아님.

---

## ADR-007 — Boilerplate vs Phase 1 implementation 경계

**날짜**: 2026-05-22

**문제**: AI council 검증에서 "ambuild=NULL이 위험하다", "TID 매핑이 없다" 등의 지적이 나왔다. 이를 보일러플레이트 단계에서 다 해결하려 하면 Phase 1 구현 작업과 섞인다.

**결정**: 명확한 책임 분리.

**보일러플레이트가 보장하는 것**:
- `make`가 .so를 성공적으로 빌드 (PGXS + nvcc 통합)
- `CREATE EXTENSION pg_cuvs` 가 PG 크래시 없이 로드
- `SHOW enable_cuvs` 동작 (GUC 등록)
- 파일 배치, Makefile, .gitignore, ADR 등 메타 구조

**Phase 1에서 처리할 항목** (council critique 중 다음은 의도적 보류):

| 항목 | 현재 상태 | Phase 1 작업 |
|---|---|---|
| `ambuild = NULL` | smoke.sql이 `CREATE INDEX USING cagra`를 시도하지 않아 노출 없음 | stub 추가 (`FEATURE_NOT_SUPPORTED` 명시) 또는 실제 구현 |
| `aminsert = NULL` | 동일 | 동일 |
| Cost model 하드코딩 (1000) | 라우팅이 실제 평가 안 됨 | `path->rows`, selectivity, dim 기반 재계산 |
| TID 매핑 없음 | `cuvs_gettuple`이 ERROR로 차단 | `CuvsSearchResult`에 `ItemPointerData` 추가 + heap 매핑 레이어 |
| 메트릭 파라미터 없음 | 항상 `L2Expanded` | `cuvs_brute_force_search`에 `metric` enum 추가 |
| `amvalidate` 누락 | PG 14+ 표준 | 콜백 추가 |
| `cuvs_gpu_available()` 매번 호출 | planner 컨텍스트에서 CUDA touch | process-level static 캐싱 |
| dimension 검증 누락 | CUDA 커널 segfault 위험 | `aminsert`에서 dim 체크 |
| conda libcuvs `-Wl,-rpath` | postmaster가 라이브러리 못 찾을 수 있음 | `SHLIB_LINK`에 `-Wl,-rpath` 추가 |

**보일러플레이트에서 실제로 수정한 결함** (council critique 중 P0):
- `Makefile`의 `OBJS`에 `src/cuvs_wrapper.o` 추가 (빌드 시스템 자체 결함)
- `sql/pg_cuvs--0.1.0.sql`의 cosine opclass support function을 `vector_cosine_distance`로 교정 (오기재)
- `src/cuvs_wrapper.cu`에 `#include <vector>` 추가 (transitive include 의존 제거)

**원칙**: 보일러플레이트는 "잘못된 코드"가 아니라 "미완성 코드"여야 한다. ERROR/`FEATURE_NOT_SUPPORTED`로 명시적으로 끊어진 곳은 보일러플레이트 단계의 정상 상태. NULL pointer crash 같이 정의되지 않은 동작만 차단.

---

## ADR-006 — pg_aidb와의 통합 표면

**날짜**: 2026-05-22

**문제**: pg_aidb가 `ai.chunks USING hnsw (embedding vector_cosine_ops)`를 쓴다. pg_cuvs 통합 시 사용자가 SQL을 바꾸지 않아야 한다.

**결정**: pg_cuvs는 pgvector와 **동일한 operator class 이름**(`vector_cosine_ops`)을 다른 AM(`cagra`)에 등록. pg_aidb는 GPU 환경에서만 `USING cagra` 인덱스를 추가 생성하면 됨 — `ai.search` SQL 코드는 변경 없음.

**참조**: `pg_aidb/design/GPU_STRATEGY.md` §3 (쿼리 패턴별 가속 효과)

---

## ADR-008 — IPC 프로토콜: Unix Domain Socket + shm zero-copy

**날짜**: 2026-05-23

**문제**: `cuvs_ipc.c`에 IPC 프로토콜이 결정되지 않은 채 스캐폴드만 존재한다. PLAN.md는 "shm_open으로 핸들만 주고받는 방식"을 언급했지만 시그널링 메커니즘이 미결이었다.

**조사**: PG-Strom 소스 및 문서 확인 결과, PG-Strom은 POSIX shm+세마포어가 아닌 **Unix Domain Socket**을 주 IPC로 사용한다. shm은 GPU Cache REDO 로그 버퍼에만 한정적으로 사용.

**결정**: 커맨드(JSON 프레임)는 UDS로 전달. 실제 벡터 데이터(쿼리 벡터, 결과 TID 배열)는 `shm_open` 공유 메모리로 zero-copy 전달. 소켓은 핸들 참조만 전송.

**결과**:
- 다중 백엔드 동시 처리가 `accept()` 루프로 자연스럽게 처리됨
- 디버깅 시 `nc -U /tmp/.s.pg_cuvs.<pid>`로 직접 테스트 가능
- Phase 2 `pg_stat_gpu_search` 통계 쿼리를 동일 소켓으로 확장 가능
- `cuvs.socket_path` GUC 미설정 시 기본값: `/tmp/.s.pg_cuvs.<postmaster_pid>`

**대안**: POSIX shm + 세마포어. 거부 — 다중 백엔드 큐 관리를 직접 구현해야 하고, 운영 디버깅이 어려움. 레이턴시 차이(수 마이크로초)는 CAGRA 검색 ms 대비 무시 가능.

---

## ADR-009 — 인덱스 영속화: GUC cuvs.index_dir

**날짜**: 2026-05-23

**문제**: CAGRA 인덱스는 VRAM 상주 인메모리 구조다. 데몬 재시작 시 인덱스가 사라지면 매번 전체 재빌드가 필요하다.

**결정**: `cuvs.index_dir` GUC로 직렬화 경로를 설정. 미설정 시 기본값 `$PGDATA/cuvs_indexes/`. cuVS 네이티브 serialize/deserialize API 사용.

**결과**:
- Phase 2 NVMe tiered caching과 자연스럽게 연결 — index_dir을 NVMe 마운트 포인트로 설정 가능
- Phase 3C object storage snapshot 연동 시 동일 경로 추상화 재사용 가능
- SIGTERM 수신 시 모든 VRAM 상주 인덱스를 직렬화 후 종료
- 시작 시 `pg_catalog.pg_index`에 존재하는 OID와 대조해 유효한 인덱스만 로드

**대안**: `$PGDATA` 고정 경로. 거부 — CAGRA 인덱스는 수 GB 단위라 $PGDATA를 급속히 팽창시키며, NVMe 분리 마운트 불가.

---

## ADR-010 — VRAM OOM 정책: 3계층

**날짜**: 2026-05-23

**문제**: cuVS는 VRAM 초과 시 CUDA OOM 에러를 반환한다. pg_cuvs가 이를 그대로 사용자에게 전파하면 운영 안정성이 떨어진다.

**결정**: 3계층 OOM 정책 적용.

```
레이어 1 — 사전 예방: 인덱스 로드 전 VRAM 여유 공간 사전 계산
레이어 2 — LRU 에비션: 공간 부족 시 최근 미사용 인덱스를 cuvs.index_dir에 직렬화 후 VRAM 해제, 재시도
레이어 3 — CPU fallback: 전체 에비션 후에도 공간 부족 시 pgvector HNSW로 라우팅 + WARNING 로그
```

**결과**:
- 사용자 쿼리는 OOM 상황에서도 성공 (성능 저하만 발생)
- LRU 에비션이 Phase 2 tiered caching의 기반 구현이 됨
- `cuvs.max_vram_mb` GUC로 물리 VRAM 미만의 예산 제한 가능

**대안**: Fail-fast(에러 전파). 거부 — 사용자가 인덱스 수를 명시적으로 관리해야 하며 운영 부담이 큼.

---

## ADR-011 — CPU fallback 트리거: 4가지 조건

**날짜**: 2026-05-23

**문제**: GPU 경로가 비활성화되어야 하는 조건이 여러 가지이며, 각각 다른 검출 메커니즘이 필요하다.

**결정**: 다음 4가지 조건에서 pgvector HNSW로 자동 전환.

| 트리거 | 메커니즘 |
|--------|---------|
| 코스트 모델 | `cuvsamcostestimate`가 GPU 경로 비용 > CPU 경로 비용으로 산출 시 planner가 자동 선택 |
| GUC 수동 전환 | `enable_cuvs = off` 설정 시 pg_cuvs_server 미접촉 |
| 데몬 장애 | `connect()` ECONNREFUSED/ETIMEDOUT 시 자동 fallback + WARNING (세션당 1회) |
| Circuit breaker | 연속 GPU 에러 n회(`cuvs.circuit_breaker_threshold`, 기본 3) 초과 시 자동 비활성화, `pg_cuvs_reset_circuit()` 함수로 재활성 |

**결과**: 모든 실패 모드에서 사용자 쿼리가 성공. GPU 서비스는 PostgreSQL 가용성에 영향을 주지 않음 (ADR-002 sidecar 원칙 강화).

---

## ADR-012 — Phase 3B DiskANN/Vamana: cuVS Vamana 네이티브 방식

**날짜**: 2026-05-23
**상태**: Phase 2에서 Phase 3B로 이관됨

**문제**: 초기 PLAN.md는 "CAGRA로 GPU 빌드 후 HNSW 포맷으로 변환"을 DiskANN 방식으로 언급했다. 그러나 cuVS에 더 직접적인 경로가 있는지 확인이 필요했다. Product Phase 2가 single-node CAGRA core로 고정되면서 DiskANN/Vamana는 Phase 3B로 이관됐다.

**조사**: cuVS에 `cuvs.neighbors.vamana`가 존재하며, DiskANN 바이너리 포맷으로 직접 저장/로드 가능(`vamana.save()` / `vamana.load()`). CAGRA→HNSW 변환 레이어 불필요.

**결정**: Phase 3B DiskANN/Vamana는 두 경로를 모두 지원한다.

```
CAGRA build(GPU) → HNSW format → CPU HNSW search   (중간 규모, RAM 상주)
Vamana build(GPU) → DiskANN binary → CPU Vamana search  (대규모, NVMe)
```

공통 원칙: **빌드만 GPU 가속, 검색은 CPU**. 디스크 기반 검색은 I/O가 병목이라 GPU 전송 오버헤드가 이득을 상쇄.

**결과**: PLAN.md의 CAGRA→HNSW 변환 방식은 cuVS `from_cagra()` API로 대체. 단, CAGRA→HNSW 경로도 CPU fallback용으로 병행 지원 가능 (`cuvs.export_hnsw = on` GUC).

**대안**: pgvectorscale DiskANN 재사용. 보류 — 라이선스 확인 필요, cuVS Vamana가 더 직접적 경로.

---

## ADR-013 — Phase 3C Object Storage: Derived Data + Snapshot Source of Truth 방향

**날짜**: 2026-05-23
**상태**: S3-specific 표현에서 GCS-first object storage 설계로 일반화됨

**문제**: 수십억 규모 인덱스를 어떻게 저장하고 멀티노드에서 공유할지 결정이 필요하다.

**결정**: 두 단계로 전환.

- **Phase 3C MVP**: 인덱스를 derived data로 취급 (WAL 제외). Object storage snapshot은 heap-compatible PostgreSQL node에서 재빌드 시간을 줄이는 캐시로 사용한다. Heap/table 배포는 PostgreSQL backup/replication이 책임진다. MVP provider는 GCS다.
- **Phase 3C v2**: Object storage를 derived index artifact의 snapshot source로 전환. 로컬 NVMe는 io_uring 비동기 프리페치 캐시. 인스턴스 교체 또는 읽기 전용 레플리카가 compatible heap을 이미 가진 경우 object storage에서 index artifact를 직접 로드한다.

**결과**:
- MVP 단계에서 구현 복잡도 최소화
- 멀티노드 공유 필요 시 object storage snapshot source로 자연스럽게 진화
- 인덱스 파일은 `pg_basebackup` WAL 스트림에서 제외 (`SPEC.md OBJSTORE-03` 참조)
- heap 없이 index artifact만 있는 노드는 사용할 수 없다. TID mapping은 로컬 PostgreSQL heap block/offset과 호환되어야 한다.

---

## ADR-014 — 쓰기 처리: AUTOVACUUM 연동 Lazy Rebuild

**날짜**: 2026-05-23
**상태**: 보완됨. Phase 2/3 분리는 `design/PLAN.md` 최신본을 따른다.

**문제**: CAGRA는 정적 그래프 인덱스라 INSERT/UPDATE/DELETE 시 실시간 업데이트가 불가능하다.

**결정**:
- Phase 2 MVP는 write event를 binary stale marker로 기록하고, stale index에 대해서는 GPU CAGRA search를 사용하지 않는다. 기본 동작은 CPU fallback이다.
- stale marker는 daemon restart 후 사라지면 오래된 artifact를 fresh처럼 로드할 수 있으므로 durable sidecar 정책을 사용한다.
- pending-delta / delta exact search는 write-heavy workload에서 GPU path를 유지하기 위한 필수 기능이지만, Phase 2 MVP의 정합성 게이트와 분리해 Phase 3 필수 항목으로 올린다.

**결과**:
- Phase 2에서는 쓰기 후 조용한 오답을 피한다. INSERT된 새 벡터는 base CAGRA에 없으므로 heap recheck만으로는 누락을 보정할 수 없다.
- Phase 3A pending-delta가 들어오면 base CAGRA result와 CPU-side delta exact result를 snapshot-aware하게 merge해 REINDEX 전에도 정합한 top-k를 유지한다.
- delta 비율이 `cuvs.rebuild_threshold` 또는 resource limit을 초과하면 GPU+delta path를 중지하고 CPU fallback한다. 자동 background rebuild는 별도 실행 주체가 생기기 전까지 Phase 3A MVP가 아니다.

**대안**:
- stale index를 계속 GPU로 검색. 거부 — INSERT/UPDATE new row 누락으로 오답 가능.
- Phase 2에서 pending-delta까지 한 번에 구현. 보류 — delta store, tombstone, rollback/durability, top-k merge까지 같이 필요해 Step 4 MVP 범위를 넘는다.
- Background Worker 주기적 재빌드. 보류 — pending-delta threshold와 결합해 Phase 3에서 재검토.

---

## ADR-015 — Phase 1.5 Test & Ops Hardening 게이트

**날짜**: 2026-05-25

**문제**: Phase 1 proof-of-mechanism 이후 sidecar, CAGRA build/search, persistence가 빠르게 들어왔지만, Phase 2 기능을 얹기 전에 durability, failure mode, test coverage, playbook 기준선이 충분히 고정되지 않았다. 이 상태에서 DiskANN, write path, tiered cache를 추가하면 기존 결함과 신규 결함의 원인 분리가 어려워진다.

**결정**: Phase 2 전에 **Phase 1.5 — Test & Ops Hardening** 단계를 둔다.

Phase 1.5 범위:
- `CREATE INDEX USING cagra` durability 계약 확정.
- unit, integration, GPU e2e coverage 확장.
- failure injection hook 추가.
- daemon signal safety 정리.
- planner path에서 CUDA runtime 직접 호출 제거.
- 운영 log level 정리.
- GPU VM build/test, daemon restart, index failure, persistence corruption, VRAM OOM, rollback playbook 작성.

**결과**:
- Phase 2 시작 전 현재 동작 계약을 회귀 테스트로 잠근다.
- 운영 장애 대응 절차를 코드와 함께 유지한다.
- Phase 2 기능 추가 시 `pg_stat_gpu_search`와 e2e tests를 기준으로 회귀를 빠르게 찾는다.

**대안**: Phase 2 기능 구현과 테스트 강화를 병행. 거부 — C/CUDA/PostgreSQL extension 조합에서는 failure mode가 겹치기 쉬워 원인 분리가 어렵다.

---

## ADR-016 — `CREATE INDEX USING cagra` 성공 조건: VRAM build + disk persistence

**날짜**: 2026-05-25

**문제**: `CREATE INDEX`가 성공했는데 `.cagra` 또는 `.tids` artifact가 디스크에 남지 않으면, PostgreSQL catalog에는 index가 존재하지만 daemon restart 후 GPU index를 복구할 수 없다. 이는 DDL durability 기대와 맞지 않는다.

**결정**: `CREATE INDEX USING cagra`는 다음 조건이 모두 성공해야 성공으로 간주한다.

- daemon 연결 성공.
- VRAM CAGRA build 성공.
- `.cagra` serialize 성공.
- `.tids` mapping persistence 성공.
- tmp write, file fsync, atomic rename, directory fsync 성공.

실패 시:
- `pg_cuvs_server`는 build failure 또는 persistence failure status를 반환한다.
- `cuvs_ambuild()`는 `ereport(ERROR)`로 `CREATE INDEX`를 실패시킨다.
- PostgreSQL catalog 변경은 transaction rollback으로 취소된다.
- SELECT search path의 daemon failure는 기존 원칙대로 CPU fallback 가능하되, DDL path는 fallback success로 처리하지 않는다.

**결과**:
- SQL DDL 성공 의미가 persistent GPU index artifact 존재와 일치한다.
- daemon restart 후 index 복구 가능성이 DDL 성공 계약에 포함된다.
- build 중 기존 index가 있는 경우 새 build/persist가 완전히 성공하기 전까지 기존 resident index를 유지해야 한다.

**대안**:
- daemon unavailable 또는 persistence failure를 WARNING으로만 남기고 CPU fallback. 거부 — catalog와 artifact 상태가 불일치한다.
- VRAM build만 성공하면 DDL 성공. 거부 — restart durability가 깨진다.

---

## ADR-017 — Phase 2 Observability: `pg_stat_gpu_search`

**날짜**: 2026-05-25

**문제**: GPU search, fallback, cache reload, daemon error가 PostgreSQL SQL 표면에서 관측되지 않으면 Phase 2의 planner/executor 개선, write/staleness 정책, VRAM tiered cache를 검증하기 어렵다.

**결정**: Phase 2 초반에 `pg_stat_gpu_search`를 추가한다.

MVP:
- stats source of truth는 `pg_cuvs_server` memory에 둔다.
- extension function이 IPC `STATUS` 또는 `STATS` command로 daemon stats를 조회한다.
- SQL view `pg_stat_gpu_search`가 per-index counters와 latency/fallback/cache 정보를 노출한다.

초기 지표:
- database OID, index OID, index name.
- calls, success, fallback, error.
- requested k, returned k, rows returned.
- average latency, p95 latency.
- GPU kernel time, IPC time, CPU recheck time.
- VRAM cache hits/misses, reload count.
- last status, last error, last search timestamp.

**결과**:
- Phase 2 기능의 운영성과 성능을 SQL에서 확인할 수 있다.
- playbook이 log scraping에만 의존하지 않는다.
- 추후 PostgreSQL native `pgstat` integration으로 확장할 수 있다.

**대안**: daemon log만 사용. 거부 — SQL 운영자가 index별 상태와 fallback reason을 조회하기 어렵다.

---

## ADR-018 — `shared_preload_libraries = 'pg_cuvs'`로 첫 쿼리 planning 비용 제거

**날짜**: 2026-05-25

**문제**: `cagra` access method가 로드된 직후 첫 쿼리의 planning time이 ~95ms로 측정됐다. 원인은 백엔드가 cost 함수에서 pg_cuvs.so를 lazy dlopen하면서 의존성 libcuvs.so(812MB)를 처음 mmap/relocate하는 비용. pgvector(planning <1ms) 대비 100배 이상 느려 운영상 받아들이기 어렵다.

추가로 cost 함수가 `cudaGetDeviceCount`를 호출해 CUDA runtime을 lazy init(~100ms)하는 별도 비용도 있었다.

**결정**:
1. cost 함수(`cuvsamcostestimate`)에서 `cuvs_gpu_available()` 호출을 제거한다. GPU 가용성은 cost 단계가 아니라 실행 시점(데몬 IPC)에 판정한다.
2. `shared_preload_libraries = 'pg_cuvs'`를 권장 설정으로 둔다. postmaster가 시작 시 한 번 libcuvs.so를 dlopen하면, 이후 모든 백엔드는 fork로 그 매핑을 copy-on-write 상속하므로 첫 쿼리 planning에서 dlopen 비용을 내지 않는다.
3. 이 설정은 `make gpu-postinstall`(= `infra/scripts/postinstall.sh`)이 idempotent하게 적용한다.
4. JIT threshold는 이 ADR의 자동 설정 범위에 포함하지 않는다. 현재 cagra query cost는 기본 `jit_above_cost`보다 낮아 JIT이 발생하지 않으며, 향후 cost model을 키운 뒤 대규모 benchmark에서 실제 JIT 발생과 latency variance를 측정한 경우에만 threshold sweep으로 결정한다.

**결과**:
- cold backend planning time: 95ms -> 0.4~0.5ms (pgvector 수준).
- PG-Strom과 동일한 패턴(무거운 GPU 라이브러리를 postmaster 시작 비용으로 amortize).
- JIT는 원인으로 확인되지 않았으므로, `jit = off` 또는 임의의 `jit_above_cost` 상향을 postinstall 기본값으로 넣지 않는다.
- 측정 확인(2026-05-25, A100): 10K×384와 1M×1536 모두 `EXPLAIN (ANALYZE)`에 `JIT:` section 미발생. 1M×1536 cold planning 0.75ms / warm 0.065ms. JIT 미조정 결정이 대규모 데이터에서도 유효함을 확인했고 threshold sweep은 수행하지 않았다.

**대안**:
- LD_PRELOAD로 libcuvs.so 강제 로드. 거부 — shared_preload_libraries가 PG 네이티브이고 _PG_init 훅까지 정상 동작.
- planner에서 lazy init 유지하고 첫 쿼리만 감수. 거부 — 운영자가 매 세션 첫 쿼리에서 95ms를 보게 되어 벤치마크/체감 품질 저하.
- 데몬이 dlopen을 대신. 거부 — planner는 백엔드 프로세스 주소공간에서 cost 함수를 호출하므로 데몬 매핑을 공유할 수 없다.
- `jit = off` 전역 적용. 보류 — 현재 문제의 원인이 아니고, 향후 분석성 workload의 JIT 이득을 막을 수 있다.
- 임의의 높은 `jit_above_cost` 자동 적용. 보류 — 값은 대규모 데이터와 현실적인 cost model에서 측정한 뒤 정해야 한다.

## ADR-019 — Durable `.tids`: versioned + checksummed header와 build/persist status 분리

**날짜**: 2026-05-25

**문제**: `.tids` sidecar는 (n_vecs, dim, metric) + TID body를 헤더 없이 저장하고, daemon startup load가 `fread` 반환값을 검사하지 않았다. rename 사이에 크래시가 나면 torn pair가 그대로 로드돼 잘못된 TID 매핑으로 검색 결과가 조용히 오염될 수 있었다. 또한 build/persist 실패가 모두 generic `CUVS_STATUS_ERROR`로 합쳐져, DDL 실패 원인을 운영자가 구분할 수 없었다.

**결정**:
1. `.tids`에 versioned + checksummed 헤더 `CuvsTidsHeader`를 둔다: `magic`(`TIDS`=0x53444954), `version`, `n_vecs`(int64), `dim`, `metric`, `body_crc32`, `reserved`. LE 전용(x86-64 데몬, `RAFT_SYSTEM_LITTLE_ENDIAN=1`).
2. `cuvs_tids_read`는 magic/version/n_vecs 범위/`reserved==0`/전체 body read/crc32를 모두 검증하고, 하나라도 실패하면 그 pair를 로드 대상에서 제외한다. `.cagra` deserialize와 `.tids` read가 둘 다 성공할 때만 resident로 등록한다.
3. IPC status를 `CUVS_STATUS_BUILD_FAILED`(GPU build/malloc 실패)와 `CUVS_STATUS_PERSIST_FAILED`(serialize/.tids/rename/fsync/registry-full)로 분리한다. `OOM_FALLBACK`/`UNAVAILABLE`은 유지.
4. `idx_tmp`의 fsync 실패를 fatal로 처리(`persist_fail`로 분기)해 durable하지 않은 `.cagra`가 commit되지 않게 한다.
5. 헤더/검증/crc32/TID encode·decode/filename parsing/status 매핑/circuit breaker를 PG·CUDA-free `src/cuvs_util.{h,c}`로 추출해, `.so`·데몬·독립 단위테스트가 같은 코드를 링크한다.

**결과**:
- crc32 + magic으로 torn/corrupt pair를 startup에서 거부(검색 오염 방지). e2e와 fault injection으로 확인.
- legacy headerless `.tids`는 magic 불일치로 거부 → pre-1.0 on-disk index는 REINDEX 필요(shipped user 없으므로 수용).
- `make test-unit` 73 assertions로 helper 검증, GPU VM에서 5개 integration 시나리오 통과.

**대안**:
- 헤더 없이 길이만으로 검증. 거부 — torn write/부분 손상을 감지 못함.
- checksum 생략하고 magic+length만. 거부 — body 손상을 못 잡음. crc32는 ~30줄, deterministic, 단위테스트 가능.
- 모든 실패를 generic ERROR로 유지. 거부 — DDL 실패 원인 구분과 운영 진단이 어렵다.

## ADR-020 — IPC `send_all`/`recv_all`는 `EINTR`을 재시도한다 (대규모 build 안전성)

**날짜**: 2026-05-25

**문제**: 1M×1536 benchmark gate run에서 데몬 journal은 `built index ... 1000000 vecs`로 성공을 기록했는데 클라이언트(`cuvs_ambuild`)는 `BUILD failed (status 1)`로 실패해 catalog를 rollback하고 6.4GB `.cagra` orphan을 남겼다. 원인: 대규모 build의 reply 수신은 백엔드가 수 분간 `recv()`를 블록하는데, `send_all`/`recv_all`이 `read/write < 0`을 무조건 치명적으로 처리해 `EINTR`(백엔드에 전달된 시그널, 예: latch SIGUSR1)에 재시도하지 않았다. 작은 build는 recv 창이 짧아 거의 안 걸리지만, 분 단위 build에서는 높은 확률로 발생한다.

**결정**: `send_all`/`recv_all`의 루프에서 `n < 0 && errno == EINTR`이면 재시도(continue)하고, `n == 0`은 peer-close로 -1을 반환한다. 두 헬퍼는 `cuvs_ipc.c`에 있어 `.so`(클라이언트)와 데몬 양쪽에 동일하게 적용된다.

**결과**:
- 수정 후 1M×1536 `CREATE INDEX`가 클라이언트에서 정상적으로 OK를 받고 end-to-end 성공(build 70.8s, p99 4.2ms).
- DDL durability 계약(데몬 성공 == DDL 성공)이 대규모 build에서도 유지된다.

**대안**:
- recv timeout만 늘림. 거부 — 타임아웃이 원인이 아니라 `EINTR`이 원인이며, 600s 안에서도 발생.
- 클라이언트에서 build 결과를 재조회(idempotent re-check). 보류 — 현재 불필요. orphan 정리/재시도 계약은 Phase 2 write-path 작업에서 함께 다룬다.

---

## ADR-021 — Phase 3F True Multi-GPU CAGRA Sharding 설계 lock

**날짜**: 2026-05-28

**문제**: Phase 3E는 index-level multi-GPU(인덱스 하나는 GPU 하나에 통째로 resident, 서로 다른 인덱스를 GPU별로 분산)까지 닫았다. 단일 logical CAGRA index가 GPU 한 장 VRAM보다 크거나 너무 hot한 경우는 3E로 해결되지 않는다.

**결정**: 단일 non-partitioned table의 단일 logical CAGRA index를 N개 shard로 분할한다. 핵심 결정:
- shard = 독립 cuVS CAGRA artifact 하나, GPU 한 장에 resident. logical index가 여러 shard/GPU를 소유. shard 하나가 여러 GPU에 걸치지 않는다.
- shard 분할 기준은 **build-order contiguous range**(global `.tids`의 `[start,end)`). vector-clustering 기반 assignment는 recall/cost trade-off가 커서 follow-up.
- shard count는 **explicit `cuvs.shard_count=N` GUC**로 결정. 기본값 `0`/`1`은 기존 unsharded 경로를 byte-identical하게 유지하고, `N>=2`일 때만 sharding 활성화. auto VRAM-기반 split은 follow-up.
- search는 logical index당 IPC 한 번 유지. 데몬이 shard별 CAGRA search를 실행하고 metric-aware comparator로 global top-k를 merge해 기존 reply format을 반환. MVP는 **sequential fanout**(기존 `g_index_mutex` 직렬화 모델 유지), parallel fanout은 follow-up.
- **fail-closed**: shard 하나라도 missing/corrupt/reload 실패면 partial ANN result를 절대 반환하지 않는다. SELECT는 CPU fallback, DDL/REINDEX는 ERROR.
- durable DDL은 all-or-nothing: 모든 shard `.cagra.tmp` + global `.tids.tmp` + `.shards.tmp`를 fsync한 뒤 rename하고, `.shards` manifest rename을 마지막 commit marker로 삼는다. manifest가 없거나 shard/checksum이 안 맞으면 logical sharded index를 load하지 않는다.
- delta/tombstone은 global `.tids` CRC와 global TID 기준을 유지한다. 1차 구현은 backend CPU delta merge fallback을 그대로 쓰고(데몬은 sharded reply에 `delta_merged=0`), daemon-side shard-aware GPU delta cache는 follow-up.

**결과**:
- artifact: legacy unsharded는 `<db>_<idx>.cagra` + `.tids` 유지. sharded는 global `.tids` + `.shards` manifest + `<db>_<idx>.s%03u.cagra`.
- 새 관측 view `pg_stat_gpu_shards`(shard별 GPU/resident/stats)와 `pg_stat_gpu_search`의 `shard_count` 컬럼 추가.
- 구현/검증은 3F-0(design lock) → 3F-5(SW MVP, 단일 GPU VM 검증 가능) → 3F-6(2x A100 hardware acceptance)까지 sub-phase로 끊고, 3F-6 통과를 Phase 3F 완료 기준으로 본다.

**대안**:
- cuVS `cuvs::neighbors::mg` SHARDED 모드 직접 사용. 보류 — 기존 daemon의 per-device pool/placement/eviction/delta/tombstone 계층과의 통합 비용이 크고, artifact durability 계약을 그대로 재사용하려면 shard = 독립 CAGRA artifact 모델이 더 단순.
- PostgreSQL partitioning recipe(3E-3)로 대체. 거부 — 그건 multiple physical index의 integration recipe이고, 3F는 단일 logical index의 internal sharding으로 사용자 DDL surface를 바꾸지 않는 것이 목표.

## ADR-022 — Phase 3G True Sharding 최적화/제품화 (Core) 설계 lock

**날짜**: 2026-05-28

**문제**: Phase 3F는 단일 logical CAGRA index의 multi-GPU sharding을 correctness/durability/fail-closed까지 닫았지만, 세 가지가 운영 품질을 막는다. (1) `handle_search`가 fanout 전체를 `g_index_mutex` 아래에서 직렬 실행하므로 query latency가 `sum(shard_i)`이고 동시 query도 GPU dispatch에서 직렬화된다. (2) shard count가 수동(`cuvs.shard_count=N`)이라 운영자가 VRAM sizing을 직접 계산해야 한다. (3) shard별 search가 `k`만 가져와서 shard 수가 늘면 recall 방어 수단이 없다.

**결정**: 3G의 본범위를 **Core productization** 세 항목으로 고정한다.
- **parallel fanout**: shard별 search를 thread로 동시 dispatch하고 기존 `delta_cand_cmp` merge를 재사용한다. 1단계는 mutex를 쥔 채 within-query 병렬(`sum -> max(shard_i)`), 2단계는 GPU dispatch 구간만 mutex 밖에서 실행하는 **lock-free window**로 cross-query 동시성까지 연다. lock-free window는 **safe-by-construction**으로 설계한다: mutex 안에서 shard descriptor(handle/gpu/tid_offset/n_vecs) + `tids` 포인터 + metric을 스냅샷하고, mutex를 풀고 parallel dispatch + join한 뒤, 다시 lock을 잡고 `(db_oid,index_oid)`로 entry를 **re-find**해서 counters/merge/stats만 갱신한다(`evict_lru`가 `g_indexes[]`를 value로 compaction해 struct를 이동시킬 수 있으므로 unlock 이후 `IndexEntry*`는 재사용하지 않는다). g_merge_metric 글로벌을 쓰는 collect+qsort merge는 re-lock 구간에서 수행해 직렬성을 유지한다(GPU dispatch만 병렬). inflight refcount/deferred-free는 **도입하지 않는다** — 현재 지원 workload에서 방어할 실제 race가 없기 때문이다(아래 불변식 참조). `cuvs.parallel_fanout`(bool, default on) per-query 토글로 A/B 및 kill switch를 제공한다.
- **lock-free 안전성 불변식(필수 유지)**: (1) sharded entry는 `gpu_device_id=0xFFFFFFFF`로 LRU eviction에서 제외되어 `evict_lru`/free 경로의 대상이 되지 않는다. (2) shard handle과 `tids` 메모리는 search snapshot 이후 free되지 않는다 — 이를 free하는 경로(REINDEX/DROP)는 PostgreSQL `AccessExclusiveLock`으로 동시 search와 직렬화되고, eviction은 (1)로 배제된다. 이 두 전제가 깨지는 future feature(sharded eviction, online shard replacement, lock-free/CONCURRENTLY REINDEX 지원 등)가 들어오면 **inflight refcount + drain(또는 deferred-free)을 반드시 재도입**해야 lock-free window가 안전하다.
- **auto VRAM-based shard count**: `cuvs.shard_count=0`을 **auto**로 정의한다. 데몬이 per-GPU budget과 `estimate_vram_bytes()` 기준으로 derive하되, **한 GPU에 들어가면 1(unsharded)로 resolve**하므로 작은 index의 기존 동작은 byte-identical하게 유지된다. `1`은 강제 unsharded, `N>=2`는 강제 N(3F 동작). 순수 helper `cuvs_auto_shard_count()`를 `cuvs_util`에 두어 CUDA 없이 unit-test한다.
- **`cuvs.shard_overfetch`**(int, default 0): shard별 요청 `k + overfetch`, global merge는 top-k 유지. default 0은 3F 동작 byte-identical, 운영자가 scale에서 recall을 위해 올린다.

**HW gate**: 3F-6과 동일하게 단일 GPU SW 검증 후 **2x A100 한 번의 유료 run**으로 닫는다 — parallel latency < sequential, recall preserved, shard placement/counter 확인, fail-closed 유지, VM stop + 비용 보고까지가 3G 완료 기준.

**결과**:
- 새 GUC `cuvs.parallel_fanout`, `cuvs.shard_overfetch`. `cuvs.shard_count=0`의 의미가 unsharded→auto로 바뀌는 것이 유일한 default-semantics 변경이며, 작은 index는 여전히 1로 resolve된다.
- `CuvsCmdFrame`에 SEARCH용 `shard_overfetch`/`parallel_fanout` 필드 추가(extension+daemon lockstep rebuild). 관측은 기존 `pg_stat_gpu_search` p50/p95/p99 + `shard_count`로 충분, 새 스키마 없음.
- 구현/검증은 3G-0(design lock) → 3G-1(parallel fanout) → 3G-2(auto count) → 3G-3(overfetch) → 3G-4(SW 검증) → 3G-5(2x A100 acceptance)로 끊고, 3G-5 통과를 Phase 3G 완료 기준으로 본다.

**follow-up으로 분리(3G 범위 밖)**:
- daemon-side shard-aware GPU delta cache(기존 Phase 3B `refresh_delta_cache`/`cuvs_bf_search` GPU delta path를 fanout merge에 통합; 현재 sharded는 `delta_merged=0`로 backend CPU merge에 위임).
- object-snapshot manifest extension(3C `CuvsManifest`/`cuvs_objstore`를 `.shards` + per-shard `.sNNN.cagra`까지 확장).
- vector-clustering shard assignment, degraded partial-recall opt-in(아니면 fail-closed-only 유지).

**대안**:
- 한 번에 full 3G(7개 항목 전부). 거부 — clustering은 recall 실험 성격이라 shippable latency win(parallel fanout)을 지연시킨다. correctness/latency를 먼저 닫고 delta-cache/snapshot은 그 구조 위에 붙인다.
- parallel fanout을 mutex를 쥔 채 within-query 병렬로만 구현. 보류 — cross-query 직렬화라는 핵심 병목을 못 풀기 때문에 lock-free window(2단계)까지 간다.
- lock-free window에 inflight refcount + cond-wait drain 추가(원안). 거부 — sharded non-evictable + REINDEX의 PG AccessExclusiveLock 직렬화로 free-during-search race가 지원 workload에서 발생하지 않으므로, 그 machinery는 불가능한 시나리오를 방어하는 over-engineering이고 cond-wait 중 entry 이동 같은 새 동시성 버그를 들여온다. safe-by-construction(스냅샷 + oid re-find) + 불변식 문서화로 대체한다.

## ADR-023 — Phase 3G.1 sharded index DROP cleanup (daemon free + artifact unlink)

**날짜**: 2026-05-28

**문제**: 3G-5 acceptance에서 드러난 누수 — `DROP INDEX`가 데몬에 아무 신호도 보내지 않는다. sharded index는 non-evictable(`gpu_device_id=0xFFFFFFFF`, `find_lru_index`가 skip)이라 DROP 후에도 GPU VRAM에 resident로 남고, on-disk artifact(`.cagra/.tids/.shards/.sNNN.cagra/.delta/.tombstone`)도 `cuvs.index_dir`에 그대로 남아 데몬 재시작 시 dropped index를 zombie로 reload한다. 3G-5에서 dead index가 per-GPU budget을 채워 새 sharded build를 막았다. REINDEX는 이미 정리된다(handle_build existing-entry 경로).

**결정**: DROP commit 시점에 데몬에 통지해 logical index를 VRAM에서 free하고 artifact를 unlink한다.
- **DROP-notify only**. sharded eviction(VRAM pressure 회수)은 제외 — sharded를 evictable로 만들면 3G-1 lock-free search invariant이 깨져 ADR-022가 버린 inflight refcount/deferred-free를 재도입해야 하므로 3G.1b로 분리한다.
- **commit 시점 발사**: backend `object_access_hook`이 cagra index의 `OAT_DROP`을 수집하고, `XACT_EVENT_COMMIT` 콜백에서만 `cuvs_ipc_drop`을 보낸다. drop 시점에 보내면 `BEGIN; DROP INDEX; ROLLBACK;`이 살아있는 index의 artifact를 파괴하므로 금지.
- **DROP은 데몬이 죽어도 실패하지 않는다**: `cuvs_ipc_drop`은 best-effort, 실패 시 backend가 `WARNING`만 남기고 PG DROP은 commit된다. 데몬-down 시 잔존 artifact는 restart/playbook으로 정리.
- 데몬 `handle_drop`은 AccessExclusiveLock 보장(해당 index에 in-flight search 없음) 하에 `g_index_mutex`로 즉시 free + registry compact + 모든 sidecar unlink. inflight guard 불필요.

**결과**:
- 새 IPC op `CUVS_OP_DROP_INDEX`(7) + `cuvs_ipc_drop`(mark_stale mirror). backend `object_access_hook` + `XACT_EVENT_COMMIT` 콜백. 데몬 `handle_drop`.
- 검증: DROP 후 `pg_stat_gpu_shards` 0 rows + VRAM 회수 + restart 후 zombie 없음 + 데몬-down DROP도 성공(WARNING).
- 알려진 한계: rolled-back SAVEPOINT 내 DROP은 여전히 기록되어 commit 시 통지될 수 있음(REINDEX로 복구; 데몬-down과 동일 등급). `DROP EXTENSION CASCADE`가 AM을 먼저 drop하면 `get_am_oid`가 Invalid → 해당 통지 누락(restart/playbook).

**3G.1b로 분리(eviction policy lock)**: sharded eviction = logical-index whole-unit(`free_index_shards`), per-shard 아님. eviction은 save_index 스킵(derived artifact durable), dirty/pending 시 fail-closed. `evict_lru`/`find_lru_index`는 `inflight>0` sharded를 skip. 구현 시 ADR-022 invariant를 갱신(sharded evictable + inflight 재도입).

**대안**:
- whole-index eviction을 3G.1에 포함. 보류 — inflight machinery 재도입 비용/리스크가 크고, 관측된 누수(dropped index 누적)는 DROP-notify만으로 해결된다. live-sharded VRAM pressure는 budget/sizing으로 대응.
- DROP을 drop-time(commit 아님)에 통지. 거부 — rollback 시 살아있는 index artifact 파괴.

## ADR-024 — Phase 3G.2/3G.3/3G.4 sharded snapshot · delta cache · eviction

**날짜**: 2026-05-28

**결정(3G.2 — sharded object snapshot/warmup)**: 단일 immutable artifact set(`.tids` + `.shards` + N `.sNNN.cagra`)을 GCS에 snapshot한다. `CuvsManifest`에 `shard_count` + `.shards` 파일의 sha256/size를 추가하고, per-shard `.cagra` 무결성은 GCS manifest가 아니라 **load 시점에 `.shards` manifest의 `artifact_crc32`로 검증**(fail-closed)한다. `cuvs_objstore_upload_sharded`가 build 후 detached로 업로드, `cuvs_objstore_download`가 manifest의 `shard_count`로 분기해 전 shard를 받고 `.tids`/`.shards` SHA를 검증한 뒤 atomically 기록한다. warmup은 기존 atomic `load_index_sharded`(전 shard deserialize 성공 후에만 등록) 덕에 partial-hot이 query에 노출되지 않는다. corrupt/missing shard·heap relfilenode mismatch는 fail-closed. `.delta`/`.tombstone`/`.stale`은 snapshot에서 제외(파생/휘발 상태).

**결정(3G.3 — shard-aware GPU delta cache)**: 글로벌 `.delta`(논리 index당 하나)를 데몬 GPU brute-force cache로 올려 sharded fanout merge에 합친다. delta cache는 단일 GPU에 있어야 하므로 sharded는 **shard 0의 GPU**(`delta_gpu_of`)에 둔다. base shard fanout은 lock-free로 두고, delta refresh+search+merge는 re-lock 구간에서 수행(작아서 직렬화 비용 무시 가능). delta cache가 살아있으면 `delta_merged=1`로 데몬이 GPU 병합, 없으면(VRAM 부족/corrupt/generation mismatch) `delta_merged=0`으로 **backend CPU delta merge fallback** 유지 → 항상 정합.

**결정(3G.4 — sharded whole-unit eviction)**: sharded logical index를 VRAM pressure에서 **whole-unit으로 evict**한다(`free_index_shards`로 전 shard + `.tids` + delta cache 해제, `save_index` 스킵 — 모든 artifact가 이미 durable). `find_lru_index`/`evict_lru`가 sharded entry(어느 shard든 해당 device에 있으면)를 후보로 삼되, **`IndexEntry.inflight>0`이면 제외**한다. 3G-1 lock-free fanout은 snapshot 직전 `inflight++`, re-lock 직후 `inflight--`로 보호 — eviction이 in-flight search의 shard handle을 free하지 못한다(ADR-022가 예고한 inflight 재도입). dirty/pending 비durable 상태가 없으므로(전부 디스크) 추가 fail-closed 체크 불요.

**결과**: `CuvsManifest` 확장 + `cuvs_objstore_upload_sharded`; `delta_gpu_of` + sharded fanout delta 병합; `IndexEntry.inflight` + sharded-aware LRU. 검증: 단일 GPU integration Scenario 22(sharded delta=gpu, CPU exact 일치)·23(sharded whole-unit eviction + manifest reload). 3G.2의 GCS transfer round-trip은 bucket이 없어 자동 검증 불가(3C/3D와 동일 상태) — compile + no-regression + download 후 load 경로(Scenario 19)로 커버.

**대안**: delta cache를 shard별로 두기. 거부 — `.delta`는 글로벌 generation(전체 `.tids` CRC) 기준이라 논리 index당 하나가 자연스럽고, shard별 분할은 TID 매핑/정합 복잡도만 키운다. eviction에 deferred-free(cond-wait). 거부 — inflight>0 victim을 단순 skip하면 충분(eviction은 best-effort)하고 cond-wait의 entry-이동 위험을 피한다.

---

## ADR-025 — 50M×384 Competitive Benchmark 결과 및 pg_cuvs 포지셔닝

**날짜**: 2026-05-30~31

**결과 요약** (bench/results/competitive.csv, design/BENCHMARK_CROSSOVER.md §12):

| engine | build_s | p50_us | QPS | recall@10 |
|---|---|---|---|---|
| diskann (2GB cache) | TIMEOUT (>5h) | NA | NA | NA |
| hnsw (64GB) | 21,879 | 12,985 | 546 | 미측정 |
| vchordrq (8192 lists) | 5,784 | 49,101 | 152 | **0.9991** (probes=5) |
| cagra (shard=4) | FAILED | NA | NA | NA |

**CAGRA 실패 원인**: 50M×384×float32 = 73.24 GiB 원시 데이터 + 빌드 워크스페이스가 2×A100-40GB (80 GB VRAM)을 초과. shard_count=2(25M/shard=38.4 GB) 및 shard_count=4 모두 실패. A100-80GB ×2 이상 필요.

**발견한 버그**: `pg_cuvs_server` 서비스 파일에 `--max-vram-mb 128` (128 MB 제한) 설정이 있었음. 40000으로 수정 (commit `bench/` 관련 커밋 참조).

**GT 버그**: `gt_faiss.py --regen`의 GBATCH=1M이 `load_binary.py --batch 50000`과 다른 RNG 시퀀스를 생성 → recall=0. GBATCH=50000으로 수정 (commit `6a74863`).

**결정**: pg_cuvs는 **GPU hot tier** 포지셔닝으로 고정한다.

```text
N <~ 10K                     → pgvector HNSW (CPU, latency 압도)
N >~ 100K or dim >= 1536     → pg_cuvs CAGRA (latency 8-12×, build 9-36×)
50M×384 (float32)            → pg_cuvs 하드웨어 한계 (A100-40GB×2 부족)
N = 50M, CPU only            → HNSW 실용적(p50=13ms, QPS=546),
                                vchordrq 빌드 빠르나 search 느림(p50=49ms, QPS=152)
DiskANN 2GB cache @ 50M      → 실용 불가 (캐시 0.9%에서 full)
```

**참조**: `BENCHMARK_CROSSOVER.md` §11(1M pilot), §12(50M competitive).

---

## ADR-026 — Phase 3B go/no-go: GPU hot tier only 포지셔닝

**날짜**: 2026-05-31

**배경**: Phase 3B spike 결과 (design/PHASE_3B_DESIGN_NOTES.md):
- cuVS Vamana in-memory: product path 아님 (upstream API 불안정)
- cuVS Vamana → PQFlash disk: cuVS 26.04 기준 NO-GO
- native NVMe cold tier: MS DiskANN native path 또는 자체 serializer 필요, 개발 비용 큼

**50M competitive benchmark 추가 근거**:
- DiskANN은 50M×2GB에서 완전히 실패 — NVMe cold tier의 실용 스케일은 1B+ 이상
- 50M 수준은 HNSW가 CPU로 충분히 커버 (p50=13ms)
- DiskANN이 필요한 순간은 RAM에도 들어가지 않는 1B+ 규모 (현재 target 밖)

**결정**:
- **3B 구현 중단**: 새 기능 추가 없음
- **포지셔닝 확정**: pg_cuvs = "GPU VRAM에 들어가는 hot vector workload" 전담
- **문서화로 마무리**: PHASE_3B_DESIGN_NOTES.md에 go/no-go 결론 명시 (별도 작업)
- **추후 재검토 조건**: VRAM 128GB+ 환경에서 100M+ 스케일 요구가 생길 때

**대안**: 3B 계속 구현. 거부 — 현재 가치 입증과 제품화가 우선이며, 1B+ 스케일 수요가 확인되기 전에 NVMe cold tier에 투자하는 것은 시기상조.

---

## ADR-027 — 다음 우선순위: Cost model → Ops → Release

**날짜**: 2026-05-31

**배경**: Phase 3A-G 기능 구현 완료. 남은 핵심 가치는 "언제 쓰면 좋은지", "planner가 맞게 고르는지", "남이 설치 가능한지"에 있다.

**결정**: 신규 기능 추가 금지. 다음 순서로 집중한다.

```
1. Competitive baseline 마무리 (BENCHMARK_CROSSOVER.md Cell B 완료, Cell C 선택적)
2. Cost model recalibration — benchmark 결과 → planner 파라미터 연결
   반영 대상: N, dim, cuvs.k, shard_count, parallel_fanout, shard_overfetch,
              delta rows, warm/cold/reload state, CPU baseline crossover
3. Phase 3B go/no-go 문서화 (ADR-026, 별도 doc)
4. 3H-full operational playbooks — sizing guide, when-to-use, GCS, runbook
5. Release hardening — clean install, compat matrix, known limitations, README
```

**근거**:
- 현재 benchmark 결과를 cost model에 연결하지 않으면 planner가 틀린 선택을 할 수 있음
- 설치 재현성이 없으면 외부 사용자가 pg_cuvs를 평가할 수 없음
- 기능 > 제품화 순서는 현재 단계에서 역전

**Cell C (1M×384 pgvectorscale/VectorChord Pareto sweep) 처리**:
- §11에 1M×384 HNSW vs CAGRA가 이미 있음
- pgvectorscale/VectorChord 추가는 선택적 — cost model 입력에 필요한 경우에만 진행
- 지금 당장 필수 아님

---

## ADR-028 — Cost model calibration: 실측 기반 상수 검증

**날짜**: 2026-05-31

**배경**: ADR-027에서 "benchmark 결과를 cost model에 연결"을 우선순위로 설정.
`cuvsamcostestimate()`의 세 상수(STARTUP/K/ROWS)가 실측 latency와 정합하는지 검증.

**실측 데이터** (bench/results/pilot.csv, dim=384, k=10):

| N | CAGRA p50 | HNSW p50 | seqscan 추정 | planner 결정 |
|---|---|---|---|---|
| 1K | 871us | 224us | ~100us | seqscan [OK] |
| 10K | 1146us | 865us | ~800us | CAGRA (tied) |
| 100K | 1228us | 8232us | ~7700us | CAGRA 6.3x [OK] |
| 1M | 1196us | 13700us | ~77000us | CAGRA 64x [OK] |

**Planner crossover 계산** (dim=384, seq_page_cost=1.0):

```
seqscan_cost ~ N * (16 + dim*4) / 8192 = N * 0.189   (dim=384)
CAGRA_cost   = 1000 + 0.5*k + 0.00001*N = 1005 + tiny
Crossover N  ~ 1005 / 0.189 ~ 5300 rows
```

즉, planner는 N > ~5300에서 CAGRA를 선택. 실측에서 N=10K 이상에서 CAGRA가 seqscan보다 빠름 — 모델이 보수적으로 5K로 설정하는 것은 적절 (N=1K-5K에서 CAGRA IPC overhead가 seqscan보다 큼).

**상수 검증 결과**:

- **STARTUP=1000**: cold CUDA init ~100ms (0.1ms/unit 기준) 모델링. warm-path IPC ~0.5ms이지만 planner가 warm/cold를 구분할 수 없음. 보수적 선택이 올바름.
- **K_COST=0.5**: k=10 기준 5 units — STARTUP 대비 무시 가능. 구조 유지.
- **ROWS_COST=0.00001**: N=1K→1M에서 p50이 871us→1196us(1.37x)로 거의 N-독립. 0.00001×N = N=1M에서 10 units (STARTUP 대비 1% 수준). 올바름.

**dim 스케일링**: dim=384→1536에서 CAGRA p50이 1228us→1605us(1.31x)로 mild. 반면 seqscan은 row 크기가 4x → pages 4x → cost 4x로 자동 반영됨. 별도 dim term 불필요.

**결정**: 상수값 변경 없음. 실측과 정합함이 확인됨. 주석 블록을 calibration rationale로 갱신 (`src/pg_cuvs.c` cost model 섹션).

**대안**: STARTUP을 warm-path 기준(~50)으로 낮춤. 거부 — cold-start 첫 쿼리에서 planner가 CAGRA를 잘못 선택하게 되며, N=1K-5K에서 GPU overhead가 seqscan보다 큰 구간을 커버 못 함.

---

## ADR-029 — Phase 3I 포지셔닝: GPU Build Accelerator for pgvector HNSW

**날짜**: 2026-06-01

**배경**: Phase 3I에서 `pg_cuvs_import_hnsw(cagra_oid, hnsw_oid)`를 구현. GPU로 CAGRA를 빌드하고 pgvector HNSW 포맷으로 변환 후 임포트.

**실측 (2026-06-01, A100-40GB, dim=384)**:

| N | GPU total (CAGRA+import) | pgvector native | speedup |
|---|--------------------------|-----------------|---------|
| 10K | 0.5s | 1.3s | 2.6x |
| 100K | 4.7s | 74.7s | **15.8x** |
| 1M | 66.3s | 918.3s | **13.9x** |

**결정**: Phase 3I를 "GPU Build Accelerator" 포지션으로 확정.

운영 가정:
- 온프렘/프라이빗 RAG 배포에서는 embedding model serving, reranker, batch embedding을 위해
  이미 GPU 서버 또는 GPU 리소스 풀이 존재하는 경우가 많다.
- 해당 GPU는 벡터 DB와 같은 랙/클러스터에 있거나, 최소한 대용량 embedding 배치와 같은
  데이터 경로 가까이에 배치될 가능성이 높다.
- 이 환경에서 GPU를 검색 서빙에 상시 묶어두는 대신, 시간이 많이 드는 인덱스 빌드/재빌드
  작업에만 빌려 쓰고, 검색은 기존 PostgreSQL/pgvector HNSW 경로로 유지하는 것이
  운영상 훨씬 낮은 도입 장벽을 가진다.

포지셔닝:
- GPU VRAM 충분: pg_cuvs CAGRA GPU 서치 (기존 경로)
- VRAM 부족 또는 GPU-less 환경: **CAGRA build + import_hnsw → pgvector HNSW 서치**
- CPU-only 환경 + 속도 불필요: pgvector HNSW native

제품 메시지:
- pg_cuvs는 pgvector를 대체하지 않고, pgvector HNSW 인덱스 빌드를 GPU로 가속하는
  선택지를 제공한다.
- 애플리케이션 쿼리, PostgreSQL 권한/백업/복구, pgvector HNSW 검색 경로는 그대로 둔다.
- GPU 장애 또는 GPU 리소스 회수는 검색 서빙 장애로 직결되지 않는다. GPU는 build job의
  가속 자원이며, 결과물은 CPU에서 서빙 가능한 pgvector HNSW index다.
- embedding serving GPU와 vector indexing GPU를 같은 리소스 풀에서 스케줄링할 수 있어,
  야간/배치 reindex 같은 운영 패턴에 잘 맞는다.

**안전성 (ADR-029 확인 범위)**:
- WAL crash-safe: `log_newpage_buffer` full-page image
- 재시작 후 정상 동작 검증 (bench/test_3i_restart.sh)
- VACUUM / REINDEX 정상 동작 확인
- 타겟 검증: non-HNSW 거부, dim/metric mismatch 거부
- pgvector 버전: 0.5.0+ (HNSW_VERSION=1, stable 2023-08~)

**한계**:
- pgvector HNSW 페이지 포맷 하드코딩 → pgvector major-version 변경 시 업데이트 필요
- "offline import only" — import 중 동시 쿼리 차단 (ExclusiveLock)
- 대형 데이터(1B+)에서 import 시간 미측정

---

## ADR-030 — MIG(Multi-Instance GPU) 지원: 검증 완료

**날짜**: 2026-06-01

**배경**: A100에서 MIG로 GPU를 분할하면 MIG 인스턴스가 별도 CUDA device로 노출된다. pg_cuvs 코드 변경 없이 동작하는지 검증.

**검증 결과 (bench/test_mig.sh, 2026-06-01)**:

| 시나리오 | 설정 | 결과 |
|---|---|---|
| 단일 MIG (3g.20gb=20GB) | shard_count=1, N=50K | **PASS** — CAGRA build+search 정상 |
| 멀티 MIG (1g.5gb×3=5GB each) | shard_count=3, N=30K | **PASS** — 3개 MIG device에 sharding 정상 |

**핵심 발견**:
- MIG 인스턴스가 별도 CUDA device로 노출되므로 `CUDA_VISIBLE_DEVICES=MIG-uuid` 설정만으로 동작
- `cuvs_detect_gpus()`가 MIG 인스턴스를 올바르게 열거 (각 5GB/20GB VRAM 인식)
- `cuvs.shard_count=3`으로 3개 MIG 인스턴스에 분산 → 결과 정합성 확인

**운영 참고**:
- GCP A100 VM에서 MIG 활성화는 reboot 필요 (nvidia-smi --gpu-reset 미지원)
- 활성화 순서: `--setup` (pending + reboot) → `--test` → `--teardown` (reboot)
- MIG로 GPU를 세분화해 여러 pg_cuvs 인스턴스가 하나의 물리 GPU를 공유 가능

**결정**: pg_cuvs는 MIG를 별도 코드 없이 지원. 운영 가이드에 MIG 설정 예시 추가 예정.

---

## ADR-031 — GPU 자원 파라미터 튜닝: 실측 가이드

**날짜**: 2026-06-01

**배경**: `max_vram_mb`, `shard_count`, `cuvs_k`, `parallel_fanout` 파라미터가 성능에 미치는 영향을 실측. VM: A100-40GB × 2, N=100K, dim=384, k=10, GPU=1.

**실측 결과 (bench/results/gpu_resources_bench.csv)**:

| 파라미터 | 값 | p50 (us) | recall@10 | 핵심 발견 |
|---|---|---|---|---|
| max_vram_mb | 40000 (기본) | 1214 | 0.792 | 기준선 |
| max_vram_mb | 2048 (제한) | 1215 | 0.802 | 100K×384≈750MB → 2GB 한도 미초과, 성능 동일 |
| shard_count | 1 | 1224 | 0.816 | 단일 GPU |
| shard_count | 2 | 2075 | 0.924 | shard merge 오버헤드 +70% latency, recall +13% |
| cuvs_k | 10 | 1207 | 0.620 | candidate 부족 → recall 저하 |
| cuvs_k | 100 | 1333 | 0.798 | 기본값, 균형 |
| cuvs_k | 200 | 1352 | 0.916 | latency +12%, recall +15% → 고정밀 요건에 적합 |
| parallel_fanout | 0 (sequential) | 1936 | 0.924 | N=100K 소규모 shard: sequential이 더 빠름 |
| parallel_fanout | 1 (parallel) | 2298 | 0.922 | 스레딩 오버헤드 > 병렬 이득 (소규모) |

**결정 가이드**:
- `max_vram_mb`: 인덱스(corpus + graph ≈ 4×corpus)가 한도 이내면 성능 영향 없음
- `shard_count=2`: latency +70%를 감수할 때만 사용 (VRAM 초과 시 자동 사용)
- `cuvs_k=200`: 높은 recall이 필요한 워크로드에 권장 (latency 비용 소)
- `parallel_fanout=0`: 소규모 인덱스(N<1M)에서 sequential이 효율적

**주의**: `parallel_fanout=1`이 유리한 구간은 shard당 N이 충분히 커서 스레딩 오버헤드를 상쇄할 때 (N>5M 예상). 소규모에서는 default off를 고려.

---

## ADR-031b — import_hnsw AccessExclusiveLock 수정 (correctness)

**날짜**: 2026-06-01  
**상태**: 결정됨 (버그 수정)

### 문제

`pg_cuvs_import_hnsw`가 target HNSW를 `ExclusiveLock`으로 열었다. `ExclusiveLock`은 `AccessShareLock`(인덱스 스캔이 잡는 락)과 충돌하지 않으므로, truncate + 페이지 재작성 도중 concurrent SELECT가 반쯤 지워진 페이지를 읽을 수 있었다.

### 결정

`index_open(hnsw_oid, AccessExclusiveLock)`으로 변경. REINDEX와 동일한 수준으로 import 완료까지 모든 concurrent access를 블록.

### 영향

- import는 offline DDL (이미 문서화)
- 기존 테스트의 `WARNING: you don't own a lock of type ExclusiveLock` 제거됨
- 변경 파일: `src/hnsw_export.c`

---

## ADR-032 — HNSW 사이드카 직렬화 조건부화

**날짜**: 2026-06-01  
**상태**: 결정됨

### 배경

`from_cagra()` CPU 직렬화(~30s @ 1M×1024)가 모든 CAGRA 빌드에 무조건 실행됐다.
CAGRA-search만 사용하는 사용자도 30s 패널티를 받는다.

### 결정

BUILD IPC 프레임에 `use_cpu_hnsw` 플래그를 추가하고, 데몬은 이 값이 1일 때만 `.hnsw` 사이드카를 직렬화한다. 백엔드는 `cuvs_cpu_hnsw_fallback` GUC 값을 전달한다.

### 영향

- CAGRA-only 빌드: 사이드카 생략 → ~30s 절감
- 3I import 경로: `SET cuvs.cpu_hnsw_fallback = on` 후 CREATE INDEX 필요
- **버그 수정 포함**: `cuvs_ipc_build()`에 `use_cpu_hnsw` 파라미터 미전달 버그 수정 (BUILD 명령이 항상 0 전달 → 사이드카 미생성)
- 변경 파일: `src/cuvs_ipc.c`, `src/cuvs_ipc.h`, `src/pg_cuvs.c`, `src/pg_cuvs_server.c`

---

## ADR-033 — UNLOGGED 인덱스 타겟 지원

**날짜**: 2026-06-01  
**상태**: 결정됨

### 배경

`pg_cuvs_import_hnsw`는 페이지마다 `log_newpage_buffer(buf, true)`를 호출해 full-page WAL을 기록한다. 1M×1024 인덱스(~8GB) 기준 WAL 기록이 ~28s를 차지한다.

### 결정

타겟 HNSW 인덱스의 `relpersistence`를 syscache에서 확인하고, UNLOGGED(`RELPERSISTENCE_UNLOGGED`)이면 `log_newpage_buffer` 호출을 생략한다. UNLOGGED 시 NOTICE 메시지 출력.

### 트레이드오프

- LOGGED (기본): crash-safe, WAL 기록, ~57s
- UNLOGGED: crash 시 인덱스 손실, WAL 없음, ~28s
- 기본 경로 동작 완전 보존 (opt-in)
- 변경 파일: `src/hnsw_export.c`, `sql/pg_cuvs--0.1.0.sql`

---

## ADR-036 — Phase 3J: pg_cuvs_import_cagra (직접 변환)

**날짜**: 2026-06-01  
**상태**: 결정됨 + 실측 완료

### 배경

`pg_cuvs_import_hnsw`는 hnswlib 중간 포맷(`.hnsw` 파일)을 경유한다:
```
CAGRA → from_cagra() (~30s) → .hnsw 파일 → 파싱 → pgvector pages (~57s)
```
`from_cagra()`를 제거하고 CAGRA adjacency list를 IPC로 직접 pgvector 페이지로 변환한다.

### 결정

새 SQL 함수 `pg_cuvs_import_cagra(cagra_oid, hnsw_oid)`:
- 새 IPC 명령 `CUVS_OP_EXPORT_ADJACENCY`: 데몬이 GPU adjacency + 벡터 → shared memory
- flat pgvector HNSW (all nodes level 0) 직접 작성
- `cpu_hnsw_fallback=on` 불필요, `.hnsw` 파일 불필요

### 실측 결과 (Cohere 1024d, N=1M, A100-40GB, 2026-06-01)

| 경로 | CAGRA build | import | 합계 | recall@10 |
|------|-------------|--------|------|-----------|
| import_cagra (direct) | 55.7s | 63.3s | **119.0s** | 0.9963 |
| import_hnsw (hnswlib) | 83.4s | 57.3s | **140.7s** | 0.9962 |

**전체 1.18× 빠름**. recall/QPS 동일 — flat HNSW가 multi-level과 동등한 품질.

### 트레이드오프

- flat HNSW: 계층 없음 → 동일 recall을 위해 ef_search를 높여야 할 수 있음
- import 단계 자체는 IPC 4GB 전송 오버헤드로 6s 느림
- 전체 speedup은 from_cagra() 제거분(28s)에서 옴

### 선택 가이드

| 요구사항 | 권장 |
|----------|------|
| 가장 빠른 경로 (crash unsafe) | import_cagra + UNLOGGED (~96s) |
| crash-safe + 빠름 | import_cagra + LOGGED (~119s) |
| multi-level HNSW 계층 보장 | import_hnsw (~140s) |

---

## ADR-034 — CAGRA 빌드 PostgreSQL 오버헤드 감소 로드맵

**날짜**: 2026-06-02  
**상태**: 계획

### 배경

pg_cuvs CAGRA 빌드는 동일 데이터를 cuVS lib 직접 사용할 때보다 ~45s 느리다.
(cuVS lib: ~10s / pg_cuvs: ~55s, N=1M×1024)

원인 분석:
- PostgreSQL heap scan + varlena decode: ~15-20s
- malloc 버퍼 누적 (realloc 패턴): ~5s
- heap 버퍼 → shm memcpy: ~5-10s (4GB 이중 복사)
- daemon mmap + GPU upload: ~3-5s
- CAGRA GPU build: ~10s

### 개선 방향 (우선순위 순)

| 방향 | 절감 예상 | 난이도 | 변경 범위 |
|------|-----------|--------|-----------|
| **double memcpy 제거** | ~2-5s | 낮음 | `pg_cuvs.c` accumulation buffer를 shm에 직접 할당 |
| **parallel maintenance workers** | ~10-20s | 중간 | `ambuild()` parallel scan API 활용; IPC는 단일 집계 유지 |
| **Streaming/pipeline** | ~10-15s | 높음 | 청크 단위 IPC + GPU transfer 겹치기; cuVS incremental API 필요 |
| **heap scan 자체** | ~15-25s | 높음 | `table_index_build_scan()` 커스터마이징 |
| **이진 벡터 저장** | ~5-10s | 높음 | varlena 외부 저장, 스키마 변경 필요 |

### 결정

단기(double memcpy + parallel workers)만 구현. Streaming/pipeline은 cuVS public API 변화를 모니터링 후 재평가.

---

## ADR-035 — import_hnsw 페이지 write 병목 감소 로드맵

**날짜**: 2026-06-02  
**상태**: 계획

### 배경

`pg_cuvs_import_hnsw` / `pg_cuvs_import_cagra`의 pgvector 페이지 write 단계가 ~52-63s.
원인: `ReadBuffer(P_NEW)` + `PageInit` + `PageAddItem×2` + `MarkBufferDirty` + `log_newpage_buffer`를 1M번 순차 실행.

### 현재 적용된 개선

- **UNLOGGED 타겟** (ADR-033): WAL 생략 → ~28s (LOGGED ~57s 대비 50% 절감)
- **Phase 3J direct path** (ADR 미정): `from_cagra()` 제거 → 전체 ~22s 절감

### 추가 개선 방향

| 방향 | 절감 예상 | 난이도 | 변경 범위 |
|------|-----------|--------|-----------|
| **병렬 페이지 write** | ~15-25s | 높음 | PG parallel worker로 페이지 범위 분할; buffer manager 조율 복잡 |
| **Bulk WAL** | ~10-15s | 높음 | 범위 단위 WAL 레코드; PG WAL internals 수정 필요 |
| **UNLOGGED + 주기적 REINDEX** | 0 (이미 가능) | 낮음 | import → UNLOGGED; crash 허용 window 후 REINDEX로 LOGGED 전환 |

### UNLOGGED + REINDEX 패턴 (지금 사용 가능)

```sql
-- 빠른 import (WAL 없음)
CREATE UNLOGGED INDEX t_hnsw ON t USING hnsw (embedding vector_l2_ops);
SELECT pg_cuvs_import_cagra('t_cagra'::regclass, 't_hnsw'::regclass);

-- maintenance window 후 WAL-safe LOGGED로 전환
REINDEX INDEX t_hnsw;  -- pgvector 재빌드, LOGGED
```

### 결정

병렬 페이지 write와 Bulk WAL은 PG internals 의존도가 높아 단기 구현 대상 제외.
UNLOGGED + REINDEX 패턴을 OPS_GPU_PLAYBOOK에 추가하여 운영 패턴으로 권장.
