# Playbook: DROP 정리 및 write-path 진단 (delta · tombstone · stale)

`DROP INDEX`가 데몬 VRAM/artifact를 제대로 회수하는지(3G.1), 그리고 INSERT/UPDATE/
DELETE 이후 sharded GPU path가 정합을 유지하는지(`.delta`/`.tombstone`/`.stale`,
3A + 3G.3 GPU delta cache) 진단한다.

---

## 1. 증상 (Symptoms)

- `DROP INDEX` 했는데 `pg_stat_gpu_shards`에 그 인덱스 shard가 남아 있다(또는 VRAM이
  안 줄어든다).
- 데몬 재시작 후 **이미 DROP한 인덱스가 zombie로 다시 로드**된다.
- 데몬이 죽은 상태에서 `DROP INDEX`가 실패할까 우려된다.
- INSERT한 행이 GPU 검색 결과에 안 나온다 / `delta_search_mode`가 기대와 다르다.
- DELETE한 행이 결과에 계속 나온다.

---

## 2. 진단

```sql
-- 해당 인덱스가 데몬에 상주하는지 (DROP 후엔 0이어야 함)
SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='<idx>';
```

**기대 출력 (DROP 후):**
```
 count
-------
     0
```
**→ 0:** 정상 cleanup  
**→ 0 아님:** 원인 A/B 분기 확인 — 데몬이 살아 있었는지 확인 → Step 2 (zombie 제거)로

```sql
-- write-path 상태
SELECT index_name, stale, stale_since, delta_rows, delta_generation,
       delta_merged_count, delta_search_mode
  FROM pg_stat_gpu_search WHERE index_name='<idx>';
```

**기대 출력 (INSERT 후 정상):**
```
 index_name | stale | delta_rows | delta_generation | delta_search_mode
------------+-------+------------+------------------+-------------------
 mg_cagra   | f     |          1 |                1 | gpu
```
**→ `delta_search_mode=cpu`:** 원인 E → Step 4 (write 정합 / GPU delta path 회복)로  
**→ `stale=t`:** write 발생 후 merge 대기 중 (정상 과도 상태)

```bash
# 인덱스의 모든 sidecar (DROP 후엔 전부 사라져야 함)
ls -1 /tmp/cuvs_indexes/<db>_<idx>.* 2>/dev/null
#  .cagra .tids .shards .sNNN.cagra .delta .tombstone .stale .relfilenode
```

