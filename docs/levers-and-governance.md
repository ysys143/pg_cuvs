# 레버 · 자원 거버넌스 · 세션 학습 (작업 문서)

> **목적**: ROADMAP "릴리스 준비 — 문서·운영 정비"가 바로 소비할 입력. 흩어진 레버/거버넌스 지식을
> 한곳에 모은 **작업용 reference**다. 문서 정리 시 §1·§2·§3은 운영자 reference/플레이북으로 승격하고,
> §5(감사 TODO)는 그 작업의 체크리스트로 쓴다.
> **정확성**: 레버 목록은 소스에서 추출(2026-06-11). 근거: ADR-069(design/DECISIONS.md), 세션 보고서
> docs/reports/2026-06-11-resource-governance-audit.md, PR #54.
> **상태 표기**: 이 문서 자체가 "현행 SSOT 후보"다. 운영자 문서로 승격되기 전까지는 작업 문서로 본다.

---

## §1. cuVS 레버 카탈로그 (소스 추출, 2026-06-11)

GUC 34개(`cuvs.*` 33 + `enable_cuvs`) · 인덱스 reloption 11개 · 데몬 CLI 플래그 9개.
"문서?" 열 = 현재 운영자 문서(README/OPS/best-practices/playbooks)에 설명이 있는가.

### 1.1 GUC

| GUC | 타입 | 기본값 | 범위 | 의미 | 문서? |
|-----|------|--------|------|------|-------|
| `enable_cuvs` | bool | true | — | GPU 라우팅 on/off | O (README/OPS) |
| `cuvs.debug` | bool | false | — | 진단 NOTICE 출력 | △ (playbook SET 예시만) |
| `cuvs.socket_path` | string | `/tmp/.s.pg_cuvs` | — | 데몬 UDS 경로 | △ (SPEC) |
| `cuvs.index_dir` | string | "" → `$PGDATA/cuvs_indexes` | — | 아티팩트 디렉터리 | O (best-practices/OPS) |
| `cuvs.circuit_breaker_threshold` | int | 3 | 1–100 | 연속 실패 차단 임계 | O |
| `cuvs.k` | int | 100 | 1–2000 | top-k | O |
| `cuvs.max_build_mem_mb` | int | 0(auto) | 0–INT_MAX | **빌드 host 메모리 하드캡** | △ (design만) |
| `cuvs.build_mem_safety_ratio` | real | 0.5 | 0.01–0.95 | auto cap = MemAvailable×ratio | △ (capacity 공식에만) |
| `cuvs.max_stale_fraction` | real | 0.10 | 0–1 | stale 허용 비율 | X |
| `cuvs.filter_auto_threshold` | real | 0.05 | 0–1 | 필터 선택도 라우팅 임계 | O (filter 실험문서) |
| `cuvs.stream_bf_selectivity_threshold` | real | 0.0(off) | 0–1 | out-of-core stream BF 트리거 | X (ADR-064만) |
| `cuvs.stream_bf_chunk_vectors` | int | 262144 | 1–INT_MAX | stream BF 청크 VRAM cap | X (ADR-064만) |
| `cuvs.max_delta_rows` | int | 10000 | 0–INT_MAX | delta cap(초과 시 REINDEX 권고) | △ (write-path playbook) |
| `cuvs.delta_search` | enum | auto | auto/cpu/gpu | delta 병합 검색 모드 | O (write-path) |
| `cuvs.search_mode` | enum | cagra | cagra/brute_force | 검색 모드 | O (README) |
| `cuvs.bf_precision` | enum | float32 | float32/float16 | BF 정밀도(fp16 VRAM 절반) | O (README/lesson) |
| `cuvs.bf_batch_wait_us` | int | 0(off) | 0–10000 | BF 배치 코얼레싱 대기 | △ (lesson) |
| `cuvs.max_batch_queries` | int | 1024 | 1–4096 | 배치 검색 상한 | X |
| `cuvs.shard_count` | int | 0(auto) | 0–CUVS_SHARDS_MAX | 멀티GPU 샤드 수 | O (OPS/README) |
| `cuvs.shard_overfetch` | int | 0 | 0–4096 | 샤드 over-fetch | O (sharding playbook) |
| `cuvs.parallel_fanout` | bool | true | — | 샤드 병렬 fanout | O (OPS) |
| `cuvs.cpu_hnsw_fallback` | bool | false | — | CPU HNSW fallback 사이드카 | O (README/OPS) |
| `cuvs.snapshot_uri` | string | "" | — | GCS 스냅샷 URI | O (GCS playbook) |
| `cuvs.cluster_id` | string | "" | — | 클러스터 식별(스냅샷 경로) | △ (SPEC) |
| `cuvs.gcs_key_file` | string | "" | — | GCS SA 키 파일 | O (GCS playbook) |
| `cuvs.warmup_threads` | int | 2 | 1–8 | warmup 스레드 풀 | △ (SPEC) |
| `cuvs.ivfpq_n_probes` | int | 64 | 1–4096 | IVF-PQ 검색 probe 수 | X (design만) |
| `cuvs.extend_chunk_size` | int | 0(auto) | 0–65536 | CAGRA extend 청크 | X (design만) |
| `cuvs.compact_delete_ratio` | real | 0.10 | 0–1 | auto-compact delete 트리거 | X |
| `cuvs.filtered_knn_hook` | bool | false | — | D-wedge Custom Scan hook | X (ADR만) |
| `cuvs.auto_compact` | bool | false | — | 백그라운드 auto-compact on | X (design만) |
| `cuvs.auto_compact_check_interval` | int | 60 | 10–3600 | bgworker 폴 간격(s) | X (design만) |
| `cuvs.auto_compact_threshold` | real | 0.10 | 0.01–1 | bgworker REINDEX 트리거 비율 | X (design만) |
| `cuvs.auto_compact_database` | string | "" | — | **POSTMASTER급**; bgworker 활성 대상 DB | X (소스만) |

