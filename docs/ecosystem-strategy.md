# pg_cuvs — cuVS 에코시스템 진입 전략

**날짜**: 2026-06-08
**관련 ADR**: ADR-062

---

## 목표

cuVS 공식 문서/README에 pg_cuvs가 "cuVS를 사용하는 DB/라이브러리"로 등재되고,
cuvs-bench에 pg_cuvs backend가 코드 기여로 포함된다.

---

## cuVS 에코시스템 현황 (2026-06-08 조사)

### 현재 공식 통합 목록 (cuVS 문서 기준)

| 시스템 | 유형 | 통합 위치 |
|--------|------|-----------|
| Milvus | 전용 벡터 DB | Milvus 자체 repo |
| Faiss | 벡터 검색 라이브러리 | Faiss repo |
| Elasticsearch / Lucene | 검색 엔진 | 진행 중 (cuvs-bench backend PR 선례) |
| Kinetica | 분석 플랫폼 | Kinetica repo |

**PostgreSQL 언급 없음 — 선점 기회.**

### 통합 모델

cuVS repo에 통합 코드가 들어가는 게 아니다. 각 DB가 자기 repo에서 cuVS를 dependency로
사용하고, cuVS 문서/README에 링크된다. 유일하게 cuVS repo에 직접 기여할 수 있는 경로는
**cuvs-bench backend** (pluggable benchmark API).

### cuVS 팀 특성

- NVIDIA 엔지니어 주도. Release cadence 2개월(26.04 → 26.06) — 활발한 프로젝트
- Stars 778, Forks 194, Open issues 598
- 마케팅보다 **기술 증거(벤치마크, 작동하는 코드)** 에 반응
- 26.06에 UDF Architecture 추가 — 커스텀 거리 함수 JIT+LTO 지원 (향후 활용 가능성)

---

## 전제 조건

진입 전 반드시 준비되어야 할 항목:

| 항목 | 현황 | 필요 작업 |
|------|------|-----------|
| GitHub repo 공개 | private | public release + 라이선스 확인 |
| 재현 가능한 벤치마크 | 내부 측정 완료 | `BENCHMARK.md` 공개 문서화 |
| 외부 사용자용 설치 가이드 | 내부 Makefile | `README.md` 정비 (설치, quick start) |
| 기본 CI | VM 수동 검증 | GitHub Actions 최소 구성 |

---

## 진입 경로 (4단계)

### 1단계 — repo 공개 + 벤치마크 공개

**목표**: 외부에서 접근·검증 가능한 상태

- GitHub repo public 전환
- `BENCHMARK.md` 공개: pgvector HNSW vs pg_cuvs CAGRA 핵심 수치
  - build 13×, latency 11× (N=1M×384, A100)
  - recall@10, VRAM 사용량, QPS
- 외부 사용자용 README 정비: 최소 요구사항, 설치, quick start

**타이밍**: 현재 가능 (4A 완료 기준으로 충분)

---

### 2단계 — cuvs-bench backend 기여 (PR)

**목표**: cuVS repo에 코드로 기여

cuvs-bench는 pluggable backend API를 갖는다. Elasticsearch backend PR이 선례.
동일 API로 `pg_cuvs` backend 구현 → PR 제출.

**효과**:
- cuVS repo에 코드가 들어가고 NVIDIA 팀과 직접 기술 교류 시작
- 이후 링크 요청이 "기여자"로서 신뢰를 갖고 진행 가능

**타이밍**: 3Q(streaming updates) 완료 후 권장.
`cuvsCagraExtend`/`cuvsCagraMerge`가 들어가면 cuVS C API를 PostgreSQL 수준에서
실제 활용하는 유일한 사례가 되어 cuVS 팀에 가장 강하게 어필할 수 있는 시점.

---

### 3단계 — cuVS 문서/README 링크 요청

**목표**: "cuVS를 사용하는 DB" 목록에 등재

- cuVS repo에 GitHub Discussion 또는 PR 오픈
- Milvus/Kinetica와 동일 수준의 링크 요청
- 첨부: 벤치마크 수치, repo 링크, 아키텍처 요약

**타이밍**: 2단계 merge 이후.

---

### 4단계 — NVIDIA 채널 노출

**목표**: 더 넓은 가시성

- RAPIDS 뉴스레터 / NVIDIA 개발자 블로그 소개 요청
- cuVS 26.06 UDF Architecture를 활용한 커스텀 메트릭 사례로 포지셔닝 가능성
- PostgreSQL 생태계(pgconf, PGDay 등) 발표와 연계

**타이밍**: 3단계 링크 등재 이후.

---

## 타이밍 요약

| 단계 | 전제 조건 | 타이밍 |
|------|-----------|--------|
| 1 repo 공개 + 벤치마크 | 없음 | 즉시 가능 |
| 2 cuvs-bench PR | 3Q 완료 | 3Q 완료 후 |
| 3 cuVS 링크 요청 | 2단계 merge | 2단계 후 |
| 4 NVIDIA 채널 | 3단계 등재 | 3단계 후 |

**핵심**: 대규모 홍보보다 기술 기여(cuvs-bench PR)와 재현 가능한 벤치마크가 먼저다.
cuVS 팀은 엔지니어 조직이므로 코드와 숫자가 가장 설득력 있다.
