# pg_cuvs 개발 계획

## 프로젝트 정체성

pg_cuvs는 단순한 "PostgreSQL용 GPU ANN 확장"이 아니다. **[Disk-centric DB World]와 [GPU-accelerated Compute World] 사이의 임피던스 불일치(Impedance Mismatch)를 해결하는 미들웨어 프로젝트**다.

핵심 원칙:
- SQL 인터페이스와 트랜잭션 시맨틱은 PostgreSQL이 그대로 유지
- 비용이 크고 병렬화가 필수적인 벡터 연산만 GPU로 오프로딩
- pgvector의 `vector` 타입, 연산자, opclass를 그대로 사용 (쿼리 변경 불필요)
- Postgres가 실행 주체 — GPU는 "고속 후보 생성기(Candidate Generator)"

---

## 아키텍처 결정

### 왜 In-Process가 안 되는가

PostgreSQL의 process-per-connection 모델에서 백엔드 프로세스마다 CUDA 컨텍스트를 초기화하면:
- 컨텍스트 1개당 수백 MB VRAM 오버헤드
- 초기화 시간 수백 ms
- 100개 연결 = 100개 컨텍스트, GPU 메모리 고갈
- GPU 크래시 시 전체 DB 재시작 위험

pg_duckdb가 DuckDB를 in-process로 링크했을 때 겪은 함정과 동일하다.

### PG-Strom식 사이드카 모델 채택

```
PostgreSQL Extension + GPU Service Daemon + Shared Memory IPC
```

- GPU 컨텍스트는 `pg_cuvs_server` 데몬 하나가 독점 소유
- 세션 1,000개여도 GPU 컨텍스트는 단 하나 (Zero-copy 공유)
- GPU 서비스 장애 시 PostgreSQL은 생존 → CPU 경로로 Graceful Degradation

IPC 특성: "SQL을 보내고 결과셋을 받는" 고수준 통신이 아니라 "벡터 배열 포인터를 넘기고 결과 인덱스 배열을 받는" 저수준 RPC. 실제 데이터는 `shm_open` 공유 메모리에, 핸들만 주고받는 방식.

### Cost Model

Planner가 GPU/CPU 경로를 자동 선택:

```
Cost_total = Cost_IPC + Cost_GPU_kernel + Cost_CPU_recheck
```

- `startup_cost = 1000`: CUDA 컨텍스트 초기화 + PCI-e 전송 오버헤드 모델링
- `per_tuple_cost = 0.0001`: GPU 대량 병렬 처리 이점 반영
- 데이터가 적으면 IPC 비용 때문에 CPU가 더 저렴 → 자동 CPU 선택
- 데이터가 많아지면 자연스럽게 GPU 경로 선택

### pgvector 통합 원칙

- 사용자 쿼리 변경 불필요: `SELECT * FROM items ORDER BY embedding <=> $1 LIMIT 10;` 그대로
- approximate search + recheck 패턴 유지 (GPU는 TID + 거리만 반환, heap 접근은 Postgres)
- MVCC, 권한(ACL), 조인은 전부 Postgres가 기존 로직대로 처리

---

## 3단계 로드맵

### Phase 1: Proof of Mechanism (현재)

**목표**: GPU 검색이 실제로 PostgreSQL 쿼리 파이프라인에서 동작함을 증명

**완료된 것** (GCP L4 GPU, PG16 기준):
- C 확장 기본 구조 (C/C++ 분리로 `float4` 타입 충돌 해결)
- `cuvsBruteForceSearch` C API + DLPack 텐서 인터페이스 연동
- pgvector `vector` 타입을 네이티브 입력 타입으로 통합
- Index AM 핸들러 (`cuvsamhandler`) — Planner 인식
- Cost Model + `enable_cuvs` GUC
- L2 / Cosine / Inner Product operator class 3종

**남은 것**:
- `pg_cuvs_server` 데몬 구현 (현재는 백엔드 프로세스 내 직접 호출)
- Shared Memory IPC 레이어
- CAGRA 인덱스 빌드 / 검색 (`cuvsCagraBuild`, `cuvsCagraSearch`)
- 인덱스 영구 저장 (세션 간 재사용)
- 기본 에러 처리 및 CPU fallback

**성공 기준**:
- `CREATE INDEX USING cagra` 후 실제 CAGRA 검색 동작
- pgvector HNSW 대비 QPS 5x 이상 (1M+ 벡터 기준)
- PostgreSQL 크래시 없이 GPU 서비스 재시작 가능

---

### Phase 2: Production Ready

**목표**: pgvectorscale과 동등한 운영 안정성 확보

**구현 항목**:

