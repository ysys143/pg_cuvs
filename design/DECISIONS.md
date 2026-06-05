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

새 SQL 함수 `pg_cuvs_import_cagra(cagra_oid, hnsw_oid)`를 `pg_cuvs_import_hnsw`와 동등한 옵션으로 제공:
- 새 IPC 명령 `CUVS_OP_EXPORT_ADJACENCY`: 데몬이 GPU adjacency + 벡터 → shared memory
- CAGRA 그래프를 pgvector HNSW 포맷에 **NSW(flat)** 구조로 직접 기록
  - 모든 노드 level 0, HNSW 계층 없음
  - Level 0 neighbor 수: graph_degree(64~128), import_hnsw의 2M(32)보다 많음
- `cpu_hnsw_fallback=on` 불필요, `.hnsw` 파일 불필요
- 선택 기준: N <= 5M + 빠른 빌드 = import_cagra / N > 10M + HNSW 보장 = import_hnsw

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

**날짜**: 2026-06-02 (2026-06-03 구현 설계 보강)
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

### 4A-1 double memcpy 제거: 구현 설계

현재 데이터 경로: `cuvs_build_callback()`(pg_cuvs.c:943)이 매 tuple마다 `memcpy`로 heap memory의 flat buffer에 복사 → scan 완료 후 `cuvs_ipc_build()`(cuvs_ipc.c:536)가 `shm_write_build_payload()`(cuvs_ipc.c:130)를 호출 → `shm_open` + `mmap` + `memcpy` x 2(cuvs_ipc.c:162-163)로 POSIX shm 세그먼트에 이중 복사. 즉 동일 데이터가 heap buffer → shm으로 한 번 더 복사된다.

변경 대상 함수:
- `grow_build_buffers()`(pg_cuvs.c:877): 현재 `realloc(bs->vectors, ...)` + `realloc(bs->tids, ...)`로 heap memory를 키운다. 변경 후에는 POSIX shm 세그먼트 위에서 `ftruncate`로 확장한다.
- `cuvs_ambuild()`(pg_cuvs.c:957): scan 시작 전에 `shm_open` + 초기 `ftruncate` + `mmap`을 수행하고, `CuvsBuildState`에 shm fd와 mmap base를 저장한다.
- `shm_write_build_payload()`(cuvs_ipc.c:130): `shm_open` + `mmap` + `memcpy` x 2를 제거하고, `cuvs_ambuild`가 이미 만든 shm 이름만 IPC frame에 전달한다.

shm 크기 결정: `cuvs_ambuild()`의 preflight 단계(pg_cuvs.c:966-995)가 `reltuples * (dim * sizeof(float) + sizeof(ItemPointerData))`로 corpus 크기를 추정한다. 이 추정치를 shm 초기 크기로 사용한다. 실제 live tuple 수가 추정치를 초과하면 `grow_build_buffers()`에서 `ftruncate`로 shm을 확장하고 `mremap`(Linux) 또는 `munmap` + 재`mmap`으로 매핑을 갱신한다. `cuvs.max_build_mem_mb`가 설정되어 있으면 이 값이 상한이다.

실패 처리: `shm_open` 또는 `ftruncate` 실패 시 기존 heap 경로(realloc + 이중 memcpy)로 degraded fallback한다. WARNING 로그를 남기고, fallback 사실을 `pg_stat_gpu_search`에서 관측 가능하게 한다. 데이터 경로가 바뀌지 않으므로 정합성 영향 없음.

### 4A-2 parallel maintenance workers: 구현 설계

현재 `cuvs_ambuild()`는 `table_index_build_scan(heapRel, indexRel, indexInfo, true, true, cuvs_build_callback, &bs, NULL)`(pg_cuvs.c:998)로 단일 프로세스 순차 스캔을 수행한다. 마지막 인자 `NULL`이 `ParallelTableScanDesc`이고, 이를 넘기면 병렬 스캔이 된다.

API 선택: PostgreSQL은 `table_index_build_scan()`에 `ParallelTableScanDesc`를 넘기는 방식과 `table_parallel_index_build_scan()`을 사용하는 방식을 모두 지원한다. 어떤 방식이 pg_cuvs의 "scan 완료 후 한 번 merge" 패턴에 맞는지는 구현 시점에 PG 버전별 API를 조사해 결정한다. 현시점에서 고정하지 않는다.

worker 구조: 각 worker가 독립적인 partial buffer(vectors + tids)를 heap memory(또는 4A-1 완료 후 shm)에 누적한다. scan 완료 후 leader가 partial buffer들을 단일 연속 buffer로 merge한다. merge는 단순 memcpy 연접이므로 O(N) — 정렬이 필요 없다(CAGRA build는 벡터 순서에 의존하지 않는다). IPC는 merge된 단일 buffer를 기존 경로로 전달한다.

thread safety: 현재 `CuvsBuildState`는 단일 프로세스 전제로 설계되어 있다(`bs->vectors`, `bs->tids`, `bs->n_vecs`를 lock 없이 갱신). parallel workers 도입 시 worker별 독립 `CuvsBuildState`를 만들고, leader가 scan 완료 후 합산한다. 공유 상태가 없으므로 lock이 불필요하다.

GUC: PostgreSQL 표준 `max_parallel_maintenance_workers` GUC를 읽어 worker 수를 결정한다. pg_cuvs 자체 GUC를 추가하지 않는다 — pgvector HNSW parallel build, btree parallel build 등 다른 AM도 이 GUC를 사용하므로 운영자에게 일관된 표면을 제공한다. `max_parallel_maintenance_workers = 0`이면 기존 단일 프로세스 경로를 그대로 탄다.

AM handler: `amcanparallel`을 등록하지 않는다. `amcanparallel`은 parallel index scan(검색 병렬화)용 콜백이고, 여기서 다루는 것은 build 병렬화다. PostgreSQL build 병렬화는 AM handler 콜백이 아니라 `ambuild()` 내부에서 `table_index_build_scan`의 parallel 인자를 사용하는 방식이다(btree `_bt_parallel_scan_and_sort` 참조). `amestimateparallelscan`, `aminitparallelscan`, `amparallelrescan`은 scan 병렬화 전용이므로 등록 불필요.

### 결과

- 4A-1(double memcpy 제거): heap scan 중 벡터가 shm에 직접 쌓이므로 scan 완료 후 `shm_write_build_payload`의 memcpy x 2가 제거된다. daemon 측 `handle_build`의 `shm_open` + `mmap` 경로는 변경 없음.
- 4A-2(parallel workers): N=1M에서 heap scan ~15-20s 구간이 worker 수에 비례해 줄어든다. IPC 이후 경로(daemon, GPU build)는 변경 없음. `max_parallel_maintenance_workers = 0`이면 기존 동작과 byte-identical.
- 단기 목표: 55s → 4A-1 후 ~50s → 4A-2 후 ~30-35s.

### 대안

- shm 대신 `palloc`으로 PG shared memory에 직접 할당. 거부 — PG shared_buffers는 크기가 고정이고 4GB 벡터 데이터를 담기에 부적합. POSIX shm은 프로세스간 공유가 목적이고 크기 제한이 시스템 메모리 한도.
- worker별 partial buffer 대신 single shared buffer + atomic counter. 거부 — `CuvsBuildState` lock-free 접근이 복잡하고, worker별 독립 buffer + 완료 후 merge가 더 단순하며 btree parallel build 선례와 일치.
- `cuvs.build_parallel_workers` 자체 GUC 추가. 보류 — `max_parallel_maintenance_workers`가 PostgreSQL 표준이고 다른 AM과 일관. pg_cuvs 고유 요구가 생기면 그때 추가.
- streaming/pipeline으로 한 번에 해결. 거부 — cuVS에 incremental build API가 없어 전체 corpus를 한 번에 넘겨야 한다. scan과 GPU transfer를 겹치려면 cuVS API 변경이 필요.

### heap scan + varlena decode 병목 후속 분석

2026-06-03 추가. ADR-034 테이블의 "heap scan 자체"(~15-25s)와 "이진 벡터 저장"(~5-10s)에 대한 상세 분석이다. 이 두 항목은 PG 오버헤드 45s 중 가장 큰 단일 구간(~15-20s, 33-44%)을 차지하지만 "높음/장기"로만 분류되어 왜 어려운지, 어떤 접근이 가능한지 기록되지 않았다.

**병목의 정확한 원인**

pgvector의 `vector` 타입은 varlena다. dim=1024면 벡터 하나가 `4 bytes(vl_len_) + 2 bytes(dim) + 2 bytes(unused) + 1024 * 4 bytes(float) = 4104 bytes`로, PostgreSQL TOAST threshold(기본 ~2KB, `TOAST_TUPLE_THRESHOLD`)를 초과한다. pgvector는 `vector` 컬럼에 `EXTENDED` storage strategy를 기본 적용하므로, 거의 모든 row의 벡터가 TOAST 테이블에 별도 저장된다.

`cuvs_build_callback()`(pg_cuvs.c:919)이 매 tuple마다 수행하는 작업:
1. `DatumGetPgVector(values[0])` → `PG_DETOAST_DATUM(d)` 호출(pg_cuvs.c:71). TOAST된 벡터를 `palloc` + LZ compression 해제(또는 외부 TOAST chunk fetch + reassemble)로 메인 메모리에 복원한다.
2. `vec->dim` 읽기 + dimension mismatch 검사(pg_cuvs.c:934-935).
3. `memcpy(dst, vec->x, dim * sizeof(float))`(pg_cuvs.c:943)로 flat buffer에 복사. dim=1024면 per-vector 4KB.
4. 콜백 반환 후 detoast된 palloc 메모리는 per-tuple memory context reset으로 해제.

오버헤드 구성(추정):
- TOAST decompression + palloc/pfree: 전체의 ~50-60%. 1M회 palloc(4KB) + pglz decompress가 주 비용.
- heap page read + visibility check(`table_index_build_scan` 내부 `heapam_index_build_range_scan`): 전체의 ~25-30%. sequential scan이라 I/O 자체는 OS page cache 히트가 대부분이지만, per-tuple `HeapTupleSatisfiesVacuum` 호출 + tuple header 파싱이 누적.
- per-vector memcpy(4KB x 1M = 4GB): 전체의 ~10-15%. L2 cache miss가 누적.