### 1.2 인덱스 reloption

| reloption | AM | 타입 | 기본값 | 문서? |
|-----------|----|------|--------|-------|
| `index_dir` | cagra | string | "" | O (ADR-045/best-practices) |
| `graph_degree` | cagra | int | 64 (8–512) | △ (test/profiling만) |
| `intermediate_graph_degree` | cagra | int | 128 (8–1024) | X |
| `build_algo` | cagra | string | auto (auto/ivf_pq/nn_descent) | X (design만) |
| `n_lists` | ivfpq | int | 1024 (1–65536) | X (design만) |
| `pq_bits` | ivfpq | int | 8 (4–8) | X (design만) |
| `pq_dim` | ivfpq | int | 0(auto=ceil(dim/2)) | X (design만) |
| `source` | pg_cuvs_hnsw | string | "" | △ (README 예시) |
| `mode` | pg_cuvs_hnsw | string | nsw (nsw/hnsw/hnswlib/hnswlib_file) | O (OPS 표) |
| `m` | pg_cuvs_hnsw | int | 16 (2–100) | X |
| `ef_construction` | pg_cuvs_hnsw | int | 64 (4–1000) | X |

### 1.3 데몬 CLI 플래그

| 플래그 | 기본값 | 문서? |
|--------|--------|-------|
| `--socket` | `/tmp/.s.pg_cuvs.server` | △ |
| `--index-dir` | `/tmp/pg_cuvs_indexes` | O |
| `--max-vram-mb` | 0 → **물리 VRAM의 90%**(`CUVS_DEFAULT_VRAM_FRACTION=0.90`) | O (단, OPS §1.4 기본값 표기 "40000" **오류**) |
| `--max-indexes` | 1024 (`CUVS_DEFAULT_MAX_INDEXES`) | X |
| `--snapshot-uri` | "" | O (GCS) |
| `--cluster-id` | "" | △ |
| `--gcs-key-file` | "" | △ |
| `--warmup-threads` | 2 | △ |
| `--gpu-devices` | 감지된 전체 GPU | X |

---

## §2. 표준 PostgreSQL 레버 상호작용 (운영자 관점, ADR-069)

> **원칙**: 표준 PG 레버는 **PG 자신의 enforcement 기계(fd.c temp 파일, MemoryContext palloc, executor 취소)를
> 통과하는 자원만** 강제할 수 있다. pg_cuvs의 핵심 자원(memfd/shm 코퍼스 · 데몬 host RAM · GPU VRAM ·
> 외부 아티팩트)은 전부 그 밖이다.

