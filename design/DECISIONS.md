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
- Phase 3 S3 연동 시 동일 경로 추상화 재사용 가능
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

## ADR-012 — Phase 2 DiskANN: cuVS Vamana 네이티브 방식

**날짜**: 2026-05-23

**문제**: PLAN.md는 "CAGRA로 GPU 빌드 후 HNSW 포맷으로 변환"을 Phase 2 DiskANN 방식으로 언급했다. 그러나 cuVS에 더 직접적인 경로가 있는지 확인이 필요했다.

**조사**: cuVS에 `cuvs.neighbors.vamana`가 존재하며, DiskANN 바이너리 포맷으로 직접 저장/로드 가능(`vamana.save()` / `vamana.load()`). CAGRA→HNSW 변환 레이어 불필요.

**결정**: Phase 2 DiskANN은 두 경로를 모두 지원.

```
CAGRA build(GPU) → HNSW format → CPU HNSW search   (중간 규모, RAM 상주)
Vamana build(GPU) → DiskANN binary → CPU Vamana search  (대규모, NVMe)
```

공통 원칙: **빌드만 GPU 가속, 검색은 CPU**. 디스크 기반 검색은 I/O가 병목이라 GPU 전송 오버헤드가 이득을 상쇄.

**결과**: PLAN.md의 CAGRA→HNSW 변환 방식은 cuVS `from_cagra()` API로 대체. 단, CAGRA→HNSW 경로도 CPU fallback용으로 병행 지원 가능 (`cuvs.export_hnsw = on` GUC).

**대안**: pgvectorscale DiskANN 재사용. 보류 — 라이선스 확인 필요, cuVS Vamana가 더 직접적 경로.

---

## ADR-013 — Phase 3 S3: Derived Data + S3 Source of Truth 방향

**날짜**: 2026-05-23

**문제**: 수십억 규모 인덱스를 어떻게 저장하고 멀티노드에서 공유할지 결정이 필요하다.

**결정**: 두 단계로 전환.

- **Phase 3 MVP**: 인덱스를 derived data로 취급 (WAL 제외). S3 스냅샷은 재빌드 시간 절약용 캐시로 사용. 장애 시 테이블에서 재빌드 가능.
- **Phase 3 v2**: S3를 Source of Truth로 전환. 로컬 NVMe는 io_uring 비동기 프리페치 캐시. 인스턴스 교체 또는 읽기 전용 레플리카가 S3에서 직접 로드.

**결과**:
- MVP 단계에서 구현 복잡도 최소화
- 멀티노드 공유 필요 시 S3 SoT 전환으로 자연스럽게 진화
- 인덱스 파일은 `pg_basebackup` WAL 스트림에서 제외 (`SPEC.md S3-03` 참조)

---

## ADR-014 — 쓰기 처리: AUTOVACUUM 연동 Lazy Rebuild

**날짜**: 2026-05-23

**문제**: CAGRA는 정적 그래프 인덱스라 INSERT/UPDATE/DELETE 시 실시간 업데이트가 불가능하다.

**결정**: pgvector HNSW와 동일한 접근 방식 채택. INSERT/UPDATE/DELETE 시 영향받은 TID를 인메모리 pending-delta set에 기록. AUTOVACUUM 또는 수동 VACUUM 시 pg_cuvs_server가 현재 heap 상태로 인덱스를 재빌드하고 VRAM을 원자적으로 교체.

**결과**:
- 별도 스케줄러 없이 PostgreSQL 기존 유지보수 사이클 재활용
- delta 비율이 `cuvs.rebuild_threshold`(기본 10%) 초과 시 VACUUM 권고 WARNING 발생
- VACUUM 전까지 stale 인덱스 허용 — recall 저하는 있으나 정확도(exact match)는 heap recheck로 보장

**대안**: Background Worker 주기적 재빌드. 보류 — AUTOVACUUM 연동이 더 단순하며 PG 생태계 관례와 일치. 필요 시 Phase 2에서 워밍업 Background Worker 추가 가능.

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
