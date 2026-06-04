# Playbook: GCS 스냅샷으로 새 노드 부트스트랩 (replica-bootstrap)

GCS 스냅샷(`cuvs.snapshot_uri`)을 이용해 새 노드(또는 artifact 유실 노드)에서
풀 재빌드 없이 warmup으로 CAGRA 인덱스를 복원하는 절차.
sharded 인덱스(`.tids`+`.shards`+N개 `.sNNN.cagra`)를 포함한다.

**범위 한정**: 이 playbook은 **heap-compatible 노드**(같은 heap에서 만든
`.relfilenode` sidecar가 있는 노드)에서 GCS 다운로드로 artifact를 복원하는 운영을
다룬다. PostgreSQL 스트리밍 물리 복제(WAL shipping / `primary_conninfo`) 환경에서
pg_cuvs artifact가 동기화되는 경로는 **TBD: 실제 replica 검증 필요** (물리 replica는
heap과 .relfilenode가 자동으로 복사되지만 데몬 측 index_dir의 artifact 동기화
타이밍 및 warmup 트리거 동작이 검증되지 않았다).

---

## 1. 증상 (Symptoms)

- 새 노드 투입 후 모든 쿼리가 CPU fallback으로 빠진다(`WARNING: pg_cuvs_server unreachable`
  또는 `pg_cuvs: cagra index not loaded`).
- `pg_stat_gpu_search`에서 `warmup_state`가 `pending` 또는 `downloading`으로 계속
  멈춰 있다.
- `pg_stat_gpu_search`에서 `resident=f`이고 `download_count=0`이라 다운로드 자체가
  시작되지 않는다.
- 로그에 `HEAP COMPAT MISMATCH ... relfilenode mismatch` 또는 `manifest SHA mismatch`.

---

## 2. 확인 명령 (Diagnostic commands)

```sql
-- warmup 상태 및 다운로드 카운터 확인
SELECT index_name, warmup_state, last_warmup_at, warmup_duration_ms,
       download_count, cache_miss_count, resident, last_status
  FROM pg_stat_gpu_search;
```

**기대 출력 (복원 완료 후):**
```
 index_name | warmup_state | download_count | resident | last_status
------------+--------------+----------------+----------+-------------
 my_cagra   | ready        |              1 | t        | ok
```
**→ `warmup_state=pending`:** snapshot_uri 설정 또는 GCS 인증 미완 → 원인 A/B  
**→ `warmup_state=downloading`:** 다운로드 진행 중 (정상 과도 상태; 완료 대기)  
**→ `warmup_state=failed`:** 로그 확인 → 원인 C/D

```bash
# 데몬 로그: warmup enqueue → 다운로드 → CRC 검증 → 로드
sudo journalctl -u pg-cuvs-server --no-pager -n 100 | \
  grep -iE "warmup|download|snapshot|objstore|manifest|relfilenode|sha|crc"
```

**기대 출력 (정상 흐름):**
```
[INFO] [objstore] warmup enqueued for <db>/<idx>
[INFO] [objstore] downloading manifest for <db>/<idx>
[INFO] [objstore] sharded download verified OK for <db>/<idx> (N shards)
[INFO] pg_cuvs_server: loaded sharded index <db>/<idx> (... vecs, N shards)
```

```bash
# .relfilenode sidecar 존재 여부 (warmup의 전제조건)
ls -1 $PGDATA/cuvs_indexes/*.relfilenode 2>/dev/null || \
  echo "no .relfilenode — warmup triggers require this file"
# 또는 cuvs.index_dir 경로:
ls -1 /tmp/cuvs_indexes/*.relfilenode 2>/dev/null
```

```bash
# 데몬 systemd unit에 snapshot-uri / cluster-id 설정 여부
sudo systemctl cat pg-cuvs-server | grep -E "snapshot-uri|cluster-id|warmup-threads"
```

---

## 3. 원인 분기 (Cause branches)