| 표준 레버 | pg_cuvs 동작 | 운영자 함의 |
|-----------|--------------|-------------|
| `statement_timeout` / query cancel / `lock_timeout` | **준수** (3S: IPC wait 인터럽트, GPU 검색 ~544ms 내 취소) | 평소처럼 동작 |
| `max_parallel_maintenance_workers` | **준수** (병렬 CAGRA 빌드 게이트; 0이면 단일 프로세스) | CREATE INDEX 병렬도 제어됨 |
| 플래너 비용 (`enable_seqscan`/`enable_indexscan`/`enable_cuvs`) | **준수** (`cuvsamcostestimate`) | 표준 플래너 노브 그대로 |
| VACUUM (`ambulkdelete`/`amvacuumcleanup`) | **준수** (톰스톤 + auto-compact 트리거) | autovacuum이 tombstone 정리 유발 |
| `maintenance_work_mem` | **미준수** — 빌드 메모리는 `cuvs.max_build_mem_mb`/`build_mem_safety_ratio` 사용 | **이 노브 올려도 빌드 메모리 안 바뀜(조용히 무시)**. cuvs GUC를 써라 |
| `temp_file_limit` | **무효** — memfd/shm 코퍼스는 `fd.c` 밖이라 PG가 못 봄 | 빌드 코퍼스 제한에 쓸 수 없음 |
| host RAM 실제 천장 | **OS/cgroup** (`systemd MemoryMax=`, `RLIMIT_AS`). `cuvs.max_build_mem_mb`는 cgroup 벽 전 clean-error soft layer | 멀티테넌트 프로덕션은 cgroup으로 제한해야 함 |
| `shared_buffers` | **미적용**(의도) — CAGRA 아티팩트는 외부파일+GPU, 버퍼매니저 우회 | 인덱스가 shared_buffers 안 먹음 |
| `tablespace` | **미준수** — `CREATE INDEX ... TABLESPACE` 무시; `index_dir` GUC/reloption 사용 | tablespace 대신 `index_dir`로 배치 |
| `pg_stat_progress_create_index` | **미구현** | 장시간 CAGRA 빌드 진행률 안 보임 |

**자원별 진짜 레버 (정책 v3)**:

| 자원 | 진짜 enforcement | cuvs 자체 레버 역할 |
|------|------------------|---------------------|
| PG 기계 내부(취소·병렬·플래너·VACUUM) | 표준 PG 레버 (이미 준수) | — |
| host RAM(코퍼스·데몬 배열) | **OS/cgroup** | `cuvs.max_build_mem_mb` = clean-error soft cap |
| VRAM(빌드 scratch) | **reactive evict-and-retry** + RMM pool cap | `cuvs.max_vram_per_gpu`(=`--max-vram-mb`) = soft admission floor |
| 정확성/복제(아티팩트) | 백엔드 `.tids` 스탬프(timeline/system_identifier) — **미구현, 백로그** | 부재→degrade+WARNING, 불일치→ERROR |

---

## §3. 이번 세션 학습 (PR #54)

자원 거버넌스 감사 + 적대적 리뷰 2라운드에서 코드로 확정한 버그를 수정. **Tier-1 GREEN 27/27(ASAN 데몬) +
Tier-2 A100 installcheck 30/30 + isolation 3/3 + no-starvation 실측.**

