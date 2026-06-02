# Playbook: Persistence 손상 복구

daemon 재시작 시 persisted index가 로드되지 않거나 `.tids` 검증이 실패하는 경우.

---

## 1. 증상 (Symptoms)

- daemon 재시작 후 journal에 특정 인덱스의 `loaded index` 메시지가 없다.
- journal에 `.tids validation failed`, `bad magic`, `crc mismatch`, `dim=0` 등의 메시지가 있다.
- journal에 `skip` 메시지와 함께 인덱스가 건너뛰어진다.
- 해당 인덱스에 대한 검색이 CPU fallback한다 (`WARNING: index not loaded on daemon`).
- `.cagra` 파일은 있으나 `.tids`가 없거나 반대인 경우.
- pre-1.0 코드베이스에서 생성된 headerless `.tids` 파일이 남아 있다.

---

## 2. 진단

### Step 1 — journal에서 손상 인덱스 확인

```bash
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'skip|validation|magic|crc|dim=0|FAILED'
```

**기대 출력 (손상 있는 경우):**
```
pg_cuvs: 16384/16392: .tids validation failed (bad magic)
pg_cuvs: skip index 16384/16392
```
**-> 출력 있음:** 손상된 인덱스 OID(`<db_oid>/<index_oid>`) 기록 후 Step 2로  
**-> 출력 없음 (손상 없음):** journal 전체를 확인하거나 daemon을 재시작 후 재확인

---

### Step 2 — magic bytes 확인

Step 1에서 확인한 OID로 `.tids` 파일의 첫 4바이트를 확인한다.

```bash
# <db_oid>_<index_oid>는 Step 1에서 확인한 값으로 교체
xxd /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids | head -1
```

**기대 출력 (정상 — magic 0x54494453 = 'T','I','D','S'):**
```
00000000: 5449 4453 0100 0000 ...
```

**기대 출력 (비정상 — magic 불일치):**
```
00000000: 0000 0000 0000 0010 ...
```

**-> 첫 4바이트가 `54 49 44 53`:** magic 일치 -> CRC 또는 n_vecs 오류 (원인 B/C) -> Step 3으로  
**-> 첫 4바이트가 다른 값:** headerless pre-1.0 파일 (원인 A) -> Step 3으로  
**-> 파일 없음 (`No such file`):** .tids만 없고 .cagra 있음 (원인 D) -> Step 3으로

---

### Step 3 — 파일 쌍 정합성 확인

```bash
ls -lh /tmp/cuvs_indexes/
```

**기대 출력 (정상):**
```
-rw-r--r-- 1 postgres postgres 45M ... 16384_16392.cagra
-rw-r--r-- 1 postgres postgres 12K ... 16384_16392.tids
```

**-> .cagra 있고 .tids 없음:** 원인 D (쌍 불완전) -> Step 4로  
**-> .cagra 크기 0 bytes:** serialize 도중 crash -> Step 4로  
**-> .tmp 파일 있음:** atomic rename 미완료 잔재 -> Step 4에서 함께 제거  
**-> 쌍 완전하나 validation 실패:** 원인 A/B/C -> Step 4로

---

## 3. 원인 분기 (Cause branches)

### A. .tids magic/version 불일치 — pre-1.0 headerless 파일

`CuvsTidsHeader.magic != CUVS_TIDS_MAGIC(0x53444954)` 이면 headerless 구형 파일이다.
pre-1.0 코드베이스가 생성한 `.tids`는 header 없이 raw uint64 배열만 기록했다.
이 파일은 의도적으로 거부된다. 복구 방법: REINDEX만 가능.
-> 복구 Step 4로

### B. .tids CRC 불일치 — 디스크 손상 또는 부분 write

`body_crc32`가 본문과 맞지 않는다. 디스크 오류 또는 fsync 전 crash로 발생.
복구 방법: REINDEX.
-> 복구 Step 4로

### C. n_vecs 범위 오류 — 파일이 잘렸거나 0바이트

`n_vecs <= 0` 또는 `n_vecs > CUVS_TIDS_MAX_VECS(1,000,000,000)`.
0 bytes `.tids` 파일: serialize 직전 crash. 복구 방법: REINDEX.
-> 복구 Step 4로

### D. .cagra 또는 .tids 중 하나만 존재 (쌍 불완전)

`startup_load_indexes`는 `.cagra` 기준으로 스캔하고 대응 `.tids`를 함께 열어 검증한다.
`.tids`가 없으면 `fopen` 실패로 `load_index`가 -1을 반환하고 해당 인덱스를 skip한다.
반대로 `.tids`만 있고 `.cagra`가 없으면 해당 쌍은 스캔 대상에서 제외된다.
복구 방법: 모든 경우 REINDEX.
-> 복구 Step 4로

### E. dim=0 — .tids header가 깨짐

`thdr.dim == 0` 이면 load를 abort한다. 복구 방법: REINDEX.
-> 복구 Step 4로

---

## 4. Step-by-step 복구

인덱스 artifact는 derived data다 — WAL 대상이 아니다. 원본 데이터(heap)는 PostgreSQL에
온전히 남아 있으므로 `REINDEX`로 재생성하면 된다. 데이터 손실 없음.