**검토한 가속 방안**

(a) PLAIN storage strategy 강제: `ALTER TABLE t ALTER COLUMN embedding SET STORAGE PLAIN`으로 벡터를 TOAST하지 않게 한다. detoast 비용(palloc + decompress)이 완전히 제거되므로 scan 구간에서 ~50-60% 절감(~8-12s)을 기대할 수 있다.

난점: tuple이 커지면(4KB+) 페이지당 row 수가 1-2개로 급감한다. heap 자체가 ~4x 커지고, sequential scan의 page 수도 비례해 증가한다. pgvector가 `EXTENDED`를 기본으로 쓰는 이유는 대부분의 쿼리(WHERE 조건, JOIN, projection)에서 벡터 전체를 읽지 않아도 되기 때문이다. PLAIN으로 바꾸면 벡터를 참조하지 않는 쿼리도 큰 tuple을 fetch하게 되어 일반 workload 성능이 저하된다. 또한 기존 pgvector 사용자의 스키마 변경이 필요하다.

실현 가능성: pg_cuvs가 강제할 수 없고 사용자 선택이다. 빌드 성능 최적화 팁으로 가이드 문서에 안내하는 것은 가능하다. "벡터 빌드 전용 테이블에서만 PLAIN을 쓰고, 서빙 테이블은 EXTENDED를 유지하라"는 패턴이 될 수 있다.

(b) 별도 binary column / side table: 벡터를 `bytea` PLAIN으로 별도 컬럼 또는 side table에 저장하고, `cuvs_ambuild`가 이 컬럼을 참조한다.

난점: 사용자 스키마 변경을 강제한다. pgvector `vector` 타입과의 이중 관리가 필요하다. pg_cuvs가 pgvector operator class를 재사용하는 현재 설계(ADR-006)와 충돌한다 — AM이 index의 opclass에 등록된 컬럼이 아닌 다른 컬럼을 읽으려면 `ambuild` 내부에서 별도 heap scan을 해야 하고, 이는 표준 `table_index_build_scan` 경로를 벗어난다. DX 저하가 크다.

실현 가능성: 낮음. 아키텍처 변경이 필요하고, 편익 대비 사용자 부담이 크다.

(c) pgvector 측 fixed-length storage 기여: 벡터 차원이 테이블 생성 시 고정이면(예: `vector(1024)`), varlena가 아닌 fixed-length 타입으로 저장할 수 있다. TOAST를 회피하면서도 PG가 타입 크기를 미리 알 수 있어 storage 최적화가 가능하다.

난점: pgvector 업스트림 변경에 의존한다. pgvector의 설계 철학은 가변 차원 지원이고, `vector` 타입이 varlena인 것은 의도적 선택이다(동일 테이블에 다른 차원의 벡터를 저장하지는 않지만, 타입 시스템 수준에서 고정 길이를 강제하지 않는다). pgvector에 이런 변경을 기여하려면 업스트림 수용 가능성을 먼저 확인해야 하며, pg_cuvs 자체 로드맵으로 제어할 수 없다.

실현 가능성: pg_cuvs 단독으로 불가. pgvector 커뮤니티와의 협의가 전제.

(d) `table_index_build_scan` 커스터마이징: 표준 `table_index_build_scan` 대신 raw page scan + 직접 tuple 파싱으로 visibility check 오버헤드를 줄인다.

난점: `HeapTupleSatisfiesVacuum`과 동등한 MVCC visibility 판정을 직접 구현해야 한다. 이를 잘못 구현하면 committed/aborted/in-progress tuple을 잘못 포함/제외해 인덱스 정합성이 깨진다. PG 버전마다 visibility 로직이 달라질 수 있어 유지보수 비용이 높다. `table_index_build_scan`은 HOT chain 처리, snapshot 관리, progress reporting 등도 포함하고 있어 대체하기 어렵다.

실현 가능성: 매우 낮음. MVCC 정합성 리스크가 성능 이득을 정당화하지 못한다.

(e) prefetch / streaming detoast: scan 루프에서 현재 tuple을 처리하는 동안 다음 N개 tuple의 TOAST chunk를 비동기로 prefetch한다.

난점: PostgreSQL에 tuple-level asynchronous detoast API가 없다. `PG_DETOAST_DATUM`은 동기 호출이다. PG의 buffer prefetch 힌트(`PrefetchBuffer`)는 heap page 수준이지 TOAST chunk 수준이 아니다. palloc은 thread-safe가 아니라 per-backend이므로, 별도 스레드에서 detoast를 수행할 수 없다. PG 코어 변경 없이는 불가능하다.

실현 가능성: 불가. PG 코어 API가 없다.

(f) `PrefetchBuffer()` heap page prefetch: PG 내장 API `PrefetchBuffer()`로 다음에 읽을 heap 페이지를 OS에 미리 요청해 I/O wait를 감소시킨다. `table_index_build_scan()` 루프 안에서 현재 tuple 처리 중 다음 N개 페이지를 prefetch하는 방식이다.

난점: `table_index_build_scan()`은 콜백 방식(`IndexBuildCallback`)이라, 콜백 안에서 "다음에 읽을 페이지 번호"를 알 수 없다. scan의 page iteration은 `table_index_build_scan` 내부(`heapam_index_build_range_scan`)가 제어하며, 콜백은 이미 fetch된 tuple만 받는다. prefetch를 삽입하려면 커스텀 scan 루프로 교체해야 하는데, 이는 (d)의 raw page scan 커스터마이징과 동일한 MVCC 정합성 리스크를 수반한다. 또한 `table_index_build_scan`은 sequential scan이므로 OS readahead(`/sys/block/*/queue/read_ahead_kb`, 일반적으로 128-256KB)가 이미 동작한다. heap 페이지가 shared_buffers에 이미 있거나 OS page cache에 있는 경우가 대부분이라 `PrefetchBuffer()`의 추가 I/O 이득이 제한적이다. 6/1 세션에서 검토 후 "PG 코어 인터페이스 복잡도 대비 이득 제한적"으로 기각했다.

실현 가능성: 낮음. 콜백 인터페이스 교체가 필요하고, sequential scan의 OS readahead와 중복된다.

**결론**

단기에 heap scan + varlena decode 자체를 의미 있게 가속할 실현 가능한 방안은 없다. parallel workers(4A-2)로 wall-clock을 분산하는 것이 현실적 대안이다.

PLAIN storage는 사용자가 선택적으로 적용할 수 있으므로, `docs/playbooks/` 또는 OPS_GPU_PLAYBOOK에 "빌드 성능 최적화 팁"으로 안내한다. 내용: "dim >= 512이고 빌드 빈도가 높은 경우, 벡터 컬럼에 `SET STORAGE PLAIN`을 적용하면 TOAST decompression 비용을 제거해 빌드 시간을 ~25-35% 줄일 수 있다. 단, 벡터를 참조하지 않는 일반 쿼리 성능이 저하될 수 있으므로 빌드 전용 테이블 또는 빌드 빈도가 높은 환경에서만 권장한다."

장기적으로는 pgvector의 fixed-length storage 지원 여부를 모니터링하고, PG 코어의 TOAST prefetch/streaming API 발전을 추적한다. 어느 쪽이든 pg_cuvs 단독으로 제어할 수 없는 외부 의존이다.

---

## ADR-035 — import_hnsw 페이지 write 병목 감소 로드맵

**날짜**: 2026-06-02 (2026-06-03 거부 근거 보강)
**상태**: 계획 (병렬 page write / Bulk WAL은 단기 제외)

### 배경

`pg_cuvs_import_hnsw` / `pg_cuvs_import_cagra`의 pgvector 페이지 write 단계가 ~52-63s.
원인: `ReadBuffer(P_NEW)` + `PageInit` + `PageAddItem×2` + `MarkBufferDirty` + `log_newpage_buffer`를 1M번 순차 실행.

### 현재 적용된 개선

- **UNLOGGED 타겟** (ADR-033): WAL 생략 → ~28s (LOGGED ~57s 대비 50% 절감)
- **Phase 3J direct path** (ADR-036): `from_cagra()` 제거 → 전체 ~22s 절감

### 추가 개선 방향

| 방향 | 절감 예상 | 난이도 | 변경 범위 |
|------|-----------|--------|-----------|
| **병렬 페이지 write** | ~15-25s | 높음 | PG parallel worker로 페이지 범위 분할; buffer manager 조율 복잡 |
| **Bulk WAL** | ~10-15s | 높음 | 범위 단위 WAL 레코드; PG WAL internals 수정 필요 |
| **UNLOGGED + 주기적 REINDEX** | 0 (이미 가능) | 낮음 | import → UNLOGGED; crash 허용 window 후 REINDEX로 LOGGED 전환 |

### 병렬 page write 거부 근거

`ReadBuffer(rel, P_NEW)`는 relation의 현재 끝에 한 블록을 순차 할당한다. 여러 worker가 동시에 `P_NEW`를 호출하면 buffer manager의 relation extension lock(`RelationExtensionLockWaiterCount`)에서 직렬화되므로, parallel worker를 써도 블록 할당 자체는 순차 병목이 남는다.

pgvector HNSW 페이지의 `nextblkno`(다음 element page 포인터, `PgvHnswPageOpaque` 필드)는 N-1번째 페이지의 opaque에 N번째 블록 번호를 기록하는 cross-page 의존성이 있다. 페이지 범위를 worker별로 분할하면 범위 경계의 `nextblkno` 링크를 사후에 patch해야 하고, 이 patch 자체가 추가 `ReadBuffer(BUFFER_LOCK_EXCLUSIVE)` + `MarkBufferDirty` + WAL을 요구한다.

WAL 레코드 순서도 문제다. `log_newpage_buffer`는 호출 순서대로 WAL LSN을 발급받는데, 여러 worker가 비순차적으로 WAL을 쓰면 crash recovery 시 페이지 replay 순서가 논리적 블록 순서와 달라진다. full-page image 방식이라 replay 순서가 결과에 영향을 주지는 않지만, WAL volume spike와 checkpoint 부하가 커질 수 있다.

