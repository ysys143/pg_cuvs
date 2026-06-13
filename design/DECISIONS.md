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
- ~~시작 시 `pg_catalog.pg_index`에 존재하는 OID와 대조해 유효한 인덱스만 로드~~ **(2026-06-05 정정: 미구현)** — 실제 `startup_load_indexes()`(`src/pg_cuvs_server.c:1494`)는 `index_dir`의 모든 파싱·CRC-valid artifact를 무조건 로드한다. 데몬은 standalone sidecar(ADR-002)라 PG 카탈로그에 연결조차 하지 않는다(소스 전체에 `pg_index`/`PQconnect` 부재). 코드의 "valid"는 파일명+CRC(ADR-019) 유효일 뿐 **카탈로그 유효가 아니다**. 결과적으로 데몬-down DROP / DROP DATABASE가 남긴 orphan artifact가 재시작 시 좀비로 재로드된다(VRAM+디스크 누수). reconciliation 설계는 **ADR-046** 참조.

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

**한계 (2026-06-05 정정)**: circuit breaker는 **백엔드 프로세스-로컬**이다 — 상태가 `cuvs_circuit_breakers` static 배열(`src/cuvs_util.c:590` `find_or_create_breaker`)에 저장돼 각 백엔드가 독립적으로 트립/리셋하고 백엔드 종료 시 사라진다. 서버 전역 공유·영속이 아니며, `pg_cuvs_reset_circuit()`도 호출 세션만 리셋한다(`src/pg_cuvs.c:2201`). "자동 비활성화"는 세션(백엔드) 단위로 이해해야 한다. (per-backend 동작 자체는 합당하나 ADR 본문이 전역 안전망처럼 읽힐 수 있어 명시.)

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

**날짜**: 2026-05-23 (2026-06-10 CERTIFIED)
**상태**: S3-specific 표현에서 GCS-first object storage 설계로 일반화됨 → **Phase 3C MVP 구현·인증 완료** (실 GCS round-trip + fail-closed 검증). 상세 인증 기록은 ADR-066.

**문제**: 수십억 규모 인덱스를 어떻게 저장하고 멀티노드에서 공유할지 결정이 필요하다.

**결정**: 두 단계로 전환.

- **Phase 3C MVP**: 인덱스를 derived data로 취급 (WAL 제외). Object storage snapshot은 heap-compatible PostgreSQL node에서 재빌드 시간을 줄이는 캐시로 사용한다. Heap/table 배포는 PostgreSQL backup/replication이 책임진다. MVP provider는 GCS다.
- **Phase 3C v2**: Object storage를 derived index artifact의 snapshot source로 전환. 로컬 NVMe는 io_uring 비동기 프리페치 캐시. 인스턴스 교체 또는 읽기 전용 레플리카가 compatible heap을 이미 가진 경우 object storage에서 index artifact를 직접 로드한다.

**결과**:
- MVP 단계에서 구현 복잡도 최소화
- 멀티노드 공유 필요 시 object storage snapshot source로 자연스럽게 진화
- 인덱스 파일은 `pg_basebackup` physical base backup payload + WAL에서 제외 (`SPEC.md OBJSTORE-03` 참조) **(2026-06-05 정정/주의: 기본 경로에서 미보장 — 단, 지역성과 직교)**:
  - **지역성 측면에선 기본값이 옳다**: artifact를 데이터와 같은 빠른 로컬 디스크에 두는 것이 연산 지역성에 맞고, 그래서 기본을 `$PGDATA/cuvs_indexes`(`src/pg_cuvs.c:519`)로 둔다.
  - **그러나 백업 멤버십과는 별개 문제다**: `pg_basebackup`은 PGDATA 트리 + 테이블스페이스를 통째로 복사하고 네이티브 per-directory 제외가 없다. 기본 경로는 PGDATA *트리 안*이라 수 GB 파생 artifact가 base backup에 실린다(WAL 자체가 아니라 base copy 멤버십 문제).
  - **둘은 양립 가능(직교)**: "PGDATA 밖" ≠ "느린 디스크". **같은 로컬 NVMe의 형제 디렉터리**(트리 밖, 예: `/var/lib/postgresql/pg_cuvs_indexes`)에 두면 지역성 유지 + 백업 제외 동시 달성.
  - **비용(실측 아님, 분석)**: 데이터 오염은 아니다(relfilenode fail-closed). 진짜 비용은 백업 비대 + **`pg_basebackup`으로 standby 신규 프로비저닝 시 쓸모없는 GB 전송 → RTO/네트워크 악화**. → P2 운영 하드닝(코드: index_dir이 DataDir 하위면 WARNING; 운영: 형제 디렉터리 프로비저닝 + 복원 시 경로 부재→rebuild). 기본값 변경은 권한 footgun(`postgres`가 PGDATA 밖 쓰기 불가) + breaking이라 보류, 기본은 유지하되 WARNING/playbook으로 유도. ROADMAP "운영 하드닝" wave + OPS_GPU_PLAYBOOK §6 참조.
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
- ~~calls, success, fallback, error.~~ **(2026-06-05 정정: 부분 구현)** — 실제 노출은 `search_count`(=OK 검색)·`error_count`뿐(`src/pg_cuvs_server.c:133-134`). 별도 "calls" 합계·"success"·"fallback" 카운터는 없다. fallback은 백엔드 plan-time 결정(`src/pg_cuvs.c:932`)이라 데몬 stats에 도달하지 않아 이 view에서 구조적으로 관측 불가.
- requested k, returned k, rows returned.
- average latency, p95 latency.
- ~~GPU kernel time, IPC time, CPU recheck time.~~ **(2026-06-05 정정: 미구현)** — 데몬은 단일 wall-clock `total_latency_us`만 기록하고(`src/pg_cuvs_server.c:135`), SRF에 `gpu_kernel_us`/`ipc_us`/`cpu_recheck_us` 컬럼이 없다. latency split은 PLAN.md(Phase 2 deferral)에서 보류로 명시했고, 외부 측정은 ADR-044(nsys/perf)로 수행했다.
- VRAM cache hits/misses, reload count.
- last status, last error, last search timestamp.

**결과**:
- Phase 2 기능의 운영성과 성능을 SQL에서 확인할 수 있다.
- playbook이 log scraping에만 의존하지 않는다.
- 추후 PostgreSQL native `pgstat` integration으로 확장할 수 있다.
- **(2026-06-05 정정)** 위 "초기 지표" 중 latency split(GPU/IPC/recheck)과 fallback·success·calls 카운터는 미구현이다(상세는 해당 항목 취소선 참조). 실제 `pg_cuvs_gpu_search_stats` 컬럼은 `search_count`/`error_count`/`avg·p50·p95·p99_latency_us` 등 wall-clock 기반이며, "초기 지표"는 의도였을 뿐 일부만 구현됐다. 운영자가 view를 보고 split/fallback이 있다고 오인하지 않도록 본 정정으로 명시한다.

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
- 검증: DROP 후 `pg_stat_gpu_shards` 0 rows + VRAM 회수 + restart 후 zombie 없음 + 데몬-down DROP도 성공(WARNING). **(주의: "restart 후 zombie 없음"은 데몬-up DROP 한정 — artifact가 unlink됐기 때문. 데몬-down DROP은 아래 한계 참조.)**
- 알려진 한계:
  - **미커밋 DROP (안전)**: `BEGIN; DROP INDEX;` 후 커밋하지 않으면 daemon에 아무것도 전송되지 않는다. `cuvs_ipc_drop`은 `XACT_EVENT_COMMIT`에서만 발사되므로 ROLLBACK 시 artifact는 그대로 보존된다.
  - ~~**rolled-back SAVEPOINT 내 DROP (잠재적 오삭제)**~~ **해소됨 (2026-06-07, ADR-060)**: `SAVEPOINT s; DROP INDEX; ROLLBACK TO s; COMMIT;`에서 살아있는 인덱스의 GPU artifact가 조기 삭제되던 문제 — `cuvs_pending_drops`를 `(Oid, SubTransactionId)` 태깅 + `RegisterSubXactCallback`(ABORT_SUB 폐기 / COMMIT_SUB reparent)으로 해결. 롤백된 subxact의 drop은 통지되지 않는다. `drop_subxact` 회귀 테스트 + installcheck 17/17.
  - `DROP EXTENSION CASCADE`가 AM을 먼저 drop하면 `get_am_oid`가 Invalid → 해당 통지 누락(restart/playbook).
- **알려진 한계 (orphan artifact 누수, 2026-06-05 4-preflight 세션에서 실측 확인 — ADR-046에서 해결 설계)**:
  - **데몬-down 중 DROP INDEX/TABLE**: `cuvs_ipc_drop`이 `status 4`(UNAVAILABLE)로 실패, PG DROP은 commit되지만 artifact 잔존. HINT는 "재시작 시까지 잔존"이라 하나 **재시작이 정리하지 않고 오히려 좀비로 재로드**한다(ADR-009 정정 참조).
  - **DROP DATABASE (데몬-up이어도)**: `object_access_hook`은 DROP을 실행하는 백엔드에서만 발화하는데, DROP DATABASE는 다른 DB에서 실행되어 대상 DB 내부 cagra 인덱스의 OAT_DROP을 보지 못한다 → per-index 통지 없음, 전부 orphan. (세션 시작 시 발견된 `<dropped_db_oid>_*` artifact의 정체.)
  - **재시작 시 좀비 재로드**: `startup_load_indexes`가 카탈로그 대조 없이 모든 artifact를 로드하므로, 위 orphan들이 VRAM에 좀비로 적재되고 다음 SIGTERM에서 다시 디스크에 저장되어 누수가 영속화된다. 실측: 디스크 23GB orphan + 데몬 시작 시 dropped-DB 인덱스(500000 vecs, ~1983MB VRAM) 적재.
  - **현 완화책(수동)**: 데몬 정지 → 카탈로그에 없는 `<db>_<idx>.*` 삭제 → 데몬 시작. (`sudo rm glob`은 0700 디렉터리에서 ubuntu 권한 글로브 확장 실패하므로 `sudo find ... -delete` 사용.)

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

~~heap recheck / MVCC visibility는 함수 내부에서 `heap_fetch`로 처리한다.~~ **(2026-06-05 정정: 본문 오기 — 아래 구현현황과 모순)** 실제로는 함수 내부에서 `heap_fetch`를 하지 않고 raw ctid + 데몬 distance를 반환하며, caller가 `JOIN ... ON ctid`로 visibility를 처리한다(`src/pg_cuvs.c:2300`, 본 ADR 구현현황과 일치). 결과는 `query_idx` 기준으로 정렬하지 않는다(GPU 반환 순서 유지).

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
- **4A의 가치는 "빌드 시간 비율"이 아니라 "PG 오버헤드 제거율"로 평가**: backend ~15.5s는 전부 제거 가능한 PG 오버헤드. **PLAIN(detoast 제거, 측정 15.5s→8.7s) + 4A-1(shm 직접 할당 → realloc page fault 39% + double memcpy 제거) + 4A-2(parallel workers → heap scan 분산)**를 결합하면 backend가 ~2-4s로 거의 소멸 → 빌드 83.5s→**~70-72s = GPU build 68s + 최소 dispatch ≈ cuVS native의 ~95%**, 그것도 MVCC/durability/DDL 통합 유지. 개별 4A는 modest하나 **결합 효과("Postgres 안전성 + cuVS native 속도")로 평가**해야 한다.
- **가치/난이도(ROI), 둘 다 유효**:
  - **4A-1 (double memcpy)**: ~2-5s(~3-6%)지만 **난이도 낮음 → quick win**. memcpy ~1.7s + realloc page fault 완화. shm 직접 할당이 **4A-2 enabler**이므로 먼저.
  - **4A-2 (parallel workers)**: heap scan+detoast 병렬화 → backend ~15.5s→~7s, **~8-12s(~10-14%)**, 난이도 중간.
  - 빌드가 일회성(CREATE INDEX/REINDEX)이라 쿼리 경로 대비 **긴급도만 낮을 뿐 저가치 아님**. 빌드 속도가 우선이면 4A-1→4A-2.
- **적응형 마이크로배칭 → CAGRA 단일 쿼리엔 제한적**(IPC 33% 상한), BF 모드·동시성 throughput에 한정해 가치.
- **ADR-043 PLAIN 권장 → 유지하되 문구를 8% 빌드 절감 + 디스크/INSERT 이득으로 보정** *(적용됨 2026-06-07: `docs/best-practices.md` §1 + `src/pg_cuvs.c` TOAST NOTICE 주석의 "~25-35%"를 실측 ~8%로 보정, main heap 134× 팽창 트레이드오프 명시)*.
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

TOAST의 이점은 "벡터를 읽지 않는 쿼리에서 detoast 비용을 피한다"는 것인데, 벡터 전용 테이블에서는 이 이점이 적용될 일이 없다. INSERT → `CREATE INDEX USING cagra` → `SELECT ... ORDER BY embedding <-> query LIMIT K` 전 경로에서 벡터 전체를 읽는다. 벡터와 비즈니스 데이터(chunk text, metadata 등)가 같은 테이블에 있는 경우, 벡터 전용 테이블 분리는 빌드 성능에는 유리하지만 검색 경로에서 chunk 조회 조인이 critical path에 추가되어 end-to-end QPS가 저하될 수 있다. 빌드 효율과 검색 QPS 중 무엇이 우선인지는 워크로드에 따라 다르며, pg_cuvs가 레이아웃을 강제하지 않는 이유가 여기 있다.

"벡터 전용 테이블을 분리하면 빌드 I/O도 준다"는 직관은 실제 영향이 제한적이다. `table_index_build_scan` 콜백은 `values[0]`(embedding)만 detoast하지만, heap main tuple 자체는 모든 컬럼을 포함한다. 그러나 chunk TEXT, metadata JSONB 같은 비벡터 컬럼이 EXTENDED(기본값)이면 main tuple에 18바이트 TOAST 포인터만 남고 실제 청크는 fetch되지 않는다. 빌드 경로의 I/O 비용은 embedding 컬럼 자체의 TOAST 처리에서 비롯되며, 다른 EXTENDED 컬럼의 존재는 main tuple 크기에 미미한 영향만 준다. 결국 "벡터 전용 테이블" 권장의 실질 근거는 테이블 분리 자체가 아니라 embedding 컬럼의 PLAIN storage 적용이 목적이다.

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

**벡터 전용 테이블 분리의 검색 QPS 트레이드오프**: 실제 RAG 워크로드에서는 embedding과 chunk text가 같은 테이블에 있는 경우가 많다. 분리하면 빌드는 유리하지만 모든 ANN 검색 결과에 chunk 조회 조인이 추가된다. embedding + chunk를 같은 테이블에 두면 ANN recheck 시 heap 접근 한 번으로 두 컬럼을 동시에 읽을 수 있어 조인 오버헤드가 없다. 검색 QPS가 우선인 환경에서는 embedding + chunk를 같은 테이블에 유지하되 embedding 컬럼에만 PLAIN을 적용하고 chunk TEXT는 EXTENDED로 유지하는 것이 합리적이다.

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
- 독자적 테이블 레이아웃 강제(embedding 단독 테이블 요구): 거부 — 비벡터 EXTENDED 컬럼은 이미 main tuple에 18바이트 포인터만 남아 분리의 빌드 I/O 이득이 작다. 반면 chunk text를 같이 두는 RAG 워크로드에서는 분리 시 검색 QPS가 저하된다(조인 추가). "CREATE INDEX만 추가하면 된다"는 DX 원칙과도 충돌한다.

---

## ADR-046 — 데몬 startup 인덱스 reconciliation: orphan artifact 누수 근절

**날짜**: 2026-06-05
**상태**: 구현 완료 (2026-06-05) — backend 주도 GC 함수. startup lazy-load 안전화(보완안)는 미채택(GC가 누수를 근절하므로 불필요).

### 배경

2026-06-05 4-preflight 세션에서 발견·실측. 데몬 `index_dir`에 카탈로그에 존재하지 않는 orphan artifact가 누적되어, 세션 시작 시 23GB가 쌓여 있었고 dropped-DB 인덱스(500000 vecs)가 ~1983MB VRAM에 좀비로 적재돼 있었다. CAGRA 빌드가 디스크 풀(98%)로 실패하는 직접 원인이 됐다.

근본 원인 3가지(실측):

1. **데몬-down 중 DROP**: `cuvs_ipc_drop`이 `status 4`(UNAVAILABLE)로 실패. PG DROP은 commit되나 artifact 잔존(ADR-023 best-effort).
2. **DROP DATABASE**: `object_access_hook`은 DROP을 실행하는 백엔드에서만 발화하는데 DROP DATABASE는 타 DB에서 실행 → 대상 DB 내부 cagra 인덱스의 OAT_DROP 미관측 → per-index 통지 전무.
3. **재시작 좀비 재로드**: `startup_load_indexes()`(`src/pg_cuvs_server.c:1494`)가 카탈로그 대조 없이 모든 파싱·CRC-valid artifact를 로드. ADR-009가 "시작 시 pg_catalog.pg_index와 대조"를 주장했으나 미구현(데몬은 standalone sidecar, PG 연결 없음). orphan이 좀비로 적재되고 다음 SIGTERM에서 재저장되어 누수가 영속화.

검증(실측):

| 시나리오 | 결과 |
|---|---|
| DROP INDEX/TABLE (데몬 up) | 정리됨(object_access_hook + commit notify) |
| DROP INDEX/TABLE (데몬 down) | orphan 잔존(status 4 WARNING) |
| DROP DATABASE (데몬 up이어도) | orphan 잔존(per-index 통지 없음) |
| 데몬 재시작 | orphan을 좀비로 재로드(VRAM+디스크 누수) |

### 결정 (설계 — 구현 보류)

핵심 제약: 데몬은 standalone sidecar(ADR-002)라 카탈로그 접근이 없다. 카탈로그를 볼 수 있는 주체는 **backend**다. 따라서 reconciliation은 backend 주도가 자연스럽다.

**권장 방향 — backend 주도 GC 함수(우선)**:
- `pg_cuvs_gc_orphans()` SQL 함수: backend가 `index_dir`을 스캔하고 각 `<db>_<idx>` artifact를 현재 DB의 `pg_index`(+ `pg_database`로 db_oid)와 대조해, 카탈로그에 없는 OID에 대해 데몬에 `CUVS_OP_DROP_INDEX`를 보내 free+unlink하거나 직접 unlink한다.
- 장점: 기존 IPC(`cuvs_ipc_drop`)·`object_access_hook` 인프라 재사용, 카탈로그 권위는 backend가 보유, sidecar 원칙 유지.
- 한계: cross-database orphan은 각 DB에서 함수 호출 필요(또는 `pg_database` 전수 대조 후 현존 DB가 아닌 db_oid는 안전하게 unlink). 운영자가 주기 실행(cron/maintenance) 또는 데몬 startup 직후 1회 backend 트리거.

**보완 — startup 안전화**: 데몬 startup_load_indexes가 즉시 좀비를 서빙하지 않도록, 로드를 lazy(첫 쿼리 시 backend가 카탈로그 확인 후 트리거)로 바꾸거나, startup 로드분을 "unverified" 상태로 두고 backend의 첫 reconciliation까지 GC 대상으로 표시하는 방안.