### Step 1 — daemon 중지

```bash
sudo systemctl stop pg-cuvs-server
```

**기대 출력:**
```
(출력 없음, 즉시 복귀)
```

```bash
sudo journalctl -u pg-cuvs-server --no-pager | tail -5
```

**기대 출력:**
```
pg_cuvs: sigterm: N indexes saved
pg_cuvs: shutdown complete
```
**-> "shutdown complete" 확인:** Step 2로  
**-> daemon이 이미 중지된 상태:** 문제 없음, Step 2로

---

### Step 2 — 손상된 artifact 쌍 제거

Step 1 (진단)에서 journal에서 추출한 OID 목록을 확인한다.

```bash
# journal에서 손상 인덱스 OID 추출
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'validation failed|skip' \
  | grep -oP '\d+/\d+' | sort -u
```

**기대 출력:**
```
16384/16392
16384/16395
```

```bash
# 손상된 쌍 제거 (db_oid/index_oid -> db_oid_index_oid 형식으로 변환)
rm /tmp/cuvs_indexes/16384_16392.cagra 2>/dev/null
rm /tmp/cuvs_indexes/16384_16392.tids  2>/dev/null

# OID가 여럿이면 각 쌍에 대해 반복
```

**기대 출력:**
```
(출력 없음)
```
**-> 성공:** Step 3으로  
**-> "No such file" 경고:** 이미 없음, 문제 없음 -> Step 3으로

---

### Step 3 — .tmp 잔재 제거 및 daemon 재시작

```bash
# .tmp 잔재 제거
rm -f /tmp/cuvs_indexes/*.tmp

# daemon 재시작
sudo systemctl start pg-cuvs-server
```

**기대 출력:**
```
(출력 없음)
```

```bash
sudo journalctl -u pg-cuvs-server -n 20 --no-pager
```

**기대 출력:**
```
pg_cuvs: listening on /tmp/.s.pg_cuvs
pg_cuvs: startup_load_indexes: loaded N indexes
```
**-> "listening on" 확인:** Step 4로  
**-> start 실패:** `journalctl -xe` 확인 후 에스컬레이션

---

### Step 4 — REINDEX

```sql
-- 손상된 인덱스 이름 확인
SELECT indexname, tablename, indexdef
FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%';
```

**기대 출력:**
```
 indexname  | tablename |              indexdef
------------+-----------+--------------------------------------
 cagra_idx  | items     | CREATE INDEX ... USING cagra ...
```

```sql
-- 인덱스 재빌드 (인덱스명 기준)
REINDEX INDEX cagra_idx;
-- 또는 테이블 기준으로 전체 재빌드
REINDEX TABLE items;
```

**기대 출력:**
```
REINDEX
```
**-> 성공:** Step 5 (검증)로  
**-> "ERROR: pg_cuvs: BUILD failed (status 4)":** daemon 미구동 -> Step 3으로 돌아가 daemon 재시작  
**-> "ERROR: pg_cuvs: BUILD failed (status 2)":** VRAM 부족 -> create-index-failure-diagnosis.md Step 2로

---

## 5. 검증 체크리스트

```bash
# REINDEX 후 daemon이 새 artifact를 로드했는지 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -20
```

**기대 출력:**
```
pg_cuvs: built index 16384/16392 (N vecs, M MB VRAM)
```
- [ ] `built index <db_oid>/<index_oid>` 메시지가 있다

```bash
# artifact 쌍 정합성 확인
ls /tmp/cuvs_indexes/*.cagra | while read f; do
  base=${f%.cagra}
  if [ ! -f "${base}.tids" ]; then
    echo "[WARN] missing .tids for $f"
  else
    echo "[OK] $f"
  fi
done
```

**기대 출력:**
```
[OK] /tmp/cuvs_indexes/16384_16392.cagra
```
- [ ] `[WARN]` 없이 `[OK]`만 출력된다

```bash
# .tids magic 재확인
xxd /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids | head -1
```

**기대 출력:**
```
00000000: 5449 4453 0100 0000 ...
```
- [ ] 첫 4바이트가 `54 49 44 53` (TIDS magic)이다

```sql
-- 검색 결과 확인 (GPU path 사용 여부)
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
SET cuvs.debug = off;
```

**기대 출력:**
```
NOTICE:  pg_cuvs: cagra scan ...
```
- [ ] `NOTICE: pg_cuvs: cagra scan` 메시지가 있다 (GPU path 사용 중, CPU fallback 없음)

---

## 6. Escalation 기준 (When to escalate)

- REINDEX 후에도 동일 인덱스에 대해 `.tids validation failed`가 반복되면:
  디스크 하드웨어 오류 가능성. `dmesg | grep -i 'error\|EIO'` 확인.
- `.tids`의 magic bytes가 올바른데 CRC가 계속 틀리면:
  파일 시스템 수준의 메타데이터 손상 가능성. `fsck` 검토.
- 복수의 인덱스가 동시에 손상된 경우: 단일 crash가 아닌 반복적 문제.
  `--index-dir`을 신뢰할 수 있는 파일 시스템으로 변경하는 것을 검토.