### A. snapshot_uri / cluster_id 미설정 — 가장 흔한 함정
warmup은 데몬 `--snapshot-uri gs://<bucket>`(+`--cluster-id`)가 있어야 GCS 경로를
알 수 있다. 이 인수가 없으면 cold registry에 넣어도 다운로드가 enqueue되지 않고
`warmup_state`가 `pending`으로 멈춘다. `cuvs.snapshot_uri` GUC만으로는 부족하다(데몬
측 인수 필요). → 복구 Step 1.

### B. GCS 인증 실패 (SA 없거나 key_file 미설정)
데몬은 (1) instance metadata SA 토큰, 실패 시 (2) `cuvs.gcs_key_file`의 SA JSON으로
인증한다. VM에 SA가 없고 key_file도 없으면 manifest 요청 시 HTTP 403/401로 실패.
→ `gpu-vm-lifecycle.md`에서 SA 부착 후 데몬 재시작. → 복구 Step 1.

### C. `.relfilenode` sidecar 없음
데몬 startup은 `.relfilenode` sidecar가 있는 인덱스만 cold warmup 후보로 등록한다.
새 노드에 heap dump/restore는 됐지만 `.relfilenode`가 누락된 경우 warmup이 트리거되지
않는다. `.relfilenode`는 `CREATE INDEX USING cagra` 시 pg 백엔드가 `$index_dir/` 아래에
기록한다. heap이 복원된 후 `REINDEX`로 재생성하는 것이 가장 안전하다.
→ 복구 Step 2.

### D. heap relfilenode mismatch (heap 불호환)
manifest의 `relfilenode`가 이 노드의 `.relfilenode`와 다르면 데몬이 로드를 거부한다
(`HEAP COMPAT MISMATCH`). 이 인덱스는 다른 heap(다른 pg 클러스터의 dump 등)에서
만들어진 것이므로 그 노드에서 REINDEX가 필요하다. → 복구 Step 2.

### E. manifest SHA mismatch (artifact 손상)
GCS에서 받은 `.tids`나 `.shards`의 SHA256이 manifest 기록과 다르면 데몬이 거부한다
(`manifest SHA mismatch`). 업로드 전 artifact가 손상됐거나 GCS 객체가 부분 덮어써진
경우. 원본 노드에서 GCS에 재업로드(REINDEX 또는 데몬 재시작으로 재업로드)가 필요하다.
→ 복구 Step 3.

---

## 4. 복구 절차 (Recovery steps)

### Step 1 — 데몬 설정 확인 및 snapshot-uri / 인증 설정

```bash
# 현재 unit에 snapshot-uri가 있는지 확인
sudo systemctl cat pg-cuvs-server | grep ExecStart
```

없으면 unit ExecStart에 추가:

```bash
# /etc/systemd/system/pg-cuvs-server.service ExecStart 예시:
# ExecStart=/usr/local/bin/pg_cuvs_server \
#   --index-dir /tmp/cuvs_indexes \
#   --max-vram-mb 40000 \
#   --snapshot-uri gs://<bucket> \
#   --cluster-id <cluster-id> \
#   --warmup-threads 2
sudo systemctl daemon-reload
sudo systemctl restart pg-cuvs-server
```

**기대 출력 (journalctl):**
```
[INFO] pg_cuvs_server: snapshot uri = gs://<bucket>, cluster = <cluster-id>
[INFO] [objstore] warmup enqueued for <db>/<idx>
```
**→ enqueue 메시지 없음:** SA 인증 실패 가능 → `gcs-snapshot-ops.md`의 SA 부착 절차 수행

---

### Step 2 — `.relfilenode` 없거나 heap mismatch → REINDEX

```sql
-- 해당 인덱스를 이 노드에서 재빌드 (heap 호환 .relfilenode + 새 artifact 생성)
REINDEX INDEX <idx>;
```

**기대 출력:**
```
REINDEX
```

```bash
# .relfilenode가 생성됐는지 확인
ls -1 /tmp/cuvs_indexes/*relfilenode
```

**기대 출력:**
```
/tmp/cuvs_indexes/<db>_<idx>.relfilenode
```
**→ 파일 생성됨:** REINDEX 성공. GCS 업로드 대기(데몬이 자동으로 스냅샷 업로드)