이런 이유로 PG 코어에서도 HNSW/GIN 같은 linked-page 인덱스의 build를 병렬화하지 않는다(btree parallel build는 sort 단계를 병렬화하고 page write는 단일 프로세스). pg_cuvs가 이를 독자적으로 구현하려면 PG buffer manager internals에 깊이 의존하게 되어 PG 버전 간 호환 비용이 높다.

### Bulk WAL 거부 근거

PostgreSQL 15에서 `log_newpage_range(rel, forknum, startblk, endblk)`가 추가되었다(src/backend/access/transam/xloginsert.c). 이 API는 지정 블록 범위의 full-page image를 한 번에 WAL에 기록해 per-page `log_newpage_buffer` 호출 오버헤드를 줄인다. pg_cuvs target은 PG16+이므로 API 자체는 사용 가능하다.

쓰지 않는 이유: `log_newpage_range`는 이미 디스크에 기록된 블록 범위를 WAL로 복사하는 API다. 현재 pg_cuvs import 경로는 `ReadBuffer(P_NEW)` → buffer pool에 페이지를 구성 → `MarkBufferDirty` → `log_newpage_buffer` → `UnlockReleaseBuffer`의 순서로 buffer manager를 통해 쓴다. `log_newpage_range`를 쓰려면 전체 페이지를 buffer manager 밖에서(`smgrextend` 또는 direct file write) 먼저 디스크에 기록한 뒤 범위 단위로 WAL을 남기는 패턴이 필요하다. 이는 현재 buffer-manager-through 경로를 완전히 바꾸는 것이고, pgvector의 build path와도 달라져 호환성 검증 비용이 높다.

대안으로 buffer manager를 유지하면서 페이지를 일정 범위(예: 1000개)씩 모아 한 번에 WAL을 쓰는 batched 방식을 생각할 수 있지만, 이는 `log_newpage_range`가 아니라 별도 WAL 레코드 타입을 정의해야 하므로 PG extension이 할 수 있는 범위를 넘는다.

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

### 결과

- 단기 개선은 ADR-033(UNLOGGED ~28s)과 ADR-036(from_cagra 제거 ~22s 절감)으로 닫혔다.
- LOGGED 경로의 추가 개선(병렬 page write, Bulk WAL)은 PG internals 발전을 모니터링하다 재검토한다. 재검토 조건: PG 코어에 인덱스 build용 bulk page write API가 추가되거나, `ReadBuffer` concurrent extension 제약이 완화되는 경우.
- UNLOGGED + REINDEX 패턴이 현재 가장 빠른 실용 경로(~28s)이며 OPS_GPU_PLAYBOOK에 문서화됐다.

### 대안

- buffer manager를 우회해 `smgrextend`로 직접 디스크에 쓰고 `log_newpage_range`로 WAL. 거부 — pgvector 페이지 포맷 재현 + buffer manager 우회는 PG 버전 간 호환 리스크가 크고, crash 시 buffer pool과 디스크 상태 불일치 위험이 있다.
- 현재 per-page `log_newpage_buffer`를 유지하되 WAL compression(`wal_compression = on`)으로 I/O 절감. 보류 — import 페이지는 entropy가 높은 float 벡터라 압축률이 낮을 수 있음. 별도 실측이 필요하며, 전역 설정이라 pg_cuvs만을 위해 켜기 어려움.
- import를 포기하고 pgvector native build에 맡김. 거부 — native build 285s vs import 28s(UNLOGGED). 현재 개선 없이도 10x 이상 빠르다.

---

## ADR-037 — Phase 3J: pg_cuvs_build_hnsw() 통합 API + INDEX_CREATE_SKIP_BUILD

**날짜**: 2026-06-02
**상태**: 결정됨 + 구현 완료 (테스트 7/7 PASS)

### 배경

Phase 3I 구현 중 근본적인 문제 발견: `pg_cuvs_import_hnsw(cagra_oid, hnsw_oid)` 사용 패턴에서 타겟 HNSW 인덱스를 미리 `CREATE INDEX USING hnsw`로 만들어야 했고, 이때 pgvector가 CPU로 전체 HNSW 빌드를 수행했다(1M×1024 기준 ~285s). GPU 가속을 쓰더라도 285s CPU 빌드를 먼저 지불하는 구조였다.

추가로 `pg_cuvs_import_hnsw`(hnswlib 경유)와 `pg_cuvs_import_cagra`(직접 변환) 두 함수가 같은 목적으로 혼재해 API 표면이 혼란스러웠다.

### 결정

**1. INDEX_CREATE_SKIP_BUILD 활용**: `create_empty_hnsw()` 내부에서 `index_create()`를 `INDEX_CREATE_SKIP_BUILD` 플래그로 호출해 pgvector `ambuild()` 호출을 건너뛴다. HNSW 인덱스 catalog entry만 생성하고 실제 그래프 채우기는 pg_cuvs가 담당한다.

**2. 통합 API**: `pg_cuvs_build_hnsw(cagra_oid regclass, mode text DEFAULT 'nsw') RETURNS regclass`로 두 함수를 통합. 내부적으로 `create_empty_hnsw()` 후 mode에 따라 static C helper를 호출한다. SQL-등록 없는 내부 helper로 리팩터링해 SPI 불필요.

**3. 4-way 모드**:

| 모드 | 방식 | 권장 |
|------|------|------|
| `nsw` (기본) | CAGRA adjacency 직접 변환, flat NSW | 권장 |
| `hnsw` | heuristic neighbor selection으로 계층 구성 | 연구용 |
| `hnswlib` | from_cagra() via /dev/shm | 권장 |
| `hnswlib_file` | from_cagra() via disk | 연구용 |

**4. Sequence counter**: `hnsw_build_seq` atomic counter로 동일 CAGRA에서 여러 번 호출 시 인덱스 이름 충돌 방지 (`pg_cuvs_hnsw_{cagra_oid}_{seq}` 형식).

### 4-way 벤치마크 결과 (Cohere 1024d, N=1M, A100-40GB, 2026-06-02, ADR-037 Phase 3J)

| 모드 | 합계 | speedup vs native 285s |
|------|------|------------------------|
| nsw (기본, 권장) | 117s | **2.4×** |
| hnsw | 144s | 2.0× |
| hnswlib (권장) | 139s | 2.0× |
| hnswlib_file | 151s | 1.9× |
| pgvector native | 285s | 1.0× |

### ef-recall pareto (Cohere 1024d, N=500K, A100-40GB, 2026-06-02, bench/results/ef_recall_sweep.csv)

| ef_search | nsw | hnsw | hnswlib | QPS(nsw/hnsw/hnswlib at ef=80) |
|-----------|-----|------|---------|-------------------------------|
| 10 | 0.740 | 0.720 | 0.733 | — |
| 20 | 0.841 | 0.833 | 0.845 | — |
| 40 | 0.923 | 0.920 | 0.925 | — |
| 80 | 0.969 | 0.968 | 0.967 | 81 / 81 / 83 QPS |
| 160 | 0.986 | 0.984 | 0.984 | — |
| 320 | 0.997 | 0.997 | 0.997 | — |

**핵심 발견**: 세 모드의 recall curve가 사실상 동일하다. nsw(계층 없음)가 hnswlib(진짜 HNSW 계층)보다 ef_search를 더 높여야 한다는 우려는 실증적으로 틀렸다. CAGRA 그래프의 높은 품질이 계층 부재를 상쇄한다. nsw의 빌드 속도 우위(5.73× vs native)는 recall 패널티 없이 유지된다. → nsw 권장 default 유지 근거 확인.

### 영향

- 기존 `pg_cuvs_import_hnsw`, `pg_cuvs_import_cagra` SQL 함수 제거
- SQL 표면: `pg_cuvs_build_hnsw(cagra_oid, mode DEFAULT 'nsw')` 하나
- `cuvs.cpu_hnsw_fallback=on` 불필요 (nsw 모드)
- 테스트 스위트 재구성: `smoke cpu_fallback edge_cases cpu_hnsw_fallback build_hnsw build_hnsw_edge metrics` 7/7 PASS

### 대안

- 기존 두 함수 유지. 거부 — 285s CPU 빌드 페널티가 GPU 가속 가치를 반감시킨다.
- `CREATE INDEX USING cagra WITH (export_hnsw=on)` 옵션으로 자동화. 보류 — 사용자가 명시적으로 변환 시점을 제어하는 것이 운영상 더 직관적. CAGRA 인덱스와 HNSW 인덱스를 독립적으로 관리 가능.

---

## ADR-040 — Phase 3M: 배치 검색 API

**날짜**: 2026-06-04
**상태**: 구현 완료 (2026-06-05, VM installcheck 10/10)

### 배경

현재 IPC는 per-query 단일 요청 구조다. 각 backend가 쿼리 벡터 하나를 UDS로 데몬에 보내고, 데몬이 K개 결과를 반환한다. cuVS CAGRA/BF search API는 이미 Q>1 쿼리 행렬을 단일 GPU dispatch로 처리하지만, pg_cuvs IPC가 Q=1만 지원해 이 기능을 활용하지 못하고 있다.

배치 검색이 유용한 시나리오:
- 벤치마크 / GT 생성: 수천 개 쿼리를 순차 단일 요청으로 보내면 IPC 왕복 수천 회 + GPU kernel launch 수천 회 발생. 단일 배치 dispatch로 대폭 절감.
- RAG 파이프라인: 한 요청에서 여러 청크 임베딩을 동시에 검색하는 패턴.
- GPU brute force(Phase 3L): Q=1 latency가 bandwidth-bound라 Q를 묶을수록 throughput이 선형 향상됨.

### 결정

`pg_cuvs_batch_search` SQL 함수를 추가한다.

```sql
SELECT * FROM pg_cuvs_batch_search(
    'items'::regclass,          -- 대상 테이블
    ARRAY[q1, q2, q3]::vector[], -- 쿼리 벡터 배열
    k := 10                     -- 각 쿼리당 반환할 결과 수
);
-- 반환: TABLE(query_idx int, ctid tid, distance float4)
```

