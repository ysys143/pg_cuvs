# pg_cuvs — GEARS Specification

GEARS (Generalized EARS) 구문:
```
[Where <정적 전제조건>]
[While <상태 전제조건>]
[When <트리거>]
The <주체> shall <행동>
```

이 문서는 `/ouroboros:interview` 세션에서 확정된 설계 결정을 GEARS 형식으로 정교화한 것이다.
ADR은 `design/DECISIONS.md` 참조. 이 문서는 ADR을 검증 가능한 요구사항으로 변환한다.

---

## 1. IPC 레이어 (ADR-002)

**IPC-01**
```
When a PostgreSQL backend issues a vector similarity query targeting a cagra index,
the pg_cuvs extension shall send a JSON command frame over a Unix Domain Socket
to pg_cuvs_server.
```

**IPC-02**
```
When a PostgreSQL backend sends a search command,
the pg_cuvs extension shall place the query vector in a pre-allocated
shm_open shared memory segment and reference it by handle in the UDS command,
such that no vector data crosses the socket boundary.
```

**IPC-03**
```
Where `cuvs.socket_path` GUC is not set,
the pg_cuvs_server shall bind to `/tmp/.s.pg_cuvs.<postmaster_pid>`.
```

**IPC-04**
```
While pg_cuvs_server is unreachable (connect() returns ECONNREFUSED or ETIMEDOUT),
the pg_cuvs extension shall fall back to pgvector HNSW and emit a WARNING to the
PostgreSQL log with the reason.
```

---

## 2. 인덱스 영속화 (ADR-002, Q3)

**PERSIST-01**
```
Where `cuvs.index_dir` GUC is not set,
the pg_cuvs_server shall use `$PGDATA/cuvs_indexes/` as the index storage path.
```

**PERSIST-02**
```
When `CREATE INDEX USING cagra` completes successfully,
the pg_cuvs_server shall serialize the built index to
`<cuvs.index_dir>/<database_oid>_<index_oid>.cagra` using the cuVS native
serialization format.
```

**PERSIST-03**
```
When pg_cuvs_server receives SIGTERM,
the pg_cuvs_server shall serialize all VRAM-resident indexes to `cuvs.index_dir`
before releasing CUDA resources and exiting.
```

**PERSIST-04**
```
When pg_cuvs_server starts,
the pg_cuvs_server shall scan `cuvs.index_dir` and load all indexes whose
corresponding PostgreSQL index OIDs still exist in `pg_catalog.pg_index`.
```

**PERSIST-05**
```
When `DROP INDEX` is executed on a cagra index,
the pg_cuvs extension shall notify pg_cuvs_server to unload the index from VRAM
and delete the corresponding file from `cuvs.index_dir`.
```

---

## 3. VRAM 관리 — 3계층 OOM 정책 (Q5)

**VRAM-01**
```
While loading a new index into VRAM,
the pg_cuvs_server shall check free VRAM against the index's estimated footprint
before invoking cuVS build or load APIs.
```

**VRAM-02**
```
When VRAM is insufficient to load a requested index and at least one other index
is resident in VRAM,
the pg_cuvs_server shall serialize the least-recently-used resident index to
`cuvs.index_dir`, free its VRAM allocation, and retry the load operation.
```

**VRAM-03**
```
When VRAM eviction of all resident indexes is insufficient to accommodate the
requested index,
the pg_cuvs_server shall refuse the load request and notify the pg_cuvs
extension to fall back to pgvector HNSW for that query.
```

**VRAM-04**
```
Where `cuvs.max_vram_mb` GUC is set,
the pg_cuvs_server shall treat that value as the upper bound on VRAM allocation
regardless of physical GPU VRAM availability.
```

---

## 4. CPU Fallback — 4가지 트리거 (ADR-003, Q6)

**FALLBACK-01 — 코스트 모델**
```
When the PostgreSQL planner estimates the cagra index scan cost to exceed the
sequential scan or HNSW scan cost (accounting for startup_cost=1000 IPC overhead
and per_tuple_cost=0.0001),
the planner shall select the cheaper CPU path without invoking pg_cuvs_server.
```

**FALLBACK-02 — GUC 수동 전환**
```
Where `enable_cuvs = off`,
the pg_cuvs extension shall not contact pg_cuvs_server and shall route all
queries to pgvector HNSW.
```

**FALLBACK-03 — 데몬 장애**
```
While pg_cuvs_server is unavailable,
the pg_cuvs extension shall fall back to pgvector HNSW for every query and
emit a WARNING once per backend session until the daemon recovers.
```

**FALLBACK-04 — Circuit Breaker**
```
While the number of consecutive GPU errors for a given index exceeds
`cuvs.circuit_breaker_threshold` (default: 3),
the pg_cuvs extension shall disable GPU routing for that index,
emit an ERROR-level log entry, and fall back to CPU until
`SELECT pg_cuvs_reset_circuit('<index_name>')` is called.
```

