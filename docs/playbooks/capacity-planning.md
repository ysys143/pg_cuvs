# Playbook: 용량 계획 (capacity-planning)

CAGRA 인덱스 하나가 GPU에 차지하는 VRAM, 디스크 artifact 크기, delta 크기를
사전에 계산해 `--max-vram-mb`와 `shard_count`를 결정하는 절차.
수치는 `src/pg_cuvs_server.c:447 estimate_vram_bytes`,
`src/cuvs_util.c cuvs_auto_shard_count`, 실측 결과
(`design/BENCHMARK_CROSSOVER.md §11·§12·§13·§16`, `bench/results/`)에 근거한다.

---

## 1. 증상 (Symptoms)

이 playbook은 장애 대응이 아닌 사전 계획 절차다. 다음 상황에서 실행한다:

- 새 인덱스(N, dim)를 빌드하기 전에 VRAM이 충분한지 확인하고 싶을 때.
- `pg_stat_gpu_cache`에서 `vram_used_mb`가 `vram_budget_mb`에 근접하고 eviction이
  잦아 shard_count 조정이 필요할 때.
- 멀티 GPU VM 또는 `--max-vram-mb` 설정값을 결정해야 할 때.
- 인덱스 artifact 디스크 요구량(`cuvs.index_dir` 파티션)을 계획할 때.

---

## 2. 확인 명령 (Diagnostic commands)

```sql
-- 현재 GPU별 VRAM 사용/여유
SELECT gpu_device_id, vram_used_mb, vram_budget_mb,
       vram_budget_mb - vram_used_mb AS vram_free_mb,
       resident_count, evictions
  FROM pg_stat_gpu_cache
 ORDER BY gpu_device_id;
```

**기대 출력 (여유 있음):**
```
 gpu_device_id | vram_used_mb | vram_budget_mb | vram_free_mb | resident_count | evictions
---------------+--------------+----------------+--------------+----------------+-----------
             0 |         1638 |          36864 |        35226 |              1 |         0
```
**→ `vram_free_mb` 부족:** 새 인덱스 빌드 전 기존 인덱스 eviction 유도 또는
shard_count 증가 / VM 교체 검토.

```sql
-- 현재 로드된 인덱스별 VRAM 소비
SELECT index_name, dim, n_vecs, vram_bytes,
       round(vram_bytes / 1048576.0) AS vram_mb,
       shard_count, resident, gpu_device_id
  FROM pg_stat_gpu_search
 ORDER BY vram_bytes DESC;
```

```bash
# GPU 물리 VRAM (장치별 총용량)
nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader,nounits
# 기대: 0, NVIDIA A100-SXM4-40GB, 40448  (단위: MiB)
```

```bash
# artifact 디스크 현황
du -sh $PGDATA/cuvs_indexes/ 2>/dev/null || du -sh /tmp/cuvs_indexes/
ls -lh /tmp/cuvs_indexes/ | grep -v '^total'
```

---

## 3. 원인 분기 (Cause branches)

### A. VRAM 추정 공식

`estimate_vram_bytes`(src/pg_cuvs_server.c:447) 기준:

```
VRAM(bytes) ≈ N × (dim × 4 + 64)
```

| N | dim | VRAM 추정 | 비고 |
|---|-----|-----------|------|
| 100K | 384 | ≈ 155 MiB | 단일 A100-40GB에 충분 |
| 1M | 384 | ≈ 1.6 GiB | 단일 A100-40GB에 충분 |
| 1M | 1024 | ≈ 4.16 GiB | 단일 A100-40GB에 충분 |
| 10M | 384 | ≈ 16 GiB | 단일 A100-40GB 절반 |
| 50M | 384 | ≈ 73.24 GiB | **2×A100-40GB 초과** (빌드 워크스페이스 포함 시 교착) |

> 50M×384 실측(§12): shard_count=4로 corpus 로딩(76.8 GB) 성공 후 데몬 교착 → 강제 종료.
> 단일 노드 한계. **A100-80GB × 2(160 GB VRAM)** 이상 필요.

빌드 워크스페이스(그래프 중간 버퍼)는 위 추정치에 **추가로** 약 같은 크기를 더
쓸 수 있다. 안전 여유를 포함한 실효 한도:

```
per-GPU 안전 가용 VRAM = 물리 VRAM × cuvs.build_mem_safety_ratio
                       = 40,448 MiB × 0.5 ≈ 20 GiB  (기본 safety_ratio=0.5)
```