내부적으로 Q개 쿼리를 단일 IPC 요청으로 데몬에 전송하고, 데몬이 Q×K 결과를 한 번의 GPU dispatch로 반환한다.

### IPC 변경

현재 `CuvsCmdFrame`은 단일 쿼리 벡터를 포함한다. 배치 요청은 shm을 통해 Q×dim float matrix를 전송하고, reply도 Q×K (tid, distance) 행렬을 shm으로 반환한다.

- request shm payload: `[Q: uint32][dim: uint32][q0_vec...q_{Q-1}_vec]` (Q×dim float32)
- reply shm payload: `[Q: uint32][K: uint32][tid_{0,0}...tid_{Q-1,K-1}][dist_{0,0}...dist_{Q-1,K-1}]` (Q×K × 12 bytes)
- `CUVS_OP_SEARCH_BATCH` opcode 추가. 기존 `CUVS_OP_SEARCH`(Q=1)는 변경 없이 유지.

shm 크기: Q×dim×4 (input) + Q×K×12 (output). Q=1000, dim=1024, K=10 기준 4MB input + 120KB output.

### SQL 인터페이스

`pg_cuvs_batch_search`는 set-returning function(SRF)으로 구현한다.
- `query_idx`: 0-based 쿼리 인덱스 (입력 배열 순서와 동일)
- `ctid`: heap TID (PostgreSQL heap recheck 대상)
- `distance`: metric-dependent distance (L2/cosine/IP)

heap recheck / MVCC visibility는 함수 내부에서 `heap_fetch`로 처리한다. 결과는 `query_idx` 기준으로 정렬하지 않는다(GPU 반환 순서 유지).

### 적용 범위

- CAGRA mode: cuVS `cagra::search`가 이미 Q×dim 행렬 입력을 지원하므로 데몬 변경이 최소화된다.
- BF mode(Phase 3L): `cuvs::neighbors::brute_force::search`도 Q×dim 행렬 입력을 지원한다. 배칭 효과가 더 크다(bandwidth-bound).
- sharded index(Phase 3F/3G): shard별 fanout에서 Q×dim을 그대로 전달하고, shard별 Q×K 결과를 global top-K로 merge한다.

### 대안

- 기존 단일 쿼리 API를 반복 호출하는 클라이언트 측 배치. 거부 — IPC 왕복 Q회 + GPU kernel launch Q회가 누적된다. Q=1000이면 UDS 왕복만 ~500ms.
- PostgreSQL `LATERAL` + 배열 unnest로 배치 효과. 거부 — backend가 row마다 별도 index scan을 열어 IPC가 Q회 발생한다.
- 데몬 내부 마이크로배칭(`cuvs.bf_batch_wait_us`, ADR-039). 이는 보완적 기능이다 — 마이크로배칭은 단일 쿼리 API를 쓰는 클라이언트들을 투명하게 묶고, 배치 API는 클라이언트가 명시적으로 Q개를 한 번에 전송하는 경로다.

### 구현 현황 (2026-06-05)

- `CUVS_OP_SEARCH_BATCH` opcode 구현. request shm: Q×dim float32, reply shm: Q×K (tid, distance).
- `pg_cuvs_batch_search(rel regclass, queries vector[], k int) RETURNS TABLE(query_idx int, ctid tid, distance real)` SRF 완성.
- CAGRA mode, BF mode(ADR-039), sharded index(3F/3G) 모두 지원. 데몬이 단일 GPU dispatch로 Q개 쿼리를 처리하고 global top-K merge(sharded) 또는 direct Q×K 반환(unsharded).
- **MVCC**: 함수 내부에서 heap_fetch를 하지 않고 raw ctid + 데몬 distance를 반환한다(단일 쿼리 AM scan이 xs_recheck=true로 두는 것과 동일 의미). caller가 `JOIN ... ON t.ctid = b.ctid`로 visibility를 처리 → 단일 쿼리 API와 byte-identical top-K 보장. 결과는 `query_idx` 기준 반환(정렬 안 함, GPU 반환 순서 유지).
- **reply 전송**: Q×K가 클 수 있어 inline UDS 대신 데몬-할당 reply shm(키는 `CuvsReplyHeader.error[]`, K는 `delta_merged`)을 사용 — `handle_export_adjacency`의 reply-shm 메커니즘 재사용.
- 주의: cuVS L2 distance는 SQUARED(pgvector `<->`의 sqrt와 다름). ranking은 동일하므로 거리값이 아닌 ctid(JOIN)로 비교.
- installcheck 10/10 PASS(`pg_cuvs_batch` 신규 테스트 포함). 각 query_idx의 top-K가 단일 쿼리 API와 일치(L2/cosine, sharded, BF mode).

---

## ADR-039 — Phase 3L: GPU Brute Force 검색 모드 사용자 노출

**날짜**: 2026-06-04
**상태**: 구현 완료 (2026-06-05, VM installcheck 10/10)

### 배경

`cuvs_brute_force_search` / `CuvsBfIndex`는 Phase 3G.3(shard-aware delta cache)에서 이미 구현됐다. 현재는 daemon 내부에서 `.delta` rows를 GPU brute-force 인덱스로 올려 CAGRA 결과와 merge하는 용도로만 쓰인다. 사용자가 직접 GPU exact search를 요청할 방법은 없다.

GPU brute force를 독립 검색 경로로 노출하면 다음 시나리오가 가능하다:
- recall=1.0이 필요한 소규모 정확도 요구 workload (N < ~1M, VRAM이 전체 벡터를 수용)
- 벤치마크 ground truth 생성 (seqscan 대비 GPU exact가 훨씬 빠름)
- CAGRA recall이 충분하지 않을 때 exact search로 전환하는 운영 knob

### 설계 원칙: 연산의 Locality

GPU brute force 성능을 좌우하는 단일 원칙은 **연산의 locality**다. matmul → L2 거리 계산 → top-K reduction 전 과정이 GPU 메모리 계층(HBM → L2 cache → register)을 벗어나지 않아야 한다.

이 원칙이 깨지는 순간: Q×N 중간 행렬(N=1M, Q=100 기준 400MB)을 CPU로 꺼내면 GPU→CPU bandwidth가 병목이 되어, GPU가 연산에 쓴 시간보다 전송 시간이 커진다. cuvs-silicon 실험에서 MPS matmul(GPU) + CPU top-K 조합이 순수 CPU(AMX cblas_sgemm)보다 느렸던 이유가 이것이다.

이 원칙이 구현 결정 전반을 관통한다:
- **IPC reply**: Q×K만 전송(K=10이면 8KB). Q×N은 절대 shm 경유 금지.
- **float16**: GPU HBM bandwidth를 두 배로 쓰는 것이 아니라, 같은 bandwidth로 두 배 더 많은 벡터를 GPU 내부에서 처리.
- **d_norms 캐시**: `||d||²`를 GPU HBM에 상주시켜 매 search마다 N번의 GPU 내 재계산 제거.
- **마이크로배칭**: Q=1을 Q=N으로 묶어 GPU 연산 locality를 높이고 kernel launch 오버헤드를 amortize.

### 결정

`cuvs.search_mode` GUC(문자열, default `'cagra'`)를 추가한다.

```sql
SET cuvs.search_mode = 'brute_force';  -- GPU exact search
SET cuvs.search_mode = 'cagra';        -- 기본 ANN (default)
```

`USING cagra` AM을 그대로 유지하고, search 경로에서 GUC 값에 따라 분기한다. 새 AM을 추가하지 않는다 — DDL 표면이 늘어나고 index 관리가 복잡해지는 데 비해 이득이 없다.

### 구현

**빌드 타임**: `CREATE INDEX USING cagra` 시 raw vector matrix를 `.vectors` sidecar로 직렬화한다 (`<db>_<idx>.vectors`, row-major + `.tids`와 동일한 versioned header). precision은 `cuvs.bf_precision` GUC(`'float32'` default / `'float16'`)로 결정한다. 기존 `.cagra`와 함께 fsync+rename으로 durable하게 저장.

**데몬 로드**: startup 시 `.vectors` sidecar가 존재하면 `CuvsBfIndex`를 별도로 로드하고 `IndexEntry.main_bf_idx`에 보관한다. `.vectors`가 없으면 BF 검색 불가(요청 시 ERROR). 로드 시 `cuVS brute_force::build`가 내부적으로 norm(`||d||²`)을 캐시하는지 확인하고, 미지원 시 `IndexEntry.bf_norms` GPU tensor를 별도 계산해 보관한다(N=1M 기준 ~10ms, 이후 매 search마다 절감).

**검색 경로**: IPC search request에 `search_mode` 필드를 추가한다. 데몬은 `search_mode == BF`이면 `main_bf_idx`로 `cuvs_brute_force_search`를 호출하고, `== CAGRA`이면 기존 경로를 유지한다. IPC reply payload는 Q×K indices + distances만 포함한다(Q×N 중간 행렬을 절대 shm 경유하지 않음 — N=1M, Q=100 기준 중간 행렬 400MB vs reply 8KB).

**마이크로배칭**: `cuvs.bf_batch_wait_us` GUC(기본 0 = 비활성)를 추가한다. 활성화 시 데몬이 지정 시간(권장 100-1000μs) 동안 BF 요청을 수집한 뒤 단일 Q=N GPU dispatch로 묶는다. Q=1이 Q=100 대비 ~19배 느린 bandwidth 특성상, 동시 요청이 많은 workload에서 throughput이 선형에 가깝게 향상된다. CAGRA 경로와 코드를 공유하지 않고 BF 전용 배칭 루프로 분리한다.

**VRAM 영향**: `.vectors` sidecar는 CAGRA 그래프와 별도로 VRAM을 사용한다. float32 기준 1M×384=1.5GB, 1M×1024=4.0GB. float16 선택 시 절반. `pg_stat_gpu_cache`에 `bf_vram_bytes`, `bf_precision` 컬럼을 노출한다.

**sharding**: `cuvs.shard_count >= 2`인 경우 각 shard의 벡터 범위를 shard별 `CuvsBfIndex`에 나눠 올리고, 기존 shard fanout 경로에서 `cuvs_brute_force_search`를 호출한다.