**기각/보류 대안**:
- 데몬이 libpq로 직접 카탈로그 조회: 보류 — sidecar에 DB 연결/크리덴셜/멀티-DB 연결 관리가 들어와 ADR-002 원칙과 충돌. backend 주도가 더 단순.
- DROP DATABASE 전용 hook: 거부 — PG는 타 DB 객체에 대한 per-index hook을 제공하지 않음. GC가 포괄.
- 주기적 데몬 자체 GC(파일 mtime 등 휴리스틱): 거부 — 카탈로그 권위 없이 삭제하면 정상 인덱스 오삭제 위험.

**즉시 적용(문서/운영)**:
- 잘못된 HINT 문구 수정: "artifacts ... may persist until the daemon restarts" → 재시작이 정리하지 않음을 반영(예: "until manual GC / pg_cuvs_gc_orphans()").
- OPS_GPU_PLAYBOOK에 수동 정리 절차 추가: 데몬 정지 → 카탈로그에 없는 `<db>_<idx>.*`를 `sudo find ... -delete`(0700 디렉터리라 `rm glob` 무효) → 데몬 시작.

### 결과 (구현 완료, 2026-06-05)

- **`pg_cuvs_gc_orphans(do_delete boolean DEFAULT false)`** SRF 구현(`src/pg_cuvs.c`, `sql/pg_cuvs--0.1.0.sql`). `index_dir`을 스캔(`.cagra`/`.shards` anchor) → `cuvs_parse_index_filename`로 `(db,idx)` 추출 → 카탈로그 대조. 반환 `(db_oid, index_oid, reason, action)`.
  - `reason`: `missing_in_catalog`(현재 DB, OID 부재) / `dead_database`(`pg_database`에 db_oid 부재) / `unverifiable_other_db`(현존 타 DB — 보수적 skip, 해당 DB에서 재실행).
  - `do_delete=false`(기본)=dry-run(would_delete/skipped 보고만). `do_delete=true`=데몬 up이면 `cuvs_ipc_drop`(VRAM free+unlink), down이면 backend 직접 unlink(파일 패밀리 prefix `<db>_<idx>.`).
  - `delete`는 SQL 예약어라 인자명은 `do_delete`. 안전: live 인덱스는 절대 보고/삭제 안 함(SearchSysCache로 보존).
- **WARNING HINT 정정**(`src/pg_cuvs.c`): "until the daemon restarts" → "재시작이 정리하지 않고 좀비로 재로드; `pg_cuvs_gc_orphans(true)` 실행 또는 OPS_GPU_PLAYBOOK 수동 절차".
- **OPS_GPU_PLAYBOOK §6**: 자동(`pg_cuvs_gc_orphans`) + 수동(`sudo find ... -delete`) 정리 절차 추가.
- **검증(VM A100/PG16, 2026-06-05)**:
  - installcheck 12/12 GREEN(기존 11 + 신규 `gc_orphans`). negative-safety 회귀: live 인덱스 미보고/미삭제, dry-run·delete 비파괴, 데몬-up DROP 후 orphan 0.
  - 수동 end-to-end(데몬-down 시나리오): 빌드→데몬 정지→DROP(orphan 잔존, 정정 HINT 발화)→dry-run(`missing_in_catalog`/would_delete)→`gc_orphans(true)`(backend 직접 unlink, deleted)→**artifact 0**→데몬 재시작 후 해당 인덱스 **미적재**(search_stats 0행). 좀비 재로드 누수 근절 확인.
- ADR-009 정정은 spec-audit PR(별도)에 반영.

---

## ADR-047 — Phase 3A 검증 certify + tombstone-aware over-fetch (delete-drift recall)

**날짜**: 2026-06-06
**상태**: 구현·검증 완료 (2026-06-06, VM A100/PG16). 코드 변경 1건(backend over-fetch), 검증 하드닝 다수.

### 배경 — false-done 역방향

3A "pending delta" 메커니즘(`.delta` append·CPU/GPU 병합·`.tombstone`·tri-mode GUC·`pg_stat_gpu_search` delta 컬럼)은 **2026-05-27 WIP 커밋들로 이미 구현**돼 있었고(`4b9c8b7` delta append, `a6bfda5` tombstone, `f75a65c` GPU merge, GPU delta cache는 3F/3G 샤딩에 섞여 — 3F/3G 완료 행이 "delta cache, eviction"을 이미 크레딧), 정확했다(snapshot-aware tombstone filter `pg_cuvs.c`: `TransactionIdDidCommit && !XidInMVCCSnapshot`, metric 정합 L2/cosine/IP). 그런데 ROADMAP는 3A를 "릴리스 후 기능(미완)"으로 표기했고, `2f97cf0`(2026-06-04)은 이미 짜인 3A-1을 ROADMAP TODO로 **재등록**했다. 원인 3가지: (1) 코드가 3F/3G·phase3a WIP로 조각조각 머지돼 독립 "3A 완료" 마일스톤이 없었고 완료 표(SSOT) reconcile 누락, (2) ROADMAP 편집이 기구현분을 미완으로 되돌림, (3) PLAN의 3A 완료 기준이 요구하는 회귀/격리/property 테스트가 실제로 없어 "검증 미완"은 부분적으로 정당했다. 직전 spec-audit(ADR-009 등)은 반대 방향(ADR가 done 주장하나 코드 없음)을 타깃해 역방향을 못 잡았다.

**결론**: 3A는 `code-complete, verification-incomplete`였다. 본 세션은 (a) 검증을 채워 certify하고, (b) 검증 중 발견한 recall 갭 1건을 수정한다.

### 결정 1 — tombstone-aware base over-fetch (코드 변경)

**문제**(검증 중 발견·실측): 삭제+VACUUM된 dead TID가 query의 top-cuvs.k base 후보를 모두 차지하면, 병합 후 tombstone 필터가 dead를 전부 제거해 SQL LIMIT보다 적은 live row를 반환한다. delete-drift gate(`max_stale_fraction`)가 잡는 대량 삭제와 달리, **gate 임계 아래(예: 2.5%)이면서 query top-k에 집중된 삭제**에서만 발현 — VM 실측: `cuvs.k=10`, 10/400 삭제, `stale=false`, `live_topk=0`(LIMIT 5인데 0행).

**결정**: `cuvs_gettuple`이 base 검색 k를 pending dead-TID 수만큼 over-fetch — `k += min(n_tomb, cuvs_k)`. tombstone 필터가 live top-k를 굶기지 못한다. 상한 `cuvs_k`로 GPU 작업 폭주 방지(더 무거운 삭제는 drift gate가 선처리). tombstone 없으면 no-op → 공통 경로 불변. 신규 헬퍼 `cuvs_tombstone_count`(=`cuvs_index_tombstone_unusable`과 동일 검증, 유효 entry 수 반환). **GUC 미추가**(자동·무설정; Karpathy 단순성).

**대안 기각**: in-AM slop-retry 루프(PG index AM은 SQL LIMIT를 모르고 executor가 LIMIT 적용 — over-fetch가 자연스러운 PG 관용구), 명시적 `cuvs.delta_slop` GUC(자동 tombstone 카운트로 불필요).

### 결정 2 — delta_search GUC: 정수 → enum 전환

