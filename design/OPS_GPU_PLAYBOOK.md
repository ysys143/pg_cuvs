# pg_cuvs GPU Operations Playbook

> 실측 기반 운영 가이드. 모든 권장값은 A100-40GB × 2, N=100K, dim=384 환경에서 측정됨.
> `bench/results/gpu_resources_bench.csv`, `bench/results/hnsw_import_bench.csv` 참조.

---

## 1. GPU 자원 파라미터 튜닝

pg_cuvs는 `work_mem`/`max_parallel_workers`에 해당하는 GPU 파라미터를 GUC로 제공한다.
실험 재현성을 위해 벤치마크 결과에 이 값들을 항상 기록한다.

### 1.1 cuvs.k (GPU candidate list 크기)

| cuvs.k | p50 변화 | recall@10 | 용도 |
|--------|---------|-----------|------|
| 10 | 기준 | 0.62 | 일반 권장하지 않음 (recall 급락) |
| 50 | +8% | 0.62 | 일반 권장하지 않음 |
| **100** | **+10%** | **0.80** | **기본값 (균형)** |
| 200 | +12% | 0.92 | 고정밀 요건 |

```sql
SET cuvs.k = 100;   -- 기본 (균형)
SET cuvs.k = 200;   -- 고정밀: latency +12%, recall +15%
```

cuvs.k < 100은 N=100K×384 합성 데이터에서 recall이 0.62 수준으로 급락했다.
실 데이터셋 특성에 따라 다를 수 있지만 100 미만은 테스트/디버그 목적으로만 사용한다.

### 1.2 cuvs.shard_count (멀티 GPU 샤딩)

| shard_count | p50 | recall@10 | 비고 |
|-------------|-----|-----------|------|
| 0 (auto) | — | — | VRAM 초과 시 자동 분산 (권장) |
| **1** | **기준** | **0.82** | **VRAM 여유 있을 때 latency 우선** |
| 2 | +70% | 0.92 | VRAM 부족 시에만 사용 |

```sql
SET cuvs.shard_count = 0;   -- 권장: VRAM 상황에 따라 자동
SET cuvs.shard_count = 1;   -- latency 최우선
SET cuvs.shard_count = 2;   -- recall 우선 또는 VRAM 부족
```

shard_count=2는 shard dispatch + merge 오버헤드로 latency가 70% 증가한다.
VRAM이 충분하다면 shard_count=1 (또는 auto=0)이 낫다.

### 1.3 cuvs.parallel_fanout (shard 병렬 dispatch)

| parallel_fanout | p50 | 권장 구간 |
|-----------------|-----|----------|
| 0 (sequential) | 1936us | N < 1M (실측) |
| 1 (parallel) | 2298us | N > 5M 예상 (미검증) |

```sql
SET cuvs.parallel_fanout = 0;   -- N < 1M: sequential이 18% 더 빠름 (실측)
SET cuvs.parallel_fanout = 1;   -- N > 5M: 가설 — 대규모에서 A/B 권장
```

N=100K에서 parallel은 스레딩 오버헤드가 GPU 병렬 이득을 초과한다.
N > 5M 구간은 아직 검증되지 않았으므로 실제 운영에서는 A/B 테스트 후 결정한다.

### 1.4 max_vram_mb (데몬 시작 플래그)

데몬 시작 시 `--max-vram-mb` 플래그로 지정한다 (GUC 아님).

```bash
# systemd override 예시
sudo systemctl set-environment CUVS_MAX_VRAM_MB=2048
sudo systemctl restart pg-cuvs-server
```

| max_vram_mb | 동작 |
|-------------|------|
| 40000 (기본) | 물리 GPU 전체 사용 |
| 2048 | N=100K×384 (≈750MB) 이내면 성능 무관 |
| < corpus_size×4 | 인덱스 적재 실패 → 에러 |

인덱스 크기 추정: `corpus_size × dim × 4bytes × 약 2~4배 (CAGRA graph)`.
예: 1M × 384 × 4 × 3 ≈ 4.6GB.

---

## 2. MIG (Multi-Instance GPU) 운영 가이드