### 성능 특성 (A100-40GB 기준, cuvs-silicon 실험 외삽)

| N×dim | Q=1 레이턴시 | Q=100 QPS | VRAM(f32) | VRAM(f16) |
|-------|-------------|-----------|-----------|-----------|
| 1M×384 | ~0.8ms | ~125,000 | 1.5 GB | 0.75 GB |
| 1M×1024 | ~2ms | ~50,000 | 4.0 GB | 2.0 GB |
| 1M×1536 | ~3ms | ~33,000 | 6.0 GB | 3.0 GB |

A100 HBM2e 대역폭(2TB/s) 기준 이론 하한값이다. Q=1 latency는 bandwidth-bound이므로 배칭 없이는 dim에 선형 비례한다. 참고: `docs/bruteforce-acceleration-lessons.md`.

### cost model

`cuvsamcostestimate`에 `search_mode='brute_force'` 분기를 추가한다. startup_cost는 VRAM bandwidth 기반으로 추정한다:

```c
/* A100 기준: N * dim * sizeof(float) / 2e12 * 1e6 (microseconds) */
double bf_latency_us = (N * dim * 4.0) / 2e6;
startup_cost = bf_latency_us;  /* cost unit = microseconds */
```

N이 작을수록 CAGRA ANN보다 저렴할 수 있으므로 planner가 자동 선택할 수 있게 한다. `cuvs.search_mode='cagra'`(default)이면 기존 cost model을 유지한다.

### 대안

- 새 `USING cagra_bf` AM 등록. 거부 — 별도 AM은 DDL 표면과 index catalog entry가 늘어나고, 기존 CAGRA 인프라(IPC, daemon, stats)를 거의 그대로 복제해야 한다. GUC 분기가 훨씬 단순하다.
- CAGRA 그래프에서 역으로 벡터를 복원해 BF 수행. 거부 — CAGRA 그래프는 neighbor 연결이지 raw vector store가 아니다. 정확한 복원이 불가능하다.
- seqscan으로 GT 생성. 가능하지만 N=1M 기준 수십 초 이상 소요. GPU BF는 수백 ms로 동일 정확도를 제공한다.
- Q×N 중간 행렬을 shm 경유 전송. 거부 — N=1M, Q=100 기준 400MB 전송이 발생해 GPU-CPU bandwidth가 병목이 된다. reply는 Q×K (K=10이면 8KB)만 전송해야 한다.

### 구현 현황 (2026-06-05)

- `.vectors` sidecar 직렬화: `CuvsVectorsHeader` (magic 'VECS', version, n_vecs, dim, metric, body_crc32, base_tids_crc32 생성 토큰). 정상 시 fsync+rename으로 durable.
- daemon 로드: `.vectors` 존재하면 `IndexEntry.main_bf_idx` (`CuvsBfIndex`)로 lazy build/로드. 손상/누락 시 BF 요청에 ERROR.
- `cuvs.search_mode` GUC (`'cagra'` default / `'brute_force'`). IPC frame에 분기, daemon이 `cuvs_brute_force_search` 호출.
- `cuvs.bf_precision` GUC (`'float32'` default / `'float16'`). cuVS index<half, float> 사용 → VRAM 절반, recall exact 유지.
- sharded BF: shard별 `.vectors` range를 shard별 `CuvsBfIndex`로 올려 기존 fanout 경로에서 BF search.
- **마이크로배칭** (`cuvs.bf_batch_wait_us`, 기본 0=비활성): **단일 전용 워커 스레드** 설계(leader election 회피). producer(연결 스레드)는 `g_index_mutex` 해제 후 요청을 enqueue하고 자신의 `done`에서 대기, 워커는 윈도우 동안 누적된 요청을 (db,index,precision,dim) 키로 그룹화해 그룹마다 `g_index_mutex` 보유 중 단일 `cuvs_bf_search_batch`로 dispatch(eviction-safe) 후 producer를 깨운다. 락 순서 index→bf로 데드락 없음. `pg_stat_gpu_search.bf_batch_count`(coalesced dispatch 수).
- 통계/cost: `pg_stat_gpu_search.search_mode='brute_force'`, `pg_stat_gpu_cache.bf_vram_mb`/`bf_precision`, `cuvsamcostestimate` BF 분기(N-scaled).
- installcheck 10/10 PASS(`brute_force` 신규 테스트: L2/cosine recall@10=1.0, float16, sharded, bf_batch_wait_us 케이스(recall 안전망 + bf_batch_count 증가)). grouping 코어 유닛 테스트(`make test-unit`). 32-concurrent 무데드락 bench(`bench/bf_microbatch_concurrency.sh`).

**구현 발견 (pre-existing, 마이크로배칭과 무관)**: planner는 GPU 인덱스 cost가 seqscan보다 낮을 때만 cagra 인덱스로 라우팅한다. 별도 단발 연결의 GPU 쿼리는 seqscan으로 fallback할 수 있고(이는 CAGRA에도 동일하게 적용되는 기존 cost 동작), 그 경우 데몬에 도달하지 않아 마이크로배칭이 engage하지 않는다. 따라서 동시 coalescing의 throughput 이득은 주로 **warm long-lived 세션**에서 실현된다. 단일 세션 GPU 경로(installcheck)에서는 정상 동작. (이 발견은 이후 **ADR-045**에서 근본 원인 규명 — 세션-로컬 `cuvs.index_dir` GUC vs cross-backend 아티팩트 — 및 `index_dir` reloption으로 수정됨.)

---

## ADR-038 — Phase 3K: pg_cuvs_build_hnsw() 함수 호출을 CREATE INDEX ... USING pg_cuvs_hnsw DDL 문법으로 전환

**날짜**: 2026-06-03
**상태**: 결정됨

### 배경

현재 CAGRA 기반 HNSW 빌드는 `SELECT pg_cuvs_build_hnsw('my_cagra'::regclass, 'nsw')` 형태의 SQL 함수 호출이다. 이 방식은 동작하지만, PostgreSQL의 인덱스 관리 표준 경로(DDL, 카탈로그, 덤프/복원)와 분리되어 있다.

### 결정

`pg_cuvs_hnsw`라는 커스텀 Access Method를 등록하고, 표준 DDL 문법으로 전환한다.

```sql
-- 기존
SELECT pg_cuvs_build_hnsw('my_cagra'::regclass, 'nsw');

-- 변경
CREATE INDEX my_idx ON items USING pg_cuvs_hnsw (embedding vector_l2_ops)
  WITH (source = 'my_cagra', mode = 'nsw');
```

`pg_cuvs_hnsw` AM의 `ambuild()` 내부에서 현재 `pg_cuvs_build_hnsw()`의 로직을 수행한다: source CAGRA 인덱스 찾기 -> `INDEX_CREATE_SKIP_BUILD`로 pgvector HNSW 껍데기 생성 -> CAGRA 그래프를 pgvector 페이지 포맷으로 변환.

### 장점

- `WITH` 절로 파라미터 전달 (mode, ef_construction 등)
- `DROP INDEX` / `REINDEX`가 자연스럽게 동작
- `pg_indexes` 카탈로그 뷰에 정상 노출
- `pg_dump` / `pg_restore` 자동 지원

### pgvector 호환성

- pgvector 내부 페이지 포맷 의존은 버전 고정 + 가이드 명시로 관리
- 지원 범위를 README/가이드에 명확히 표기 (예: pgvector >= 0.7.0)
- CI에서 호환 매트릭스 테스트

### 대안

- 함수 호출 방식 유지. 거부 — pg_dump/restore에서 인덱스가 누락되고, DROP INDEX/REINDEX가 동작하지 않으며, pg_indexes 카탈로그에 노출되지 않아 운영 표면이 PostgreSQL 표준과 괴리된다.
- `CREATE INDEX USING cagra WITH (export_hnsw=on)` 빌드 타임 옵션. 보류 — CAGRA 인덱스와 HNSW 인덱스의 lifecycle이 결합되며, ADR-037에서 이미 독립 관리가 운영상 더 직관적이라고 결정했다.

---

## ADR-041 — Phase 3K: pg_cuvs_hnsw의 source를 선택적으로 (heap에서 ephemeral CAGRA 빌드)

**날짜**: 2026-06-04
**상태**: 결정됨 (구현/VM 검증 완료, installcheck 8/8)

### 배경

ADR-038/3K로 `CREATE INDEX ... USING pg_cuvs_hnsw WITH (source = 'my_cagra')`가 동작하지만, source CAGRA 인덱스를 먼저 만들어야 하고(2단계), 변환 후 보통 `DROP INDEX my_cagra`로 정리한다. 더 중요하게 `REINDEX`가 저장된 `source` relopt를 다시 찾으므로, source CAGRA가 살아 있어야만 재빌드된다(source를 DROP했으면 REINDEX 불가).

### 결정

`source`를 선택적으로 만든다. 생략하면 `ambuild`가 heap을 직접 스캔해 **ephemeral CAGRA를 GPU에서 빌드 → pgvector HNSW 페이지로 변환 → 임시 CAGRA를 drop**한다. 한 DDL로 완결되고, `REINDEX`는 heap에서 self-contained하게 재빌드된다.

- **source 명시**: 기존 CAGRA를 재사용(빌드 절약 + 그 CAGRA를 GPU 검색용으로도 보존). 한 번의 GPU 빌드로 CAGRA + HNSW 두 인덱스를 얻는다.
- **source 생략**: ephemeral CAGRA. CAGRA는 HNSW를 만드는 중간 빌드 산물(`.o`와 유사)이므로 변환 후 정리한다. CAGRA를 보존하려면 source 모드를 쓴다.

구현:
- `cuvs_build_cagra_from_heap` 헬퍼를 `cuvs_ambuild`(USING cagra)와 source-less 경로가 공유(corpus 스캔 + `cuvs_ipc_build`). (Tidy First 구조 추출, 동작 불변.)
- source-less: `self(my_hnsw) oid`를 키로 ephemeral CAGRA 빌드(`shard_count=1` — sharded는 adjacency export 불가), `PG_TRY/PG_FINALLY`로 변환 후 **성공/실패 무관하게 `cuvs_ipc_drop`**.
- `cuvs_index_metric`을 opfamily-OID `static` 캐시 비교에서 **opclass-family 이름 기반**(`cuvs_metric_from_opclass_name`)으로 변경 — cagra/pg_cuvs_hnsw 두 AM이 한 세션에 쓰여도 metric이 정확하다. (이전 캐시는 첫 호출 AM의 opfamily에 고정돼, 두 번째 AM의 cosine/ip를 "unrecognized → L2"로 오인했다.)

