# 스펙 무결성 감사 (false-done) — 2026-06-05

ADR-009 사건(설계 문서가 "완료"로 적었으나 미구현 → orphan 누수의 근본 원인)을 계기로, `design/DECISIONS.md`의 각 ADR "결과/결정/구현현황"과 `design/PLAN.md`의 "완료 기준"이 주장하는 동작이 실제 소스에 있는지 grep/read로 교차검증했다. 핵심 의심 휴리스틱: (a) standalone sidecar 데몬(PG 연결 없음, ADR-002)이 할 수 없는 동작 주장, (b) positive 테스트만 있고 negative/cleanup/failure-path 테스트가 없는 기능.

각 항목은 소스로 재확인했고, 발견은 **임의 코드 수정 없이 해당 ADR에 정정 노트(취소선 + 날짜)로 추가**했다(ADR-009 선례, add-only).

## 발견 (false-done / 과대표기 우선)

| # | 주장 (요약) | 문서 위치 | 소스 증거 (file:line) | 판정 | 심각도 | 조치 |
|---|---|---|---|---|---|---|
| 1 | `pg_stat_gpu_search`가 GPU kernel / IPC / CPU recheck time을 노출 | ADR-017 (초기 지표) | SRF는 `avg/p50/p95/p99` wall-clock만(`sql/pg_cuvs--0.1.0.sql:144-`), 데몬은 단일 `total_latency_us`(`src/pg_cuvs_server.c:135`). PLAN.md도 split "아직 없다" | OVERSTATED | med | ADR-017 정정 (deferred 명시, ADR-044 외부측정 cross-ref) |
| 2 | `pg_stat_gpu_search`가 fallback count/reason 노출 | ADR-017; PLAN.md:177 | 데몬 struct에 fallback/success 카운터 없음(`src/pg_cuvs_server.c:133-134`), `grep fallback_count`=0. fallback은 백엔드 plan-time 결정(`src/pg_cuvs.c:932`)이라 데몬 미도달 | NOT-IMPLEMENTED | med | ADR-017 정정 (구조적 미관측 명시) |
| 3 | ADR-017 지표 "calls, success, fallback, error" — calls/success 구분 | ADR-017 | 데몬은 `search_count`(=OK)·`error_count`만(`src/pg_cuvs_server.c:133-134`) | OVERSTATED | low | ADR-017 정정 (실제 컬럼으로 reword) |
| 4 | ADR-040 본문: batch search가 함수 내부에서 `heap_fetch` 처리 | ADR-040 본문(DECISIONS.md:1212) | 코드는 raw ctid 반환, 내부 heap_fetch 없음 — caller가 ctid JOIN(`src/pg_cuvs.c:2300`). 본 ADR 구현현황(1231)이 본문과 모순 | OVERSTATED (자기모순) | low | ADR-040 본문 정정 (구현현황이 정답) |
| 5 | ADR-011 circuit breaker가 자동 GPU-비활성 안전망 | ADR-011 | breaker 상태가 백엔드 static 배열(`src/cuvs_util.c:590`), 종료 시 소멸. `pg_cuvs_reset_circuit`도 호출 세션만(`src/pg_cuvs.c:2201`) | PARTIAL | med | ADR-011 한계 노트 (process-local 명시) |
| 6 | OBJSTORE-03: 인덱스 파일은 `pg_basebackup`에서 제외 | ADR-013; SPEC.md:388-393 | 기본 `index_dir`=`$PGDATA/cuvs_indexes`(`src/pg_cuvs.c:519`), 제외 훅 없음 → 기본 설정 위반 | PARTIAL/UNVERIFIED | med | ADR-013 주의 노트 (PGDATA 밖 권장) |

## 정상(재확인) — false-done 아님

| 주장 | 위치 | 상태 |
|---|---|---|
| ADR-009 startup 카탈로그 대조 로드 | ADR-009 | 미구현이나 **이미 정정 완료**(2026-06-05). 데몬에 `pg_index`/`PQconnect` 부재 재확인 |
| ADR-046 `pg_cuvs_gc_orphans()` | ADR-046 | 미구현이나 **상태="계획"으로 정확**. high-severity orphan 누수의 미해결 항목 — Phase B에서 구현 |
| ADR-042 OFFSET-aware K, ADR-043 EXTENDED NOTICE | 각 ADR | 상태="계획" — 미구현이 정상 |
| ADR-024 3G.2 GCS round-trip | ADR-024; PLAN.md:719 | "자동 검증 불가" 정직하게 disclosed — 과대표기 아님 |

## 시스템적 원인

회귀 스위트(`test/sql/*`, 11개)가 전부 happy-path다. orphan reconciliation / DROP DATABASE cleanup / 데몬-down DROP / fallback counting / GCS round-trip의 **negative·failure-path 테스트가 없다.** #1·#2·#5·#6이 CI에서 안 걸린 이유 — 깨진 계약이 failure-path 테스트 없이는 관측 불가하기 때문. Phase B에서 orphan GC negative 케이스를 추가해 이 부류의 첫 회귀 테스트를 만든다.