### B. auto shard_count 결정 규칙

`cuvs_auto_shard_count`(src/cuvs_util.c) 로직:

```
needed = estimate_vram_bytes(N, dim)
if needed <= per_gpu_budget:
    shard_count = 1  (unsharded)
else:
    shard_count = ceil(needed / per_gpu_budget)
    cap = min(n_gpus, 256, n_vecs / 2)
    shard_count = min(shard_count, cap)
```

`cuvs.shard_count=0`(auto)일 때 위 공식이 적용된다.
단일 GPU VM에서는 `n_gpus=1`이므로 auto는 항상 1(unsharded)로 떨어진다.
sharding은 **멀티 GPU VM**에서만 auto가 ≥2를 생성한다.

### C. `--max-vram-mb` 권장값

```
권장 = 물리 VRAM(MiB) × 0.70 ~ 0.80
```

| GPU | 물리 VRAM | 권장 --max-vram-mb |
|-----|-----------|-------------------|
| A100-40GB | 40,448 MiB | 28,000 ~ 32,000 |
| A100-80GB | 81,920 MiB | 57,000 ~ 65,000 |

100% 설정 시 CUDA 내부 컨텍스트 오버헤드로 OOM이 발생할 수 있다.
`cuvs.max_build_mem_mb=0`(auto) 상태에서는 `build_mem_safety_ratio=0.5`가
빌드 워크스페이스를 제한한다.

### D. artifact 디스크 크기 추정

| artifact | 크기 공식 | 예시 (1M×384) | 예시 (1M×1024) |
|----------|----------|----------------|----------------|
| `.cagra` | ≈ N×(dim×4+64) + 16 bytes | ≈ 1.6 GiB | ≈ 4.2 GiB |
| `.tids` | 32 + N×8 bytes | 1M → 8 MiB | 1M → 8 MiB |
| `.shards` | 40 + shard_count×40 bytes | shard=2 → ~120 B | shard=2 → ~120 B |
| `.delta` (최대) | 32 + max_delta_rows×(8+dim×4) | 10K rows, 384d → ≈ 15 MiB | 10K rows, 1024d → ≈ 41 MiB |
| `.s000.cagra` (sharded) | 위 `.cagra` ÷ shard_count | shard=2 → ≈ 800 MiB each | — |

> `.delta` 최대값은 `cuvs.max_delta_rows`(기본 10000) 기준.
> sharded 인덱스의 `.shards` manifest는 수십 바이트라 무시 가능.
> `.tombstone`/`.stale` 마커는 수 바이트.

---

## 4. 복구 절차 (Recovery steps)

### 단계 1 — 신규 인덱스 VRAM 사전 계산

```
N = 목표 벡터 수
dim = 벡터 차원

VRAM_estimate_MiB = N × (dim × 4 + 64) / (1024 × 1024)
```

예:
```
N=5,000,000, dim=768
= 5M × (768×4 + 64) / (1024×1024)
= 5M × 3136 / 1048576
≈ 14.95 GiB
```

```sql
-- 현재 여유와 비교
SELECT gpu_device_id,
       vram_budget_mb - vram_used_mb AS vram_free_mb
  FROM pg_stat_gpu_cache;
-- 여유 > VRAM_estimate + 빌드 워크스페이스(≈ estimate × 1.5 안전 마진)이면 OK
```

### 단계 2 — shard_count 및 GPU 구성 결정

```
per_gpu_safe = max_vram_mb × build_mem_safety_ratio
(기본: A100-40GB → max_vram_mb=36864, safety_ratio=0.5 → per_gpu_safe≈18 GiB)

if VRAM_estimate <= per_gpu_safe:
    shard_count = 0 (auto → 1)  # 단일 GPU 충분
else:
    need_shards = ceil(VRAM_estimate / per_gpu_safe)
    # need_shards개 GPU가 있어야 빌드 가능
    # 50M×384: VRAM_estimate=73 GiB, per_gpu_safe=18 GiB → need=5 shards
    #   2×A100-40GB로는 불가. A100-80GB(per_gpu_safe=36 GiB) × 2 → need=3, cap=2 → 미충족
    #   실측으로 50M×384는 단일 노드 한계 확인됨
```

### 단계 3 — `--max-vram-mb` 설정 및 데몬 재시작

```bash
# systemd unit ExecStart에 max-vram-mb 반영
sudo systemctl cat pg-cuvs-server | grep ExecStart
# ExecStart 라인에 --max-vram-mb 28000 (A100-40GB 권장) 또는 57000 (A100-80GB 권장) 추가
sudo systemctl daemon-reload
sudo systemctl restart pg-cuvs-server
```

