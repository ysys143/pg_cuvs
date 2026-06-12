# 세션 보고서 — 자원 거버넌스 감사 + 확정 버그 3개 수정

| 항목 | 값 |
|------|-----|
| 날짜 | 2026-06-11 |
| 범위 | 표준 PostgreSQL 레버 준수 감사 · 적대적 리뷰 2라운드 · 자원 거버넌스 정책(v3) · 확정 버그 3개 수정 |
| 결과 | ADR-070 채택 + ROADMAP 등재 + 버그 #1·#2·#3 전부 출고(PR #54) + 부수로 IVF-PQ eviction 크래시 수정 + Tier-1 데몬 ASAN 상시 |
| 검증 | **Tier-1 CI GREEN 27/27, 데몬 AddressSanitizer 빌드**(`vram_accounting`=#1, `build_lock`=#2, `build_oom`=#3, `build_multi_oom`=#2/#3 병렬경로). #2/#3은 세 빌드 경로(`handle_build`/`build_sharded`/`handle_build_multi`) 전부 적용. 빌드 락 동시성·sharded 멀티GPU는 Tier-2 |

---

## 1. 요약 (TL;DR)

"pg_cuvs가 `maintenance_work_mem`을 준수하나?"라는 질문에서 출발해 표준 PostgreSQL 레버 전반의
준수 여부를 감사하고, 적대적 리뷰 2라운드로 정책 초안을 검증했다.

- **핵심 결론**: 표준 PG 레버는 **PG 자신의 enforcement 기계(fd.c · MemoryContext · executor)를 통과하는
  자원만** 강제할 수 있다. pg_cuvs의 핵심 자원(memfd/shm 코퍼스 · 데몬 host RAM · GPU VRAM · 외부 아티팩트)은
  전부 그 밖이라, `maintenance_work_mem`/`temp_file_limit`로 통제하려는 시도는 **category error**다.
  진짜 레버는 host RAM=**OS/cgroup**, VRAM=**reactive evict-retry**, 정확성=**백엔드 스탬프**다.
- **부수 성과**: 정책과 무관하게 **확정 버그 3개**(VRAM 회계 · 빌드 락 starvation · 빌드 OOM retry)를 수정·출고
  (Tier-1 GREEN 26/26). #3 검증 중 **더 오래된 잠복 버그(IVF-PQ eviction NULL-handle SEGV)**까지 ASAN으로
  적발·수정하고, Tier-1 데몬을 **AddressSanitizer 상시 빌드**로 전환. 데이터 안전/성능 등급.

## 2. 배경 / 목표

repo 공개 전 하드닝 맥락에서, 외부 운영자가 익숙한 PG 노브로 pg_cuvs의 자원을 통제할 수 있는지 물었다.
감사는 "준수/미준수"의 단순 분류를 넘어, **어떤 레버가 무엇을 통제해야 하는가**라는 설계 원칙으로 확장됐다.

## 3. 표준 레버 준수 현황 (감사)

| 표준 레버 | 준수 | 근거 |
|-----------|------|------|
| `statement_timeout` / query cancel / `lock_timeout` | 준수 | 3S: IPC wait가 cancel 콜백으로 GPU 검색 중단 → `CHECK_FOR_INTERRUPTS` |
| `max_parallel_maintenance_workers` | 준수 | ADR-034 §4A-2 병렬 CAGRA 빌드(자체 `CreateParallelContext`) |
| 플래너 비용 (`enable_*`) | 준수 | `cuvsamcostestimate` (GPU-vs-CPU plan-time 라우팅 포함) |
| VACUUM (`ambulkdelete`/`amvacuumcleanup`) | 준수 | 톰스톤 경로 + `.stale` 게이트 |
| `maintenance_work_mem` | **미준수** | 자체 `cuvs.max_build_mem_mb` 사용 (의미 불일치) |
| `temp_file_limit` | **미준수** | memfd/shm은 `fd.c` 밖 — PG가 강제 불가 |
| `pg_stat_progress_create_index` | 미구현 | 장시간 CAGRA 빌드 진행률 미노출 |
| `shared_buffers` (CAGRA 경로) | 미적용(의도) | 외부파일+GPU, 버퍼매니저 우회 |
| `tablespace` (CAGRA 아티팩트) | 미준수 | `index_dir` GUC/reloption 사용 |

`SnapshotAny` 병렬 스캔 "false negative" 주장은 **기각**(검증): `table_index_build_scan`이 튜플별 가시성을
처리하는 표준 btree 패턴.

## 4. 적대적 리뷰 2라운드 — 무엇이 철회/재정의됐나

3개 렌즈(PG-시맨틱 · 운영-SRE · GPU-시스템)로 정책 초안을 공격하고, 비판자 주장도 코드로 적대적 검증했다.

**철회(category error로 확정)**:
- `maintenance_work_mem`-as-build-ceiling — scratch≠full materialization, 기본 64MB면 정상 빌드도 ERROR,
  per-backend라 system-wide 아님, 병렬 워커당 N배.
- `temp_file_limit`-on-memfd/shm — `fd.c`가 이 fd들을 절대 못 봄.

**재정의**:
- "데몬을 거버넌스 권위자로" → 데몬은 PG 내부(pg_control/timeline)를 못 읽고, system_identifier는 standby에서
  primary와 동일(클론). admission은 새 IPC 왕복+lease+TOCTOU가 필요한 무거운 신규 설계 → 미채택, 백로그.
- "빌드 scratch를 정적 배수(3-10x)로 예측" → cuVS에 peak 질의 API 없음 → reactive evict-retry가 정답.

**비판자 과대주장 기각(코드 검증)**: `SnapshotAny` false-negative(표준 패턴), `amcanparallel` 미선언
(pg_cuvs는 자체 `CreateParallelContext` 구동이라 무관).

## 5. 최종 정책 v3 — 자원이 *사는 곳*으로 강제 계층 선택

| 자원 | 진짜 enforcement 레버 | cuvs 자체 레버의 역할 |
|------|----------------------|----------------------|
| PG 기계 내부 (취소·병렬 grant·플래너·VACUUM) | 표준 PG 레버 — **이미 준수** | 없음, 유지 |
| host RAM (코퍼스·데몬 배열) | **OS/cgroup** (`MemoryMax=`, `RLIMIT_AS`) | `cuvs.max_build_mem_mb`는 cgroup 벽 전 clean ERROR soft-layer. 대안: corpus→PG `BufFile`이면 `temp_file_limit` 진짜 적용 |
| VRAM (빌드 scratch) | **reactive evict-and-retry** + RMM pool cap | `cuvs.max_vram_per_gpu`는 soft floor, 예측 천장 아님 |
| 정확성/복제 (아티팩트) | **백엔드** `.tids` 스탬프(system_identifier+timeline) + plan-time 검증 | 불일치→ERROR(fail-closed); 부재→degrade+SQL WARNING |

전문은 ADR-070. 대형 항목(cgroup 가이드·scratch-aware admission·백엔드 스탬프·corpus→BufFile·daemon
host-bytes cap)은 ROADMAP 트리거 백로그.

## 6. 확정 버그 3개 + 수정 (PR #54)

### 6.1 VRAM 회계 누락 — `fix(vram)`
- **증상**: `total_vram_used`(`pg_cuvs_server.c`)가 unsharded `main_bf_vram_bytes`, sharded
  `shards[].bf_vram_bytes` 미합산 → eviction/admission 과약정 → 빌드/검색 OOM.
- **수정**: 두 필드 합산. IVF-PQ `ivfpq_vram_bytes`는 `vram_bytes`와 중복 set이라 **제외**(이중계상 방지).
- **검증**: `vram_accounting.sql` — CREATE INDEX 후(CAGRA only) vs brute_force 검색 후(CAGRA+BF) 스냅샷,
  총량이 BF만큼 증가하는지.

### 6.2 빌드 OOM evict-retry 부재 — `fix(build)`
- **증상**: `cuvs_cagra_build` NULL(모든 실패 동일) 시 즉시 BUILD_FAILED. `estimate_vram_bytes`가 빌드
  scratch를 빼므로 사전 `ensure_vram` 통과 후에도 OOM 가능.
- **수정**: wrapper가 `std::bad_alloc`(RMM OOM 포함)을 `cuvs_last_build_was_oom()`로 신호 + CPU shim 미러 +
  `inject_build_oom` seam(opcode 20) + 데몬이 OOM 시 `evict_lru` 후 1회 재시도(실제로 VRAM을 비운 경우만).
- **검증**: `build_oom.sql`(victim 인덱스 상주 + OOM 1회 주입 → evict + retry 성공, CREATE INDEX OK).

### 6.3 (부수 발견) IVF-PQ eviction 크래시 — `fix(evict)`
- **발견 경위(정직)**: 6.2 retry를 처음 Tier-1에 올리자 **데몬이 SEGV**(`build_oom` 중 다운). 로컬 macOS는
  데몬 실행환경 부재(PG14≠PG16)라 무재현 → **Tier-1 데몬을 AddressSanitizer로 빌드**해 백트레이스 확보.
- **근본 원인**: `evict_lru → save_index → cuvs_cagra_serialize(e->handle)`인데 **IVF-PQ 엔트리는 `e->handle==NULL`**
  (인덱스가 `ivfpq_handle`에 있음) → NULL deref. `ivfpq_smoke`가 남긴 IVF-PQ가 LRU일 때 6.2의 retry `evict_lru`가
  건드려 터짐. **IVF-PQ 인덱스는 원래부터 안전하게 evict 불가**(maxidx는 CAGRA-only라 못 잡음 — ADR-068 soft-LRU의 사각).
- **수정**: `evict_lru`에 IVF-PQ 분기(아티팩트 durable → save 없이 free + reload-on-demand, sharded 패턴) +
  `save_index` NULL-handle 방어. **ASAN을 Tier-1 데몬 빌드에 상시 편입**(UAF/오버플로 상시 커버).
- **교훈**: "원인 모르는 크래시는 추측 패치 금지". 가설(UAF)을 세우되 **ASAN으로 확정**하고 고쳤다 — 추측대로 짰으면
  실제 원인(IVF-PQ NULL handle)을 놓쳤을 것. 그리고 #3가 노출한 건 #3 버그가 아니라 **더 오래된 잠복 버그**였다.

### 6.4 빌드 락 starvation — `fix(build)`
- **증상**: `handle_build`/`build_sharded`가 `g_index_mutex`를 GPU 빌드(수 분)+디스크 I/O 내내 보유 →
  모든 검색/통계/드롭 블록.
- **수정**: reservation-counter(`g_pending_build_vram`, `total_vram_used`가 합산)로 VRAM 예약 후 GPU 빌드
  구간만 언락, 디스크 커밋은 락 유지(`finish_build_commit` 에러 경로 보존). 양 경로 적용.
- **검증**: `build_lock.sql` — 빌드 정상 완료 + reservation no-leak(drop 후 VRAM 베이스라인 복귀).
- **한계(정직)**: 동시성(starvation 부재)은 멀티클라이언트 부하 필요 → Tier-2. `build_sharded`는 single-GPU
  shim으로 미검증 → Tier-2 멀티GPU 검증 권장.

## 7. 교훈

- **"표준 레버 준수"는 enforcement 경계로 판단한다.** 노브가 자원을 강제하지 못하면 "준수"는 무의미하고
  오히려 거짓 안전감을 준다(`temp_file_limit`이 memfd/shm에 무력한 것처럼).
- **적대적 검증은 양방향.** 비판자도 코드로 검증해야 한다 — `SnapshotAny`/`amcanparallel` 과대주장을 그대로
  수용했으면 멀쩡한 코드를 "고칠" 뻔했다.
- **예측보다 반응.** 예측 불가능한 자원(빌드 VRAM scratch)은 정적 추정 대신 reactive(evict-retry)가 옳다.

## 8. 산출물 / 남은 것

### 이번 세션
- `design/DECISIONS.md` ADR-070 (정책 + 버그 3개 + IVF-PQ eviction 수정)
- `ROADMAP.md` — 버그 #1·#2·#3·IVF-PQ eviction(출고) + 병렬빌드·대형 거버넌스(트리거 백로그)
- PR #54 — 버그 #1·#2·#3 + IVF-PQ eviction 수정 + #2/#3 병렬경로(handle_build_multi) 적용 + Tier-1 회귀 4종 + 데몬 ASAN 상시. **CI GREEN 27/27**
- 본 보고서

### Tier-2 검증 (A100, cuVS, 2026-06-11)
- **installcheck 30/30 + isolation 3/3** — 4종 신규 테스트·IVF-PQ eviction·병렬빌드 전부 실 GPU에서 통과. shim에서 reconcile한 expected가 실 GPU 출력과 일치(VRAM MB 결정적).
- **빌드 락 starvation 부재 확정** — 6.97초 GPU 빌드 중 동시 검색 25회 각 50–110ms(psql connect 포함). 빌드가 검색을 블록하지 않음.

### 남은 것 (트리거 백로그)
- `build_sharded` 멀티GPU 검증 — 샤드 reservation/eviction은 2+ GPU 필요(dev VM은 단일 A100).
- cgroup/systemd `MemoryMax=` 운영 가이드
- scratch-aware VRAM admission (intermediate/graph degree 기반 또는 RMM pool cap)
- 백엔드 아티팩트 스탬프(timeline/system_identifier) — standby/PITR fail-closed
- corpus → PG `BufFile` 옵션 (`temp_file_limit` 적용)
- daemon host-bytes cap + evict-on-host-pressure
