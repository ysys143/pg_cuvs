#!/bin/bash
# test_mig.sh — pg_cuvs MIG(Multi-Instance GPU) 기능 검증
#
# GCP A100 VM에서 MIG 활성화는 reboot이 필요합니다.
#
# 실행 순서 (2단계):
#
#   [Step 1] MIG 활성화 + 재시작 (1회):
#     sudo bash bench/test_mig.sh --setup
#
#   [Step 2] 재부팅 후 테스트 (nohup 비동기):
#     nohup sudo bash bench/test_mig.sh --test > /tmp/test_mig.log 2>&1 &
#     sudo tail -f /tmp/test_mig.log            # 진행 폴링
#     sudo grep 'PASS\|FAIL\|ERROR' /tmp/test_mig.log  # 결과 확인
#
#   [Teardown] 원복 + 재부팅:
#     sudo bash bench/test_mig.sh --teardown
#
# 검증 시나리오:
#   Test 1: GPU 0 단일 MIG 인스턴스(3g.20gb=20GB)로 정상 빌드/서치
#   Test 2: GPU 0 MIG 3개(1g.5gb×3=5GB each) → shard_count=3 멀티 동작

set -euo pipefail

DB=contrib_regression
IDX_DIR=/tmp/cuvs_indexes
DAEMON_SERVICE=pg-cuvs-server
PG_SERVICE=postgresql
MODE=${1:---test}

ts()  { date '+%H:%M:%S'; }
log() { echo "[$(ts)] $*"; }
sep() { echo "════════════════════════════════════"; }

run_sql() { sudo -u postgres psql "$DB" -tAc "$1" 2>/dev/null; }

start_daemon() {
    local cuda="${1:-}"
    if [ -n "$cuda" ]; then
        sudo systemctl set-environment CUDA_VISIBLE_DEVICES="$cuda"
    else
        sudo systemctl unset-environment CUDA_VISIBLE_DEVICES 2>/dev/null || true
    fi
    sudo systemctl start "$DAEMON_SERVICE"
    for i in $(seq 1 20); do
        [ -S /tmp/.s.pg_cuvs ] && { log "  daemon ready (${i}s)"; break; }
        sleep 1
    done
}

stop_daemon() {
    sudo systemctl stop "$DAEMON_SERVICE" 2>/dev/null || true
    sudo systemctl unset-environment CUDA_VISIBLE_DEVICES 2>/dev/null || true
    sleep 2
}

get_mig_uuids() {
    nvidia-smi -L 2>/dev/null | grep 'MIG.*Device' | \
        sed 's/.*UUID: \(MIG-[^)]*\)).*/\1/' | tr '\n' ',' | sed 's/,$//'
}

verify_build_search() {
    local label="$1" n="$2" shard_count="$3"
    log "  verify: N=$n shard_count=$shard_count"
    run_sql "CREATE EXTENSION IF NOT EXISTS vector;" >/dev/null
    run_sql "CREATE EXTENSION IF NOT EXISTS pg_cuvs;" >/dev/null
    run_sql "SET cuvs.index_dir='$IDX_DIR'; SET cuvs.shard_count=$shard_count;
             DROP TABLE IF EXISTS mig_test CASCADE;
             CREATE TABLE mig_test (id bigint, embedding vector(4));
             INSERT INTO mig_test SELECT i,
               ('[' || (i*0.05) || ',0,0,0]')::vector
             FROM generate_series(1,$n) i;" >/dev/null

    if run_sql "CREATE INDEX mig_cagra ON mig_test USING cagra (embedding vector_l2_ops);" >/dev/null 2>&1; then
        local r
        r=$(run_sql "SET enable_seqscan=off;
                     SELECT id FROM mig_test
                     ORDER BY embedding <-> '[0.5,0,0,0]' LIMIT 1;" 2>/dev/null)
        if [ -n "$r" ]; then
            log "  [PASS] $label: top-1=$r"
        else
            log "  [FAIL] $label: empty search result"
        fi
    else
        log "  [FAIL] $label: CAGRA build failed"
    fi
    run_sql "DROP TABLE IF EXISTS mig_test CASCADE;" >/dev/null 2>/dev/null || true
    run_sql "DROP EXTENSION IF EXISTS pg_cuvs;" >/dev/null 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════════════════════════

case "$MODE" in

--setup)
    sep; log "=== MIG Setup: Enable pending + reboot ==="; sep
    log "Current state:"
    nvidia-smi --query-gpu=mig.mode.current,mig.mode.pending --format=csv,noheader
    log "Enabling MIG on GPU 0 (pending — reboot needed)..."
    sudo nvidia-smi -i 0 -mig 1
    log "After reboot, run:"
    log "  nohup sudo bash bench/test_mig.sh --test > /tmp/test_mig.log 2>&1 &"
    sleep 3
    sudo reboot
    ;;

--teardown)
    sep; log "=== MIG Teardown: Disable + reboot ==="; sep
    stop_daemon
    sudo nvidia-smi mig -i 0 -dci 2>/dev/null || true
    sudo nvidia-smi mig -i 0 -dgi 2>/dev/null || true
    sudo nvidia-smi -i 0 -mig 0
    log "MIG pending disable set. Rebooting..."
    sleep 3
    sudo reboot
    ;;

