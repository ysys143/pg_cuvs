# Competitive Baseline Plan — pgvectorscale / VectorChord vs pg_cuvs

> 목적: **"pgvectorscale / VectorChord와 비교해도 pg_cuvs가 의미 있는 구간이 남는가"**를
> Pareto frontier로 숫자화한다.
> 상위 컨텍스트: [[BENCHMARK_CROSSOVER.md]] §11 (HNSW vs CAGRA 1차 crossover 완료).

## 1. 비교 대상

| 시스템 | 버전 | 알고리즘 | 연산 | 설치 방식 |
|---|---|---|---|---|
| pgvector HNSW | 최신(이미 설치) | HNSW | CPU, RAM-resident | 이미 설치됨 |
| **pgvectorscale** | v0.9.0 | StreamingDiskANN + SBQ | CPU, disk-resident | .deb 또는 cargo+pgrx |
| **VectorChord** | v1.1.1 | IVF + RaBitQ | CPU, disk-resident | .deb 또는 cargo+make |
| pg_cuvs CAGRA | 현재 빌드 | CAGRA | GPU, VRAM-resident | 이미 설치됨 |

CPU 베이스라인 둘 다 **larger-than-RAM**을 지원하는 disk-resident 인덱스 → HNSW와의 또 다른 축.

## 2. 설치 계획 (VM: `pg-cuvs-dev`, Ubuntu 22.04, PG16, x86_64)

### 2.1 pgvectorscale

```bash
# .deb 설치 (권장)
curl -s https://packagecloud.io/install/repositories/timescale/timescaledb/script.deb.sh | sudo bash
sudo apt-get install -y postgresql-16-pgvectorscale

# 설치 확인
psql -d postgres -c "SELECT * FROM pg_available_extensions WHERE name = 'vectorscale';"

# 공유 라이브러리 등록 (shared_preload_libraries 불필요 — vectorscale은 불필요)
```

패키지 없을 경우 fallback (소스 빌드):
```bash
sudo apt-get install -y cargo clang libclang-dev
cargo install --locked cargo-pgrx
cargo pgrx init --pg16 pg_config
git clone --branch 0.9.0 https://github.com/timescale/pgvectorscale && cd pgvectorscale/pgvectorscale
cargo pgrx install --release
```

### 2.2 VectorChord

```bash
# .deb 설치 (권장, PG16 확인 필요)
wget https://github.com/tensorchord/VectorChord/releases/download/1.1.1/postgresql-16-vchord_1.1.1-1_amd64.deb
sudo apt install ./postgresql-16-vchord_1.1.1-1_amd64.deb

# shared_preload_libraries에 추가 필요
psql -c "ALTER SYSTEM SET shared_preload_libraries = 'pg_cuvs,vchord';"
sudo systemctl restart postgresql
```

패키지 없을 경우 fallback (소스 빌드):
```bash
# VectorChord는 x86_64 소스 빌드 시 Clang 필수 (GCC 링커 오류)
sudo apt-get install -y clang
curl -fsSL https://github.com/tensorchord/VectorChord/archive/refs/tags/1.1.1.tar.gz | tar -xz
cd VectorChord-1.1.1 && CC=clang make build && make install
psql -c "ALTER SYSTEM SET shared_preload_libraries = 'pg_cuvs,vchord';" && sudo systemctl restart postgresql
```

### 2.3 Smoke 확인 (설치 완료 후)

```bash
# AVX2 플래그 확인 (pgvectorscale 런타임 요구사항)
grep -c avx2 /proc/cpuinfo && echo "avx2 OK"

# Extension 로드 확인
psql -d bench -c "CREATE EXTENSION IF NOT EXISTS vectorscale CASCADE;"
psql -d bench -c "CREATE EXTENSION IF NOT EXISTS vchord CASCADE;"

# 소형 smoke build (N=1000, dim=64)
psql -d bench <<'SQL'
DROP TABLE IF EXISTS smoke;
CREATE TABLE smoke(id int, v vector(64));
INSERT INTO smoke SELECT i, array_fill(random()::float4, ARRAY[64])::vector
  FROM generate_series(1,1000) i;
CREATE INDEX ON smoke USING diskann (v vector_l2_ops) WITH (num_neighbors=30);
SELECT count(*) FROM smoke ORDER BY v <-> smoke.v LIMIT 5;
DROP TABLE smoke CASCADE;
SQL

# VectorChord smoke (vchordrq.probes GUC 유무 확인)
psql -d bench -c "SHOW vchordrq.probes;" 2>&1 || echo "no probes GUC — need multi-index Pareto"
```

