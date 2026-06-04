# pg_cuvs 운영 Playbook 색인

운영 중 마주치는 증상별 진단/복구 절차 모음. 각 playbook은 6개 절 형식
(**1. 증상 · 2. 확인 명령 · 3. 원인 분기 · 4. 복구 절차 · 5. 검증 명령 ·
6. Escalation 기준**)을 따르며, 가능한 한 실제 검증된 시나리오에 근거한다.

## VM / 인프라
- [gpu-vm-build-and-test.md](gpu-vm-build-and-test.md) — GPU VM 빌드/설치/테스트 워크플로.
- [gpu-vm-lifecycle.md](gpu-vm-lifecycle.md) — VM start/stop/reset, **ephemeral IP 변경**,
  **stop/start 후 NVIDIA driver mismatch → reset**, GCS용 **service account 부착**,
  멀티 GPU VM start/stop + 비용. *(Phase 3)*

## 빌드 / DDL
- [create-index-failure-diagnosis.md](create-index-failure-diagnosis.md) — `CREATE INDEX USING cagra` 실패 진단.
- [persistence-corruption-recovery.md](persistence-corruption-recovery.md) — `.cagra`/`.tids` 손상/불완전 쌍 복구.
- [rollback-and-cleanup.md](rollback-and-cleanup.md) — 실패한 빌드/트랜잭션 롤백·정리.

## 런타임 / 검색
- [daemon-restart-recovery.md](daemon-restart-recovery.md) — 데몬 재시작 후 인덱스 reload.
- [vram-oom-fallback.md](vram-oom-fallback.md) — VRAM 부족, eviction, CPU fallback.
- [jit-threshold-sweep.md](jit-threshold-sweep.md) — JIT/cost 임계값 측정.

## 멀티 GPU / 샤딩 *(Phase 3)*
- [multi-gpu-sharding-ops.md](multi-gpu-sharding-ops.md) — sharded build, `pg_stat_gpu_shards`
  배치 확인, `parallel_fanout`(+p50 log2-bucket 주의), `shard_overfetch`, 손상 shard
  fail-closed→REINDEX, whole-unit eviction + reload.

## GCS 스냅샷 *(Phase 3)*
- [gcs-snapshot-ops.md](gcs-snapshot-ops.md) — SA/IAM/bucket 설정, 업로드/다운로드
  round-trip 검증, sharded 복원(`.relfilenode`-driven warmup), heap relfilenode 거부.

## Write-path / DROP *(Phase 3)*
- [drop-and-write-path-diagnosis.md](drop-and-write-path-diagnosis.md) — `DROP INDEX`
  데몬 VRAM/artifact 정리(+재시작 zombie 검증), delta/tombstone/stale, sharded GPU
  delta cache(`delta_search_mode`).

## 용량 계획 / 업그레이드 *(Phase 3H-full)*
- [capacity-planning.md](capacity-planning.md) — VRAM 추정 공식(`estimate_vram_bytes`,
  `cuvs_auto_shard_count`), N×dim별 단일/멀티 GPU 표, `--max-vram-mb` 권장값(물리
  70–80%), artifact 디스크 크기, 50M×384 단일 노드 한계.
- [release-upgrade.md](release-upgrade.md) — 0.1.0 설치/재설치(`make install` +
  `make install-server`), 데몬·PostgreSQL 재시작 순서, artifact magic 호환성,
  향후 `ALTER EXTENSION pg_cuvs UPDATE` 절차(TBD: 첫 릴리스 후 검증).

## 노드 부트스트랩 *(Phase 3H-full)*
- [replica-bootstrap.md](replica-bootstrap.md) — GCS snapshot으로 새 노드 부트스트랩
  (`snapshot_uri`/`cluster_id`/`gcs_key_file`), warmup_state 흐름,
  `.relfilenode` heap 호환 확인, manifest SHA 검증.
  (TBD: 스트리밍 물리 복제 환경 검증 필요.)

## 벤치마크
- [large-dataset-benchmark.md](large-dataset-benchmark.md) — 대규모 데이터셋 벤치 절차.
- [benchmark-runbook.md](benchmark-runbook.md) — *(Phase 3H-full)* 벤치 하네스 3종
  (`make gpu-bench` / `make gpu-cohere` / `make gpu-anbench`) 사용법, 결과 위치
  (`bench/results/`, `design/anbench/`), GT 버그 이력, capacity-planning.md 연계.

---

> Phase 3H-full 기준(이 색인): 위 16개 중 8개(*Phase 3* 및 *Phase 3H-full* 표시)가
> 멀티 GPU/GCS/샤딩/DROP/용량계획/업그레이드/bootstrap/벤치마크 운영 표면을 커버한다.
> 설계 근거: `design/DECISIONS.md` ADR-013, ADR-021..024.
