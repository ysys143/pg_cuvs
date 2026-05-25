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

## 2. 확인 명령 (Diagnostic commands)

```bash
# daemon journal에서 skip/validation 오류 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep -E 'skip|validation|magic|crc|dim=0|FAILED'

# index_dir의 파일 목록 (쌍이 완전한지 확인)
ls -lh /tmp/cuvs_indexes/
# 예상: <db_oid>_<index_oid>.cagra + <db_oid>_<index_oid>.tids 쌍

# .tids 파일의 첫 4바이트 (magic 확인: 'TIDS' = 0x54494453 little-endian = 54 49 44 53)
# CUVS_TIDS_MAGIC = 0x53444954 (little-endian 'TIDS')
xxd /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids | head -2
# 첫 4바이트가 54 49 44 53이 아니면 magic 불일치

# .cagra 파일 크기 (0 bytes면 serialize 도중 중단됨)
ls -la /tmp/cuvs_indexes/*.cagra

# tmp 파일 잔재 (원자적 rename이 완료되지 못한 흔적)
ls /tmp/cuvs_indexes/*.tmp 2>/dev/null
```

```sql
-- 해당 인덱스가 pg_catalog에 존재하는지 확인
SELECT indexname, tablename, indexdef
FROM pg_indexes
WHERE indexdef LIKE '%USING cagra%';
```

---

## 3. 원인 분기 (Cause branches)

### A. .tids magic/version 불일치 — pre-1.0 headerless 파일

`CuvsTidsHeader.magic != CUVS_TIDS_MAGIC(0x53444954)` 이면 headerless 구형 파일이다.
pre-1.0 코드베이스가 생성한 `.tids`는 header 없이 raw uint64 배열만 기록했다.
이 파일은 의도적으로 거부된다. 복구 방법: REINDEX만 가능.

### B. .tids CRC 불일치 — 디스크 손상 또는 부분 write

`body_crc32`가 본문과 맞지 않는다. 디스크 오류 또는 fsync 전 crash로 발생.
복구 방법: REINDEX.

### C. n_vecs 범위 오류 — 파일이 잘렸거나 0바이트

`n_vecs <= 0` 또는 `n_vecs > CUVS_TIDS_MAX_VECS(1,000,000,000)`.
0 bytes `.tids` 파일: serialize 직전 crash. 복구 방법: REINDEX.

### D. .cagra 또는 .tids 중 하나만 존재 (쌍 불완전)

`startup_load_indexes`는 `.cagra` 기준으로 스캔하고 대응 `.tids`를 함께 열어 검증한다.
`.tids`가 없으면 `fopen` 실패로 `load_index`가 -1을 반환하고 해당 인덱스를 skip한다.
반대로 `.tids`만 있고 `.cagra`가 없으면 해당 쌍은 스캔 대상에서 제외된다.
복구 방법: 모든 경우 REINDEX.

### E. dim=0 — .tids header가 깨짐

`thdr.dim == 0` 이면 load를 abort한다. 복구 방법: REINDEX.

---

## 4. 복구 절차 (Recovery steps)

인덱스 artifact는 derived data다 — WAL 대상이 아니다. 원본 데이터(heap)는 PostgreSQL에
온전히 남아 있으므로 `REINDEX`로 재생성하면 된다. 데이터 손실 없음.

### 손상된 artifact 제거 후 REINDEX

```bash
# 1. daemon 중지 (SIGTERM으로 정상 종료 — 유효한 인덱스는 저장됨)
sudo systemctl stop pg-cuvs-server

# 2. 손상된 .cagra/.tids 쌍 제거
#    <db_oid>_<index_oid>는 journal의 skip 메시지에서 확인
rm /tmp/cuvs_indexes/<db_oid>_<index_oid>.cagra
rm /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids

# .tmp 잔재도 제거
rm -f /tmp/cuvs_indexes/*.tmp

# 3. daemon 재시작
sudo systemctl start pg-cuvs-server
sudo journalctl -u pg-cuvs-server -n 20 --no-pager
```

```sql
-- 4. PostgreSQL에서 인덱스 재빌드
--    인덱스 이름은 pg_indexes에서 확인
REINDEX INDEX cagra_idx;
-- 또는 테이블 기준으로 전체 재빌드
REINDEX TABLE items;
```

### pre-1.0 headerless .tids 파일 일괄 정리

```bash
# daemon 중지
sudo systemctl stop pg-cuvs-server

# journal에서 validation failed된 인덱스 OID 목록 추출 후 해당 쌍 제거
# (쌍 전체를 제거해야 불완전 쌍으로 인한 혼란을 막는다)
sudo journalctl -u pg-cuvs-server --no-pager | grep 'validation failed\|skip' \
  | grep -oP '\d+/\d+' | sort -u
# 출력 예: 16384/16392

rm /tmp/cuvs_indexes/16384_16392.cagra 2>/dev/null
rm /tmp/cuvs_indexes/16384_16392.tids  2>/dev/null

sudo systemctl start pg-cuvs-server
```

```sql
-- 해당 인덱스 전부 REINDEX
-- pg_indexes에서 cagra 인덱스 목록 확인 후
REINDEX INDEX <index_name>;
```

---

## 5. 검증 명령 (Verification commands)

```bash
# REINDEX 후 daemon이 새 artifact를 로드했는지 확인
sudo journalctl -u pg-cuvs-server --no-pager | tail -20
# "loaded index <db_oid>/<index_oid>" 또는 "built index" 메시지 확인

# artifact 쌍 정합성 확인 (쌍이 맞아야 함)
ls /tmp/cuvs_indexes/*.cagra | while read f; do
  base=${f%.cagra}
  if [ ! -f "${base}.tids" ]; then
    echo "[WARN] missing .tids for $f"
  else
    echo "[OK] $f"
  fi
done

# .tids magic 재확인
xxd /tmp/cuvs_indexes/<db_oid>_<index_oid>.tids | head -1
# 첫 4바이트: 54 49 44 53 (TIDS magic, little-endian)
```

```sql
-- 검색 결과 정확도 확인
SET cuvs.debug = on;
SELECT id FROM items ORDER BY embedding <-> '[1,0,0,0]'::vector LIMIT 1;
-- NOTICE: pg_cuvs: cagra scan ... 확인 (GPU path 사용)
SET cuvs.debug = off;
```

---

## 6. Escalation 기준 (When to escalate)

- REINDEX 후에도 동일 인덱스에 대해 `.tids validation failed`가 반복되면:
  디스크 하드웨어 오류 가능성. `dmesg | grep -i 'error\|EIO'` 확인.
- `.tids`의 magic bytes가 올바른데 CRC가 계속 틀리면:
  파일 시스템 수준의 메타데이터 손상 가능성. `fsck` 검토.
- 복수의 인덱스가 동시에 손상된 경우: 단일 crash가 아닌 반복적 문제.
  `--index-dir`을 신뢰할 수 있는 파일 시스템으로 변경하는 것을 검토.