### 2.4 실패 기준 (smoke에서 하나라도 해당하면 중단·문서화)
- `CREATE EXTENSION` 실패 (없는 .so, 버전 mismatch)
- 소형 build 5분 초과 (이상 이상)
- Segfault / PostgreSQL crash
- avx2 미확인 → pgvectorscale 런타임 실패 위험

## 3. DDL 템플릿

### pgvectorscale DiskANN

```sql
-- 튜닝 파라미터: num_neighbors (build degree), search_list_size (build ef)
CREATE INDEX idx_diskann ON items USING diskann (v vector_l2_ops)
  WITH (num_neighbors = 50, search_list_size = 100);

-- 검색 시 GUC 조정으로 recall/latency sweep 가능 (단일 인덱스에서 OK)
SET diskann.query_search_list_size = 50;   -- 낮을수록 빠르고 recall 낮음
SELECT id FROM items ORDER BY v <-> $1 LIMIT 10;
```

### VectorChord IVF+RaBitQ

```sql
-- 튜닝 파라미터: lists (IVF 클러스터 수), residual_quantization
-- 런타임 GUC 유무는 smoke에서 확인 (vchordrq.probes 또는 vchordrq.ef_search)
-- GUC 없으면 → recall sweep = lists 달리해 인덱스 복수 빌드
CREATE INDEX idx_vchord_L4096 ON items USING vchordrq (v vector_l2_ops)
  WITH (options = $$
    [build.internal]
    lists = [4096]
    spherical_centroids = false
    residual_quantization = true
    build_threads = 8
  $$);
-- 예비 (낮은 lists = 빠른 build, 낮은 recall):
CREATE INDEX idx_vchord_L1024 ON items USING vchordrq (v vector_l2_ops)
  WITH (options = $$ [build.internal] lists = [1024] residual_quantization = true $$);
```

### Reference baselines (이미 확보)

```sql
-- pgvector HNSW (기존 harness, ef_search GUC로 sweep)
CREATE INDEX ON items USING hnsw (v vector_l2_ops) WITH (m=16, ef_construction=200);
SET hnsw.ef_search = 40;

-- pg_cuvs CAGRA (기존 harness, cuvs.k GUC로 sweep)
CREATE INDEX ON items USING cagra (v vector_l2_ops);
SET cuvs.k = 128;
```

## 4. Tuning 전략 — Pareto frontier

목표: **단일 최적 설정이 아니라 recall/latency Pareto curve**를 각 엔진에서 추출해 비교.

| 엔진 | Recall sweep 방법 |
|---|---|
| HNSW | `hnsw.ef_search` 5–10개 값 sweep (단일 인덱스, query time) |
| pgvectorscale | `diskann.query_search_list_size` sweep (단일 인덱스, query time) |
| VectorChord | smoke에서 GUC 확인 → GUC 있으면 단일 인덱스; 없으면 `lists` 달리해 2–3개 인덱스 |
| CAGRA | `cuvs.k` sweep (기존 harness, single GPU) |

각 sweep point에서 기록:
- `recall@10` (vs GT)
- `p50_us`, `p95_us`, `avg_us` (클라이언트 측 `\timing` 기반, 동일 잣대)
- `QPS` (pgbench -c8 -T15)
- `index_bytes` (`pg_relation_size`)

**Non-dominated** points(recall↑ 또는 latency↓가 동시에 개선되지 않는 점)만 Pareto로 남긴다.

### Recall target 기준 요약 (summary 표에 사용)

| recall target | 의미 |
|---|---|
| 0.90 | 최소 품질 기준 |
| 0.95 | 주 비교 기준 (HNSW baseline 대비) |
| 0.99 | 고정밀 구간 (엔진별 도달 여부) |

각 target에서 가장 낮은 `avg_latency_us`를 달성한 엔진이 해당 구간의 승자.

## 5. Pilot 매트릭스

| cell | N | dim | k | 목적 |
|---|---|---|---|---|
| **A** | 100K | 384 | 10 | CAGRA crossover 하단 — 작은 N, CPU가 유리한 구간 |
| **B** | 1M | 384 | 10 | CAGRA 1차 우위 구간 — CPU 베이스라인이 얼마나 따라오는가 |
| **C** | 1M | 1536 | 10 | CAGRA 결정적 우위 구간 — 고차원에서 격차 확인 |

Cell A는 pgvectorscale/VectorChord가 가장 유리한 구간 → 여기서 지면 pg_cuvs는 전 구간 우위.