**DiskANN Access Method**
- `USING diskann` AM 구현 (GPU-accelerated build, CPU search)
- CAGRA → HNSW 변환 활용: GPU로 빠르게 빌드, CPU 메모리로 내려 HNSW 포맷 변환
- VRAM 부족 환경에서의 현실적 타협안

**계층적 캐싱 (Tiered Storage)**
```
NVMe (Cold) → System RAM (Warm) → GPU VRAM (Hot)
```
- Hot Set (상위 20%): CAGRA로 GPU VRAM 상주
- Cold Set (나머지): DiskANN으로 디스크 유지
- 쿼리 시 Hot Set GPU 탐색 → 결과 부족 시 Cold Set 디스크 탐색

**VRAM 병목 대응**
- Sharded Build: 전체 데이터를 k-means 기반 파티셔닝, Shard별 로컬 그래프 생성
- 2단계 계층 그래프: L1(0.1~1% 대표 노드, GPU 상주) + L0(전체, SSD)
- PQ 압축 후보 생성기: GPU에 압축 벡터(64byte PQ code)만 올려 수용량 10~30배 확장

**운영 가시성**
- `pg_stat_gpu_search` 뷰: 쿼리당 GPU 커널 실행 시간, PCI-e 전송 시간, candidate 수, CPU recheck 비율, GPU cache hit ratio

**성공 기준**:
- 10M 벡터 안정적 운영 (OOM 없음)
- `pg_stat_gpu_search` 지표 수집
- Background Worker를 통한 인덱스 웜업

---

### Phase 3: Scale Out

**목표**: 수십억 규모, S3 기반 인덱스 스토리지

**구현 항목**:

**S3 기반 Immutable Index Snapshot**
- 인덱스를 LSM-Tree 방식으로 관리: 변경은 로컬 WAL → 주기적으로 S3에 통째로 빌드/업로드
- 읽기: S3 URL + 메타데이터만 보유, 필요 청크를 로컬 NVMe 캐시로 프리페치
- `io_uring` 기반 비동기 프리페칭
- 인덱스는 Derived Data (WAL 제외, 언제든 재생성 가능)

**GPU-accelerated DiskANN Pipeline**
- 전역 kNN 그래프 빌드의 GPU 개입
- Multi-node / Multi-GPU 로드 밸런싱
- 자동화된 인덱스 파티셔닝 및 리빌드 정책

**성공 기준**:
- 1B 벡터 인덱스 빌드 가능
- S3에서 인덱스 로드 후 검색 동작

---

## 기술 위험 및 대응

| 위험 | 대응 |
|------|------|
| CUDA 컨텍스트 파편화 | 사이드카 데몬으로 컨텍스트 단일화 |
| GPU OOM | VRAM 사용량 모니터링 + CPU fallback |
| GPU Service SPOF | Task Queue 백프레셔 + Fail-fast to CPU |
| PCI-e 전송 병목 | Cost Model에 전송 비용 반영, 소규모 쿼리는 CPU 자동 선택 |
| 인덱스 일관성 (MVCC) | GPU는 TID만 반환, heap 가시성 체크는 Postgres가 담당 |
| DiskANN ↔ cuVS 임피던스 불일치 | 제어층 직접 설계 (라이브러리 문제가 아닌 시스템 설계 문제) |

---

## pg_cuvs vs 대안

| | pg_cuvs | pgvectorscale | pg_lance | Milvus/Qdrant |
|---|---|---|---|---|
| 실행 모델 | PG + GPU 사이드카 | PG In-process | PG + LanceDB | 전용 서버 |
| SQL/트랜잭션 | 완전 지원 | 완전 지원 | 제한적 | 없음 |
| GPU 가속 | CAGRA (VRAM) | 없음 | 없음 | 일부 |
| 데이터 규모 | VRAM ~ 수십억 (Tiered) | ~수십억 (DiskANN) | 제한 없음 (S3) | 제한 없음 |
| 운영 복잡도 | 중간 | 낮음 | 낮음 | 높음 |
| 타겟 워크로드 | Operational (실시간 추천/검색) | Operational | Analytics | Operational |

---

## 참고 자료

- [RAPIDS cuVS 문서](https://docs.rapids.ai/api/cuvs/stable/)
- [pgvectorscale](https://github.com/timescale/pgvectorscale) — DiskANN + Cost Model 구현 참고
- [PG-Strom](https://github.com/heterodb/pg-strom) — GPU 사이드카 아키텍처 참고
- `pgstrom-results/docs/gcp-vs-experiments/` — GCP L4 벤치마크 및 C 확장 개발 기록
- `AI사업방향_0214/노트2_pg_cuvs_기술전략.md` — 전략 원본 문서