**기대 출력 (DROP 후):**
```
No such file or directory
```
**→ 파일이 남아 있음:** 데몬-down 중 DROP 발생 → 원인 B → Step 2 (artifact 수동 정리)로

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -iE "dropped index|delta cache|crc"
```

**기대 출력 (정상 DROP):**
```
[INFO] pg_cuvs_server: dropped index <db>/<idx>
```
**→ 로그 없음 + artifact 잔존:** 데몬-down 중 DROP → Step 2로  
**→ `delta cache` 경고:** delta cache 불가 상태 → Step 4로

---

## 3. 원인 분기 (Cause branches)

### A. DROP cleanup 동작 (3G.1)
`DROP INDEX`는 **commit 시점**에 데몬으로 통지된다(backend `object_access_hook`이
cagra 인덱스 DROP을 모았다가 `XACT_EVENT_COMMIT`에서 `cuvs_ipc_drop` 발사). 데몬
`handle_drop`이 인덱스를 VRAM에서 free + registry compact + 모든 sidecar(`.cagra/
.tids/.shards/.sNNN.cagra/.delta/.tombstone/.stale/.relfilenode`)를 unlink한다.
→ DROP 후 `pg_stat_gpu_shards` 0행 + 재시작해도 zombie 없음.
→ Step 1 (DROP 후 정리 검증)으로

### B. 데몬-down 상태의 DROP
데몬이 죽어 있으면 `cuvs_ipc_drop`이 UNAVAILABLE → backend는 **WARNING만 남기고
DROP은 정상 commit**(DROP을 절대 막지 않음). 단 이 경우 데몬/디스크 정리가 안 되어
artifact가 남는다 → 데몬이 살아난 뒤 그 인덱스는 catalog에 없으므로, 남은 artifact는
수동/재시작 정리 대상.
→ Step 2 (zombie 제거 / artifact 수동 정리)로

### C. rolled-back DROP / DROP EXTENSION CASCADE (알려진 한계)
commit 시점 통지라 `BEGIN; DROP INDEX; ROLLBACK;`은 통지 안 됨(정상). 단 savepoint
rollback 내 DROP, 또는 `DROP EXTENSION CASCADE`가 AM을 먼저 제거하는 경우는 통지가
누락될 수 있다 → 그 결과 남는 artifact는 REINDEX/수동 정리.
→ Step 2 (artifact 수동 정리)로

### D. delta/tombstone/stale 의미
INSERT/UPDATE(new) → `.delta`(pending vectors), DELETE/UPDATE(old) → `.tombstone`,
기타 write → `.stale` 마커. 검색 시: sharded는 base shard fanout 결과 + **데몬 GPU
delta cache**(3G.3, shard 0 GPU에 brute-force)를 병합해 `delta_merged=1`로 GPU에서
처리; delta cache 불가(VRAM 부족/corrupt/generation mismatch)면 `delta_merged=0`으로
**backend CPU delta merge fallback**. tombstone은 backend가 snapshot-aware로 global
TID 기준 필터(샤딩 무관).
→ Step 3 (write 정합 확인)으로

### E. delta_search_mode가 cpu
`cuvs.delta_search_mode`(0=auto/1=cpu/2=gpu) 또는 GPU delta cache 미가용이면 cpu.
delta가 `cuvs.max_delta_rows` 초과면 GPU+delta 중지 → REINDEX 권고.
→ Step 4 (GPU delta path 회복)로

---

## 4. Step-by-step 복구

### Step 1 — DROP 후 정리 검증

```sql
DROP INDEX <idx>;
```

**기대 출력:**
```
DROP INDEX
```
**→ 에러 없음:** Step 계속

```sql
SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='<idx>';
```

**기대 출력:**
```
 count
-------
     0
```
**→ 0:** 정상 VRAM 해제 확인  
**→ 0 아님:** 데몬이 살아 있었는지 확인 → 살아 있었는데 남으면 데몬 바이너리가 3G.1 이전인지 점검

```bash
ls /tmp/cuvs_indexes/<db>_<idx>.* 2>/dev/null || echo "no artifacts (clean)"
```

**기대 출력:**
```
no artifacts (clean)
```
**→ 파일이 남아 있음:** 데몬-down 중 DROP이었음 → Step 2로  
**→ clean:** 데몬 재시작 후 다시 `pg_stat_gpu_shards` 조회해 zombie 없음 확인

---

### Step 2 — zombie 제거 / artifact 수동 정리

> 데몬-down 중 DROP이었던 경우(= artifact 잔존) 실행.

```bash
# catalog에 대응 인덱스 없는지 먼저 확인
ls /tmp/cuvs_indexes/<db>_<idx>.*
```

```bash
sudo rm -f /tmp/cuvs_indexes/<db>_<idx>.*
sudo systemctl restart pg-cuvs-server
```

**기대:** 재시작 후 `pg_stat_gpu_shards` 조회 시 해당 인덱스 행 없음  
**→ 성공:** zombie 없음 확인 → 완료  
**→ 여전히 행이 있음:** `journalctl | grep "loaded index"` 로 startup_load가 어떤 파일을 잡았는지 확인

---

### Step 3 — write 후 정합 확인

```sql
INSERT INTO mg VALUES (999999,
  (SELECT ('['||string_agg((random())::text,',')||']')::vector(16)
   FROM generate_series(1,16)));
```

```sql
SET cuvs.k=10; SET enable_cuvs=on; SET enable_seqscan=off;
SET cuvs.parallel_fanout=on;
SELECT bool_or(id=999999)
  FROM (SELECT id FROM mg
        ORDER BY v <-> (SELECT v FROM mg WHERE id=999999)
        LIMIT 10) q;
```

**기대 출력:**
```
 bool_or