---

### Step 3 — manifest SHA mismatch → 원본 노드에서 재업로드

원본 노드(artifact를 만든 노드)에서:

```bash
# 데몬 재시작으로 artifact를 재로드 → GCS에 재업로드 트리거
sudo systemctl restart pg-cuvs-server
sudo journalctl -u pg-cuvs-server --no-pager | grep "snapshot complete"
```

**기대 출력:**
```
[INFO] [objstore] sharded snapshot complete <db>/<idx> shards=N ts=... relfilenode=...
```

재업로드 완료 후 새 노드에서 데몬 재시작으로 warmup 재시도.

---

## 5. 검증 명령 (Verification commands)

```sql
-- warmup 완료 + 상주 확인
SELECT index_name, warmup_state, warmup_duration_ms, download_count, resident
  FROM pg_stat_gpu_search;
```

**기대 출력:**
```
 index_name | warmup_state | warmup_duration_ms | download_count | resident
------------+--------------+--------------------+----------------+----------
 my_cagra   | ready        |               8420 |              1 | t
```

```sql
-- sharded 인덱스: shard 수만큼 resident 확인
SELECT count(*) AS resident_shards
  FROM pg_stat_gpu_shards
 WHERE index_name='<idx>' AND resident;
-- 기대: shard_count 값과 일치
```

```bash
# 데몬 로그에서 sharded download verified 확인
sudo journalctl -u pg-cuvs-server --no-pager | grep "sharded download verified"
# 기대: [INFO] [objstore] sharded download verified OK for <db>/<idx> (N shards)
```

```sql
-- top-k가 CPU exact와 일치 (heap 호환 검증)
SET enable_cuvs=on; SET enable_seqscan=off;
CREATE TEMP TABLE g AS
  SELECT id FROM <table> ORDER BY v <-> (SELECT v FROM <table> WHERE id=<seed>) LIMIT 10;
SET enable_cuvs=off; SET enable_seqscan=on;
CREATE TEMP TABLE c AS
  SELECT id FROM <table> ORDER BY v <-> (SELECT v FROM <table> WHERE id=<seed>) LIMIT 10;
SELECT (SELECT array_agg(id ORDER BY id) FROM g) =
       (SELECT array_agg(id ORDER BY id) FROM c) AS topk_match;
-- 기대: t
```

- [ ] `warmup_state = ready`
- [ ] `download_count >= 1`
- [ ] `resident = t`
- [ ] sharded 인덱스: `resident_shards = shard_count`
- [ ] top-k 결과가 CPU exact와 일치

> 검증됨(Scenario 19, gcs-snapshot-ops.md): upload → wipe local(.relfilenode만 남김)
> → warmup download → `resident_shards = 2` + top-k가 CPU exact와 일치.

---

## 6. Escalation 기준 (When to escalate)

- `warmup_state=failed`가 반복되고 journalctl에 `403 Forbidden`: SA IAM 권한 부족.
  버킷에 `roles/storage.objectAdmin` 부여 여부 재확인 → `gcs-snapshot-ops.md`.
- `manifest SHA mismatch`가 원본 노드 재업로드 후에도 재발: GCS 객체가 중간에 변경됐을
  가능성. GCS 버킷 versioning 켜져 있으면 latest 객체 확인.
- `.relfilenode` mismatch가 반복(REINDEX 해도 나옴): heap OID가 달라졌을 가능성.
  `pg_indexes` 뷰에서 `indexrelid`와 heap 확인 후 에스컬레이션.
- TBD: 스트리밍 물리 복제(WAL shipping) replica 환경에서 standby의 데몬 warmup 동작
  (artifact 동기화 타이밍, `recovery_target_timeline`, `hot_standby` 조합)은
  미검증 — 실제 replica 구성에서 검증 후 이 playbook에 추가 필요.

관련: `gcs-snapshot-ops.md`, `gpu-vm-lifecycle.md`(SA/IP), `daemon-restart-recovery.md`.  
설계 근거: ADR-013(object storage), ADR-024(sharded snapshot/warmup).
