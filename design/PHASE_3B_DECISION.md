# Phase 3B — NVMe Cold Tier / DiskANN: Go/No-Go Decision

**날짜**: 2026-05-31
**결론**: **NO-GO** — 구현 보류. GPU hot tier 포지셔닝으로 고정.

---

## 배경

Phase 3B의 목적은 CAGRA GPU 인덱스가 VRAM에 들어가지 않는 대규모 데이터셋을
NVMe 기반 DiskANN/Vamana 방식으로 처리하는 것이었다.

제안된 구현 경로:
1. **cuVS Vamana in-memory** — cuVS 네이티브 Vamana API 사용
2. **cuVS Vamana → PQFlash disk** — GPU build + NVMe 검색
3. **MS DiskANN native serializer** — pgvectorscale 방식 재구현
4. **자체 NVMe serializer** — pg_cuvs 자체 구현

---

## Spike 결과 (2026-05-29~30)

### cuVS Vamana 경로 (경로 1, 2)

- **cuVS 26.04 기준 NO-GO**: `cuvs::neighbors::vamana` API가 build/search까지는 동작하나
  PQFlash disk path (`vamana.save()` 이후 NVMe 검색)가 현재 cuVS 버전에서 미완성 또는 불안정.
- cuVS 네이티브 Vamana in-memory: API가 CAGRA 대비 안정성이 낮고 product path 삼기 부적절.

### 50M×384 DiskANN Competitive Benchmark (2026-05-30)

pgvectorscale 공식 벤치마크 조건(BUILD_MEM=2GB)으로 재현:

| 조건 | 결과 |
|---|---|
| N=50M, dim=384, cache=2GB | 50M 중 ~470K(0.9%) 적재 후 cache full → 5h timeout |
| p50 latency | 측정 불가 (인덱스 미완성) |
| recall | 측정 불가 |

비교: HNSW 50M (BUILD_MEM=64GB)은 p50=13ms, QPS=546으로 실용적.

### 비교 엔진 결과 (50M×384)

| engine | build | p50 | QPS | recall |
|---|---|---|---|---|
| HNSW | 21,879s | 13ms | 546 | ~0.93 (1M pilot 기준) |
| vchordrq | 5,784s | 49ms | 152 | 0.9991 |
| DiskANN 2GB | TIMEOUT | NA | NA | NA |
| CAGRA | FAILED | NA | NA | NA |

---

## Go/No-Go 판단 기준

### DiskANN이 필요한 조건

1. 데이터가 GPU VRAM에 들어가지 않는다 (float32 기준: A100-40GB → 10M×384 한계)
2. 데이터가 RAM에도 들어가지 않는다 (HNSW가 비현실적)
3. 쿼리 latency가 수백 ms 허용 가능하다 (NVMe I/O 특성)
4. 운영 비용이 GPU 추가 구매보다 저렴하다

### 현재 상황

- **조건 1**: 50M×384 이상에서 CAGRA 한계 확인 (ADR-025)
- **조건 2**: 50M에서 HNSW가 여전히 실용적 (p50=13ms, 64GB RAM)
- **조건 3**: DiskANN 검색 latency는 수십~수백 ms 수준 → 많은 use case에서 부적합
- **조건 4**: 100M 이상에서 GPU 추가가 DiskANN 구현 비용보다 저렴할 수 있음

### 결론

DiskANN이 실질적으로 필요한 스케일은 **1B+ 벡터** 수준이다.
현재 pg_cuvs의 target은 그 아래다. 구현 비용 대비 가치가 없다.

---

## 결정

**Phase 3B 구현 중단.**

### 현재 포지셔닝

```text
pg_cuvs = "GPU VRAM에 들어가는 hot vector workload"

N <~ 10K                      → pgvector HNSW
N 10K ~ VRAM 한계             → pg_cuvs CAGRA
N > VRAM 한계, RAM 수용 가능  → HNSW (CPU)
N > RAM 한계 (1B+)            → DiskANN / sharding 재검토 시점
```

### 3B 재검토 조건

다음 중 하나가 충족될 때 재개:
1. 1B+ 벡터 수요가 실제 고객/파트너에서 확인됨
2. cuVS PQFlash API가 stable release에서 검증됨
3. VRAM 128GB+ GPU(예: H100 NVMe) 환경에서 GPU build + NVMe search가
   HNSW 대비 명확한 우위를 보임

---

## 영향 범위

- `src/`: 변경 없음 (Phase 3B 코드 없음)
- `design/DECISIONS.md`: ADR-026 참조
- `design/BENCHMARK_CROSSOVER.md`: §12 DiskANN 결과 참조
- 향후 README의 "When to use" 섹션에 위 포지셔닝 반영 필요