### 안전성

- **abort zombie 방지**: ephemeral CAGRA를 `ambuild` 내부 `PG_FINALLY`로 항상 drop하므로, ambuild 종료 시 데몬에 임시 CAGRA가 없다. 이후 CREATE INDEX가 abort돼도(인덱스 relfilenode 롤백) zombie가 남지 않는다. `object_access_hook`은 CREATE 실패를 통지하지 못하고 명시적 cagra DROP만 감지하므로(ADR-023), 이 ambuild-내부 drop이 필수다.
- VM 검증: source-less 빌드/조회/`REINDEX`(source 없이)/cosine metric 정확/`leftover_ephemeral_cagra = 0`(데몬 registry zombie 없음) 확인, installcheck 8/8.

### 대안

- source 필수 유지. 거부 — 2단계 + 정리가 번거롭고, REINDEX가 source 생존에 의존한다.
- `WITH (keep_cagra=true)` 보존 옵션. 거부 — "데몬에만 CAGRA 남기기"는 가리키는 `USING cagra` 카탈로그 인덱스가 없어 검색에 못 쓰고 DROP 시 정리도 안 되는 유령이 되며, "CAGRA 카탈로그 인덱스 동시 생성"은 source 모드와 결과가 같아 중복이다. CAGRA 보존이 필요하면 `USING cagra` + source 모드를 쓴다(ADR-038이 보류한 `export_hnsw=on`과 같은 이유).

## ADR-045 — 인덱스 self-describing: `index_dir` reloption (cross-session seqscan 폴백 근절)

**날짜**: 2026-06-05
**상태**: 구현 완료

### 배경

같은 유효한 cagra 인덱스에 대해, 인덱스를 빌드한 세션은 GPU `Index Scan`을 받는데 **별도 연결**은 동일 쿼리에 `Seq Scan`으로 폴백하는 현상이 관측됐다. 근본 원인을 코드 분석 + AI council(Codex 교차검증)으로 확정했다:

- 플래너 비용함수 `cuvsamcostestimate()`의 8개 `gpu_off` 게이트 중 `cuvs_index_has_artifact()` 등은 `get_index_dir()/<db>_<idx>.<ext>` 경로의 아티팩트(`.tids` 등)를 `stat`한다.
- `get_index_dir()`는 **세션-로컬 GUC** `cuvs.index_dir`(`PGC_SUSET`)를 읽고, 비어 있으면 `$PGDATA/cuvs_indexes`로 폴백한다.
- 빌드 세션 A가 `SET cuvs.index_dir='/tmp/cuvs_indexes'`로 빌드하면 데몬은 거기에 아티팩트를 기록한다. GUC를 설정하지 않은 별도 세션 B는 `get_index_dir()`가 `$PGDATA/cuvs_indexes`를 반환 → 엉뚱한 디렉터리를 `stat` → `!has_artifact`로 `gpu_off` 발동 → cost `1e15` → Seq Scan.

핵심 진단: **디렉터리 해석은 프로세스-로컬 상태인데, 아티팩트는 `(db_oid, index_oid)`로만 키잉되는 영속 cross-backend 상태**다. 영속·공유 아티팩트의 위치를 프로세스-로컬 설정으로 해석하면, 설정을 맞추지 않은 백엔드는 조용히 폴백한다(silent degrade). 실행(search)은 데몬이 자기 `g_index_dir`+VRAM 캐시로 서빙하므로 영향이 없고, **플래너 게이트 한정** 버그다.

### 결정

인덱스를 **자기서술(self-describing)** 하게 만든다 — CAGRA AM에 `index_dir` **reloption**을 추가하여 빌드 당시 디렉터리를 `pg_class.reloptions`(카탈로그, cross-backend stable)에 영속한다. 디렉터리 해석은 **3단계 우선순위**:

1. 인덱스 `index_dir` reloption (카탈로그 영속 — 세션 GUC와 무관)
2. 세션 GUC `cuvs.index_dir`
3. `$PGDATA/cuvs_indexes` 폴백

(2)+(3)은 기존 `get_index_dir()` 그대로다. reloption이 없으면 `rd_options==NULL`로 기존 경로를 타므로 **순수 additive**(기존 인덱스/테스트 무회귀, byte-identical).

구현 요점:
- reloption 인프라는 `pg_cuvs_hnsw` AM의 기존 기계(`add_reloption_kind`/`add_string_reloption`/`build_reloptions`/`amoptions`)를 미러링한다(`src/pg_cuvs.c`의 `CuvsCagraOptions`, `cuvs_cagra_init_reloptions`, `cuvs_cagra_amoptions`). 빌드-타임 불변이므로 `AccessExclusiveLock`.
- 리졸버 3종: `cuvs_reloption_index_dir(Relation)`(rd_options 직접 읽기), `cuvs_resolve_index_dir_rel(Relation)`(빌드/인서트/벌크삭제 — relcache open 불필요), `cuvs_resolve_index_dir(Oid)`(플래너 게이트·공유 path builder — `try_index_open(oid, NoLock)`).
- **플래너에서 reloption 읽기**: `IndexOptInfo`에는 `rd_options`가 없으므로 게이트는 relcache를 열어야 한다. cost-estimation 시점에 플래너가 이미 `get_relation_info()`에서 같은 엔트리를 열어 락을 transitive하게 보유하므로 `try_index_open(oid, **NoLock**)`은 refcount-only relcache lookup이다 — IPC/CUDA/lock-manager 트래픽 없음, 게이트가 이미 수행하는 `stat()`/`fread()`보다 훨씬 저렴. 동시 DROP 레이스는 `try_index_open`이 NULL 반환 → 폴백으로 방어. 결과 문자열은 close 전에 static 버퍼로 복사해 dangling을 방지.
- `get_index_dir()` 9개 호출부 스왑: 6개 Oid-게이트/공유 helper → `cuvs_resolve_index_dir(index_oid)`; 3개 Relation 사이트(빌드 `cuvs_ipc_build` 인자, `.relfilenode` write, ambulkdelete tids read) → `cuvs_resolve_index_dir_rel(indexRel)`. per-row aminsert/ambulkdelete writer는 공유 helper를 통해 reloption 해석을 transitive하게 상속(relcache lookup은 기존 per-row flock+파일 I/O 대비 무시 가능).

### 결과

- VM(A100-40GB, PG16) `make installcheck` **11/11** — 기존 10개 byte-identical(무회귀, reloption 부재 시 GUC 경로 불변) + 신규 `reloption_dir` GREEN.
- **Cross-session 실증**(별도 연결, `SHOW cuvs.index_dir` = empty):
  - `WITH (index_dir='/tmp/cuvs_indexes')`로 빌드한 인덱스 → `Index Scan`(reloption이 카탈로그에서 빌드-타임 디렉터리 해석). **수정됨**.
  - 세션 GUC만으로 빌드한(no reloption) 인덱스 → `Seq Scan`(게이트가 `$PGDATA`를 보고 `/tmp`의 아티팩트를 못 찾음). 비-reloption 인덱스에는 footgun이 잔존 → reloption 사용이 권장 패턴임을 확인.
- 변경 파일: `src/pg_cuvs.c`(reloption 인프라 + 리졸버 3종 + 9 스왑), `test/sql/reloption_dir.sql` + `test/expected/reloption_dir.out`, `Makefile`(REGRESS). IPC ABI/온디스크 포맷/SQL 시그니처/`.control` **무변경**, 마이그레이션 없음.

### 대안

- **GUC를 `PGC_SUSET`→`PGC_SIGHUP`(서버 전역)으로 변경**: 거부(이번 수정에서 번들 안 함). footgun을 reloption 없는 인덱스에도 원천 차단하지만, 9개 테스트의 per-session `SET cuvs.index_dir`을 깨고 `postgresql.conf` 마이그레이션을 강제하며 로컬 테스트 편의를 해친다. reloption은 무회귀로 버그를 직접 해결하므로 우선. 필요 시 별도 하드닝 항목으로 분리 가능.
- **빌드-타임 디렉터리를 별도 meta 사이드카에 기록**: 거부. 사이드카 자체의 위치를 찾는 chicken-and-egg가 재발한다(카탈로그만이 cross-backend stable한 앵커).
- **path builder에 `dir`를 인자로 전달(시그니처 변경)**: 거부. 게이트/라이터별로 해석 컨텍스트(Oid vs Relation)가 달라 ~14개 호출부 churn이 발생한다. per-row 비용이 파일 I/O에 지배되어 무시 가능하므로, 공유 helper를 Oid 기반으로 두는 최소 변경이 더 surgical하다.
- **reloption을 자동 캡처(미지정 시 빌드가 resolved dir를 reloptions에 기록)**: 거부. `pg_class.reloptions`는 사용자 `WITH` 입력이며, 계산값을 사후 기록하려면 카탈로그 UPDATE가 필요해 hacky하다. 명시적 `WITH (index_dir=...)`가 깔끔하고 의도가 드러난다.

## ADR-044 — 연산 지역성 프로파일링 및 latency split 측정

**날짜**: 2026-06-04 (2026-06-05 측정 완료)
**상태**: 측정 완료 (`docs/profiling-results.md`)

### 배경

pg_cuvs의 데이터 경로(빌드/검색/export)에서 PG backend - daemon - GPU 사이의 데이터 이동 비용이 정량적으로 측정되지 않은 상태다. Phase 2에서 `ipc_us`/`gpu_kernel_us`/`cpu_recheck_us` latency split을 의도적으로 deferred했고(PLAN.md Phase 2 의도적 deferral), 현재 daemon wall-clock latency 하나만 기록하고 있다.