| # | 버그 | 근본 원인 | 수정 | 검증 |
|---|------|-----------|------|------|
| 1 | VRAM 회계 누락 | `total_vram_used`가 unsharded `main_bf_vram_bytes`/sharded `shards[].bf_vram_bytes` 미합산 → eviction 과약정 → OOM. (IVF-PQ `ivfpq_vram_bytes`는 `vram_bytes`와 중복이라 비-합산) | 두 필드 합산 | `vram_accounting.sql` |
| 2 | 빌드 락 starvation | `handle_build`/`build_sharded`/`handle_build_multi`가 `g_index_mutex`를 GPU 빌드 내내 보유 → 검색/통계/드롭 블록 | reservation-counter(`g_pending_build_vram`)로 GPU 빌드 구간 언락, 디스크 커밋은 락 유지. **세 빌드 경로 전부** | `build_lock.sql` + Tier-2 실측(6.97s 빌드 중 동시검색 25회 각 50–110ms) |
| 3 | 빌드 OOM evict-retry 부재 | `cuvs_cagra_build` NULL(OOM 구분 불가) 즉시 BUILD_FAILED; `estimate_vram_bytes`가 빌드 scratch 미포함 | OOM 신호(`cuvs_last_build_was_oom`, RMM bad_alloc 포함) + evict 후 1회 재시도. **세 빌드 경로 전부**. `inject_build_oom` seam(IPC opcode 20) | `build_oom.sql`, `build_multi_oom.sql` |
| 4(부수) | IVF-PQ eviction 크래시 | `evict_lru→save_index→cuvs_cagra_serialize(e->handle)` SEGV — IVF-PQ는 `handle==NULL`. **기존 잠복**(IVF-PQ 인덱스 evict 불가; maxidx는 CAGRA-only라 못잡음). #3 retry가 노출 | `evict_lru` IVF-PQ 분기(save 없이 free+reload, sharded 패턴) + `save_index` NULL 방어 | ASAN 백트레이스로 확정 |

**방법론 학습**:
- **ASAN으로 확정, 추측 패치 금지**: #3 retry가 Tier-1에서 데몬 SEGV → 가설(UAF) 세웠으나 **Tier-1 데몬을
  `-fsanitize=address`로 빌드**(`EXTRA_SERVER_CFLAGS` 훅, ci.yml)해 백트레이스로 진짜 원인(IVF-PQ NULL handle) 확정.
  UAF로 추측 패치했으면 못 고쳤을 것. **ASAN은 Tier-1 데몬 빌드에 상시 편입**.
- **로컬 macOS는 데몬 무실행**(MAP_ANONYMOUS는 `-D_DARWIN_C_SOURCE`로 syntax-check만; PG14≠PG16) → 데몬
  동시성/크래시 클래스는 **CI-ASAN 또는 Tier-2(A100)가 유일 검증 수단**. 자세히 [[reference_tier1_sql_test_authoring]].
- **Tier-1 SQL 테스트 함정**: GPU/BF 경로 강제는 `SET enable_seqscan=off` + 리터럴 쿼리벡터(서브쿼리 operand는
  seqscan 유발). VRAM은 MB 반올림(코퍼스 ≥~2MB 필요). 병렬빌드 강제는 `min_parallel_table_scan_size=0` +
  `max_parallel_maintenance_workers>0`. expected는 손작성 말고 CI `regression.diffs`→`patch`로 reconcile.

**관련 코드**: `src/pg_cuvs_server.c`(회계/eviction/락/retry), `src/cuvs_wrapper.{cu,h}`·`cuvs_wrapper_shim_cpu.c`
(OOM 신호/inject), `src/pg_cuvs.c`·`cuvs_ipc.{c,h}`(SQL fn/IPC), `.github/workflows/ci.yml`(ASAN),
`test/sql/{vram_accounting,build_lock,build_oom,build_multi_oom}.sql`.

---

## §4. 남은 작업 (트리거 백로그 — ROADMAP 등재됨)

- `build_sharded` 멀티GPU 검증 (2+ GPU 필요, dev VM은 단일 A100)
- scratch-aware VRAM admission (intermediate/graph degree 기반 또는 RMM pool release-threshold cap)
- 백엔드 아티팩트 스탬프(timeline/system_identifier) — standby/PITR fail-closed
- corpus → PG `BufFile` 옵션 (`temp_file_limit`을 *진짜로* 적용)
- daemon host-bytes cap + evict-on-host-pressure
- cgroup/systemd `MemoryMax=` 운영 가이드

---

## §5. 문서화 감사 결과 (문서 정리 작업의 체크리스트)