`cuvs.delta_search`는 원래 `DefineCustomIntVariable`(정수 0=auto/1=cpu/2=gpu)인데 PLAN·ROADMAP 산문은 `auto|cpu|gpu`(문자열)로 기술돼 `SET cuvs.delta_search='auto'`가 `invalid value` ERROR였다. 1차(PR #18)에선 문서를 int(0/1/2)로 정정하고 enum 전환은 후속으로 뒀으나, **곧바로 enum으로 완전 전환**(후속 PR): 형제 GUC `cuvs.search_mode`(enum)와 일관되고 산문이 그대로 참이 되며, 미릴리스·미채택 상태라 정수 인터페이스 파괴 비용이 ≈0인 지금이 가장 싼 시점.

- **변경**: `DefineCustomEnumVariable("cuvs.delta_search", ..., cuvs_delta_search_options={auto,cpu,gpu}, 기본 auto)`. 백업 변수 `cuvs_delta_search_mode`는 int 유지(enum이 int*에 저장) → 사용처(`do_cpu` 로직) 무수정.
- **파괴(의도적, 잔재 없음)**: `SET cuvs.delta_search = 0|1|2`(정수)는 이제 `invalid value` ERROR. 정수 표기는 소스·테스트·문서에서 전량 제거(grep + subagent + 런타임 `pg_settings` 3중 검증).
- **무관 별개**: SRF 출력 컬럼·IPC `delta_search_mode`(0=none/1=cpu/2=gpu, 마지막 검색이 쓴 모드)는 GUC와 다른 개념 — text(gpu/cpu/none)로 렌더되며 그대로 둔다.
- **양형 동시 수용(string GUC+check_hook)은 기각**: 정수 호환을 위한 우회지만, 미채택 상태에선 깔끔한 enum이 더 단순·표준. 기존 `=1` 호환 가치 < 추가 복잡도.

### 결과 (검증, VM A100/PG16, 2026-06-06)

- **코드**: `src/pg_cuvs.c` — `cuvs_tombstone_count` + `cuvs_gettuple` over-fetch(커밋 `aaeb2da`). delete-drift recall `live_topk` 0(pre)→5(post).
- **회귀(`make installcheck` 15/15 GREEN)**: `pending_delta`(cap overflow fail-closed, Sc15 resident GPU delta cache built=`delta_rows`/`delta_merged_count`/`delta_vram_bytes` 프록시, tri-mode 0/1/2 정합, `max_delta_rows=0` Phase2 복귀), `delta_recall`(over-fetch red→green).
- **격리(`make installcheck-isolation` 2/2 GREEN, pg_isolation_regress 신규 도입)**: 동시 DELETE 가시성(old RR snapshot 유지·fresh snapshot 제외), 미커밋 delta cross-session 격리. (주의: tombstone delete_xid의 snapshot-**보호** 분기는 VACUUM이 OldestXmin 통과 후에만 tombstone을 쓰므로 순수 격리로는 도달 불가 — heap recheck가 해당 창에서 부하 담당. 스펙 주석에 명시.)
- **e2e(`gpu-test-delta-restart` PASS)**: valid `.delta` 데몬 재시작 생존(GPU delta cache 재빌드) + corrupt `.delta` fail-closed(validity gate → CPU reroute, 정답 유지).
- **단위(`make test-unit` GREEN)**: `.tombstone` 헤더 포맷(round-trip + bad magic/version/reserved/음수 거부).
- **운영 교훈**(memory 후보): `pg_cuvs`가 `shared_preload_libraries`라 `.so` 재설치 후 backend가 구코드 유지 — extension `.c` 변경 시 **PostgreSQL 재시작 필수**(데몬 별개). VM↔로컬 빌드는 expected `.out`을 VM에서 생성·회수, `rsync --delete`가 VM 전용 파일을 지움에 주의.

---

## ADR-048 — Phase 3O: Pre-filter ANN (필터 검색)

**날짜**: 2026-06-06 (분석·보류 2026-06-07, 구현 완료 2026-06-08)
**상태**: 완료 (2026-06-08) — CAGRA-first BITSET prefilter. PR #36(BF prefilter), #37(CAGRA-first 업그레이드).

### 구현 요약 (2026-06-08)

접근 B(custom scan + TID→bitset)의 "qual 미전달 장벽"은 `cuvs_filtered_knn` SRF가 이미 호출자로부터 `ctid[]`를 받는 구조로 우회됐다. D(post-filter, PR #35)의 인프라(`CuvsCmdFrame.filter_shm_key`, daemon filter 경로, `heapTID→item_id` 역방향 맵)를 그대로 재활용해 prefilter를 추가 구현:

- **역방향 맵**: daemon 빌드 타임에 `rev_tids[]`(sorted) + `rev_item_ids[]` 구성. 쿼리 타임에 필터 TID를 이진탐색으로 item_id로 변환.
- **BITSET 생성**: cuVS 규칙 `bit=1=exclude` 기준 all-ones 초기화 후, 필터 item_id 비트만 클리어.
- **CAGRA-first 전략**: `handle_search` 3O 경로는 `e->handle != NULL`이면 CAGRA + bitset_filter 먼저 시도(`cuvs_cagra_search_filtered`). 실패 시 BF prefilter fallback(`cuvs_bf_search_filtered`). 실패 시 D-wedge fallback.
- **GUC**: `cuvs.filter_auto_threshold`(기본 0.05). selectivity < threshold → use_prefilter=1, else D-wedge.
- **search_mode**: 4=cagra_prefilter, 3=bf_prefilter(fallback).

**성능 특성**:

| 경로 | 알고리즘 | Recall | 탐색 복잡도 |
|------|---------|--------|-------------|
| D-wedge | BF 전체 + post-filter | <1.0 @ 저 sel | O(N) |
| BF prefilter | BF + bitset | 1.00 (exact) | O(filter\_N) |
| CAGRA prefilter | CAGRA graph + bitset | ~0.95+ (approx) | O(log filter\_N) |

**검증**: installcheck 19/19 + isolation 2/2 GREEN. filter_comparison golden 변경 없음(소규모 테스트 데이터셋에서 CAGRA recall=1.00).

**문제**: 현재 pg_cuvs는 post-filter만 지원한다 — GPU가 top-k 후보를 반환한 뒤 PG executor가 WHERE 조건으로 recheck. WHERE 절 선택성이 높으면 GPU 후보 대부분이 recheck에서 탈락해 IPC 왕복과 VRAM 작업이 낭비된다. 예: `WHERE category = 'A' ORDER BY embedding <-> $1 LIMIT 10`에서 category='A'가 5%이면 GPU 후보 200개 중 190개가 탈락.

### 설계 분석 (2026-06-07, 착수 직전 코드 확인)

**핵심 장벽 (원안이 놓친 부분)**: 원안은 "backend가 filter 조건을 비트맵으로 평가"한다고 했으나, **PG 인덱스 AM API는
인덱스 컬럼이 아닌 WHERE qual을 AM에 넘겨주지 않는다.** `cuvs_gettuple`(src/pg_cuvs.c:2242)은 `scan->orderByData[0]`
(쿼리 벡터)만 받고, `WHERE category='A'` 같은 다른-컬럼 qual은 ScanKey로 전달되지 않아 **executor가 heap row에
post-filter로 적용**한다. 즉 "WHERE→bitvector→daemon"을 하려면 AM이 qual을 알아야 하는데 그 hook이 없다. 비트맵을
만들려면 custom scan node/planner hook으로 qual을 포착하고, 매칭 heap TID를 corpus position으로 매핑해야 한다.

이로써 두 접근의 스코프가 ~10× 갈린다:

- **접근 A — iterative over-fetch (pragmatic, pgvector `iterative_scan`류)**: executor가 필터를 유지하고,
  `cuvs_gettuple`이 버퍼 소진 시 k를 키워 재검색해 `LIMIT` 충족까지 결과를 공급한다. 필터/bitset/custom-scan/cuVS
  filtered API **모두 불요** — `cuvs_gettuple` 한 곳 + cap GUC. ~1세션·저위험. '진짜 pre-filter'는 아니나
  (post-filter + on-demand over-fetch) **사용자 문제(고선택성 필터 시 결과 부족)를 해결**한다.
- **접근 B — true bitvector pre-filter (원안)**: custom scan node/planner hook으로 qual 포착 → corpus position
  bitset(TID→position 매핑) → `CuvsCmdFrame.filter_shm_key` → daemon이 cuVS filtered CAGRA search. 최고 품질이나
  **아키텍처 침습적·멀티세션·고위험**. cuVS bitset filter API 검증 선행(현재 미검증). GUC `cuvs.prefilter_threshold`
  (밀도 상한)로 고선택성만 pre-filter, 저선택성은 post-filter(저선택성에서 graph traverse 품질 저하 회피).

**결정 (보류, 2026-06-07)**: 게이트 난이도가 원안 추정 대비 크고(qual 미전달 장벽 → 스코프 10×), 고선택성 필터
워크로드의 실수요도 미확인이라 **보류**. 재개 시 ROI상 **접근 A부터**(대부분 가치를 저비용에) 착수하고, 품질이 실제로
요구될 때 B를 검토. **트리거**: 고선택성 필터 + 벡터 검색 워크로드가 실제 문제로 제기되는 시점.

**대안 기각** (B 내부): post-filter + K 과다 요청(선택성 높을수록 K 추정 불안정), 항상 pre-filter(저선택성 품질 저하).

---

## ADR-049 — Phase 3P: IVF-PQ 및 추가 cuVS 알고리즘

**날짜**: 2026-06-06
**상태**: 수락됨 — 구현 완료 (installcheck 20/20 PASS, 2026-06-08)

**문제**: CAGRA는 VRAM에 float32 원본 벡터 전체를 유지해야 한다 (N=1M, dim=1024 → ~4 GB). 1B+ 벡터 또는 VRAM 제약 환경에서 비실용적이다. IVF-PQ(Inverted File Index + Product Quantization)는 벡터를 압축 코드로 대체해 VRAM을 10–100× 절감하되 recall 트레이드오프가 있다.

**결정**: cuVS IVF-PQ를 새 PG access method `ivfpq`로 등록한다 (`CREATE INDEX USING ivfpq`). CAGRA AM과 완전히 독립된 경로로 구현하되, 동일한 UDS+shm IPC 인프라를 재사용한다.

**구현 방향**:
- 새 AM handler `pg_cuvs_ivfpq_handler` 등록.
- reloption: `n_lists`(IVF 클러스터 수, 기본 1024), `pq_bits`(코드워드 비트, 기본 8), `pq_dim`(서브공간 수, 기본 dim/2).
- GUC: `cuvs.ivfpq_n_probes`(탐색 클러스터 수, 기본 64).
- `CUVS_OP_BUILD_IVFPQ`, `CUVS_OP_SEARCH_IVFPQ` op 추가.

**알고리즘 선택 가이드** (문서화):

| 알고리즘 | 적합 규모 | VRAM | Recall |
|----------|-----------|------|--------|
| CAGRA | ~10M 이하 | 높음 | 최고 |
| IVF-PQ | 100M+ | 낮음 | 중간 |

**대안 기각**: 단일 AM에서 알고리즘 선택 reloption — build/search 시그니처가 달라 AM 레벨 추상화 비용이 큼.

**트레이드오프**: 두 AM 동시 유지로 daemon·테스트 복잡도 증가. IVF-PQ recall은 `n_probes` 튜닝에 민감.

---

## ADR-051 — Phase 3Q: CAGRA Streaming Updates (cuvsCagraExtend + cuvsCagraMerge)

**날짜**: 2026-06-06
**상태**: 설계 단계

**배경**: cuVS 26.04 C API에 다음 세 함수가 이미 완전히 노출되어 있음을 VM 헤더 검증으로 확인 (2026-06-06):
- `cuvsCagraExtend(res, params, additional_dataset, index)` — full rebuild 없이 VRAM 내 CAGRA에 새 벡터 추가
- `cuvsCagraMerge(res, params, indices, num_indices, filter, output_index)` — 여러 인덱스 병합 + `cuvsFilter`로 삭제 벡터 제거
- `cuvsCagraExtendParams.max_chunk_size` — throughput vs recall 튜닝 (0=auto)

**문제**: 현재 3A delta는 INSERT를 `.delta` 파일에 누적하고 search-time에 CPU/GPU로 병합한다. delta가 쌓일수록 검색 레이턴시가 증가하고, DELETE는 tombstone + over-fetch workaround로만 처리된다. cuVS C API가 이미 더 나은 경로를 제공하는데 활용되지 않고 있다.

**결정**: INSERT/UPDATE/DELETE를 cuVS native API로 처리한다.
- **INSERT**: `cuvsCagraExtend`로 VRAM 내 CAGRA 인덱스에 직접 추가 — `.delta` 파일 불필요
- **DELETE**: tombstone bitvector 유지 → 주기적 `cuvsCagraMerge(filter=tombstone)` 로 dead 벡터를 인덱스에서 제거
- **UPDATE**: Extend(신규) + tombstone(구)
- 3A `.delta` CPU 병합 경로는 deprecated(3Q 완료 후 제거)

**구현 방향**:
- `CUVS_OP_EXTEND`: IPC op 추가. backend가 새 벡터를 shm에 기록, daemon이 `cuvsCagraExtend` 호출 후 disk serialize(내구성).
- `CUVS_OP_COMPACT`: `cuvsCagraMerge(filter=tombstone bitvector)` → 새 인덱스로 atomic swap → old VRAM 해제.
- GUC: `cuvs.extend_chunk_size`(`max_chunk_size` 제어, 0=auto).
- VRAM 예산: extend로 인덱스가 grow하므로 `vram_bytes` 추적 갱신.
- `aminsert`가 IPC EXTEND를 호출하도록 변경 (현재: delta file append).

**3A와의 관계**: 3Q 완료 후 `.delta` 파일 경로 deprecate. 3A tombstone 메커니즘은 유지 (COMPACT op의 filter 소스로 재활용). 4C background compaction은 `CUVS_OP_COMPACT`를 자동 트리거하는 상위 레이어가 된다.

**트레이드오프**:
- Extend 후 serialize → INSERT마다 disk I/O. 배치 처리(`aminsert` 버퍼링)로 완화 가능.
- VRAM grow: 대량 INSERT 후 eviction 전까지 VRAM 압박. extend 전 예산 체크 필요.
- `max_chunk_size=0`(auto)이면 cuVS가 최적 청크 자동 선택.

**대안 기각**: 현행 3A delta 유지 — cuVS native API 대비 search-time 병합 비용, 코드 복잡도, recall 저하 모두 열등.

---

## ADR-052 — Phase 3R: CAGRA 빌드 파라미터 reloption

**날짜**: 2026-06-07
**상태**: 구현·검증 완료 (2026-06-07)

> **구현 결과 (설계 대비 정정)**:
> - `build_algo`는 소문자 enum **`auto | ivf_pq | nn_descent`, 기본 `auto`**(설계의 `IVF_PQ` 강제 대신 cuVS 휴리스틱 — 대용량 ivf_pq / 소량 nn_descent 자동 선택이 더 안전). DDL-time validator로 오타 fail-closed.
> - cuVS **26.04는 `build_algo` enum이 아니라 `cagra::index_params::graph_build_params` std::variant**(`ivf_pq_params(extents)` / `nn_descent_params(igd)` / `std::monostate`=auto). wrapper에서 이 variant로 매핑.
> - `intermediate_graph_degree >= graph_degree`(cuVS 요구)를 빌드 전 ereport로 fail-closed. range: graph_degree 8–512, intermediate 8–1024.
> - 전달 경로: reloption → `cuvs_resolve_build_params_rel` → `CuvsCmdFrame`(graph_degree/intermediate_graph_degree/build_algo, wire ABI co-deploy) → 데몬 → `cuvs_cagra_build{,_multi}`. HNSW ephemeral 빌드는 cuVS 기본값 전달(사용자 튜닝 미상속). `build_algo=0`/degree<=0 → 기본 경로 무변경.
> - **검증(VM A100/PG16)**: reloption 파싱+카탈로그 durability; build_algo 3종 빌드+검색; validation fail-closed; **파라미터 실적용 실증** — `.cagra` adjacency 크기 Δ = n_vecs × Δgraph_degree × 4 bytes(graph_degree 8 vs 256: 0.80MB vs 5.76MB, Δ 정확히 4.96MB); installcheck **16/16**(신규 `build_params`) + isolation 2/2. `build_params`는 REGRESS 맨 끝(gc_orphans의 extension-presence 골든 비교란).

**문제**: CAGRA 빌드 파라미터(`graph_degree`, `intermediate_graph_degree`, `build_algo`)가 cuVS 기본값에 고정되어 있다. 사용자가 recall↔build-time 트레이드오프를 제어할 수 없다. 4A preflight 측정(`docs/profiling-results.md`) 기준 GPU build ~68s가 천장인 상황에서 `graph_degree` 축소(32→16)로 build time과 VRAM을 모두 절감할 수 있으나 현재 수단이 없다.

**결정**: `CREATE INDEX ... WITH (graph_degree=..., build_algo=...)` reloption으로 핵심 빌드 파라미터를 노출한다.

| reloption | 타입 | cuVS 기본값 | 설명 |
|-----------|------|-------------|------|
| `graph_degree` | int | 64 | 그래프 연결도 — 높을수록 recall 향상, VRAM·build time 증가 |
| `intermediate_graph_degree` | int | 128 | build 중간 그래프 크기 — 낮추면 build 속도 향상, recall 소폭 저하 |
| `build_algo` | enum | `IVF_PQ` | `IVF_PQ`(기본) 또는 `NN_DESCENT` |

**구현 방향**:
- `cuvs_relopts` 파싱에 3개 항목 추가.
- `CuvsCmdFrame`(또는 별도 build params struct)에 파라미터 포함, daemon의 `cuvsCagraIndexParams` 직접 설정.
- reloption 미지정 시 cuVS 기본값 통과 (0/NULL → default).

**트레이드오프**:
- `graph_degree` 과소 설정 시 recall 저하 위험 → 문서에 권장 범위 명시.
- 파라미터 변경 후 REINDEX 필요 (reloption은 인덱스 생성 시 1회 소비).

---

## ADR-053 — Phase 3S: statement_timeout / 취소 전파

**날짜**: 2026-06-07
**상태**: 구현·검증 완료 (2026-06-07)

**문제**: PG의 `statement_timeout`/query cancel이 daemon IPC로 전파되지 않았다. backend의 `cuvs_ipc_search`는
reply recv에서 `SO_RCVTIMEO`(30s) + EINTR-retry로 막혀 `CHECK_FOR_INTERRUPTS`가 없어, 걸린 GPU 검색이
`statement_timeout` 후에도 backend를 무기한 잡아 연결 고갈을 유발했다. **별도로 데몬에 SIGPIPE 핸들러가 없어**,
client가 reply 도중 끊으면(닫힌 소켓에 `write`) 데몬 **전체가 죽어** 모든 backend의 GPU 인덱스를 잃을 수 있었다(잠재 버그).

**결정 (구현)**: 두 계층.
- **backend (interruptible wait)**: `cuvs_ipc.c`에 `recv_all_interruptible`(reply 헤더 대기를 `poll` ~250ms
  슬라이스로 쪼개고, 등록된 wait 콜백으로 pending cancel 감지; 전체 30s budget). 취소 시 소켓을 닫고
  `CUVS_STATUS_CANCELED`(11) 반환. 콜백(`cuvs_search_wait_should_abort`)은 `QueryCancelPending`/`ProcDiePending`만
  검사하고 **longjmp 금지**(PG-free 라이브러리가 소켓/shm을 못 정리하면 누수) — PG 호출자가 `cuvs_gettuple`에서
  반환 후 `CHECK_FOR_INTERRUPTS`로 실제 인터럽트를 raise.
- **daemon (생존)**: `signal(SIGPIPE, SIG_IGN)` — 끊긴 client에 reply `write`가 `EPIPE(-1)`로 실패 → per-connection
  thread가 정리(데몬 생존). 취소된 client의 in-flight GPU 검색은 완료되나 결과는 폐기(소켓 닫힘).

**설계 대비 정정**:
- **`CUVS_OP_CANCEL` 미도입**: cuVS 검색은 커널 중간 취소 불가 → proactive cancel op 대신 **소켓 close + SIGPIPE
  무시**로 정리. 더 단순하고 충분.
- **`cuvs.ipc_timeout_ms` GUC 미도입**: 30s budget + 250ms poll 하드코딩(필요 시 후속 GUC화).
- **SIGPIPE 무시는 설계에 없던 핵심 추가** — 그 자체로 기존 잠재 데몬-크래시 버그를 닫음.

**결과 (VM A100/PG16, integration sc24)**: `statement_timeout=500ms`가 3s-지연 검색을 **~544ms에 취소**(이전엔
무기한/30s) + 데몬이 그 disconnect를 견디고 후속 검색을 정상 서빙. 전체 integration suite GREEN, installcheck
17/17(정상 검색 무회귀).

**한계/트레이드오프**: 취소된 검색의 GPU 작업은 끝까지 돌고 결과만 폐기(VRAM 낭비는 있으나 연결 고갈 방지가 목적).
build IPC(`recv_all`, 600s)는 아직 비인터럽트 — 긴 CREATE INDEX 취소는 후속(같은 콜백 메커니즘 재사용 가능).

---

## ADR-054 — fp16 입력 벡터 (트리거 백로그)

**날짜**: 2026-06-07
**상태**: 트리거 대기 — cuVS C API fp16 지원 확인 필요

**배경**: CAGRA는 현재 float32 입력만 사용한다. cuVS Python binding은 fp16 입력을 지원하나, C API(`cuvsCagraBuild`, `cuvsCagraSearch`)의 half-precision input 지원 여부는 VM 헤더 미확인 상태다.

**결정(조건부)**: cuVS C API가 fp16 입력을 지원하는 것이 확인되면 `CREATE INDEX ... WITH (precision=fp16)` reloption을 추가한다.

**기대 효과**:
- VRAM 사용량 float32 대비 ~50% 절감 (벡터 데이터 부분 한정).
- 동일 VRAM 예산에서 인덱스 크기 2× 향상.
- recall 저하 여부는 데이터셋 의존 — 벤치마크 필수.

**트리거**: VM에서 `cuvsCagraBuild`의 `dataset` 인자에 CUDA_R_16F dtype 전달 가능 여부 확인 + recall 저하 < 0.5% 실측.

---

## ADR-055 — EXPLAIN ANALYZE GPU 타이밍 (트리거 백로그)

**날짜**: 2026-06-07
**상태**: 트리거 대기 — 명시적 진단 수요

**배경**: `pg_stat_gpu_search`는 누적 통계를 제공하지만 `EXPLAIN (ANALYZE, BUFFERS)` output에 GPU kernel time / IPC latency가 표시되지 않는다. 쿼리별 병목 진단이 불가능하다.

**결정(조건부)**: PG custom scan node에 GPU 타이밍을 노출한다.

**구현 방향**:
- daemon 응답 프레임에 `gpu_kernel_us`, `ipc_roundtrip_us` 필드 추가.
- backend custom scan의 `ExplainCustomScan` 콜백에서 타이밍을 `EXPLAIN` output에 주입.
- `InstrStartNode`/`InstrStopNode`로 PG 표준 타이밍과 분리.

**트리거**: 프로덕션 배포 후 쿼리별 GPU latency 분해 수요가 명시적으로 제기되는 시점. ADR-044가 이미 외부 측정 완료 — SQL 노출 한계가치는 낮으나 제품 완성도에 기여.

---

## ADR-056 — VACUUM 연동 tombstone 정리 (트리거 백로그)

**날짜**: 2026-06-07
**상태**: 트리거 대기 — Phase 3Q 완료 후

**배경**: 현재 DELETE tombstone은 4C bgworker(auto REINDEX) 또는 수동 REINDEX로만 제거된다. PG autovacuum이 heap dead tuple을 제거할 때 인덱스 AM의 `ambulkdelete` hook이 호출된다. 이 hook에서 `CUVS_OP_COMPACT`(3Q 구현)를 트리거하면 별도 bgworker 없이 PG autovacuum 스케줄을 재활용할 수 있다.

**결정(조건부)**: `ambulkdelete`에서 tombstone 비율이 `cuvs.compact_on_delete_ratio`를 초과하면 `CUVS_OP_COMPACT`를 동기 호출한다.

**4C와의 관계**: 4C(bgworker auto-REINDEX)는 full REINDEX 방식, VACUUM 연동은 incremental compaction(3Q의 `cuvsCagraMerge`). 두 메커니즘은 동일 `CUVS_OP_COMPACT`를 공유하므로 병행 가능하다. VACUUM 연동은 4C보다 자연스러운 PG 통합이지만, `ambulkdelete`가 동기 호출이라 VACUUM이 느려지는 부작용 있음 — 비동기 dispatch 또는 threshold 설계가 필요.

**트리거**: 3Q 완료 + autovacuum 중 tombstone 지연 정리가 실측 문제로 확인되는 시점.

---

## ADR-060 — Subtransaction-aware DROP 수집 (RegisterSubXactCallback)

> *(2026-06-07 번호 정정: 본 ADR은 ADR-057로 작성됐으나 빌드 corpus memfd 핸드오프 ADR-057과 충돌해 ADR-060으로 이동. memfd ADR-057이 코드·ROADMAP·PLAN 전반에서 참조되는 정본이라 그쪽을 유지.)*

**날짜**: 2026-06-07
**상태**: 구현·검증 완료 (2026-06-07) — 당초 "트리거 대기 보류"였으나 트랜잭션 의미론 위반 버그라 즉시 구현.

**문제**: `SAVEPOINT s; DROP INDEX; ROLLBACK TO s; COMMIT;` 패턴에서 `object_access_hook`이 OAT_DROP을 `cuvs_pending_drops`에 수집할 때 어느 subtransaction에서 발생했는지 기록하지 않았다. SAVEPOINT가 롤백돼도 OID가 리스트에 남아 상위 commit 시 `cuvs_ipc_drop`이 발사 — **살아있는 인덱스의 GPU artifact가 조기 삭제**(REINDEX로만 복구). PostgreSQL은 `ROLLBACK TO s`로 인덱스를 보존하는데 pg_cuvs만 어겼다. (`BEGIN; DROP INDEX;` 미커밋은 안전 — `XACT_EVENT_COMMIT`에서만 발사.)

**결정**: `cuvs_pending_drops` 각 엔트리를 `(Oid, SubTransactionId)`로 태깅하고 `RegisterSubXactCallback`로 subxact 생애를 추적한다.

**구현**:
- 수집 시 `GetCurrentSubTransactionId()`를 함께 기록(`CuvsPendingDrop{oid, subid}`).
- `SUBXACT_EVENT_ABORT_SUB(mySubid)`: `subid == mySubid` 엔트리 제거(롤백된 subxact의 drop 폐기).
- `SUBXACT_EVENT_COMMIT_SUB(mySubid → parentSubid)`: `subid == mySubid` 엔트리를 부모로 **reparent**(RELEASE 후 상위가 결정할 때까지 생존). *이 reparenting은 당초 설계("ABORT_SUB level 이상 제거")가 빠뜨린 부분 — 중첩(SAVEPOINT 안 SAVEPOINT + RELEASE + 바깥 ROLLBACK) 정확성에 필수.*
- commit된 top 트랜잭션의 `XACT_EVENT_COMMIT`에서만 `cuvs_ipc_drop` 발사.

**결과 (VM A100/PG16, `pg_stat_gpu_search` 잔존 = daemon 보유)**:
- `ROLLBACK TO savepoint`: resident=1(생존) + 카탈로그 보존
- 중첩 RELEASE + 바깥 ROLLBACK: resident=1(reparent 후 폐기)
- RELEASE + top COMMIT: resident=0(실제 drop 발사)
- 평범한 top-level DROP: resident=0(무회귀)
- installcheck **17/17**(신규 `drop_subxact`, REGRESS 맨 끝) + isolation 2/2 GREEN.

---

## ADR-050 — Phase 4C: Background Compaction + CREATE INDEX CONCURRENTLY 정합성

**날짜**: 2026-06-06
**상태**: 설계 단계

**문제 1 — 수동 REINDEX 운용 부담**: delta가 `delta_cap`을 초과하면 fail-closed. 쓰기가 많은 워크로드에서 DBA가 delta 크기를 모니터링해 REINDEX 타이밍을 수동으로 잡아야 한다.

**문제 2 — CREATE INDEX CONCURRENTLY DELETE 정합성**: `cuvs_ambuildempty`가 no-op이고, CONCURRENTLY 흐름에서 DELETE가 발생했을 때 tombstone이 올바르게 처리되는지 명시적으로 설계·검증하지 않았다. 이론적으로는 delta CRC 무효화로 문제가 없지만 테스트 커버리지가 없다.

**결정**:
1. **Background worker auto-compaction**: PG bgworker가 `pg_stat_gpu_search`를 폴링해 `delta_rows > cuvs.auto_compact_threshold` 인덱스에 대해 `REINDEX INDEX CONCURRENTLY`를 자동 실행.
2. **CONCURRENTLY DELETE 정합성 검증**: DELETE가 섞인 concurrent build `pg_isolation_regress` 시나리오 추가. 필요 시 `cuvs_ambuild` 시작 시점에 기존 delta/tombstone 명시적 무효화 경로 추가.

**구현 방향**:
- `_PG_init`에서 `cuvs_compaction_worker` bgworker 등록.
- GUC: `cuvs.auto_compact = on|off`(기본 off), `cuvs.auto_compact_check_interval`(기본 60s), `cuvs.auto_compact_threshold`(기본 base의 10%).
- `pg_stat_gpu_search`에 `last_compact_at`, `compact_count` 컬럼 추가.

**전제 조건**: REINDEX CONCURRENTLY가 pg_cuvs AM에서 올바르게 동작함을 먼저 검증 (4C-0 선행).

**대안 기각**: daemon 측 compaction — daemon은 PG heap에 접근 불가. autovacuum 통합 — AM별 커스텀 훅 없어 별도 bgworker가 더 단순.

---

## ADR-057 — 빌드 corpus 핸드오프: memfd + SCM_RIGHTS 하이브리드 (누수-안전 + 무복사)

**날짜**: 2026-06-07
**상태**: 구현·검증 완료 (PR 진행)
**관련**: ADR-034 §4A-1(double memcpy 제거 — 본 ADR이 대체), ADR-044(프로파일링), ADR-046(orphan GC)

### 배경

CAGRA 빌드는 backend가 corpus(vectors+tids)를 모아 GPU 데몬(별도 프로세스)에 넘긴다. 기존 경로는
corpus를 **heap + 이름있는 shm 복사본으로 이중** 보유(`shm_write_build_payload`의 memcpy×2). 두 문제:
(1) 큰 corpus에서 peak RSS가 2×corpus(대규모 OOM 위험), (2) 크래시(SIGSEGV/SIGKILL/OOM-killer) 시
`/dev/shm/pg_cuvs_bld_*`가 **고아 누수**(`PG_FINALLY`는 신호에 안 돎). 단순 shm 직접할당(ADR-034 §4A-1)은
(1)은 해결하나 (2)가 남아 단순-shm 접근은 revert됨.

### 결정: 3-tier corpus, 기본은 익명 memfd + fd 전달

`cuvs_build_corpus.{c,h}`(PG-free)가 corpus 버퍼를 추상화한다. 런타임 tier 선택:

| Tier | 메커니즘 | 누수 방지 |
|------|---------|----------|
| **T1 memfd**(기본) | `memfd_create` + `SCM_RIGHTS`로 **fd 자체**를 데몬에 전달 | 이름 없음 → 고아 불가. 커널이 모든 죽음(SIGKILL 포함)에 fd/mmap 회수 |
| **T2 named-shm** | `shm_open` + `flock(LOCK_EX)`(best-effort) | flock 기반 reaper가 죽은-주인만 회수(`_PG_init` sweep) |
| **T3 heap** | 기존 heap + `shm_write_build_payload` | 프로세스 종료 회수 |

memfd 불가(구커널/seccomp) 시 T2, shm_open도 불가 시 T3로 graceful degrade. 모든 tier가 데몬의
`[vectors][tids]` 연속 레이아웃을 동일 생성. tids는 항상 작은 heap 버퍼(스캔 후 finalize에서 append).

데몬 `handle_build`: index_dir recv를 `recvmsg`로 바꿔 ancillary fd 동시 수신 — fd 있으면 `mmap(fd)` 후
즉시 `close`(매핑이 메모리 유지), 없으면 기존 `shm_open(shm_key)`. 평문 send에도 호환(하위호환).

backend는 corpus를 `PG_TRY/PG_FINALLY`로 감싸 정상·ERROR·취소 경로에서 `cuvs_corpus_close`(munmap/close
/+unlink) 보장. **memfd는 그 위에 비정상 종료까지 커널이 보장** — PG_FINALLY가 못 도는 SIGKILL/FATAL에서도
누수 0. `_PG_init`는 (a) 재시작 시 T2 dead-owner 고아 sweep, (b) `CUVS_FORCE_CORPUS` 테스트 seam.

### 결과 (VM A100/PG16 실측)

- **누수-안전(핵심)**: memfd × {cancel, terminate(FATAL), SIGKILL, SIGSEGV, daemon-kill} 전부 **고아 0 +
  Shmem 기준선 복귀**(`/proc/meminfo Shmem` = memfd 익명 + named shm 통합 회계). SIGKILL ×12 soak에서
  누적 0. T2 reaper는 죽은-주인 회수·산-주인 보존(unit + `_PG_init` sweep 실증).
- **오버헤드(north-star)**: N=500k×1024 빌드 39.2s 중 GPU build ~33s, **backend 오버헤드 ~6s**(scan+
  detoast+fill+무복사 IPC). memfd 핸드오프가 **copy 오버헤드를 ~0**으로(데몬이 corpus를 직접 mmap). 잔여
  ~6s는 detoast+heap scan(PLAIN storage·4A-2 parallel 영역). peak RSS −32%(이중버퍼 제거; 단순-shm A/B와 동일).
- **무회귀**: installcheck 15/15 + isolation 2/2 GREEN(memfd tier).

### 대안 기각

- **단순 named-shm 직접할당**(ADR-034 §4A-1): RSS는 해결하나 크래시 고아 누수 잔존. memfd가 구조적으로 제거.
- **flock reaper만으로 named-shm 안전화**: 가능하나 (탐지·회수) 방식이고 /dev/shm 의존·컨테이너 64MB 제약
  잔존. memfd는 (발생-불가) 방식이고 익명이라 컨테이너 제약도 없음. reaper는 memfd-불가 fallback 전용으로 유지.
- **PG DSM**: POSIX shm 위 PG 관리이나 외부 데몬이 이름으로 여는 use-case에 부적합(PG 내부 결합).

> **검증 하네스**: `infra/scripts/leak-verify.sh`(death-mode 매트릭스 + soak + 음성대조). 단위:
> `test/unit/test_build_corpus.c`(golden byte-identity, memfd refcount, flock 산/죽음 판별).

---

## ADR-058 — 빌드 병렬화: parallel maintenance workers (ADR-034 §4A-2 구현)

**날짜**: 2026-06-07
**상태**: 구현·검증 완료
**관련**: ADR-034 §4A-2(계획), ADR-057(memfd corpus — 최종 핸드오프 재사용)

### 배경

ADR-057이 copy 오버헤드를 ~0으로 만든 뒤 남은 backend 오버헤드는 **heap scan + per-tuple detoast**(직렬).
north-star(raw cuVS 대비 오버헤드 0)를 위해 이를 PostgreSQL parallel index build로 병렬화한다.

### 결정

`cuvs_build_cagra_from_heap`이 `plan_create_index_workers()`로 worker를 받으면(표준
`max_parallel_maintenance_workers` 게이트; CONCURRENTLY·미선언dim 제외) `cuvs_build_parallel`로 분기한다
(nbtsort.c 패턴). 각 참가자(리더+워커)가 공유 `ParallelTableScanDesc`로 **디스조인트 share**를 스캔해 자기
**named-shm(T2) partial corpus**(`[vecs][tids]`)를 만들고 DSM 슬롯에 (n_vecs, reltuples, shm_name) 기록.
리더는 partial들을 **memfd 최종 corpus로 연접 merge**(vec/tid region별, 정렬 불요)해 데몬에 전달 후 partial을
unlink. `cuvs_corpus_detach`로 워커가 segment를 이름으로 리더에 넘긴다. workers=0이면 단일 경로(byte-identical).

**정합 근거**: CAGRA 순서 독립 + 데몬 sharding이 position 기준 + (vectors[i],tids[i]) position pairing →
연접이 pairing 보존. worker partial = T2(fd-passing 불가, DSM은 fd 채널 아님); 최종 = memfd(누수-안전).
worker 크래시 시 partial orphan은 ADR-057 flock reaper가 회수.

### 결과 (VM A100/PG16)

- **정합(핵심)**: 고유-벡터 100k×128에서 self-NN(벡터=자기 최근접) **단일 5/5 = 병렬(4 workers) 5/5**. merge가
  (vec,tid) pairing 보존. installcheck 15/15 + isolation 2/2 무회귀(소형 테이블은 worker 0 → 단일 경로).
- **오버헤드(north-star)**: bench_500000(dim1024) backend **~6.2s(w0) → ~4.5s(w4), −27%**(total 39.2→36.5s,
  GPU floor 지배라 wall-clock marginal). **merge가 병목**: 분산스캔으로 스캔은 ~5배 빨라지나(~1.2s) 리더의
  merge 복사(partial→최종 memfd, corpus 2-pass, ~3s)가 절감분을 대부분 먹음. merge 복사가 4A-2 이득의 상한
  (ADR-057이 없앤 복사를 부분 재도입). 추가 절감 = 데몬이 worker partial 다중 mmap(merge 제거) 또는 PLAIN(detoast 제거).

### 대안 기각 / 한계

- **worker partial을 memfd로 + fd 전달**: PG parallel worker↔leader는 소켓이 없어 SCM_RIGHTS 불가 → named-shm 채택.
- **데몬이 worker별 다중 shm을 직접 mmap**(merge 복사 제거): 데몬 프로토콜·sharding 대수술 → 보류. merge 복사가
  4A-2 이득의 상한. 추가 절감은 **PLAIN storage**(detoast 제거, ADR-043)와 결합이 직교적으로 가장 큼.

## ADR-059 — 빌드 merge 복사 제거: 데몬 multi-partial direct H2D (ADR-058 §4A-2b)

**날짜**: 2026-06-07
**상태**: 구현·검증 완료
**관련**: ADR-058(parallel workers — merge가 병목), ADR-057(corpus/reaper 재사용), ADR-043(PLAIN — 직교 보완)

### 배경

ADR-058 병렬 빌드의 상한은 **리더 merge 복사**였다: 워커별 named-shm partial을 리더가 최종 memfd corpus로
연접(corpus 2-pass, ~3s/~4GB host 복사)해 데몬에 넘겼다 — ADR-057이 없앤 복사를 부분 재도입. ADR-058이 보류로
남긴 "데몬이 worker별 다중 shm을 직접 mmap"을 구현해 merge를 제거한다.

### 결정

리더는 partial을 merge하지 않고 **N개 descriptor(shm_name, n_vecs) 리스트**를 데몬에 전달한다
(`cuvs_ipc_build_multi`, `CuvsCmdFrame.n_partials`; 0이면 기존 단일 corpus 경로 byte-identical). 데몬
`handle_build_multi`는 리스트를 **검증**(Σn_vecs==total, 이름 `/pg_cuvs_bld_*` 형식, 각 shm 크기 ==
n_vecs_i*(dim*4+8))하고 각 partial을 mmap한 뒤, **single-shard(흔한 경우)는 device 행렬 1개에 partial별 offset
H2D**(`cuvs_cagra_build_multi` — 단일 `raft::copy`를 N회로 분할) → **host corpus 복사 0**. global tids만 호스트
조립(작음, total*8B). `.vectors` sidecar는 partial에서 스트리밍(`cuvs_vectors_write_multi`, 증분 crc → 연접과
byte-identical). multi-shard(대형, partial이 shard 경계 교차)는 host 연속 조립 후 기존 `build_sharded` 폴백.
persist/registry/reply tail은 단일 경로와 공유(`finish_build_commit`로 추출).

**정합 근거**: CAGRA 순서 독립 + positional (vec,tid) pairing → partition 연접 == 단일 corpus. n_parts==1은
offset-0 단일 copy라 기존 `cuvs_cagra_build`와 동일(위임). 리더는 데몬 reply 후 partial unlink, 크래시 시
ADR-057 flock reaper backstop(누수 클래스 불변 — 노출 시간만 김).

### 결과 (VM A100/PG16)

- **정합(핵심)**: 고유-벡터 50k×128 self-NN **단일(w0) 5/5 == 병렬(w4 multi-partial) 5/5**, 데몬 로그가
  `[handle_build_multi] direct multi-H2D` 확인. installcheck 15/15 + isolation 2/2 GREEN. sidecar
  byte-identity 단위(`cuvs_vectors_write_multi` == 단일, +empty-partition skip) 로컬 227 passed.
- **오버헤드(north-star)**: bench_500000 dim1024 — backend(total−GPU floor) **단일 ~6.3s(39.35s−~33s) →
  병렬 multi-partial ~3.7s(36.67s−~33s)**. merge 복사 제거를 데몬 로그로 실증(연접 단계 소멸). wall-clock은
  GPU floor(~33s/37s≈89%) 지배라 marginal(저널 1s 해상도 내 노이즈). 구조적 이득: 리더가 더 이상 2번째 full
  corpus(merge 버퍼)를 들지 않음 → backend peak RSS −corpus(~2GB). /dev/shm 고아 0.
- **남은 레버**: PLAIN storage(detoast 제거, ADR-043 — 직교, 단일/병렬 양쪽 적용).

### 대안 기각 / 한계

- **multi-shard도 direct H2D**: partial이 shard 경계 교차 → 1차는 host 조립 + `build_sharded` 폴백(현 동작 보존).
  대형 인덱스만 해당. 후속에서 shard-aware 분배 가능.
- **wall-clock 천장**: GPU build(~33s)가 빌드를 지배 → 어떤 backend 최적화도 wall-clock을 ~33s 밑으로 못 내림.
  ADR-059의 가치는 north-star(backend 오버헤드·peak RSS 제거)이지 wall-clock이 아니다.

---

## ADR-061 — 전략 포지셔닝: 쿼리당-비용 세그먼트 + exact filtered brute-force wedge

**날짜**: 2026-06-07
**상태**: 전략 방향 채택 (구현 미착수). 상세 분석·쟁점은 [design/STRATEGY_NOTES.md](STRATEGY_NOTES.md).

**배경**: 경쟁 데이터(VectorChord 0.4 / 3B 사례)가 그동안의 암묵적 전제 "cuVS 핵심가치 = 대규모 → 3P(IVF-PQ)가
로드맵 핵심"을 깼다. VectorChord는 **32억 벡터를 GPU 없이 CPU+NVMe로 월 $12k**(p50 761ms, top-500)에 서빙하며
"in-memory/GPU의 막대한 비용 회피"를 명시한다. 즉 *규모만으로는 GPU가 정당화되지 않으며*, 저장-바운드 규모는
CPU+디스크가 비용으로 구조적으로 이긴다.

**결정 (전략 방향)**: 아래 세 렌즈로 pg_cuvs를 재포지셔닝하고, 그에 따라 우선순위를 조정한다.

1. **분모(A)**: 워크로드는 *벡터당 비용*(아카이브/배치 → CPU+디스크) 또는 *쿼리당 비용*(온라인 RAG/추천, 높은 QPS·
   저지연 → GPU)에 지배된다. **pg_cuvs는 쿼리당-비용 지배 세그먼트만 표적한다.** 거대-corpus-저비용은 쫓지 않는다.
2. **메모리 계층(B)**: GPU는 VRAM→PCIe 절벽(~80×)으로 우아한 spill이 없다. 이게 규모 약점의 *근본*이며, 3P조차
   못 없앤다. CPU+디스크의 매끄러운 비용 경사(RAM→NVMe→S3)를 GPU는 갖지 못한다.
3. **알고리즘은 하드웨어 종속(C)**: 필터 검색의 최적해가 다르다. CPU는 brute-force가 비싸 graph-traversal-with-filter
   (ACORN/bit-vector graph scan)를 쓴다. **GPU는 brute-force가 거의 공짜라 filter→brute-force가 지배**하며, 이는
   **exact(recall=1.0)**라 ACORN의 연결성/recall 문제가 애초에 없다. pg_cuvs는 VectorChord의 prefilter를 베끼지
   말고 GPU-네이티브 exact filtered brute-force를 해야 한다.

**채택 wedge (D)**: **exact filtered brute-force.** 고선택성 `WHERE` + 벡터 검색에서 PG가 평가한 필터를 bitset으로
GPU에 넘겨 brute-force exact top-k. killer app = **멀티테넌트 SaaS RAG**(테넌트당 데이터 작음, 항상 `tenant_id`
필터, 높은 QPS = 쿼리당-비용 지배). 전체 corpus가 수십억이어도 GPU는 필터된 부분만 들고 exact·sub-ms.

**근거 (cuVS 26.04 헤더 스파이크, 2026-06-07, VM cuvs_dev)**:
- `cuvsBruteForceSearch(..., cuvsFilter prefilter)` 네이티브 지원(`brute_force.h:148`). `cuvsFilter={addr,type}`,
  `type∈{NO_FILTER,BITSET,BITMAP}`(`common.h:23-39`). **BITSET(1D row-allow)이 WHERE-필터에 정확히 맞음.**
  CAGRA/IVF-Flat/IVF-PQ도 `cuvsFilter` 지원 → 저선택성은 CAGRA+filter(근사)로 분기. bitset 빌더 C API는 없어
  DLManagedTensor(packed bits)로 우리가 구성(소항목).
- 보너스: `cuvs/neighbors/tiered_index.h`(base ANN + incremental **bfknn** 버퍼 + 자동 compaction) +
  `cuvsCagraExtend/Merge` 확인. **3Q(`.delta` 대체; 3A는 완료)의 네이티브 구현 후보**이자 D와 같은 brute-force 메커니즘으로 수렴.

**우선순위 조정**:
- north-star(빌드/H2D 오버헤드) = **해자 1순위 유지** (빌드 속도 = GPU의 베끼기 어려운 edge; 4A/ADR-059가 옳았음).
- **D = 신규 1순위 후보** (해자 + 차별화 클레임 + killer app, 빌드 부담 낮음). 3O(ADR-048)를 흡수 — D가 3O의
  GPU-네이티브 정답 버전.
- **3P(ADR-049) 우선순위 하락**: "규모 핵심"이 아니라 "VRAM working-set 천장 올리기"로 재정의. 압축 품질은
  RaBitQ에 짐. selectivity로 규모를 다르게 푸는 D가 필터 워크로드엔 우월. **온라인 대용량의 1차 답은 파티션(아래)이고,
  3P는 whale/global fallback으로 더 격하.**
- **온라인 대용량 = 파티션 pruning + 기존 LRU** (정련, 스파이크 검증 2026-06-07): tenant LIST/RANGE 파티션 →
  `WHERE tenant=X` planner pruning → 작은 파티션 인덱스 → 데몬 기존 LRU(3D cold registry + eviction)가 VRAM 캐시로
  동작. 스파이크 실증: pruning이 단일 인덱스 스캔(Append 없음, GPU==CPU exact), `--max-vram-mb 4`에서 2/6 상주 +
  cold reload 정합 유지. **단 범위는 ≤64 활성 파티션** — recipe+회귀테스트 정식화 완료(#32), 스케일 측정(2026-06-08):
  **`MAX_INDEXES=64`가 하드월**(130 파티션 → 64만 resident=슬롯캡, VRAM 아님; 축출 파티션 쿼리는 ERROR+REINDEX 필요,
  런타임 슬롯-캡 축출은 auto-reload 안 됨), 현실크기 캐시-미스 꼬리 **~750ms/122MB**(~150MB/s, GB급은 수 초; north-star가 깎음).
  → 수백+ 테넌트 온라인은 **선결: MAX_INDEXES 상향/동적화 + 런타임-축출 auto-reload 배선**(ROADMAP 백로그). 상세 STRATEGY_NOTES §G.
- **3A는 완료(ADR-047)** — `.delta` pending-delta 동작 중, 미완 아님. **3Q(미래)**의 streaming updates
  (`cuvsCagraExtend/Merge`)에 대해 cuVS `tiered_index`(base ANN + bfknn 버퍼 + 자동 compaction)가 네이티브 구현
  후보. 3A의 `.delta`를 tiered_index로 이관하는 건 선택적 미래 리팩터(갭 아님).

**미해결 쟁점**:
- ~~**포지셔닝 충돌(E)**~~ **해소됨(2026-06-07)**: `PROJECT_POSITIONING.md`의 "exact GPU vector search" Avoid 문구는
  **stale이었음** — pg_cuvs는 3L(ADR-039)에서 이미 `search_mode='brute_force'` exact 검색(recall=1.0, fp16에서도
  exact, 소규모 N에선 CAGRA보다 저렴해 planner 자동선택)을 출하했다. Avoid에서 제거하고 BF=exact / CAGRA-default=근사로
  분리, 메시징을 scoped Prefer로 교정. 경계: BF는 무필터 대규모 N에선 O(N)이라 "소규모/필터·선택적에서 exact+저렴"으로 한정.
- ~~온라인-스케일 파티션 갭~~ **정식화·측정 완료(2026-06-08)**: recipe+회귀테스트(#32), MAX_INDEXES=64 하드월 특성화, 캐시-미스
  ~750ms/122MB 측정. **잔여 = 백로그**(MAX_INDEXES 상향/동적화 + 런타임-축출 auto-reload). 정확한 64 vs 128 경계 + PG planning @ 많은 파티션은 미특성화.
- D 확장판(host backing + VRAM 파티션 LRU 캐시)의 PCIe 예산 현실성, 저선택성↔graph 분기 임계값 추정.
- tiered_index가 `.delta` 머신을 어디까지 대체하나(3Q 후보, 별도 스파이크).
- PG plumbing: `WHERE + ORDER BY <-> LIMIT`을 filter→brute-force 경로로 라우팅(custom scan/bitmap 소비).
- H streaming/out-of-core BF(미착수, 분석은 STRATEGY_NOTES §H): exact throughput 플레이, 온라인 단일-쿼리엔 부적합.

**대안 기각**: 3P를 규모 해법으로 선두 추진 — VectorChord 데이터상 거대-corpus는 CPU+디스크가 비용으로 이김,
GPU가 질 게임. 3O 접근 B(CPU식 graph prefilter) — GPU에선 brute-force가 더 단순·exact·빠름.


---

## ADR-062 — cuVS 에코시스템 진입 전략

**날짜**: 2026-06-08
**상태**: 전략 채택 — 단계별 실행 대기

**배경**: cuVS(rapidsai/cuvs) 에코시스템 조사(2026-06-08) 결과:
- 현재 공식 통합: Milvus, Faiss, Elasticsearch(진행 중), Kinetica
- PostgreSQL 관련 언급 전무 — 선점 기회
- 통합 모델: DB 자체 repo에서 cuVS dependency 사용 → cuVS 문서에 링크
- cuVS repo에 코드를 직접 기여하는 유일한 경로: **cuvs-bench pluggable backend**
- cuVS 팀 특성: NVIDIA 엔지니어, 마케팅보다 기술 증거(벤치마크, 작동 코드)에 반응

**결정**: 4단계 순차 진입.

| 단계 | 내용 | 타이밍 |
|------|------|--------|
| 1 | repo 공개 + 벤치마크 공개(`BENCHMARK.md`) | 즉시 가능 |
| 2 | cuvs-bench backend PR | 3Q 완료 후 |
| 3 | cuVS 문서/README 링크 요청 | 2단계 merge 후 |
| 4 | NVIDIA 채널(뉴스레터/블로그) 노출 | 3단계 후 |

**2단계가 핵심**: cuvs-bench PR은 cuVS repo에 코드가 들어가는 유일한 경로이자,
NVIDIA 팀과 직접 기술 교류를 시작하는 접점. Elasticsearch backend PR이 선례.

**3Q와의 연계**: `cuvsCagraExtend`/`cuvsCagraMerge`가 완료되면 cuVS C API를
PostgreSQL 수준에서 실제 활용하는 유일한 사례가 된다 — cuVS 팀에 가장 강하게 어필할 수 있는 타이밍.

**대안 기각**: 대규모 마케팅 캠페인 선행 — cuVS 팀은 엔지니어 조직이므로
기술 기여(코드, 벤치마크)가 마케팅보다 효과적. 홍보는 링크 등재 이후 자연스럽게.

상세 계획: [docs/ecosystem-strategy.md](../docs/ecosystem-strategy.md)

---

## ADR-063 — D-wedge 필터 스파이크: Option B + Option A 구현 결과

**날짜**: 2026-06-08
**상태**: 스파이크 완료 — PR #35 검토 대기. 프로덕션 전환은 임계값 로직·타입 안전성 강화 후.

### 배경

ADR-061에서 exact filtered brute-force(D wedge)를 1순위 전략으로 지정. 핵심 미결 사항은 PG 인덱스 AM이 비-인덱스-컬럼 qual을 AM에 넘기지 않는다는 제약 — 이를 우회해 GPU BF 검색에 필터를 end-to-end로 전달하는 경로 설계가 스파이크 목표였다.

### IPC 프로토콜 확장

`CuvsCmdFrame`에 두 필드 추가:

```c
uint32_t n_filter_tids;      /* 0 = no filter */
char     filter_shm_key[64]; /* POSIX shm name for sorted uint64_t TID array */
```

필터 TID 배열은 POSIX shm으로 전달(메모리 매핑, zero-copy). 데몬은 BF top-bk 결과에 binary search로 TID whitelist 교차 — `found ? keep : skip`. `n_filter_tids=0`이면 무필터 BF 경로로 fall-through.

데몬 내부 overfetch: `n_filter_tids > 0`이면 `bk_target = k * 4` (post-filter 생존자를 k개 채우기 위한 candidate 확보).

### Option B — Function API (`cuvs_filtered_knn`)

```sql
cuvs_filtered_knn(index_rel regclass, query vector,
                  filter_tids bigint[], k integer)
RETURNS TABLE (ctid tid, distance float4)
```

caller가 TID whitelist를 `bigint[]`(block<<16|off 인코딩)로 직접 전달. AM 제약 완전 우회.

**구현 과정에서 발견된 버그 2개**:

1. **`STRICT` 문제**: SQL 함수 선언이 `LANGUAGE C STABLE STRICT`였다. PostgreSQL의 `STRICT`는 임의의 인자가 NULL이면 C body를 실행하지 않고 즉시 NULL(SRF에서는 empty set) 반환. `NULL::bigint[]` 필터를 넘기면 C 코드가 아예 호출되지 않아 항상 0 rows. **수정**: `STRICT` 제거 → `CALLED ON NULL INPUT`(기본값). C body에서 `PG_ARGISNULL(2)` 검사 후 unfiltered BF 경로 분기.

2. **overfetch 부족**: 필터 TID가 200개(200/800 = 25%)일 때 `k=10`만 요청하면 daemon이 10개 후보를 반환하고 그 중 평균 2~3개만 whitelist를 통과 → recall 저하. **수정**: PG 측에서도 `k_fetch = min(k*4, 4000)` 요청, emit loop에서 `min(n_results, k)`만 방출. 데몬 내부 4x와 합산해 실효 candidate pool ≈ 16x.

**결과**: `n=10, wrong_tenant=0` (수정 전 n=8), `n_unfiltered=10` (수정 전 0).

### Option A — Custom Scan Hook

`set_rel_pathlist_hook`에서 CAGRA 인덱스 + 비-인덱스-컬럼 qual + ORDER BY vector `<->` const + LIMIT이 모두 있는 RelOptInfo를 감지해 `CustomPath → CustomScan`으로 교체. SQL 변경 없이 투명하게 동작.

실행 경로:
1. `cuvs_cs_build`: heap seqscan + qual 평가 → TID set 구축 → `cuvs_ipc_search_filtered`
2. `cuvs_cs_exec`: TID별 heap fetch → virtual slot 복사 → `ExecProject`

**핵심 구현 이슈 2개**:

1. **`ExecInitQual` 슬롯 포인터 최적화**: `ExecInitQual(exprs, parent)`는 parent의 `ss_ScanTupleSlot` 포인터를 `EEOP_SCAN_FETCHSOME` 옵코드에 직접 bake한다. parent의 virtual slot이 비어있는 상태에서 qual 평가 시 항상 0을 읽어 모든 행이 필터에서 탈락(800행 중 799개 통과해야 할 qual이 0개 통과). **수정**: `ExecInitQual(exprs, NULL)` + `CreateStandaloneExprContext()` — eval 시점에 `filter_ctx->ecxt_scantuple`을 동적으로 지정.

2. **`ExecProject` slot ops 불일치**: `ps_ProjInfo`는 virtual `ss_ScanTupleSlot`을 기준으로 컴파일됨. heap-AM slot을 직접 넘기면 `tts_ops` 불일치로 SIGSEGV. **수정**: `slot_getallattrs(heap_fetch_slot)` 후 `tts_values/tts_isnull`을 virtual scan slot에 수동 복사 → `ExecStoreVirtualTuple` → `ExecProject`. `ExecAssignScanProjectionInfo` + `ExecInitScanTupleSlot` 패턴으로 단순화 가능하나, CustomScan hook에서 `ExecInitCustomScan` 이후 slot 교체는 PG 버전별 내부 구현 차이 가능성이 있어 현 방식 유지.

**결과**: `EXPLAIN`에 `Custom Scan (CuvsFilteredScan)` 노출, `n=10, wrong_tenant=0`, hook off 시 기존 Index Scan 복원 확인.

### 교훈 (향후 C 함수 작성 시 체크리스트)

| 항목 | 설명 |
|------|------|
| `STRICT` vs `CALLED ON NULL INPUT` | NULL 인자를 함수 내에서 처리할 의도라면 `STRICT` 금지. NULL이 들어오면 C body 자체가 실행되지 않는다. |
| `ExecInitQual` parent 인자 | CustomScan에서 qual을 별도 ExprContext로 평가할 때는 `NULL` 전달. parent 전달 시 해당 parent의 slot 포인터가 ExprState에 bake된다. |
| `tts_ops` 불일치 | heap-AM slot과 virtual slot은 `tts_ops`가 다르다. `ExecProject` 호출 전 slot 타입을 맞추거나 값을 복사해야 한다. |
| IPC + 4x overfetch | post-filter는 후보 소진 위험이 있다. PG 측과 데몬 측 모두 overfetch를 적용해야 k개를 안정적으로 반환한다. |

### 현재 상태 및 다음 단계

스파이크로서 Option B, A 모두 동작 확인. 프로덕션 전환 전 필요한 작업:

- **Option B**: 타입 안전성 (`bigint[]` 인코딩을 wrapper 함수로 추상화), 저선택성↔고선택성 분기 임계값 GUC
- **Option A**: GUC `cuvs.filtered_knn_hook`(현재 off 기본값 유지), 선택성 추정으로 BF/CAGRA 자동 선택
- **공통**: delta 통합 (`.delta` 벡터를 BF 필터 경로에도 포함), `EXPLAIN ANALYZE` GPU 타이밍 노출

**대안 기각**: Option C (GUC 세션 변수 `cuvs.prefilter_col/val`) — 타입 안전성 없음, 병렬 쿼리 위험, 프로덕션 부적합.

## ADR-065 — GPU 메모리 풀 격리와 VRAM budget 강제 한계

**날짜**: 2026-06-10 (2026-06-10 VM 검증으로 메커니즘 정정)
**상태**: 확정 (제약 기록) — 구현 미착수, VRAM budget GUC 착수 시 참조 필수.

### 배경

CAGRA extend OOM 주입 테스트(`pg_cuvs_eat_vram()`으로 CUDA 여유 메모리를 4 MiB로 축소)에서 `cuda_oom_blocked_extend = f` — extend가 계속 성공하는 현상이 발견됐다.

### 근본 원인: GPU 메모리 풀 캐싱 (메커니즘 정정 2026-06-10)

**초안 가설(RMM pool)은 VM 검증에서 반증됐다.** cuVS 헤더를 `pool_memory_resource` / `set_current_device_resource`로 grep한 결과 매칭 없음 — cuVS는 자체 RMM pool을 설치하지 않고 "현재 device resource"를 그대로 사용한다. 따라서 관찰된 캐싱(raw free 4 MiB인데 extend 성공)의 출처는 RMM pool이 아니라 **CUDA async mempool**(`cudaMallocAsync`)일 가능성이 높다 — driver가 해제된 메모리를 풀에 보유하며, 이는 `cudaMemGetInfo`에 보이지 않는다.

**미확정**: cuVS가 실제로 resolve하는 active device resource가 무엇인지(plain `cudaMalloc` sync vs async mempool vs 외부 주입 RMM resource) VM에서 추가 확인 필요. 단 어느 쪽이든 핵심 결론(아래)은 불변이다.

결과:
- raw `cudaMemGetInfo`가 반환하는 free memory = driver mempool이 보유한 캐시를 제외한 잔여
- cuVS extend/search의 workspace 조달은 이 mempool 내부에서 이뤄짐
- raw CUDA 잔여가 4 MiB여도 mempool 캐시에 충분한 메모리가 있으면 cuVS 연산 성공

### 결정: raw CUDA API 기반 VRAM budget 체크 금지

`cudaMemGetInfo` / `cudaMalloc` probe를 VRAM budget 강제 수단으로 사용하지 않는다. 이 값은 driver mempool 캐시를 반영하지 않아 실제 cuVS workspace 가용량과 괴리가 있다. **이 결론은 메커니즘이 RMM pool이든 CUDA async mempool이든 동일하게 성립한다 — VM 검증으로 오히려 강화됨.**

**실효 budget 강제 경로 (미착수, 메커니즘 정정 반영)**:
- active device resource가 **CUDA async mempool**이면: `cudaMemPoolGetAttribute`/`cudaMemPoolSetAttribute` 경유 — `cudaMemPoolAttrReservedMemCurrent`(현재 예약량 조회), `cudaMemPoolAttrReleaseThreshold`(반환 임계값 제어). **RMM pool API 아님.**
- active device resource가 외부 주입 RMM pool이면: `pool_size()` 등 RMM API 조회 가능 — 단 깔끔한 public `free_size()` 부재. 게다가 cuVS 기본 경로가 아님.
- **선결**: 어느 경로든 active device resource 확정이 먼저. VM에서 cuVS dispatch 시점의 resource 종류 확인 필요.
- `set_vram_budget(0)` 현재 기본값 = 무제한 — 프로덕션 배포 전 재검토 필요.

### 현재 OOM 안전망

raw budget 체크가 신뢰 불가임에도 OOM 안전은 다른 경로로 보장된다:

1. cuVS 내부 메모리 할당이 실제 OOM 시 예외 throw
2. `handle_extend()`의 try-catch가 이를 잡아 delta fallback으로 전환
3. `_pr.poison()`으로 손상된 PooledRes 반환 차단

즉, **OOM 시 데이터 유실은 없다**. budget 강제의 부재는 "사전에 막지 못한다"는 것이지 "OOM 후 깨진다"는 것이 아니다.

### 영향 범위

| 컴포넌트 | 영향 |
|----------|------|
| `cuvs.vram_budget_mb` (미구현) | 구현 시 CUDA mempool attribute 경유(active resource 확정 후) — RMM pool API 아님 |
| Streaming BF 청크 크기 (ADR-064) | **free space 조회 불필요** — 보수적 고정 청크 cap(`cuvs.stream_bf_chunk_vectors`)으로 설계. 스트리밍은 청크 크기와 정확도가 무관하므로 mempool 잔여를 알 필요가 없음 |
| OOM 주입 테스트 | `g_inject_extend_oom` 플래그 방식 유지 — raw CUDA 조작으로는 mempool 캐시 우회 불가 |

### 대안 기각

- **raw `cudaMemGetInfo` 기반 budget 체크**: driver mempool 캐시 미반영으로 신뢰 불가 — 기각
- **`cudaMalloc` probe-and-free**: 동일 이유 + probe 자체가 mempool 밖에서 할당돼 cuVS와 경쟁 발생 가능 — 기각
- **RMM pool API(`pool_free_size()`) 조회**: 초안 권고였으나 VM 검증에서 cuVS가 해당 pool을 쓰지 않음이 확인 — 기각. 실제 경로는 CUDA mempool attribute(active resource 확정 후)

### 해소 — sane default budget (2026-06-10, repo 공개 전, PR feat/vram-budget-default)

"`set_vram_budget(0)` 기본 무제한" 재검토를 닫았다. **핵심 통찰**: 실효 budget 강제는 raw
`cudaMemGetInfo`도 mempool attribute 조회도 필요로 하지 않는다 — 데몬이 **자기 회계**
(`total_vram_used` = Σ per-index 추정 `vram_bytes`)로 강제하면 되고, 이건 어떤 device-memory
조회보다 신뢰 가능하다(데몬은 자기가 할당한 것을 정확히 안다). 남은 결손은 (1) 기본값이 무제한,
(2) 회계가 추적 못 하는 cuVS workspace + CUDA context의 headroom.

- **(a) 기본 budget = 총 VRAM의 보수적 비율**(`CUVS_DEFAULT_VRAM_FRACTION=0.90`, `pg_cuvs_server.c`
  startup): `--max-vram-mb` 미지정 시 무제한 대신 per-device 총량의 90%로 cap. 10% headroom이
  미추적 workspace/context를 흡수. 총량은 `g_gpus[dev].total_vram_bytes`(불변·신뢰)에서. 명시
  runtime-unlimited은 `pg_cuvs_set_vram_budget(0)`로 여전히 가능(테스트가 사용).
- **(b) mempool-aware free**(`cuvs_wrapper.cu` `cuvs_vram_free_bytes_on`): budget==0(runtime
  unlimited) headroom 경로가 쓰는 free 추정을 ADR-065가 지목한 `cudaMemPoolGetAttribute`
  (`ReservedMemCurrent`−`UsedMemCurrent` = reclaimable)로 보정. best-effort(에러 시 raw free
  유지) — (a) 적용 후엔 budget이 항상 설정되므로 이 경로는 좁은 explicit-unlimited 케이스에만
  쓰임. compile + 무회귀 확인.

**검증 (A100-40GB, cuVS 26.04.00)**: `make gpu-test-vram` — 무플래그 데몬 기본 budget = 36418 MB
(40465 총량의 90%), 무제한 아님·총량 미만 PASS. 회귀 무영향: installcheck 26/26 + isolation 3/3
GREEN. **상태 갱신: 무제한 기본값 결손 해소.** mempool attribute 기반 *자동 sizing*(아래 follow-up)은
여전히 별개 — 본 작업은 "안전한 기본 cap"이지 "동적 sizing"이 아니다.

### 후속 (ADR-065 follow-up)

active device resource 확정 + CUDA async mempool attribute 기반 자동 sizing은 별도 작업으로 분리. Streaming BF(ADR-064) 첫 컷은 이를 기다리지 않고 보수적 GUC cap으로 출하한다.

## ADR-064 — Streaming / Out-of-Core BF: sidecar-gather 경로 (1차 구현 완료)

**날짜**: 2026-06-09 (1차 구현 2026-06-10)
**상태**: 1차 구현 완료 — 3O 역방향 맵 재활용, pread 기반 out-of-core gather + 청크 GPU BF + running top-k 머지. 분석 출처: STRATEGY_NOTES §H.

### 1차 구현 (2026-06-10)

`CUVS_OP_SEARCH_STREAM_BF` op + `handle_search_stream_bf()`. 경로: filter TID → 3O
역방향 맵(`rev_tids`/`rev_item_ids`) 이진탐색 → item_id 수집 → `.vectors` 사이드카에서
`pread`로 필터 통과 벡터만 gather(전체 상주 없음, offset = `sizeof(header) + item_id*dim*4`)
→ `cuvs.stream_bf_chunk_vectors` 단위 청크 GPU BF(`cuvs_brute_force_search`) → host에서
running 정확 top-k 머지. `last_search_mode=6`(stream_bf).

- **청크 크기 = 정확도 무관**: running top-k 머지는 임의 청킹에 대해 exact. 청크 크기는
  VRAM footprint 노브일 뿐 결과 불변 → GPU 잔여 메모리 조회 불필요(`cudaMemGetInfo` 미사용,
  ADR-065 준수). `cuvs.stream_bf_chunk_vectors` 고정 cap(기본 262144).
- **자동 전환**: `cuvs.stream_bf_selectivity_threshold`(기본 0.0=off). selectivity가 이 값
  미만이면 stream BF 사용, 3O in-VRAM prefilter보다 우선. 사이드카 부재 시 3O prefilter로 폴백.
- **검증**: `test/sql/stream_bf_recall.sql` — 작은 데이터셋 + `threshold=1.0` + 작은 청크 cap으로
  스트림 경로 강제, CPU exact ground truth와 정확 일치(recall@k=1.0), tenant 격리,
  `search_mode='stream_bf'`, 3O 경로와 parity.

**후속(트리거)**: (1) mempool-aware 청크 auto-sizing(ADR-065 follow-up), (2) selectivity×Q
자동 라우팅(현재 수동 GUC), (3) VRAM 초과 대규모 스케일 실측(회귀 CI 밖).

### 배경

VRAM을 초과하는 벡터 데이터셋(예: 벡터 400GB, VRAM 40GB)에서 고선택성 필터 쿼리를 GPU로 처리하는 경로가 없다. 현재 GPU 검색은 인덱스 전체(CAGRA graph + `.vectors` sidecar)가 VRAM에 상주해야 동작한다. VRAM 초과 시 OOM 또는 multi-GPU sharding으로만 대응 가능하다.

D-wedge(ADR-063)의 post-filter 방식도 동일 제약 — GPU가 VRAM 내 전체 벡터에 BF를 수행한 뒤 CPU에서 솎는다. 선택성이 아무리 높아도 GPU는 전체 corpus를 봐야 한다.

### 핵심 병목 분석

streaming BF의 naive 구현(`WHERE` 통과 행 → heap fetch → 행렬 구성 → H2D → GPU BF)에서 gather 단계가 지배 비용이 된다:

```
filter 통과 heapTID 집합
  → heap page random fetch (buffer pool 경합, scattered I/O)
  → vector detoast (TOAST면 청크 fetch + LZ 해제)
  → contiguous 버퍼 복사
  → H2D transfer → GPU BF
```

이는 [Filter-Agnostic Vector Search on PostgreSQL (Lu et al., 2026)](https://arxiv.org/abs/2603.23710)이 CPU HNSW에서 발견한 것과 동형 — page access가 distance computation을 압도한다.

### 결정: sidecar-gather 경로

heap 대신 `.vectors` sidecar를 gather 소스로 사용한다.

`.vectors` sidecar는 item_id 순서로 벡터를 연속 저장하며 detoast가 필요 없다. 3O(ADR-048)가 빌드하는 역방향 맵 `heapTID → item_id`를 재활용하면 gather 경로가 다음과 같이 바뀐다:

```
filter 통과 heapTID 집합
  → 역방향 맵으로 item_id 집합 변환  O(n_filter), 메모리 내
  → .vectors sidecar에서 item_id별 read  heap random I/O 및 TOAST 비용 없음
  → contiguous 버퍼 구성 → H2D → GPU BF (필터 통과분만)
```

### 적소와 한계

**적합한 워크로드**:
- 고선택성 필터 + VRAM 초과 데이터 (online 쿼리 포함)
- exact 대규모 배치 스코어링 / ground-truth 검증

**부적합한 워크로드**:
- 저선택성 (필터 통과 벡터 >> VRAM) — gather 비용이 이득을 상쇄
- Q=1 저선택성 온라인 쿼리 — corpus 전체 스윕 부담. throughput 플레이로 Q가 커야 amortize됨 (A100 기준 compute-bound 임계 Q ≳ 5,000)

selectivity × Q 조합에 따라 streaming BF / D-wedge post-filter / CAGRA 중 최적이 갈린다. 전환 임계값은 3O 완료 후 실험으로 확정한다.

### 착수 조건 (DEFERRED 해제 기준)

1. **3O 완료** — `heapTID → item_id` 역방향 맵이 daemon 메모리에 상주하는 시점 (재구현 없이 재활용)
2. **고선택성 online 쿼리 수요 확인** — 단일 GPU VRAM 초과 + 필터 선택성 < 10% 워크로드가 실측 문제로 보고되는 시점

### 구현 골자 (착수 시)

- daemon: `CUVS_OP_SEARCH_STREAM_BF` op 추가
- `handle_search_stream_bf()`: filter TID → item_id 변환(역방향 맵) → sidecar read → GPU BF dispatch
- IPC: 기존 `filter_shm_key` 필드 재활용, `use_stream_bf` 플래그 추가
- GUC: `cuvs.stream_bf_selectivity_threshold` (자동 전환 임계값)
- running top-k 머지: 청크별 GPU BF 결과를 host에서 누적 정렬
- **청크 크기는 보수적 고정 cap GUC(`cuvs.stream_bf_chunk_vectors`, 예: 기본 262144)로 결정** — raw `cudaMemGetInfo` 사용 금지(ADR-065). 스트리밍 BF는 청크 크기와 정확도가 무관(작으면 반복만 늘 뿐 답 불변)하므로 GPU 메모리 잔여를 조회할 필요 자체가 없다. mempool attribute 기반 자동 sizing은 ADR-065 follow-up으로 분리 — 첫 컷은 기다리지 않는다. (초안의 "RMM pool API 기반 청크 사이징"은 VM 검증에서 cuVS가 해당 pool을 안 씀이 확인돼 철회)

### 대안 기각

- **heap-based gather**: random page I/O + TOAST 비용이 GPU 이득을 상쇄 — sidecar 경로 대비 열위
- **IVF-PQ (ADR-049)**: VRAM 절감 접근이지 VRAM 초과 exact search 해법이 아님
- **multi-GPU sharding (3E/3F/3G)**: GPU 대수 선형 증가 요구 — 비용 문제를 하드웨어로 해결

---

## ADR-066 — Phase 3C 인증 + 매니페스트 버전 호환 게이트

**날짜**: 2026-06-10
**상태**: ACCEPTED / 구현·검증 완료

**배경 (reverse false-done)**: 소스 감사 결과 3C/3D 본체(`src/cuvs_objstore.c` libcurl GCS 클라이언트, 빌드 후 detached 업로드, warmup cold-miss 다운로드+SHA256+relfilenode hard-reject, 3D warmup 풀/cold 등록/관측 컬럼)는 **이미 live 경로에 배선**돼 있었으나 ROADMAP는 3C/3D를 "미완료"로 표기했다. 3A(ADR-009)의 정반대 사례 — 코드는 있는데 SSOT가 뒤처짐. 진짜 결손은 (1) GCS round-trip이 실제로 검증된 적이 없음(엔드포인트 하드코딩 + 테스트 버킷 부재; ADR-024/3G status/spec-audit가 반복 인용한 "bucket 부재로 자동 검증 불가"), (2) 매니페스트 계약의 몇몇 빈틈.

**결정 1 — 닫은 매니페스트 빈틈** (`CuvsManifest` / `cuvs_objstore.c` / `pg_cuvs_server.c`):
- `pg_cuvs_version` 하드코딩 `"0.1.0"` → 단일 출처 `src/cuvs_version.h`의 `PG_CUVS_VERSION`(=0.3.0, `pg_cuvs.control`과 동기).
- **`cuvs_version` 필드 신설** — 스펙 계약이 요구하나 누락돼 있었음. `CUVS_BUILD_VERSION`(빌드 시 링크된 cuVS, =26.04.00) 스탬프. CAGRA 직렬화가 cuVS 버전에 종속되므로 cross-version 로드는 안전 미보장.
- `base_generation`이 업로드 시 항상 `0`이던 것(주석: "not tracked at upload time")을 빌드 시 이미 계산되는 `.tids` body_crc32(`cuvs_crc32`)로 배선 — unsharded(`finish_build_commit`에 `base_generation` 파라미터 추가, 양 콜러가 `tids_gen` 전달) + sharded(`build_sharded`에서 `base_gen` 호이스트) 양 경로.

**결정 2 — load 시 버전 호환 fail-closed 게이트** (`cuvs_objstore_download`): 매니페스트 파싱 후 다운로드 전, `manifest.cuvs_version != CUVS_BUILD_VERSION`이면 hard-reject(REINDEX 요구). 빈 `cuvs_version`(pre-cert artifact)은 unknown→reject(fail-closed). `pg_cuvs_version` drift는 WARN만. 가드 순서: **relfilenode → OID → cuVS version → (download) SHA256**.

**결정 3 — 실 버킷 검증 (사용자 선택: "real ephemeral bucket on VM")**: CI-portable emulator 대신 A100 VM에서 **실제 ephemeral GCS 버킷**으로 round-trip 인증. `infra/scripts/objstore-roundtrip-e2e.sh` + `make gpu-test-objstore`: 버킷 생성(trap으로 파괴) → `--snapshot-uri` 데몬 → 빌드/업로드 → 로컬 wipe(`,relfilenode`만 유지)+restart→warmup 다운로드→exact recall → 3종 fail-closed(corrupt SHA / relfilenode mismatch / cuVS version mismatch) 각각 reject 확인. VM SA가 gcloud 신원과 동일해 IAM 추가 배선 불필요. **emulator 기반 자동 회귀는 후속(트리거)** — `STORAGE_EMULATOR_HOST` 엔드포인트 오버라이드 + fake-gcs-server.

**결정 4 — 3D 보조 동작 마감** (warmup 관측성 + graceful fallback): 후속 점검에서 3D 보조 2항목을 닫았다. (a) **warmup 관측 통계 보존**: `last_warmup_at`/`warmup_duration_ms`/`download_count`/`cache_miss_count`가 cold→hot 전환 시 0으로 리셋되던(사실상 미구현 — 컬럼은 있으나 항상 0) 결함 수정 — `IndexEntry`에 4필드 추가, `reset_entry_stats`에서 0 초기화, warmup 워커 hot 전환부에서 측정값 propagate, hot-entry stat fill이 실제 값 노출. (b) **graceful CPU fallback**: 인덱스 하이드레이션 불가(아티팩트 거부)면 로컬 `.tids` 부재 → backend plan-time artifact gate가 쿼리를 CPU seqscan으로 라우팅 → 정상 top-k 반환(에러/빈 결과 아님). 데몬-측 cache-miss 카운터(`.tids` local-but-not-hot 좁은 창)는 기존 integration scenario 12(eviction/reload)가 이미 커버.

**검증 증거 (2026-06-10, A100-40GB, cuVS 26.04.00)**: `make gpu-test-objstore` 전 항목 PASS — 업로드, 매니페스트 계약 필드, warmup 하이드레이션 recall@10 일치, **warmup 관측성 보존(download_count=1·last_warmup_at set)**, **하이드레이션 불가 시 graceful CPU fallback top-10 정확**, 3종 fail-closed reject, 버킷 생성·파괴 클린. 회귀 무영향: installcheck **25/25** + isolation **3/3** GREEN. 관련: [[ADR-013]], ADR-024(sharded snapshot), ADR-065(cudaMemGetInfo 금지 — 본 작업과 직교).

## ADR-067 — CI 전략: 2-tier (CPU-reference shim + on-demand GPU)

**날짜**: 2026-06-10
**상태**: ACCEPTED / 구현·검증 완료 (2026-06-11). Tier 1 `ci.yml`(CPU shim, 매 PR 자동·무료) + Tier 2 `gpu.yml`(UI 버튼 `workflow_dispatch` + WIF 키리스 GCP 인증, self-hosted A100, 실 installcheck 26/26) — PR #46–48, #50. 스펙: [design/CI_STRATEGY.md](CI_STRATEGY.md). 잔여: emulator 기반 GCS CI 회귀(트리거).

### 배경

GPU CI는 무료 옵션이 없다 — GitHub hosted 러너에 GPU가 없어 self-hosted(사용자 유료 VM)밖에 길이 없다. 그러나 이번 세션의 false-done 버그(3O rev map 미빌드 PR#39, manifest version 스탬프·base_generation=0 ADR-066)는 **하나도 GPU 커널 버그가 아니라 glue**(IPC 직렬화, 데몬 라우팅, fail-closed, `search_mode` 라벨링, manifest 계약)에서 났다. cuVS 커널 자체는 upstream에서 검증된다. → 실제로 무는 버그 클래스는 GPU 없이 잡을 수 있다.

### 결정: 2-tier

- **Tier 1 (CPU-reference shim, GitHub hosted `ubuntu-latest`, 매 PR 자동, 무료)**: `src/cuvs_wrapper.h`(GPU/CUDA 호출의 단일 경계) 전 심볼을 CPU exact-kNN 구현으로 대체하는 TU(`cuvs_wrapper_shim_cpu.c`, `make PGCUVS_CPU_SHIM=1`). 데몬·백엔드 나머지는 불변. plumbing·IPC 계약·fail-closed·mode 라벨링·정확성(shim이 exact라 ground truth)·VRAM 회계 로직을 검증.
- **Tier 2 (실 A100 installcheck, self-hosted GPU VM, 사용자 on-demand)**: **`workflow_dispatch` UI 버튼만**(쓰기권한자 전용; `/gpu-test` 코멘트·`gpu-ci` 라벨 자동 트리거는 fork-PR가 VM에서 임의 코드 실행 위험이라 **비채택**). GPU 커널 correctness·approximate recall 회귀·실 mempool VRAM 거동·latency. release가 아니라 **사용자가 원할 때** 트리거(비용 통제권 사용자). release는 트리거 아닌 머지 정책("최근 Tier 2 GREEN 확인")으로 얹음.

### 무료 성립 조건

(1) public repo = hosted 러너 무제한 무료(공개가 전제). (2) shim 빌드가 CUDA 툴킷 의존 0 — shim 미니 헤더가 `cudaStream_t` 등을 `typedef void*`로 stub, opaque 핸들을 host 구조체로 실체화. → 순수 CPU 빌드.

### Tier 1이 잡는다 / 못 잡는다 (정직)

- **잡는다**: IPC struct padding(CI Linux oracle 클래스), `search_mode` 라벨링(3O false-done을 잡았을 것), fail-closed(SHA/relfilenode/version/base_generation reject), rev map 빌드(build·load 양 경로), manifest 계약, recall(exact ground truth), VRAM 회계 제어흐름. **데몬 자기-회계 budget(ADR-065 해소: 총량 90% cap)은 shim이 fake 총량만 주면 cap 산술이 CPU에서 진짜 돈다** → VRAM budget·OOM·evict_lru 결정적 테스트.
- **못 잡는다(→ Tier 2 전속)**: 실 cuVS/CUDA 통합(API 오용/dtype/stream sync), **approximate recall 회귀**(shim은 exact라 그래프 품질 저하 안 보임), 실 mempool 거동(ADR-065), latency·멀티GPU 실배치.

### 정직성 라벨 (필수)

green CI badge가 "GPU 검증됨"으로 읽히면 그 자체가 CI의 false-done이다. README/badge에 "Tier 1=CPU reference로 plumbing·계약·정확성, GPU 커널·approximate recall·실 VRAM은 on-demand A100(Tier 2)" 명시.

### 대안 기각

- **GPU CI를 매 PR 자동(self-hosted 상시)**: 비용 — 사용자 무예산. 기각, on-demand로 대체.
- **release-only GPU 트리거**: GPU 경로 건드린 비-release PR을 못 막음. 기각, on-demand가 상위호환.
- **mock 없이 unit test만**: glue·IPC·fail-closed·mode·recall 전부 미검증 — 실제 버그 클래스를 놓침. 기각.
- **CPU fallback 경로만 CI**: 데몬/GPU 경로(false-done이 살던 곳) 미검증. 기각, shim이 데몬 경로를 실제로 태움.

### 후속

ADR-066 비고의 emulator 기반 GCS 회귀(`STORAGE_EMULATOR_HOST` + fake-gcs-server)를 Tier 1에 편입 가능. 관련: [[ADR-065]](VRAM 자기-회계), [[ADR-066]](objstore 인증), 3O PR#39(false-done 교훈).

---

## ADR-068 — MAX_INDEXES: 하드월 → 소프트 LRU working-set 캡

**날짜**: 2026-06-10
**상태**: ACCEPTED / 구현·검증 완료 (repo 공개 전, 멀티테넌트 온라인 스케일)

**문제**: 데몬 레지스트리가 `MAX_INDEXES=64` 고정 배열(`g_indexes[64]`). 타깃이 멀티테넌트인데
65번째 테넌트/파티션에서 막히면 첫 외부 사용자 이탈 사유(ADR-061 / STRATEGY_NOTES §G).

**근본 원인(감사로 3개 확인, 단일 아님)**:
1. `load_index`가 레지스트리-풀에서 **슬롯 확보 eviction을 안 함**(`ensure_vram`은 VRAM만 evict).
   VRAM 여유가 있는데 슬롯이 다 차면 `-1` → 검색 miss auto-reload(`handle_search`)가 풀 상태에서
   실패 → 축출된 테넌트가 REINDEX/재시작 없이 못 돌아옴(조용한 CPU 폴백). **이것이 진짜 하드월** —
   build는 이미 evict-on-full로 통과하고 있었다.
2. `finish_build_commit`이 풀에서 hard-ERROR + rollback (반면 `build_sharded`는 graceful defer).
3. 캡 자체가 고정 64.

**결정**: 레지스트리를 *하드월*에서 *소프트 LRU working-set 캡*으로 전환. 최근 N개만 상주,
나머지는 on-demand reload — `handle_search`의 search-miss 경로가 원래 하려던 동작.
- **(A) configurable cap**: `g_indexes`/`g_cold_indexes`를 startup-`calloc` 포인터로 전환,
  크기 = `--max-indexes`(기본 **1024**, was 64). startup 1회 할당·런타임 realloc 없음 →
  `&g_indexes[i]` 주소 + compacting-shift 제거 불변식 유지. `handle_stats`의 스택 배열은
  malloc로(VLA 금지). 16개 사용처를 `g_max_indexes`로 치환.
- **(B) `load_index` 슬롯-확보 eviction**: 풀이면 `evict_lru`로 LRU를 비워 reload 성공
  (sharded 포함). auto-reload 실질 배선.
- **(C) build 경로 graceful defer**: `finish_build_commit`(CAGRA) + IVF-PQ 빌드가 풀에서
  rollback+ERROR 대신 아티팩트 보존 + OK + "reload on demand"(build_sharded 패턴). 캡 초과
  안전망(A의 1024로 드물지만).

**대안 기각**: (a) 단순 상수 상향만 — 여전히 벽 + (B) 미해결로 축출분 복귀 불가. (b) 런타임
realloc 무한 성장 — pointer-stability 위험, startup-fixed로 충분(`--max-indexes` 상향 가능).

**검증 (A100, cuVS 26.04.00)**: `make gpu-test-maxidx` — `--max-indexes 4`로 10테넌트 빌드
**ERROR 0** + 전 테넌트 쿼리 정확 + `pg_stat_gpu_cache` evictions=16/**reloads=10**(축출분이 GPU로
재하이드레이션 = B 증명). 프로덕션 기본 1024 로그 확인. 회귀 무영향: installcheck 26/26 +
isolation 3/3 GREEN. 관련: [[ADR-061]](전략), STRATEGY_NOTES §G.

## ADR-069 — 벤치마크 실험 프로토콜 v2: 물리/판단 분리 + 코스트모델 보정 루프 선행

**날짜**: 2026-06-11
**상태**: ACCEPTED / 프로토콜 확정, 실행 미착수 (스펙: [design/BENCHMARK_PROTOCOL.md](BENCHMARK_PROTOCOL.md))

**문제**: 엄밀 벤치마크에는 세 가지 구조적 함정이 있다. (1) **자원-기울임 공정성** — GPU에 VRAM
40GB를 주고 CPU에 4GB·2프로세스만 주는 비교는 무효인데, DRAM과 VRAM은 GB로 등화 불가.
(2) **보정-재실행 결합** — 코스트모델은 현재 미보정 상수 3개(`CUVS_STARTUP_COST` 등,
CROSSOVER §8)이고 보정엔 촘촘한 교차곡선이 필요한데, 순진하게 설계하면 코스트모델 수정마다
전체 벤치마크 재실행. 국소 신호로 전체를 수정하는 위험도 동반. (3) **차원 폭발** —
{5 사이즈}×{PG 노브}×{인덱스 노브}×{3 config}×{필터}×{증분} 격자 전수는 불가능.

**결정** (4개):
1. **물리/판단 분리**: forced-hnsw/forced-cuvs 곡선(=물리)은 코스트모델 무관 불변 자산으로 1회
   실측. planner-auto(=판단)는 둘 중 하나를 고를 뿐이므로 **EXPLAIN-only 스윕**(셀당 ~ms)으로
   검증 → 보정 루프 비용이 "전체 재실행"에서 "EXPLAIN 재스윕 수 분"으로 절감. 분리가 깨지는
   경계를 규율로 고정: 루프 중 plan-time 상수만 수정 허용, runtime 라우팅
   (`filter_auto_threshold` 등) 수정은 영향 셀만 스코프 재실측. 전 결과 행에
   `cost_model_version`/`runtime_routing_version` 태그.
2. **보정 루프 선행**: Stage A(교차-밀집 물리, 적응 이분탐색: 거친 log-격자 + ratio 부호전환
   구간만 분할) → Stage B(regret 기준 보정 루프: ε=10% 무차별 밴드 밖 regret>0 셀 0개가 합격 —
   교차점 근방 오분류는 손해 ~0이라 무의미) → Stage C(동결·버전) → Stage D(필터·증분·Pareto·
   논문 스위트). **동결 전에 planner-auto 개입 스위트 금지**(전부 무효화되므로).
3. **자원 공정성 = $/J 등화 + raw 공개**: GB 등화 포기. (a) same-box(GPU 인스턴스 부속 CPU/RAM
   전체)와 (b) iso-$(동일 시간당 단가 CPU 인스턴스) 두 baseline 동시 보고 + raw 자원
   (VRAM/RSS/core-s/GPU-s/J) 전면 기록으로 독자 재정규화 보장.
4. **Ring 구조 경쟁자 계약**: Ring A(in-PG 정면: pgvector/pgvectorscale/vchord, iso-recall),
   Ring B(앵커: raw cuVS/faiss — 통합 세금 상한, QPS 슛아웃 금지), Ring C(외부 DB:
   milvus/qdrant/lancedb — 별도 system-level 문서, 운영 비대칭 병기), Ring D(10억+/아카이브 —
   천장 기록만). 각 시스템은 한 링에만.

**대안 기각**: (a) 균일 밀집 격자 — 교차 무관 구간 낭비, 이분탐색이 교차 구간(10K–100K, 마침
실측 최저가 구간)만 조밀화. (b) 오분류 0 합격 기준 — 교차점 경계에서 달성 불가·무의미, regret
한정으로 대체. (c) DRAM/VRAM GB 등화 — 단일 "공정" 숫자는 존재하지 않음, 등화 대신 가격
부여(§2). (d) 보정 루프를 스위트 뒤에 배치 — 수정 시 필터·증분 등 auto 결과 전량 무효화.

**정직한 한계(사전 등록)**: cold/warm·delta backlog·데몬 큐 깊이는 plan-time에 원리적으로 알 수
없어 envelope 도달 불가 구간이 존재할 수 있다 — 보정 실패가 아니라 "plan-time 정보 부족"
클래스의 발견으로 분류, 처방은 런타임 적응 후보로 백로그 적재.

관련: ADR-039(BF 자동선택), ADR-044(프로파일링), ADR-061(전략·표적 세그먼트), ADR-063/064(필터
경로), CROSSOVER §8(코스트모델 재보정 플래그). 트랙: 제품(P0–P5)과 논문(R1–R5) 분리, 상세는
프로토콜 §14.

---

## ADR-070 — 자원 거버넌스 원칙(어떤 레버가 무엇을 통제하나) + 확정 버그 3개

**날짜**: 2026-06-11
**상태**: ACCEPTED / 원칙 채택 + 버그 3개 수정 진행 (repo 공개 전 하드닝)

### 배경

"pg_cuvs가 `maintenance_work_mem`을 준수하나?"라는 질문에서 출발해 표준 PostgreSQL 레버
전반의 준수 여부를 감사하고, 적대적 리뷰 2라운드(PG-시맨틱·운영-SRE·GPU-시스템 렌즈)로 정책 초안을
검증했다. 두 라운드가 초기 가설("문제되는 host 자원을 표준 PG 레버로 천장 씌우자")을 상당 부분
반증했고, 그 과정에서 **정책과 무관하게 코드로 확정된 버그 3개**가 드러났다.

### 결정: 자원이 *실제 사는 곳*으로 강제 계층을 고른다

표준 PG 레버는 **PG 자신의 enforcement 기계(fd.c temp 파일, MemoryContext palloc, executor 취소)를
통과하는 자원만** 강제할 수 있다. pg_cuvs의 핵심 자원(memfd/shm 코퍼스, 데몬 host RAM, GPU VRAM,
외부 아티팩트)은 전부 그 밖이다. 따라서:

| 자원 | 진짜 enforcement 레버 | cuvs 자체 레버의 역할 |
|------|----------------------|----------------------|
| PG 기계 내부 (취소·병렬 grant·플래너·VACUUM) | 표준 PG 레버 — **이미 준수** | 없음, 유지 |
| host RAM (코퍼스·데몬 배열) | **OS/cgroup** (`MemoryMax=`, `RLIMIT_AS`) | `cuvs.max_build_mem_mb`는 cgroup 벽 닿기 전 깨끗한 ERROR를 내는 soft fail-fast일 뿐(보증 아님). 대안: 코퍼스를 PG `BufFile`로 옮기면 `temp_file_limit`이 *진짜로* 적용 |
| VRAM (빌드 scratch) | **reactive evict-and-retry** + RMM pool release-threshold cap | `cuvs.max_vram_per_gpu`는 soft admission floor, 예측 천장 아님 |
| 정확성/복제 (아티팩트) | **백엔드가** `.tids` 헤더에 system_identifier+timeline 스탬프 + plan-time 검증(데몬은 백엔드 판정을 신뢰만) | timeline 불일치→ERROR(fail-closed); 부재→degrade+SQL-level WARNING |

**철회(category error로 확정)**:
- `maintenance_work_mem`-as-build-ceiling — 의미 불일치(scratch≠full materialization), 기본 64MB면
  정상 빌드도 ERROR, per-backend라 system-wide 아님, 병렬 워커당 N배.
- `temp_file_limit`-on-memfd/shm — PG의 `fd.c`가 이 fd들을 절대 못 봄. 노브가 거짓말을 함.

### 확정 버그 3개 (코드 검증 완료, 이번에 수정)

1. **VRAM 회계 누락** — `total_vram_used`(`pg_cuvs_server.c:560-582`)가 unsharded `main_bf_vram_bytes`,
   sharded `shards[].bf_vram_bytes`를 합산 안 함 → eviction 과약정 → 빌드/검색 OOM. (IVF-PQ는
   `vram_bytes`와 `ivfpq_vram_bytes`를 둘 다 set하므로 후자를 더하면 이중계상 — 주의.)
2. **빌드 락 starvation** — `handle_build`/`build_sharded`가 `g_index_mutex`를 GPU 빌드(`cuvs_cagra_build`,
   수 분)·디스크 I/O 내내 보유 → 그동안 모든 검색/통계/드롭 블록. 뮤텍스가 진짜 필요한 건 VRAM 회계와
   registry swap뿐. → reservation-counter로 GPU 빌드 구간 언락.
3. **빌드 OOM evict-retry 부재** — `cuvs_cagra_build` NULL(OOM 구분 불가) 시 즉시 BUILD_FAILED.
   `estimate_vram_bytes`가 빌드 scratch를 빼므로 사전 `ensure_vram` 통과 후에도 OOM 가능. OOM 신호
   (`cuvs_last_build_was_oom`, RMM `bad_alloc` 포함) + `inject_build_oom` seam(opcode 20) + 데몬이 OOM 시
   evict 후 1회 재시도. **수정 완료**.

**부수 발견 — IVF-PQ eviction 크래시(기존 잠복 버그)**: #3 retry를 Tier-1 CI에 올리자 데몬이 SEGV. ASAN
백트레이스로 근본 원인 확정 — `evict_lru → save_index → cuvs_cagra_serialize(e->handle)`인데 IVF-PQ 엔트리는
`e->handle==NULL`(인덱스가 `ivfpq_handle`에 있음) → NULL deref. `ivfpq_smoke`가 남긴 IVF-PQ 인덱스가 LRU일 때
retry의 `evict_lru`가 이를 건드려 터짐. **IVF-PQ 인덱스는 원래부터 안전하게 evict 불가**(maxidx는 CAGRA-only라
못 잡음; ADR-068 soft-LRU의 사각). 수정: `evict_lru`에 IVF-PQ 분기(아티팩트 durable → save 없이 free +
reload-on-demand, sharded 패턴) + `save_index` NULL-handle 방어. **ASAN을 Tier-1 데몬 빌드에 상시 편입**(UAF/오버플로 커버).

### 대안 기각

- **데몬을 거버넌스 권위자로(admission control)**: 데몬은 PG 내부(pg_control/timeline)를 못 읽고, 코퍼스는
  백엔드가 접촉 전 이미 할당하며, system_identifier는 standby에서 primary와 동일(클론). 새 IPC 왕복+lease+
  TOCTOU 처리가 필요한 무거운 신규 설계 → 이번 미채택, 백로그.
- **빌드 scratch를 정적 배수(3-10x)로 예측**: intermediate/graph degree·IVF-PQ vs NN-descent·AUTO 불투명,
  cuVS에 peak 질의 API 없음 → 과거부+과소부 동시. reactive evict-retry가 정답(버그 #3).

### 검증

Tier-1 CPU shim 회귀(installcheck, **PR #54 GREEN 27/27, 데몬 ASAN 빌드**): `vram_accounting`(버그#1),
`build_lock`(버그#2 — 빌드 정상 + reservation no-leak), `build_oom`(버그#3 — OOM 1회 주입 → evict + retry 성공),
`build_multi_oom`(#2/#3을 **병렬빌드 `handle_build_multi`**에 적용 — 강제 병렬 + OOM → evict + retry, 데몬 로그로
`[handle_build_multi] build OOM ... retrying` 확인). 모두 ASAN 무크래시. **#2/#3은 세 빌드 경로 전부 적용**
(`handle_build`/`build_sharded`/`handle_build_multi`). 빌드 락 **동시성**(starvation 부재)과 `build_sharded`
멀티GPU는 단일 클라이언트·단일 GPU shim으로 검증 불가 → Tier-2. 대형 항목(cgroup 가이드, scratch-aware admission,
백엔드 스탬프, corpus→BufFile, daemon host-bytes cap)은 ROADMAP 트리거 백로그.

**Tier-2 검증 (A100, cuVS, 2026-06-11)**: installcheck **30/30** + isolation **3/3** 실 GPU 통과(신규 4종·IVF-PQ
eviction·병렬빌드 포함; shim reconcile expected가 실 GPU와 일치). **빌드 락 starvation 부재 확정** — 6.97초 GPU
빌드 중 동시 검색 25회 각 50–110ms(블록 없음). 잔여: `build_sharded` 멀티GPU(2+ GPU 필요, dev VM은 단일 A100).

### 후속

관련: [[ADR-068]](MAX_INDEXES 소프트 LRU — 같은 eviction/회계 경로), [[ADR-065]](VRAM 자기-회계),
[[ADR-057]](corpus 핸드오프). 보고서: `docs/reports/2026-06-11-resource-governance-audit.md`.

---

## ADR-071 — standalone `bruteforce` AM: no-graph-build exact GPU 검색

**날짜**: 2026-06-12
**상태**: ACCEPTED / 설계 확정, 구현 미착수 (구현 담당: 로컬/VM 세션, 핸드오프: 이슈 #56)

**문제**: GPU exact brute-force가 **단독 실행 불가**. 현재 BF는 cagra 인덱스의
`cuvs.search_mode=brute_force` 모드로만 접근 가능 → `CREATE INDEX USING cagra`(비싼 그래프
빌드)를 강제당한다. 그런데 데몬 BF 검색 경로는 `.vectors`+`.tids` 사이드카만 읽고 `.cagra`
그래프는 **전혀 보지 않는다**(검증: `pg_cuvs_server.c:985-1049` — `CuvsBfIndex`는 `.vectors`를
`cuvs_vectors_read`로 lazily 빌드, "brute_force must never evict a base CAGRA index"). 즉 BF에게
그래프는 순수 dead weight인데 빌드가 강제로 생산한다. 이는 BF의 **시그니처 가치 = no-build**
(즉시 쿼리·항상 최신·exact·유지보수 0)를 무효화한다. 측정 근거(operational-guide.md): gpu-bf가
cpu-seq를 60–500× 압도하고, N≲50k에선 cagra보다도 빠르다(exact인데).

**결정**: standalone **`bruteforce` AM** 추가. 빌드는 `.tids`+`.vectors` 사이드카만 생성하고
`cuvsCagraBuild`(그래프 구축)를 **스킵**한다. 검색·비용 분기·사이드카 I/O는 **기존 BF 경로를
그대로 재사용**(데몬 검색 코드 무변경). 이로써 BF가 그래프 없이 빌드(=O(N) 복사 수준)되고,
3-path 결정면({cpu-seq, gpu-bf, gpu-cagra})에서 gpu-bf가 **독립 비용화 가능한 1급 경로**가 된다.

**구현 (로컬, 6 touch-point)**:
1. AM 등록 — `sql/`: `CREATE ACCESS METHOD bruteforce HANDLER ...` + opclass(`vector_l2_ops` 등).
2. `ambuild` — 데몬에 "vectors-only build" 요청 → `.tids`+`.vectors`만 persist, 그래프 스킵.
3. IPC build frame에 `vectors_only` 플래그 1개 추가(`cuvs_ipc.c`).
4. 데몬 `handle_build` — 플래그 set 시 `cuvsCagraBuild` 분기 스킵, `.tids`+`.vectors`만 영속화
   (tmp+rename+fsync 경로 재사용, `pg_cuvs_server.c`).
5. `amcostestimate` — 기존 BF 분기(`STARTUP + ROWS·N`) 재사용. (`CUVS_STARTUP_COST` 보정은 별건.)
6. `amgettuple`/search — 기존 BF 검색 경로 재사용(데몬 무변경).
   검증: `make installcheck` 녹색 + GHA `build=true` 회귀.

**합격 기준**: `CREATE INDEX ... USING bruteforce`가 그래프 없이 빌드되고(빌드시간이 동일 N의
cagra 대비 대폭↓), 검색 recall=1.0, VRAM에 그래프 부재(사이드카만 상주), gpu-bf 쿼리 성능이
기존 "cagra+search_mode=bf"와 동등.

**대안 기각**: (A) `cagra WITH (build_graph=false)` reloption — 최소 변경이나 의미론 어색("그래프
없는 cagra 인덱스")이고 플래너가 BF를 독립 경로로 비용화 못함 → 임시방편으로만 가치, 정석 아님.
(C) 순수 heap-scan custom scan(아티팩트 0, 진짜 no-build) — `.tids` 생성/MVCC·TOAST 매 쿼리
재마샬링 비용 + seqscan 대체 노드라 hook이 달라 변경 규모 큼 → 후속 백로그.

**정직한 한계**: standalone BF도 `.vectors` 사이드카(원본 코퍼스 N·dim·4B; 1M·1024≈4GB)를
디스크/VRAM에 상주시킨다 → 큰 N에서 VRAM 압박(`bf_float16`/스트림 BF ADR-064로 완화). 따라서
이건 "진짜 no-build(아티팩트 0)"가 아니라 **"no-graph-build(O(N) 사이드카만)"**이다. 또한
load-dependent `bf_batch_wait` 라우팅과 `CUVS_STARTUP_COST` 재보정은 이 ADR 범위 밖(별도 작업).

관련: ADR-039(BF 자동선택), ADR-064(스트림/OOC BF), ADR-069(벤치 프로토콜),
[operational-guide.md](../docs/operational-guide.md)(측정 근거 — gpu-bf vs cpu-seq/cagra), 이슈 #56·#58.

---

## ADR-072 — Disk DiskANN tier 재개 시 구현 방향: cuVS 업스트림 기여(advocate/PR), 포크 회피

**날짜**: 2026-06-12
**상태**: ACCEPTED / 방향 기록 (실행은 ADR-026 트리거 충족 시). 근거: PHASE_3B_SPIKE.md, ADR-025/026, ADR-061

**문제**: 3B(larger-than-RAM disk DiskANN cold tier)는 ADR-026에서 보류됐으나, 향후 트리거(1B+ 수요 / cuVS PQFlash stable / 128GB+ VRAM) 충족 시 "어떻게 구현하나"가 미기록 상태였다. 스파이크(PHASE_3B_SPIKE.md) 결과 cuVS 26.04는 Vamana **그래프 빌드는 정상**(in-memory recall 0.999)이나 **disk/PQFlash 직렬화가 PQFlash 비호환**(`sector_aligned` 출력이 1001 섹터 vs DiskANN 417 섹터 → PQFlash가 rerank 벡터를 잘못된 오프셋에서 읽어 recall 0.405로 붕괴). 또한 `cuvsVamanaSearch` 부재(Vamana는 build+serialize 전용, 검색은 MS DiskANN CPU에 의존). 이 갭을 닫는 길은 세 갈래이고, 그 선택과 기각을 고정한다.

**결정**: 재개 시 다음 방향만 취한다 — **우리는 어답터/기여자, 포크는 최후 수단**.

1. **방향 1 (advocate) — GPU Vamana disk 검색기**: cuVS disk index를 GPU에서 직접 검색(= cuVS #2197). 셋 중 유일하게 검색까지 GPU. **그러나 우리가 구현하지 않는다 — NVIDIA/cuVS 영역**. 이유: (a) GPUDirect Storage 기반 disk graph traversal은 연구급 시스템 프로젝트로 Postgres 확장 범위 밖, (b) PQFlash는 I/O 바운드(쿼리당 SSD 읽기가 지배)라 GPU 이득이 본질적으로 불확실 — 대량 배치(throughput)+GDS라야 의미, 단일 쿼리 latency엔 CPU 대비 이점 미미. → cuVS #2197을 업스트림에 advocate하고 어답터로 남는다.

2. **방향 3 (선택 — 할 거면 이것) — PQFlash 호환 cuVS serializer**: cuVS `vamana::serialize(sector_aligned)`가 MS DiskANN PQFlash 섹터 포맷을 정확히 쓰게 한다(= cuVS #1501, #905=OPQ codebooks에 의존). GPU 가치는 **빌드에 한정**(검색은 stock MS DiskANN CPU PQFlash) — 3I "GPU 빌드 → CPU HNSW 서빙"의 larger-than-RAM 버전으로 일관. **포크가 아니라 업스트림 PR**로 추진(유지보수 ~0, 굿윌). 우리 스파이크의 섹터-패킹 데이터(파일을 다 조립해도 1001 vs 417로 붕괴 — #1501이 현재 보는 "빠진 파일" 차원을 넘는 레이아웃 결함)를 #1501에 기여.

**대안 기각**:
- **방향 2 (PQFlash 포크/어댑터)**: MS DiskANN 리더를 포크해 cuVS 레이아웃을 읽게 함. 기각 — 얻는 건 CPU 검색(GPU 가치 0 = pgvectorscale 영역)인데 **MS DiskANN 포크 + cuVS·DiskANN 두 moving target을 동시 유지**해야 함(둘 중 하나만 포맷 바꿔도 깨짐). 최악의 유지보수 위치.
- **자체(in-tree) 포크로 방향 3 수행**: 업스트림 PR 거부 시에만 고려. 기본은 업스트림.
- **즉시 착수**: 기각 — ADR-026 트리거 미충족(세그먼트 비표적·CPU+NVMe 경제성 우위). VRAM 천장 밀기는 IVF-PQ(3P/ADR-049)·streaming BF(ADR-064)로 이미 처리됨.
- **vchord 공존으로 cold tier 대체** (2026-06-14 검토): DiskANN 라이브러리 대신 VectorChord(RaBitQ, CPU)를 large-N cold tier로 채택하고 공존시키는 안. vchord는 프로덕션 검증(3.2B halfvec, p50 761ms)·50M recall 0.9991·cuVS 블로커 없음·노력 ≈0(추천/공존)으로 단기 리스크가 낮다. 그럼에도 **기각** — (a) AGPL-3.0 전염 + 우리 manifest/fail-closed/GCS-snapshot 거버넌스 **밖**, (b) 별도 확장이라 "self-contained hot+cold 단일 시스템" 제품 서사가 깨짐, (c) GPU 빌드 레버리지가 k-means(=pgpu 영토)로 축소. 서빙은 양쪽 다 CPU+NVMe rerank로 동급이나, **소유권·permissive 라이선스(MS DiskANN MIT + cuVS Apache-2.0)·통합 거버넌스**를 cold tier에서도 유지하는 쪽이 제품 목표에 부합. → cold tier는 우리가 소유한다(DiskANN 라이브러리). 단 vchord를 우리가 GPU-가속하지는 않는다(pgpu가 이미 한 일, ADR 비교는 본 ADR 말미 참조). 대규모 수요자에겐 vchord/pgpu를 공존 선택지로 안내하되 plan of record는 아니다.

**cuVS 업스트림 이슈 매핑** (스파이크 발견 = 이미 트래킹, 전부 OPEN, milestone 없음): #2197 Vamana/DiskANN search on GPU(방향 1) · #1501 Vamana SSD index ↔ diskannpy 호환(방향 3, #905 의존) · #1380 host-dataset build `cudaErrorIllegalAddress` · #905 OPQ codebooks/rotation. (+ #1753/#1943 fp16, #1423 cuvs-bench, #906 disk 포맷 문서). 비고: cuVS가 26.08에서 Vamana 활발(#2214 device permute, 예제 DRAFT #2064) → 우선순위 상향 advocate 적기.

**순위**: 방향 3(업스트림 기여) > 방향 1(advocate) > 방향 2(회피).

관련: ADR-025(50M 벤치·포지셔닝), ADR-026(3B go/no-go·트리거), ADR-061(전략·표적 세그먼트), ADR-049(IVF-PQ), ADR-064(streaming BF), PHASE_3B_SPIKE.md / PHASE_3B_DECISION.md.

**업계 선례 — EDB pgpu / VectorChord** (2026-06-14 비교): pgpu(EnterpriseDB, Rust/pgrx, AGPL-3.0, v2.0.0)는 cuVS **k-means centroid만** GPU로 계산해 vchord external-build에 넘기는 "GPU 빌드 가속기"다 — 검색은 vchord(CPU). 즉 우리 3I/방향 3과 같은 "GPU 빌드 → CPU 서빙" 철학의 상용 선례이나 **인덱스 계열이 IVF로 다르고 Vamana/PQFlash와 무관**하므로 DiskANN 구현 참조 가치는 없고(난관=cuVS #1501 PQFlash serializer), 전략 패턴 검증 의미만 있다. EDB의 다른 GPU 자료(spark-rapids-tutorial)는 OLAP를 외부 Spark+RAPIDS로 오프로드하는 튜토리얼로 벡터 검색과 무관. 요지: **EDB 노선 = GPU build/analytics 오프로드, in-Postgres GPU search 부재** → pg_cuvs의 GPU filtered exact hot tier 자리는 미개척으로 남아 있고, cold tier 소유(본 ADR)는 그와 별개의 통합·라이선스 판단이다.

---

## ADR-073 — GPU exact brute-force 1급화: 상주 `flat` AM (A1) + transient 무인덱스 (B, 후속) + 플래너 라우팅

**날짜**: 2026-06-14
**상태**: ACCEPTED. **A1(`flat` 상주 AM) + B(transient 무인덱스 CustomScan) 구현·VM 검증 완료**(2026-06-14, A100). 라우팅 cost 캘리브레이션(`cuvs.gpu_bruteforce` off→auto 승격)은 후속 단계. ADR-071을 **흡수/supersede**한다.

**문제**: GPU exact brute-force는 cuVS를 가장 잘 드러내는 핵심 기둥(filtered exact BF = CPU 대응물 없는 cuVS-네이티브 차별점)인데, 두 가지로 *2급 시민*에 머물러 있었다. (1) BF가 cagra 인덱스의 `cuvs.search_mode='brute_force'` **옵션**으로만 접근 가능 → 비싼 그래프 빌드를 강제당하고 플래너가 BF를 독립 비용화 못함. (2) ADR-071의 `bruteforce` AM 안은 `CREATE INDEX`로 "무빌드"를 선언하는 **형용모순**.

**결정**: GPU exact BF를 1급으로 승격. 능력은 하나(matmul→L2→topK, exact, recall=1.0)이고 **워크로드가 물질화 방식을 가른다** — 두 체제로 구현한다.
- **A1 = 상주 `flat` AM (W2: 읽기 多·쓰기 간헐·안정 코퍼스).** `CREATE INDEX … USING flat (… vector_l2_ops) WITH (precision='float16')`. *진짜 평면 벡터 저장소(.vectors)를 지으므로* `flat` 명명이 정직(FAISS IndexFlat식; `bruteforce` 형용모순 회피). 빌드 1회 분할상환·VRAM warm. 신선도는 기존 cagra delta/tombstone 재사용(INSERT→`.delta`, 검색 시 CPU-exact 머지).
- **B = transient 무인덱스 (W1: 쓰기 폭주·항상최신·ad-hoc 필터).** `SET cuvs.gpu_bruteforce=on`(기본 off→Tier-2 캘리브레이션 후 auto). 인덱스 없이 플래너가 vector ORDER BY를 transient GPU-BF CustomScan(`CuvsTransientBF`)으로 라우팅. **구현·검증 완료**(round-2 FATAL = Sort 수용(pathkey 미주장) + 실행시 파라미터 바인딩으로 회피).
- **멘탈 모델**: "flat 인덱스 생성 여부 = 유일한 W2/W1 스위치." 사용자는 A1/B를 몰라도 됨.

**라우팅 매트릭스** (플래너 cost 자동, 사용자 개입 0): exact+읽기→**A1** / exact+쓰기→**B** / 근사+대규모 N→**cagra·ivfpq**. A1은 exact지만 O(N) 전수 스캔 → 근사 허용+대규모면 cagra가 *읽기 속도*로 이김. read/write 트레이드오프: **A1은 쓰기에 지불·읽기를 산다**(인덱스 中 최경량: O(N) 복사, 그래프/PQ 없음), **B는 읽기에 지불·쓰기를 산다**(쓰기 비용 0).

**원칙(불가침)**: cagra 경로 **무변경**(회귀 안전). flat의 신규 경로는 cagra `gettuple`/`cost`/`build`를 **복제(duplicate)하지 리팩터하지 않는다** — cagra 것은 `cuvs.search_mode` GUC 종속이라 verbatim 재사용 시 flat에서 NULL handle deref. 회귀 펜스(`brute_force, delta_recall, pending_delta, stream_bf_recall, cagra_streaming, auto_compact`) 변경 전/후 녹색.

**A1 구현 (검증 완료)**:
- **`pg_cuvs.c`**: `flatamhandler`(cagra 스캔/insert/delete 콜백 재사용) + 전용 `flat_gettuple`(`search_mode=1` 강제, GUC 무관) + `flat_amcostestimate`(exact N-cost 분기 강제, 8 게이트 전부 유지) + `cuvs_build_flat_from_heap`/`flat_ambuild`(vectors-only) + `cuvs_flat_amoptions`(`index_dir`+`precision`, layout-compatible로 `cuvs_resolve_index_dir_rel` 재사용) + DROP 훅(`cuvs_object_access`)에 flat oid 매칭.
- **`cuvs_ipc.{h,c}`**: `CUVS_OP_BUILD_FLAT`(21) + `cuvs_ipc_build_flat`(`cuvs_ipc_build` 복제, op만 상이).
- **`pg_cuvs_server.c`**: `IndexEntry.is_flat`(NULL handle + unsharded = 신규 상태); `handle_build_flat`(`.tids`+`.vectors`만 persist, **`finish_build_commit` 미사용**=`.cagra` serialize 회피, 독자 tmp+rename+commit); `load_index` flat 분기(`.cagra`/`.ivfpq` 부재 + `.vectors` 존재 → handle=NULL·is_flat=1·graph VRAM 미예약); `startup_load_indexes` `.vectors` 발견 패스(재시작 복원); 검색 가드.
- **비용 재보정**: 신규 `CUVS_FLAT_STARTUP_COST=50.0`(공유 `CUVS_STARTUP_COST=1000` **미변경** — "cagra 무변경" 원칙 준수). 핸드오프는 공유 상수 하향을 제안했으나 flat이 독자 cost 함수를 가지므로 전용 상수가 blast radius 0이며 동일 목표 달성(작은 N에서 flat 선택).
- **`pg_cuvs.control`** `default_version` 0.3.0→**0.4.0**(Task 1 누락분 — 안 하면 `CREATE EXTENSION`이 flat 없는 0.3.0 설치).

**적대검증이 잡은 라이프사이클 FATAL (전부 수정·검증)**:
1. `handle_search_batch`(`use_bf = … \|\| e->is_flat`) — 없으면 flat batch 검색이 `cuvs_cagra_search_batch(NULL handle)`로 **데몬 전체 크래시**.
2. `handle_search` — BF 게이트에 `e->is_flat` 추가 + cagra dispatch 앞 NULL-handle 구조적 가드.
3. `load_index`/`startup_load_indexes` flat 분기 — 없으면 재시작 후 flat **unsearchable**.
4. **`evict_lru` flat 분기 (세션 중 발견)** — flat 엔트리가 cagra `else`로 떨어지면 `save_index`(handle==NULL → -1 refuse) 호출로 **축출 불가**가 되어 VRAM을 영구 점유. → ivfpq처럼 save 없이 free.
5. **`handle_drop`에 `free_main_bf_cache` 추가 (세션 중 발견)** — flat의 *유일* GPU 할당(resident BF)이 DROP 시 누수. (cagra의 2차 BF 캐시 누수도 동시 해소; pre-existing IVF-PQ handle 누수는 범위 밖으로 명시.)
- freshness/recall(delta merge)·필터/WHERE 경로 NULL-safe는 검증 SOUND(수정 불필요).

**기각 대안** (적대적 단련 기록): **A2(유령 인덱스: AM+no-op build+transient amgettuple)** — HOT 비활성(저장 0인데 대가)+상속 cost가 `CUVS_FB_NO_ARTIFACT`로 비활성+transient verb 부재+매쿼리 재마샬, AM 이점은 A1이 동일하게 가짐 → 순비용. **A1 > B > A2.** / **테이블-키 1회-빌드 상주 코퍼스** — INSERT/UPDATE 무에러 누락(round-1 FATAL). / **B pathkey-claim CustomScan** — silent mis-ordering(round-2 FATAL). / **B plan-time Const만** — `<-> $1` 우회→근사 강등(round-2 FATAL). / **gpucache 복제** — PG-Strom master에서 죽은 코드.

**ADR 관계**: **ADR-071 흡수/supersede**(071의 vectors-only AM = 본 A1, `bruteforce`→`flat` rename + B/라우팅 추가). **ADR-039**(search_mode GUC BF): A1/B로 대체되는 deprecation 궤적(`search_mode='brute_force'`는 deprecated 표기). **ADR-049**(AM-per-algo): A1이 동일 하우스 스타일. **ADR-061**(filtered wedge): B가 그 plumbing 실현.

**검증 증거 (Tier-2, A100 `pg-cuvs-dev`/PG16, 2026-06-14)**: `make installcheck` **31/31 GREEN**(flat_smoke + 회귀 펜스 6 전부) + isolation **3/3 GREEN**. flat_smoke 5 assertion 전부 통과: recall@10=1.0 vs seqscan **with `search_mode`=DEFAULT('cagra')**(GUC 무관 증명), `Index Scan using flat_l2` + Sort 노드 없음, INSERT(`id=1001`) delta 머지 가시, `<-> $1` 파라미터 exact, DROP 무크래시. **재시작 내구성** 별도 검증: flat 빌드→데몬 재시작→동일 exact 결과(`5,6,31,4,30` = seqscan GT, `.vectors`에서 재로드). **업그레이드 경로** 0.3.0→0.4.0 `ALTER EXTENSION UPDATE` 동작.

---

**B 구현 (검증 완료, 2026-06-14)** — transient 무인덱스 GPU exact BF CustomScan:
- **`pg_cuvs.c`**: `set_rel_pathlist_hook`을 `cuvs_dwedge_add_path`(ADR-063, **동작 보존**) + `cuvs_transient_bf_add_path`(B)로 분리 — D-wedge의 "CAGRA 인덱스 필요 + WHERE 필요" early-return이 B(무인덱스·WHERE 선택적)를 막던 모순 해소. B는 **무인덱스 단일테이블 top-k에서만 발화**(FATAL-k 가드: 단일 base rel·조인/OFFSET/GROUP·DISTINCT·WINDOW·HAVING 없음·Const LIMIT 1..100000·단일 거리 ORDER BY 키 — LIMIT k가 rel을 증명적으로 bound하는 유일 shape). **pathkey 미주장(Sort 수용)**. 쿼리벡터 Expr은 `custom_exprs`에 저장→실행시 `ExecInitExpr`/`ExecEvalExpr` 바인딩(`$1` 근사 cagra 강등 회피). CustomScan은 `estate->es_snapshot`으로 힙 스캔(filter-first·NULL 벡터 skip)→leak-safe huge 코퍼스 컨텍스트(host 하드캡)→transient verb→top-k TID 페치. `parallel_safe=false`. metric은 `pg_amop`(amoppurpose='o') 스캔으로 거리연산자→opfamily→이름 매핑(`<->`는 btree 정렬연산자가 아니라 `get_ordering_op_properties` 부적합 — 구현 중 발견·수정). 신규 GUC `cuvs.gpu_bruteforce`(off/auto/on, 기본 off; auto는 v1에서 off로 동작) + `cuvs.gpu_bruteforce_max_mb`(host 코퍼스 하드캡, 기본 2GiB).
- **`cuvs_ipc.{h,c}`**: `CUVS_OP_SEARCH_BF_TRANSIENT`(22) + `cuvs_ipc_search_bf_transient` — 코퍼스 `[vecs][tids]`를 memfd(SCM_RIGHTS)/shm로 핸드오프(빌드 핸드오프 `CuvsBuildCorpus` 재사용), 쿼리는 별도 shm, 응답=header+`CuvsResult[]`. 비-OK는 fatal(B는 CPU fallback 없음); 데몬 에러메시지 반환. 코퍼스는 응답 후 close(shm 티어 데몬이 이름으로 open하는 레이스 회피).
- **`pg_cuvs_server.c`**: `handle_search_bf_transient` — 코퍼스+쿼리 mmap, **비축출(non-evicting) VRAM admission**(`g_index_mutex` 하 `total_vram_used()+needed > budget` OR `needed > free` → `OOM_FALLBACK`; **resident flat/cagra 인덱스 절대 축출 안 함** — `ensure_vram`/`evict_lru` 미사용), `cuvs_brute_force_search`, row→tid 매핑, **IndexEntry 무접촉**(find_index/load_index/record_search_stat 미호출).

**B 적대검증이 코드 전에 잡은 FATAL** (architect+critic 2-reviewer): (1) **FATAL-k** — `root->parse->limitCount`는 조인 inner-side/OFFSET/agg에서 해당 rel을 bound하지 않음(silent under-return) → 단일테이블 shape로 제한. (2) **FATAL-Expr** — 쿼리 Expr을 `custom_private`에 두면 setrefs/copyObject/parallel에서 깨짐 → `custom_exprs`로. (3) **C1 비축출** — `ensure_vram`(evict_lru) 재사용 시 one-shot 코퍼스가 resident 인덱스 축출(회귀) → 전용 비축출 체크. (4) **C2 훅 분리** — D-wedge early-return이 B 차단. (5) `parallel_safe=false`·`es_snapshot` 일관성(D-wedge의 이중 `GetTransactionSnapshot()` skew 회피)·호스트 OOM 하드캡·NULL 벡터 skip·결과버퍼 k-사이징.

**B 검증 증거 (Tier-2, A100/PG16, 2026-06-14)**: `make installcheck` **32/32 GREEN**(transient_bf + 회귀 펜스 7) + isolation **3/3 GREEN**. transient_bf 5 assertion: `gpu_bruteforce=off`→`Seq Scan`(플랜 무변경), `on`→`Custom Scan (CuvsTransientBF)` under Sort + recall@10=1.0 vs seqscan, `<-> $1` prepared→CuvsTransientBF·exact(근사 강등 없음), WHERE 필터 exact(filter-first), 1-byte budget→`ERROR ... (status 2) ... fail-closed`·데몬 생존.

**B 라우팅 캘리브레이션 (ADR-069 루프, 측정 완료 2026-06-14, A100/PG16)**: unfiltered 단일쿼리 top-k에서 B vs **병렬 CPU seqscan** 교차점을 Stage A 실측(`work_mem=4GB`, TIMING OFF, warm median/5; dim 384·768 × N 1k..300k). **결과: 교차점 없음** — B가 전 구간 2–8× 느림(seq/B 최대 0.51 @768/100k, 1.0 미달). 원인: 쿼리당 O(N·dim) 코퍼스 재마샬(detoast→memfd→H2D)+IPC가 지배; GPU 커널은 빠르나 데이터 이동세가 단일쿼리 이득을 상쇄(dim↑일수록 격차 축소, 미교차). **결정(regret-averse)**: `auto`를 plan-time B 경로 추가로 승격하지 **않음** — 전 셀에서 2–8× 느린 엔진 라우팅(regret>0)이 됨. `auto`는 off-behavior 유지(스텁 아님, 실측 검증). B의 진짜 가치(always-fresh W1·filtered wedge·동시성 CPU-offload)는 plan-time cost가 못 보는 **런타임 속성**(ADR-069 "plan-time 정보 부족" 클래스) → 명시적 `on`으로 도달, live `auto`는 런타임 적응 라우터/filtered 교차점 측정의 후속. **코드 변경**: F5 `on`-force 수정만(`on`→startup=0,total=1.0; 종전 N-증가 cost는 작은 N에서 seqscan에 밀려 force 실패). cost 상수 신설 없음(B가 이기는 셀이 없어 미사용).

**후속(carry-forward)**: filtered-path 교차점 측정(B의 filter-first가 CPU exact-filtered 대비 우위 구간) + 런타임 적응 라우팅(동시성/offload 신호) = live `auto`의 전제 / release-prep(모든 flat/B 작업 뒤: BENCHMARK·doc-coherence·ops-playbook·GitHub Pages·org 이전).

관련: ADR-071(흡수), ADR-039(BF GUC deprecation), ADR-049(AM-per-algo), ADR-061(filtered wedge), ADR-064(streaming BF), ADR-047(delta/tombstone freshness), 플랜 SSOT `snappy-strolling-brook.md`.