이로 인해 최적화 우선순위가 추정치에 의존한다. 빌드 경로에서 TOAST detoast의 분산 palloc이 cache miss를 얼마나 유발하는지(ADR-034/043 근거), IPC overhead가 GPU 커널 대비 어느 비율인지, buffer manager overhead가 page write에서 얼마인지(ADR-035 거부 근거) 모두 코드 분석 기반 추정이다. Phase 4A/4B 최적화 착수 전에 실측해야 우선순위를 정확히 잡을 수 있다.

### 결정

3단계 프로파일링을 수행한다. 테스트 조건: N=1M, dim=1024, Cohere Wikipedia, A100-40GB.

**1단계 — daemon GPU 프로파일링 (nsys)**

도구: NVIDIA Nsight Systems (`nsys profile --trace=cuda,nvtx,osrt`). 대상: `pg_cuvs_server` 데몬 프로세스. 검색/빌드 양쪽 경로 모두 측정한다.

측정 항목:
- GPU 커널 실행 시간 (CAGRA search, BF search, CAGRA build)
- CUDA memcpy H2D/D2H (쿼리 벡터 전송, 결과 TID/distance 반환, 빌드 시 corpus 전송)
- 커널 launch overhead
- shm mmap 읽기/쓰기 시간 (IPC payload)
- socket send/recv 시간 (UDS command/reply)

목표: GPU 커널 vs IPC overhead 비율 확인. IPC가 지배적이면 4A-1(double memcpy 제거)의 우선순위가 올라가고, GPU 커널이 지배적이면 cuVS 파라미터 튜닝이 우선이 된다.

**2단계 — backend 빌드 경로 프로파일링 (perf)**

도구: `perf record -g -e cache-misses,LLC-load-misses,LLC-store-misses`. 대상: PG backend 프로세스 (`CREATE INDEX USING cagra` 실행 중).

측정 항목:
- heap scan → detoast → memcpy → shm write 각 구간의 cache miss 핫스팟
- TOAST(EXTENDED) vs PLAIN에서 `perf stat` 비교 → ADR-043 실증 검증에 cache miss 수치 추가
- `numastat -p`로 프로세스별 NUMA 노드 메모리 할당 분포 확인

목표: detoast(palloc + pglz)의 cache miss 비용을 정량화. ADR-034의 "TOAST decompression ~50-60%" 추정치를 실측으로 검증/보정한다.

**3단계 — buffer manager overhead 측정 (pg_stat_io + perf probe)**

도구: `pg_stat_io` (PG16+), `perf probe`로 `ReadBuffer`/`MarkBufferDirty` 동적 트레이스포인트. 대상: HNSW export page write 경로(`write_elem_page`, hnsw_export.c).

측정 항목:
- buffer pool lookup + lock acquire 시간
- page write당 평균 overhead
- WAL(`log_newpage_buffer`) 비용 비율

목표: ADR-035의 "buffer manager 제약" 거부 근거를 정량 수치로 뒷받침한다. buffer manager overhead가 전체 page write의 주 병목이 아니면 병렬화의 기대 효과도 재평가한다.

### 결과

- 검색 경로: `ipc_us` / `gpu_kernel_us` / `memcpy_us` 분해 수치를 이 ADR에 기록한다.
- 빌드 경로: `heap_scan_us` / `detoast_us` / `memcpy_us` / `shm_write_us` / `gpu_build_us` 분해 수치를 기록한다.
- export 경로: `page_write_us` / `buffer_mgr_overhead_us` / `wal_us` 분해 수치를 기록한다.
- TOAST vs PLAIN cache miss 수치 비교를 ADR-043 실증 검증에 반영한다.
- 이 수치를 기반으로 Phase 4A/4B 최적화 우선순위를 재검증한다.
- `docs/profiling-results.md`에 측정 환경/방법/결과를 아카이브한다.

### 측정 결과 (2026-06-05, A100-40GB / PG16 / N=1M dim=1024)

전체 수치·방법·제약은 `docs/profiling-results.md`. 요약:

**측정 제약**:
- GCP VM은 하드웨어 PMU 카운터 미지원(`cache-misses`/`LLC`/`instructions`/`cycles` 전부 `<not supported>`) → **cache-miss 핫스팟 측정 불가**. 대안: `task-clock` 시간 샘플링 + EXTENDED/PLAIN 빌드 시간 델타(detoast 비용 직접 측정).
- nsys 2023.4.4는 다중 스트림 빌드 캡처 시 qdstrm→nsys-rep 변환 실패("Wrong event order") → 검색만 nsys로 측정, **빌드 GPU 시간은 데몬 journal 타임스탬프**로 측정.

**검색 경로 (CAGRA Q=1)** — 데몬 wall-clock 1077.5µs = GPU 커널 ~715µs(66%) + memcpy ~4.4µs(0.4%) + IPC/overhead ~358µs(33%). **GPU:IPC ≈ 2:1, GPU-bound**. memcpy 무시 가능(zero-copy shm 실증).

**빌드 경로 (EXTENDED)** — wall-clock 83.5s = **GPU CAGRA build ~68s(82%)** + backend(heap/detoast/memcpy/shm) ~15.5s(18%). backend CPU 중 page fault ~39%(realloc), memcpy ~11%. **ADR-034의 "GPU 10s vs PG 45s" 추정은 역전 — GPU build가 지배**(ADR-036 55.7s, ADR-020 70.8s와 일관).

**TOAST vs PLAIN** — 빌드 83.5s vs 76.7s(**8% 차, detoast ~6.8s**), INSERT 100k 3130ms vs 2811ms(10%), heap 58MB+13GB TOAST vs 7.8GB(TOAST 없음). 검색 GPU latency는 storage 무관. **ADR-043 추정(25-35% 빌드 절감)을 8%로 하향 보정**(ADR-043 표 갱신).

**export 경로 (pg_cuvs_hnsw, 1M 페이지 LOGGED)** — wall-clock 63.5s, `write_elem_page` 77% 중 **buffer manager(ReadBuffer/extension) ~39% 총**, WAL(log_newpage/XLogInsert) ~14% 총, page fill ~25% 총. WAL: 1,000,238 records / 4441 MB / 1,000,026 FPI. **ADR-035 "buffer manager 제약" 거부 근거 실증**.

**우선순위 재검증**:
- **빌드 천장 재설정**: 빌드의 82%가 GPU build(cuVS 내부, 제어 불가), 어떤 4A도 빌드를 ~68s 밑으로 못 내림. ADR-034의 "PG overhead 45s"는 틀렸고 backend는 ~15.5s가 천장.
- **4A는 가치/난이도(ROI)로 판단, 둘 다 유효**:
  - **4A-1 (double memcpy)**: ~2-5s(~3-6%)지만 **난이도 낮음 → quick win**. memcpy ~1.7s + realloc page fault 완화. shm 직접 할당이 **4A-2 enabler**이므로 먼저.
  - **4A-2 (parallel workers)**: heap scan+detoast 병렬화 → backend ~15.5s→~7s, **~8-12s(~10-14%)**, 난이도 중간.
  - 빌드가 일회성(CREATE INDEX/REINDEX)이라 쿼리 경로 대비 **긴급도만 낮을 뿐 저가치 아님**. 빌드 속도가 우선이면 4A-1→4A-2.
- **적응형 마이크로배칭 → CAGRA 단일 쿼리엔 제한적**(IPC 33% 상한), BF 모드·동시성 throughput에 한정해 가치.
- **ADR-043 PLAIN 권장 → 유지하되 문구를 8% 빌드 절감 + 디스크/INSERT 이득으로 보정**.
- **ADR-035 병렬 page write 제외 → 유지**(buffer manager 39% 거부 근거 강화).

### 대안

- Nsight Compute (ncu): 거부 — 커널 내부 warp/SM 레벨 최적화용이며, 현 단계에서는 커널 간(IPC vs GPU vs memcpy) 비용 분배가 우선이다. nsys가 적합하다.
- dtrace/SystemTap: 거부 — perf로 충분한 범위를 커버하고, 추가 도구의 설치/학습 비용이 불필요하다. A100 VM은 Linux이므로 perf가 네이티브 지원된다.


## ADR-042 — Phase 3N: OFFSET-aware K 자동 조정

**날짜**: 2026-06-04
**상태**: 계획

### 배경

현재 pg_cuvs는 `ORDER BY embedding <-> query LIMIT K`만 지원한다. OFFSET을 쓰면 executor가 seqscan fallback하거나, GPU 경로가 상위 K개만 반환한 뒤 executor가 앞 offset개를 drop하려 해서 결과가 K-offset개로 줄어드는 문제가 있다. Django Paginator(`queryset[offset:offset+limit]`), Rails `.offset(N).limit(K)`, Spring Data `PageRequest.of(page, size)`, SQLAlchemy `.offset().limit()` 같은 프레임워크 수준 pagination이 pg_cuvs 인덱스에서 동작하지 않는다.

### 결정

`cuvsamcostestimate()` 또는 `cuvs_beginscan()`/`cuvs_gettuple()` 경로에서 PG 플래너의 LIMIT+OFFSET 정보를 읽어, daemon에 보내는 IPC의 K 값을 `offset + limit`으로 자동 조정한다. daemon은 `K = offset + limit`개의 후보를 CAGRA/BF에서 검색하고 전체를 반환한다. backend의 `amgettuple`는 앞 offset개를 skip하고 나머지를 반환한다.

### 근거

Qdrant Query API가 동일한 방식을 사용한다(`offset + limit`을 내부 K로 사용, 앞부분 drop). 벡터 인덱스(CAGRA, HNSW, brute force 모두)는 그래프/행렬 탐색 구조라 "이전 검색의 N번째부터 이어서 탐색"이 불가능하다. 전체 `offset + limit`개를 한 번에 검색해 앞부분을 버리는 것이 유일한 현실적 경로다.

DX 관점에서 pg_cuvs가 "PostgreSQL 인덱스답게" 동작하는 문법 범위를 DDL(Phase 3K `CREATE INDEX ... USING pg_cuvs_hnsw`) 이후 DML(`SELECT ... LIMIT K OFFSET N`)까지 확장한다. ORM이 생성하는 표준 SQL이 GPU 경로에서 그대로 동작하는 것이 목표다.

### 제약