### 2.1 MIG 개요

NVIDIA A100/H100에서 GPU를 최대 7개 MIG instance로 분할.
GCP에서는 MIG 활성화에 **VM 재부팅**이 필요하다.

pg_cuvs는 MIG를 코드 변경 없이 지원한다.
MIG instance가 일반 CUDA device처럼 노출되므로 `CUDA_VISIBLE_DEVICES=MIG-uuid`
설정만으로 원하는 slice에 데몬을 바인딩할 수 있다.

**검증 결과 (2026-06-01, A100-SXM4-40GB)**:

| 시나리오 | MIG 구성 | shard_count | N | 결과 |
|----------|----------|-------------|---|------|
| 단일 MIG | 1× 3g.20gb (20GB) | 1 | 50K | PASS |
| 멀티 MIG | 3× 1g.5gb (5GB each) | 3 | 30K | PASS |

### 2.2 GCP A100 MIG 활성화 (2단계)

MIG 활성화는 pending 상태로 설정된 후 재부팅이 완료되어야 적용된다.

**Step 1: MIG 활성화 + 재부팅**

```bash
sudo bash bench/test_mig.sh --setup
# 내부 동작:
#   sudo nvidia-smi -i 0 -mig 1   (pending 설정)
#   sudo reboot
```

**Step 2: 재부팅 후 인스턴스 생성 + 테스트 (비동기)**

```bash
nohup sudo bash bench/test_mig.sh --test > /tmp/test_mig.log 2>&1 &
sudo tail -f /tmp/test_mig.log
sudo grep 'PASS\|FAIL\|ERROR' /tmp/test_mig.log
```

**Teardown: MIG 해제 + 재부팅**

```bash
sudo bash bench/test_mig.sh --teardown
# 내부 동작:
#   nvidia-smi mig -i 0 -dci / -dgi   (compute/gpu instance 삭제)
#   nvidia-smi -i 0 -mig 0             (pending 해제)
#   sudo reboot
```

### 2.3 수동 MIG instance 생성

```bash
# GPU 0에 20GB MIG instance 1개 생성 (Profile 9 = 3g.20gb)
sudo nvidia-smi mig -i 0 -cgi 9 -C

# GPU 0에 5GB MIG instance 3개 생성 (Profile 19 = 1g.5gb)
sudo nvidia-smi mig -i 0 -cgi 19 -C
sudo nvidia-smi mig -i 0 -cgi 19 -C
sudo nvidia-smi mig -i 0 -cgi 19 -C

# MIG UUID 확인
nvidia-smi -L   # "MIG ... UUID: MIG-xxx" 형식으로 출력
```

### 2.4 pg_cuvs 데몬을 특정 MIG instance에 바인딩

```bash
# UUID 수집 (쉼표 구분)
MIG_UUIDS=$(nvidia-smi -L | grep 'MIG.*Device' \
    | sed 's/.*UUID: \(MIG-[^)]*\)).*/\1/' \
    | tr '\n' ',' | sed 's/,$//')

# systemd 환경변수로 전달
sudo systemctl set-environment CUDA_VISIBLE_DEVICES="$MIG_UUIDS"
sudo systemctl start pg-cuvs-server

# 단일 MIG instance인 경우 예시
sudo systemctl set-environment CUDA_VISIBLE_DEVICES="MIG-abc123..."
sudo systemctl start pg-cuvs-server
```

### 2.5 MIG 주요 제약

| 제약 | 내용 |
|------|------|
| 재부팅 필요 | GCP A100에서 MIG 활성화/비활성화 모두 재부팅 필요 |
| Profile 혼재 불가 | 같은 GPU에 다른 Profile(3g.20gb + 1g.5gb)은 동시 생성 불가 |
| NVLink 미지원 | MIG instance 간 peer-to-peer 없음 |
| N 제한 | 1g.5gb(5GB)에서 단일 shard는 N ≈ 1.3M×384 한도 (index≈5GB) |

---

## 3. GPU Build Accelerator (3I) 운영

### 3.1 offline import 절차