실행 순서: A → B → C (각 cell 완료 후 중간 확인, 이상하면 중단).

## 6. Harness 설계

### 신규 파일

| 파일 | 역할 |
|---|---|
| `bench/run_pgvectorscale.sh` | diskann 인덱스 빌드 + `diskann.query_search_list_size` sweep |
| `bench/run_vectorchord.sh` | vchordrq 인덱스 빌드 + recall sweep (GUC 또는 multi-index) |
| `bench/pareto.py` | recall-latency Pareto 추출 + CSV → frontier 표 출력 |

### 결과 CSV 스키마 확장 (기존 호환)

기존 `pilot.csv` 헤더에 `index_bytes` 열 추가:
```
system,index,N,dim,k,recall_target,build_s,qps,p50_us,p95_us,p99_us,avg_us,recall_at_k,params,index_bytes
```

`bench/run_pilot.sh`의 CSV append 부분에 `pg_relation_size('bench_idx')` 추가 예정.

### 기존 재사용

- `bench/common.py`, `bench/gt.py`, `bench/recall.py`, `bench/pctl.py` — 변경 없이 재사용.
- 동일 `data.fbin`/`gt.ibin` → 공정성 보장 (같은 GT로 모든 엔진 채점).
- `bench/bench_1m1536.sh` 패턴(스트림 로드) → VectorChord/pgvectorscale도 동일 방식.

## 7. 실행 순서 (VM 비용 최소화)

```text
1. [무비용] 본 계획 문서 확정 (지금)
2. [VM 기동] 설치 smoke (2.3절) + 소형 build 확인 → 성공 시 다음
3. [VM 계속] Cell A (100K×384) × 4 engines × recall sweep
4. [결과 확인] Cell A Pareto 확인 → 의미 있으면 B로
5. [VM 계속] Cell B (1M×384) × 4 engines
6. [결과 확인] Cell B 확인 → C 진행 여부 결정
7. [VM 계속 or 종료] Cell C (1M×1536) — CPU baseline build time이 길 수 있음(VectorChord 빠를 것)
8. [VM 정지] TERMINATED 확인
9. [무비용] Pareto 표 + "when to use" 갱신 + 커밋
```

## 8. Go / No-Go 기준

| 결과 | 판정 | 조치 |
|---|---|---|
| pg_cuvs가 recall@0.95 기준 Cell C에서 CPU 대비 latency 2× 이상 우위 | **GO** (강한 포지션 유지) | README "when to use" 표에 CPU baseline 대비 수치 추가 |
| CPU baseline이 N=1M×384에서 pg_cuvs와 동등한 recall@latency | **PARTIAL** | hot-tier 포지션 좁힘; 1M×1536 차별화 강조 |
| CPU baseline이 모든 셀에서 pg_cuvs를 recall@latency로 이김 | **NO-GO** (현재 단일 GPU 기준) | 멀티-GPU CAGRA(Phase 3F)로 재검증 또는 포지션 재정의 |
| pgvectorscale/VectorChord 설치 smoke 실패 | **ABORT** | 원인 기록 후 설치 방법 변경 검토 |

## 9. 운영 기준

- VM은 실행 직전 명시적으로 start, 결과 확인 직후 stop/TERMINATED 확인.
- ephemeral IP 변경 시 `.env.gpu` 및 gcloud 명령 IP 업데이트.
- 단일 cell build가 **2시간** 초과 → 중단, 원인(OOM? CPU 병목?) 문서화.
- 설치 실패 또는 crash → VM 정지, 원인 기록 후 별도 issue.
- 모든 실측 결과는 `bench/results/competitive.csv`에 append (pilot.csv와 분리).

## 10. 불확실성 / 확인 필요 항목

| 항목 | 내용 | 확인 방법 |
|---|---|---|
| VectorChord 런타임 GUC | `vchordrq.probes` 또는 `vchordrq.ef_search` 유무 | `SHOW vchordrq.probes;` in smoke |
| pgvectorscale .deb PG16 가용성 | packagecloud에 PG16 .deb 있는지 | smoke 설치 시 확인 |
| VectorChord .deb PG16 amd64 가용성 | GitHub releases 페이지 확인 | `wget` 시 404 여부 |
| VectorChord build 시간 @ 1M×1536 | IVF k-means는 빠를 수 있음(분 단위 예상) | Cell C smoke |
| pgvectorscale SSD 이점 | `/tmp`는 tmpfs라 SSD 이점 없음; 결과 해석 시 주의 | Cell B 결과 해석 |