2026-06-11 감사(Explore 3종 + 코드 검증). 운영자 문서 = README / design/OPS_GPU_PLAYBOOK.md /
docs/best-practices.md / docs/playbooks/*.

### 5.1 [높음] 드리프트 — 문서가 존재하지 않는 GUC를 안내 (코드로 확정: 소스 정의 0개)
- `design/SPEC.md §13`(line 664/666/667): `cuvs.max_vram_mb`·`cuvs.rebuild_threshold`·`cuvs.export_hnsw` — **셋 다 소스에 없음**.
  실제: VRAM 상한은 데몬 플래그 `--max-vram-mb`(GUC 아님); delta 트리거는 `cuvs.compact_delete_ratio`/`max_delta_rows`;
  export_hnsw는 deferred(미구현).
- `design/SPEC.md` line 154/265/294 본문도 위 가짜 GUC 참조.
- `README.md:179`: `cuvs.gcs_bucket` GUC 설정 안내 — **없음**. 실제는 `cuvs.snapshot_uri`/`cluster_id`/`gcs_key_file`.
- **조치**: 위 정정(저비용·즉시).

### 5.2 [높음] 단일 레버 reference 부재
- GUC/reloption/플래그 통합 reference 문서 없음(이 문서 §1이 그 시드). `design/SPEC.md §13`이 가장 근접하나
  GUC 12개만 + 스테일 + `design/`(내부).
- §1을 `docs/reference-guc.md`(또는 가이드사이트 References) 운영자 reference로 승격.

### 5.3 [높음] ADR-069 거버넌스가 운영자 문서에 0건
- `maintenance_work_mem`/`temp_file_limit`/`cgroup`/`MemoryMax` 언급이 README/OPS/best-practices/playbooks에
  **전무**(코드로 확인). ADR-069 + 세션 보고서에만 존재.
- 운영자가 `maintenance_work_mem`로 빌드 메모리 제한 시도 → 조용히 무효(경고 없음).
- **조치**: §2를 OPS 플레이북 "표준 PG 레버" 절로 승격.

### 5.4 [중간] 신규 산출물이 런북에 미반영(backport 필요)
- `pg_stat_gpu_fallback` 뷰(PR#43) — 모든 playbook에 **0건**(fallback 급증의 핵심 관측 지표인데).
- ADR-069 수정(IVF-PQ eviction 크래시, 빌드 OOM evict-retry) — 세션 보고서에만; `vram-oom-fallback.md`/
  `create-index-failure-diagnosis.md`에 미반영.
- 모니터링 임계값이 4개 문서에 산재 → 단일 알림 기준 없음.

### 5.5 [중간] 미설명 레버 (§1에서 "X"/"△")
- 완전 미설명 GUC: `auto_compact*`(4), `stream_bf_*`(2), `ivfpq_n_probes`, `max_batch_queries`,
  `extend_chunk_size`, `compact_delete_ratio`, `filtered_knn_hook`, `max_stale_fraction`.
- 완전 미설명 reloption: CAGRA `intermediate_graph_degree`/`build_algo`, IVF-PQ `n_lists`/`pq_bits`/`pq_dim`,
  hnsw `m`/`ef_construction`.
- 미설명 플래그: `--max-indexes`, `--gpu-devices`.

### 5.6 [낮음] TBD/스테일
- `release-upgrade.md`: cross-version 업그레이드 = TBD(템플릿만, migration script 없음).
- `replica-bootstrap.md`: 스트리밍 물리복제 standby warmup = 미검증(TBD).
- `OPS_GPU_PLAYBOOK §1.4`: `--max-vram-mb` 기본값 "40000" 표기 오류(실제 0→90%).
- `rollback-and-cleanup.md:47` `extversion = 1.0` vs `release-upgrade.md` `0.1.0` 불일치.
- `large-dataset-benchmark.md`: backend peak RSS 컬럼 빈칸.

### 5.7 구조: 운영 문서가 흩어져 있음
- 운영 표면이 5곳 분산(OPS_GPU_PLAYBOOK / playbooks 16종 / best-practices / README / reports).
- ROADMAP "릴리스 준비 — 문서·운영 정비"가 이미 자가진단: "흩어진 런북을 OPS_GPU_PLAYBOOK로 연결",
  "단일 reference 부재", "host RAM cgroup 가이드". → *잊은* 게 아니라 *아직 안 한* 상태.

### 정상 (잘 돼 있음 — 유지)
증상기반 playbook 16종(daemon-down/VRAM-OOM/create-index 실패/영속성 손상/GCS/샤딩/capacity/rollback),
VRAM 예산 공식·eviction/LRU·`.so` 재로드 시 PG 재시작·GCS round-trip — 견고.