large offset은 GPU 자원 소모가 증가한다. CAGRA의 `itopk_size`가 K에 비례하므로 `offset = 10000, limit = 10`이면 GPU가 10010개 후보를 탐색해야 한다. recall도 K가 커지면 내부 탐색 범위 한계로 감소할 수 있다. `offset > cuvs.max_offset_warning`(기본 1000) 시 NOTICE를 남겨 운영자에게 keyset pagination 전환을 권고한다.

### 결과

- `SELECT ... ORDER BY embedding <-> query LIMIT 10 OFFSET 100`이 GPU CAGRA/BF/sharded 경로에서 정상 동작한다.
- OFFSET 0일 때 기존 동작과 byte-identical(K 계산이 `0 + limit = limit`).
- `pg_stat_gpu_search`의 `requested_k`에 `offset + limit` 값이 반영된다.
- ORM(Django, Rails, Spring Data, SQLAlchemy)의 pagination 패턴이 GPU 인덱스에서 동작한다.

### 대안

- 서버 측 result cache(stateful pagination): daemon이 이전 검색 결과를 캐싱하고 cursor처럼 이어서 반환. 거부 — 현재 IPC는 stateless(요청-응답 1회)이며, 세션 친화 라우팅/캐시 만료/메모리 관리가 필요해 복잡도가 크게 증가한다. daemon의 `g_index_mutex` 직렬화 모델과도 충돌한다.
- cursor 기반 keyset pagination(`WHERE distance > last_distance`): ANN 그래프에서 distance 기반으로 이어서 탐색하는 것이 구조적으로 불가능하다. 동일 distance에 여러 벡터가 있을 수 있어 deterministic한 resume point를 잡을 수 없다.
- 별도 pagination SQL 함수(`pg_cuvs_search_page(rel, query, page, size)`): PG 표준 `LIMIT`/`OFFSET` 문법을 못 쓰면 ORM 호환이 깨져 실용 가치가 없다.

---

## ADR-043 — 벡터 전용 테이블의 PLAIN storage 권장 및 빌드 시 자동 감지

**날짜**: 2026-06-04
**상태**: 계획

### 배경

pgvector `vector` 타입은 varlena `EXTENDED` storage가 기본이다. dim >= 512이면 벡터 하나가 PG TOAST threshold(~2KB)를 초과해 거의 모든 row가 TOAST 테이블에 별도 저장된다. CAGRA 빌드 시 `cuvs_build_callback()`이 매 tuple마다 `PG_DETOAST_DATUM`(palloc + pglz decompress)을 수행하며, 이것이 heap scan 오버헤드의 50-60%를 차지한다(ADR-034 후속 분석).

TOAST의 이점은 "벡터를 읽지 않는 쿼리에서 detoast 비용을 피한다"는 것인데, 벡터 전용 테이블에서는 이 이점이 적용될 일이 없다. INSERT → `CREATE INDEX USING cagra` → `SELECT ... ORDER BY embedding <-> query LIMIT K` 전 경로에서 벡터 전체를 읽는다. 벡터와 비즈니스 데이터(metadata, text 등)가 같은 테이블에 있을 때만 TOAST가 유의미하며, 이 경우에도 벡터 전용 테이블을 분리하고 id로 조인하는 것이 양쪽 모두 최적이다.

핵심 원칙: **MVCC를 해치지 않는 선에서 최대의 성능 보장**. PLAIN storage는 PG의 정상 기능이며 visibility check, WAL, 트랜잭션 격리에 영향을 주지 않는다.

### 결정

1. **문서에 벡터 전용 테이블 + PLAIN storage를 best practice로 명시한다.**

권장 스키마 패턴:
```sql
-- 벡터 전용 테이블 (PLAIN storage)
CREATE TABLE vectors (
    id bigint PRIMARY KEY,
    embedding vector(1024)
);
ALTER TABLE vectors ALTER COLUMN embedding SET STORAGE PLAIN;

-- 비즈니스 데이터 테이블
CREATE TABLE documents (
    id bigint PRIMARY KEY,
    title text,
    content text,
    metadata jsonb
);

-- 검색 후 조인
SELECT d.title, d.content
FROM (
    SELECT id FROM vectors
    ORDER BY embedding <-> $1 LIMIT 10
) v
JOIN documents d ON d.id = v.id;
```

PLAIN storage의 효과: dim=1024 기준 빌드 시 TOAST decompression(palloc + pglz) 비용이 완전히 제거된다. heap scan 구간에서 ~25-35% 절감(~15-20s → ~10-13s) 기대.

PLAIN storage의 트레이드오프: tuple이 커져(4KB+) 페이지당 row 수가 1-2개로 감소하고 heap 자체가 ~4x 커진다. 벡터를 참조하지 않는 쿼리(COUNT, id-only SELECT 등)에서 불필요하게 큰 tuple을 fetch하게 된다. 벡터 전용 테이블에서는 이런 쿼리가 드물므로 영향이 제한적이다.

기존 TOAST된 데이터: `SET STORAGE PLAIN` 후에도 기존 row는 TOAST에 그대로 남는다. `VACUUM FULL` 또는 테이블 재적재(pg_dump/restore, `CREATE TABLE ... AS SELECT`)가 필요하다. 따라서 신규 테이블에서 처음부터 설정하는 것을 권장한다.

2. **`CREATE INDEX USING cagra` 시점에 EXTENDED storage 자동 감지 → NOTICE 출력.**

`cuvs_ambuild()`에서 indexed column의 `attstorage`를 syscache에서 읽어 `TYPSTORAGE_EXTENDED`('x')인지 확인한다. EXTENDED이면 다음 NOTICE를 출력한다:
```
NOTICE: pg_cuvs: column "embedding" uses EXTENDED storage (TOAST).
HINT: For faster builds, consider: ALTER TABLE ... ALTER COLUMN embedding SET STORAGE PLAIN;
       See: docs/best-practices.md
```

pg_cuvs가 storage를 강제 변경하지는 않는다. 사용자가 의도적으로 EXTENDED를 유지하는 경우(벡터와 비즈니스 데이터가 같은 테이블에 있고 분리할 수 없는 경우)를 존중한다.

3. **`docs/best-practices.md`에 권장 스키마 패턴을 문서화한다.**

### 실증 검증

ADR-043의 근거(detoast 오버헤드 50-60%, PLAIN 전환 시 빌드 ~25-35% 절감)는 코드 분석 기반 추정이므로, PLAIN storage 전환 시 실제 빌드/검색/INSERT 성능 차이를 실측으로 확인한다.

테스트 조건: N=1M, dim=1024, Cohere Wikipedia, A100-40GB. 동일 데이터를 두 테이블에 적재하고 비교한다.

측정 완료 (2026-06-05, A100-40GB / PG16 / N=1M dim=1024). 상세: `docs/profiling-results.md` §4.

| 측정 항목 | EXTENDED (기본) | PLAIN | 비고 |
|-----------|-----------------|-------|------|
| (a) CAGRA 빌드 시간 | **83.5 s** | **76.7 s** | PLAIN 6.8s(8.1%) 빠름. detoast = backend 구간 15.5s→8.7s 차 |
| (b) heap 크기 | **58 MB main + 13 GB TOAST** | **7813 MB main, TOAST 없음** | PLAIN main heap 134× 크나 총 디스크는 작음(13GB→7.8GB) |
| (c) 검색 latency (CAGRA GPU) | **storage 무관** | **storage 무관** | CAGRA 그래프는 VRAM 상주; heap recheck는 K=10개만 detoast(무시 가능) |
| (d) 검색 latency (pgvector HNSW CPU) | 미측정 | 미측정 | (c)와 동일 논리 — recheck K개만 영향; 별도 HNSW 인덱스 필요해 보류 |
| (e) INSERT throughput (100k) | **3130 ms** | **2811 ms** | PLAIN 10% 빠름(TOAST 삽입 오버헤드 회피) |
| cache-miss 핫스팟 | **측정 불가** | **측정 불가** | GCP VM 하드웨어 PMU 미지원(`<not supported>`). task-clock 샘플링·빌드 델타로 대체 |

**결론 (추정 보정)**: PLAIN 빌드 절감은 **8%로 ADR-043 추정(25-35%)보다 작다**. 빌드의 82%가 GPU build(ADR-044)라 detoast(8%)의 절감 여지가 제한적이기 때문. 다만 PLAIN은 빌드(8%)·INSERT(10%)·총 디스크 모두 유리하므로 **권장은 유지하되, NOTICE/best-practice 문구의 "~25-35% 빌드 절감"을 "~8% 빌드 절감 + 디스크/INSERT 이득"으로 보정**한다.

### 결과

- `cuvs_ambuild()`가 EXTENDED storage를 감지하면 NOTICE를 출력한다. pg_cuvs가 스키마를 변경하지 않으므로 MVCC 안전성에 영향 없다.
- 벡터 전용 테이블 + PLAIN storage 패턴이 best practice 문서에 기록된다.
- 구현 난이도가 낮아(syscache 읽기 + ereport 한 줄) release hardening에 포함한다.
- TOAST vs PLAIN 실증 벤치마크를 release hardening 단계에서 실행하고, 결과를 이 ADR과 best-practices.md에 반영한다.

### 대안

- `ambuild()`에서 자동으로 PLAIN으로 변경. 거부 — DDL 부작용이며, 사용자 동의 없는 스키마 변경은 위험하다. 다른 세션이 동일 테이블을 사용 중일 수 있고, `ALTER TABLE ... SET STORAGE`는 `AccessExclusiveLock`이 필요해 online DDL과 충돌한다.
- 별도 binary column(`bytea PLAIN`): 거부 — 스키마 변경 강제, pgvector opclass 재사용 설계(ADR-006)와 충돌, DX 저하(ADR-034 후속 분석 (b) 참조).
- pgvector fixed-length storage 기여: 보류 — 업스트림 의존, pg_cuvs 단독 불가(ADR-034 후속 분석 (c) 참조).
- WARNING 대신 ERROR로 EXTENDED를 차단. 거부 — 기존 pgvector 사용자의 테이블에서 인덱스 생성 자체를 막으면 도입 장벽이 너무 높다.

---