---

## 5. 쓰기 처리 — Lazy Rebuild (Q9)

**WRITE-01**
```
When a row is inserted, updated, or deleted in a table that has a cagra index,
the pg_cuvs extension shall record the affected TID in an in-memory pending-delta
set without modifying the VRAM-resident index.
```

**WRITE-02**
```
While the pending-delta set for a cagra index exceeds `cuvs.rebuild_threshold`
(default: 10% of index size),
the pg_cuvs extension shall emit a WARNING advising the operator to run VACUUM.
```

**WRITE-03**
```
When AUTOVACUUM or manual VACUUM processes a table that has a cagra index,
the pg_cuvs_server shall rebuild the index from the current heap state and
replace the resident VRAM index atomically.
```

---

## 6. Phase 2 — DiskANN (Q7)

**DISKANN-01**
```
When `CREATE INDEX USING diskann` is executed,
the pg_cuvs_server shall build a Vamana graph using cuVS GPU acceleration
(`cuvsVamanaBuild`) and serialize it to DiskANN binary format in `cuvs.index_dir`.
```

**DISKANN-02**
```
When a similarity query targets a diskann index,
the pg_cuvs extension shall perform CPU-side Vamana search
(`cuvsVamanaSearch`) on the NVMe-stored index without loading the full graph into VRAM.
```

**DISKANN-03**
```
Where a cagra index exists and `cuvs.export_hnsw = on`,
the pg_cuvs_server shall additionally export an HNSW-format copy via
`cuvs::neighbors::hnsw::from_cagra()` to support CPU fallback without re-reading
the heap.
```

**DISKANN-04 — Tiered Search**
```
While a cagra (VRAM-hot) index and a diskann (NVMe-cold) index both exist on the
same column,
the pg_cuvs extension shall query the cagra index first and supplement with
diskann results only when the cagra candidate set is below `k`.
```

---

## 7. Phase 3 — S3 연동 (Q8)

**S3-01**
```
Where `cuvs.s3_bucket` GUC is configured,
when an index build or rebuild completes,
the pg_cuvs_server shall upload the serialized index to S3 in chunked parts
using the path `s3://<bucket>/pg_cuvs/<cluster_id>/<database_oid>/<index_oid>/`.
```

**S3-02**
```
Where `cuvs.s3_bucket` is configured,
when pg_cuvs_server starts and a local index file is missing from `cuvs.index_dir`,
the pg_cuvs_server shall download the index from S3 to local NVMe cache
using io_uring async prefetch before servicing queries.
```

**S3-03**
```
The pg_cuvs index files shall be treated as derived data and shall not be
included in `pg_basebackup` WAL streams.
```

**S3-04**
```
Where multiple PostgreSQL instances share the same `cuvs.s3_bucket` and
`cuvs.cluster_id`,
read-only replicas shall load indexes from S3 without rebuilding from heap.
```

---

## 8. 코스트 모델 (ADR-003)

**COST-01**
```
The `cuvsamcostestimate` function shall set `startup_cost = 1000` to model
UDS IPC round-trip and CUDA context warm-up overhead.
```

**COST-02**
```
The `cuvsamcostestimate` function shall set `cpu_per_tuple = 0.0001` to reflect
GPU batch parallelism advantage over CPU sequential distance computation.
```

**COST-03**
```
When estimating cagra index scan cost,
the `cuvsamcostestimate` function shall scale `startup_cost` by
`(vector_dimensions / 1536.0)` to account for PCI-e transfer cost
proportional to vector size.
```

---

## 9. 운영 인터페이스 (GUC 목록)

| GUC | 타입 | 기본값 | 설명 |
|-----|------|--------|------|
| `enable_cuvs` | bool | on | GPU 경로 전역 토글 |
| `cuvs.socket_path` | string | (auto) | UDS 소켓 경로 |
| `cuvs.index_dir` | string | `$PGDATA/cuvs_indexes/` | 인덱스 직렬화 경로 |
| `cuvs.max_vram_mb` | int | 0 (unlimited) | VRAM 사용 상한 |
| `cuvs.circuit_breaker_threshold` | int | 3 | 연속 실패 임계값 |
| `cuvs.rebuild_threshold` | float | 0.10 | delta 비율 임계값 (VACUUM 권고) |
| `cuvs.export_hnsw` | bool | off | CAGRA 빌드 시 HNSW 병행 export |
| `cuvs.s3_bucket` | string | '' | Phase 3 S3 버킷 (비어있으면 비활성) |
| `cuvs.cluster_id` | string | '' | Phase 3 멀티노드 식별자 |