--test)
    sep; log "=== MIG Test ==="; sep

    # 자원 경합 체크
    if pgrep -f 'test_3i_bench|bench_50m' >/dev/null 2>&1; then
        log "ERROR: competing benchmark process running. Stop it first."
        exit 1
    fi

    log "MIG state:"
    nvidia-smi --query-gpu=mig.mode.current,mig.mode.pending --format=csv,noheader
    nvidia-smi -L

    if ! nvidia-smi --query-gpu=mig.mode.current --format=csv,noheader | grep -q "Enabled"; then
        log "ERROR: MIG not active on any GPU. Run --setup and reboot first."
        exit 1
    fi

    stop_daemon
    sudo systemctl start "$PG_SERVICE" 2>/dev/null || true
    sleep 2

    # ── Test 1: 단일 MIG 인스턴스 (3g.20gb = 20GB) ────────────────────────
    sep; log "TEST 1: Single MIG instance 3g.20gb (20GB, Profile 9)"; sep
    log "Creating 1x 3g.20gb on GPU 0..."
    sudo nvidia-smi mig -i 0 -cgi 9 -C
    sleep 2
    nvidia-smi -L

    MIG1=$(get_mig_uuids | cut -d',' -f1)
    log "MIG UUID: $MIG1"
    start_daemon "$MIG1"
    verify_build_search "test1-single-3g20gb" 50000 1

    stop_daemon
    sudo nvidia-smi mig -i 0 -dci 2>/dev/null || true
    sudo nvidia-smi mig -i 0 -dgi 2>/dev/null || true
    log "TEST 1 complete."

    # ── Test 2: 멀티 MIG 인스턴스 (1g.5gb × 3 = 5GB each) ───────────────
    sep; log "TEST 2: Multi MIG 1g.5gb × 3 → shard_count=3 (Profile 19)"; sep
    log "Creating 3x 1g.5gb on GPU 0..."
    sudo nvidia-smi mig -i 0 -cgi 19 -C
    sudo nvidia-smi mig -i 0 -cgi 19 -C
    sudo nvidia-smi mig -i 0 -cgi 19 -C
    sleep 2
    nvidia-smi -L

    MIG_3=$(get_mig_uuids | tr ',' '\n' | head -3 | tr '\n' ',' | sed 's/,$//')
    log "MIG UUIDs: $MIG_3"
    start_daemon "$MIG_3"
    # 5GB 슬라이스 × 3: N=30K (각 10K/shard, ~160MB each)
    verify_build_search "test2-multi-1g5gb-x3" 30000 3

    stop_daemon
    sudo nvidia-smi mig -i 0 -dci 2>/dev/null || true
    sudo nvidia-smi mig -i 0 -dgi 2>/dev/null || true
    log "TEST 2 complete."

    start_daemon ""
    sep; log "=== ALL DONE. Run --teardown to restore normal GPU mode. ==="; sep
    ;;

*)
    echo "Usage: $0 [--setup | --test | --teardown]"
    exit 1
    ;;
esac