```sql
-- 0. CAGRA 인덱스 생성 (GPU 빌드 + .hnsw 사이드카 자동 저장)
CREATE INDEX my_cagra ON items USING cagra (embedding vector_l2_ops);

-- 1. pgvector HNSW 타겟 인덱스 준비 (빈 껍데기)
CREATE INDEX my_hnsw ON items USING hnsw (embedding vector_l2_ops);

-- 2. import (AccessExclusiveLock 획득 → 완료까지 해당 HNSW index 쿼리 불가)
SELECT pg_cuvs_import_hnsw('my_cagra'::regclass, 'my_hnsw'::regclass);

-- 3. 이후 pgvector HNSW로 서치 (GPU 불필요)
SET enable_cuvs = off;
SELECT * FROM items ORDER BY embedding <-> $1 LIMIT 10;
```

### 3.2 빌드 시간 참조 (VM 실측, 2026-06-01)

| N | dim | CAGRA build | HNSW import | GPU 합계 | pgvector native | speedup |
|---|-----|-------------|-------------|----------|-----------------|---------|
| 10K | 384 | 0.3s | 0.2s | **0.5s** | 1.3s | 2.6x |
| 100K | 384 | 2.7s | 2.1s | **4.7s** | 74.7s | **15.8x** |
| 1M | 384 | 27.3s | 39.0s | **66.3s** | 918.3s | **13.9x** |

recall@10 = 1.0000 (synthetic random 특성; 실제 임베딩에서는 낮아짐 — 재측정 필요)  
build/import speedup은 벡터 내용 무관, 실 데이터에서도 유사 예상.

### 3.3 offline-only 제약 설명

`pg_cuvs_import_hnsw`는 target HNSW index에 `AccessExclusiveLock`을 획득한다.
이는 REINDEX 와 동일한 수준의 잠금으로, import 트랜잭션이 커밋될 때까지
해당 인덱스를 사용하는 모든 SELECT가 블록된다.

권장 운영 패턴:
- 별도 테이블에 새 인덱스를 만들어 import 후 view/rename으로 교체 (minimal downtime)
- maintenance window 중에 import 실행
- `pg_cuvs_import_hnsw` 전에 `SET lock_timeout = '5min'` 으로 타임아웃 설정

### 3.4 crash recovery

`log_newpage_buffer`로 WAL full-page image를 기록한다.
트랜잭션 롤백 또는 crash 후 PostgreSQL 재시작 시 HNSW index는 일관된 상태로 복구된다.
(import 도중 crash → 재시작 후 해당 HNSW index는 truncation 이전 상태 또는 빈 상태로 복구됨;
재import 필요.)

---

## 4. 모니터링

```sql
-- GPU 인덱스별 검색 통계
SELECT index_name, search_count, avg_latency_us, p50_us, p95_us,
       build_time_ms, search_mode
FROM pg_stat_gpu_search
ORDER BY search_count DESC;

-- 현재 search_mode 확인
-- 0 = gpu_cagra, 1 = cpu_hnsw (3I-1 fallback), 2 = cpu_fallback
SELECT index_name, search_mode FROM pg_stat_gpu_search;
```

```bash
# GPU 상태
nvidia-smi --query-gpu=name,memory.used,memory.free,utilization.gpu \
    --format=csv,noheader

# 데몬 상태
sudo systemctl status pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 50
```

---

## 5. 알려진 제약 및 한계

| 항목 | 내용 |
|------|------|
| pgvector 의존 | hnsw_export.c는 pgvector 0.5.0+ 페이지 레이아웃(HNSW_VERSION=1)에 핀됨 |
| offline import only | AccessExclusiveLock; 온라인 교체 불가 |
| DiskANN no-go | cuVS 26.04 PQFlash API 불안정, 50M timeout; 3B 단계 보류 |
| MIG reboot 필요 | GCP A100 MIG 활성화/비활성화 시 VM 재부팅 |
| GCS 버킷 필요 | Phase 3G snapshot restore는 GCS 버킷 설정 필요 |
| parallel_fanout N>5M | 대규모 검증 미완료 (가설) |