**기대 출력 (journalctl):**
```
[INFO] pg_cuvs_server: GPU 0 (NVIDIA A100-SXM4-40GB): 40448 MB total, budget 28000 MB
[INFO] pg_cuvs_server: listening on /tmp/.s.pg_cuvs
```

### 단계 4 — 인덱스 빌드 및 VRAM 사용 확인

```sql
CREATE INDEX my_cagra ON t USING cagra (v vector_l2_ops);
-- 빌드 후 실제 VRAM 사용량 확인
SELECT index_name, round(vram_bytes/1048576.0) AS vram_mb, n_vecs, dim
  FROM pg_stat_gpu_search WHERE index_name='my_cagra';
```

---

## 5. 검증 명령 (Verification commands)

```sql
-- VRAM 예산 대비 사용률
SELECT gpu_device_id,
       vram_used_mb,
       vram_budget_mb,
       round(100.0 * vram_used_mb / vram_budget_mb, 1) AS use_pct,
       evictions, reloads
  FROM pg_stat_gpu_cache;
```

**기대 출력 (정상):**
```
 gpu_device_id | vram_used_mb | vram_budget_mb | use_pct | evictions | reloads
---------------+--------------+----------------+---------+-----------+---------
             0 |         1638 |          28000 |     5.9 |         0 |       0
```
**→ `use_pct < 80%`, `evictions = 0`:** 용량 충분  
**→ `evictions > 0` 또는 `use_pct >= 90%`:** budget 부족 → `--max-vram-mb` 상향 또는
기존 인덱스 REINDEX 필요성 검토

```sql
-- 실제 빌드된 artifact 크기와 추정 비교
SELECT index_name, n_vecs, dim,
       round(vram_bytes/1048576.0) AS vram_mb_actual,
       round(n_vecs::numeric * (dim*4.0+64) / 1048576, 1) AS vram_mb_est
  FROM pg_stat_gpu_search;
-- actual ≈ est 이면 estimate_vram_bytes 공식이 적중한 것
```

```bash
# artifact 크기 실측 (1M×384 예시)
ls -lh /tmp/cuvs_indexes/ | grep -E "\.cagra|\.tids|\.delta"
# .cagra ≈ 1.6 GiB, .tids ≈ 8 MiB (1M 기준)
```

- [ ] `pg_stat_gpu_cache.evictions = 0` (또는 허용 범위 내)
- [ ] `use_pct < 80%`
- [ ] `vram_mb_actual ≈ vram_mb_est` (추정 공식 검증)
- [ ] 빌드 완료 후 `resident = t`

> 검증 근거: `estimate_vram_bytes`(src/pg_cuvs_server.c:447) + `cuvs_auto_shard_count`
> (src/cuvs_util.c) 코드 로직; pilot 실측(BENCHMARK_CROSSOVER.md §11: 1M×384 CAGRA build
> 28s, §16: 1M×1024 CAGRA build 85s); 50M×384 한계 확인(§12: 2×A100-40GB 교착 → 단일 노드
> 한계).

---

## 6. Escalation 기준 (When to escalate)

- `VRAM_estimate > n_gpus × per_gpu_safe` (shard cap 이후에도 초과): 더 큰 GPU VM
  (A100-80GB 또는 GPU 수 증가) 필요. 50M×384의 경우 A100-80GB × 2(160 GB)가 최소 요건.
- 빌드 중 데몬이 `no CUDA GPUs detected` 또는 `VRAM budget exceeded`로 죽는 경우:
  `vram-oom-fallback.md` + `gpu-vm-lifecycle.md` 참조.
- `--max-vram-mb`를 물리 VRAM 90% 이상으로 설정해도 빌드 실패 반복: CUDA 내부
  컨텍스트 + cuBLAS 임시 버퍼가 예상보다 큼 → `build_mem_safety_ratio` 하향(0.4).
- `delta.vram_bytes` 계산에서 41 MiB(1024d, 10K rows)가 VRAM budget을 압박:
  `cuvs.max_delta_rows` 축소 또는 REINDEX 주기 단축 검토.

관련: `vram-oom-fallback.md`, `multi-gpu-sharding-ops.md`, `benchmark-runbook.md`.  
설계 근거: ADR-021(샤딩), ADR-024(sharded eviction), src/pg_cuvs_server.c estimate_vram_bytes.