---------
 t
```
**→ t:** INSERT한 행이 GPU 검색에 나옴 (delta cache 정상)  
**→ f:** delta path 문제 → Step 4로

```sql
SELECT delta_search_mode FROM pg_stat_gpu_search WHERE index_name='mg_cagra';
```

**기대 출력:**
```
 delta_search_mode
-------------------
 gpu
```
**→ gpu:** GPU delta cache 병합 정상 (Scenario 22 검증됨)  
**→ cpu:** Step 4로

---

### Step 4 — GPU delta path 회복

```sql
-- delta가 너무 커서 cpu로 떨어지면 base 재빌드로 delta/stale 해소
REINDEX INDEX <idx>;
```

**기대 출력:**
```
REINDEX
```

```sql
-- 재빌드 후 delta_search_mode 재확인
SELECT delta_search_mode FROM pg_stat_gpu_search WHERE index_name='<idx>';
```

**기대 출력:**
```
 delta_search_mode
-------------------
 gpu
```
**→ gpu:** 회복 완료  
**→ 여전히 cpu:** VRAM 여유 확인 + `.delta` generation이 현재 `.tids`와 맞는지 확인

```sql
-- GPU delta를 강제로 확인하고 싶을 때 (관측용)
SET cuvs.delta_search_mode = 2;   -- gpu (auto가 기본)
```

---

## 5. 검증 체크리스트

```sql
-- DROP cleanup: 0행
SELECT count(*) FROM pg_stat_gpu_shards WHERE index_name='<idx>';
```

**기대 출력:**
```
 count
-------
     0
```

```bash
# 디스크 sidecar 없음
ls /tmp/cuvs_indexes/<db>_<idx>.* 2>/dev/null || echo "no artifacts (clean)"
```

**기대 출력:**
```
no artifacts (clean)
```

```sql
-- sharded GPU delta: INSERT한 행이 검색에 나오고 데몬이 GPU 병합
SELECT bool_or(id=999999)
  FROM (SELECT id FROM mg
        ORDER BY v <-> (SELECT v FROM mg WHERE id=999999)
        LIMIT 10) q;
SELECT delta_search_mode FROM pg_stat_gpu_search WHERE index_name='mg_cagra';
```

**기대 출력:**
```
 bool_or
---------
 t

 delta_search_mode
-------------------
 gpu
```

- [ ] DROP 후 `pg_stat_gpu_shards` count = 0
- [ ] DROP 후 `/tmp/cuvs_indexes/<db>_<idx>.*` 없음 (no artifacts clean)
- [ ] 데몬 재시작 후에도 zombie 없음 (0행 유지)
- [ ] INSERT한 행이 GPU 검색 결과에 나옴 (`bool_or = t`)
- [ ] `delta_search_mode = gpu` (delta cache 가용 시)

> 검증됨(Scenario 22): sharded 인덱스에 INSERT한 행이 GPU delta cache로 병합되어
> 검색에 나오고 `delta_search_mode=gpu`, 결과는 CPU exact와 일치.

---

## 6. Escalation 기준 (When to escalate)

- DROP 후에도 shard 행이 남으면: 데몬이 살아 있었는지(통지 도달) 확인 — 죽어 있었으면
  위의 수동 정리. 살아 있었는데 남으면 데몬 바이너리가 3G.1 이전인지 점검.
- `delta_search_mode`가 계속 cpu: VRAM 여유(delta는 non-evicting, base를 밀어내지
  않음) + `.delta` generation이 현재 `.tids`와 맞는지(REINDEX로 정렬).
- DELETE한 행이 계속 보이면: tombstone generation mismatch 또는 stale 마커 누락 →
  REINDEX.
- delta가 `cuvs.max_delta_rows`를 상시 초과: write rate가 높음 → REINDEX 주기 단축
  또는 임계값/정책 재검토.

관련: `multi-gpu-sharding-ops.md`, `gcs-snapshot-ops.md`, `vram-oom-fallback.md`.
설계 근거: ADR-023(DROP-notify), ADR-024(shard-aware delta cache).
