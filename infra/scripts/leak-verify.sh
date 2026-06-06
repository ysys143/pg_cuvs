#!/bin/bash
# ADR-048 leak/orphan verification harness.
#
# Invariant after any death + settle: /dev/shm has no pg_cuvs_bld_* orphan AND
# /proc/meminfo Shmem returns to ~baseline (catches memfd anon + named shm).
# Probes the death-mode matrix (memfd + shm tiers), an accumulation soak, and a
# negative-control mutation that proves the detector itself fires.
#
# Run on the GPU VM (declared-dim table `bench1m` must exist). Needs sudo.
set -u
PSQL="sudo -u postgres psql -d postgres -tA"
PASS=0; FAIL=0
shmem(){ awk '/^Shmem:/{print $2}' /proc/meminfo; }
orphans(){ ls /dev/shm 2>/dev/null | grep -c pg_cuvs_bld; }
ok(){ PASS=$((PASS+1)); echo "  [PASS] $1"; }
bad(){ FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

pg_healthy(){ for i in $(seq 1 60); do $PSQL -c "SELECT 1" >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
settle_shmem(){ local p=-1 c; for i in $(seq 1 90); do c=$(shmem); [ "$c" = "$p" ] && break; p=$c; sleep 1; done; }

# start a build, wait until corpus is mapping (Shmem risen), echo backend pid
start_build_until_mapped(){
  local base=$1
  $PSQL -c "DROP INDEX IF EXISTS bench1m_cagra;" >/dev/null 2>&1
  ( $PSQL -c "CREATE INDEX bench1m_cagra ON bench1m USING cagra (embedding vector_l2_ops);" >/tmp/lv_ci.txt 2>&1 ) &
  local bpid="" i cur
  for i in $(seq 1 300); do
    bpid=$($PSQL -c "SELECT pid FROM pg_stat_activity WHERE query LIKE 'CREATE INDEX bench1m_cagra%' AND pid<>pg_backend_pid() LIMIT 1;" 2>/dev/null | tr -d ' ')
    cur=$(shmem)
    [ -n "$bpid" ] && [ $((cur-base)) -gt 800000 ] && { echo "$bpid"; return 0; }
    sleep 0.1
  done
  echo "$bpid"
}

# one matrix cell: $1=label, $2=kill-method(shell snippet using $BPID), $3=expect_orphan(0/reaped)
cell(){
  local label="$1" killcmd="$2" expect="$3"
  local base bpid d
  base=$(shmem)
  bpid=$(start_build_until_mapped "$base")
  [ -z "$bpid" ] && { bad "$label: backend not found"; return; }
  eval "$killcmd"
  pg_healthy || { bad "$label: PG did not recover"; return; }
  wait 2>/dev/null
  settle_shmem
  d=$(( $(shmem) - base ))
  if [ "$expect" = "reaped" ]; then
    # shm tier crash: orphan expected, then _PG_init sweep (restart) reaps it
    if [ "$(orphans)" -ge 1 ]; then ok "$label: orphan left as expected ($(orphans))"; else echo "  [note] $label: no orphan (cleanup ran on this path)"; fi
    sudo systemctl restart postgresql@16-main >/dev/null 2>&1; pg_healthy
    [ "$(orphans)" = "0" ] && ok "$label: reaped after _PG_init sweep" || bad "$label: orphan survived sweep ($(orphans))"
  else
    [ "$(orphans)" = "0" ] && ok "$label: 0 orphan" || bad "$label: $(orphans) orphan(s)!"
    [ "$d" -lt 250000 ] && ok "$label: Shmem returned (delta ${d}kB)" || bad "$label: Shmem leak ${d}kB"
  fi
}

echo "=== Tier: MEMFD (default) ==="
cell "memfd/cancel"    'sudo -u postgres psql -d postgres -tAc "SELECT pg_cancel_backend($bpid)" >/dev/null'    fresh
cell "memfd/terminate" 'sudo -u postgres psql -d postgres -tAc "SELECT pg_terminate_backend($bpid)" >/dev/null' fresh
cell "memfd/SIGKILL"   'sudo kill -9 $bpid'    fresh
cell "memfd/SIGSEGV"   'sudo kill -SEGV $bpid' fresh
cell "memfd/daemon-kill" 'sudo kill -9 $(pgrep -f pg_cuvs_server | head -1); sleep 1; sudo systemctl restart pg-cuvs-server' fresh

echo "=== Soak: memfd x cancel x 80 (accumulation slope) ==="
S0=$(shmem); O0=$(orphans)
for n in $(seq 1 80); do
  base=$(shmem); bpid=$(start_build_until_mapped "$base")
  [ -n "$bpid" ] && sudo -u postgres psql -d postgres -tAc "SELECT pg_cancel_backend($bpid)" >/dev/null 2>&1
  wait 2>/dev/null
done
settle_shmem; SN=$(shmem); ON=$(orphans)
echo "  soak: Shmem ${S0}->${SN}kB (delta $((SN-S0))kB over 80)  orphans ${O0}->${ON}"
[ "$ON" = "0" ] && [ $((SN-S0)) -lt 300000 ] && ok "soak: no accumulation" || bad "soak: leak trend (orphans=$ON dShmem=$((SN-S0))kB)"

echo "=== Negative control (mutation): detector must FIRE on a real orphan ==="
sudo -u postgres sh -c 'umask 0; : > /dev/shm/pg_cuvs_bld_777777_0'
sudo touch -d "1 hour ago" /dev/shm/pg_cuvs_bld_777777_0
if [ "$(orphans)" -ge 1 ]; then ok "mutation: detector sees the injected orphan ($(orphans))"; else bad "mutation: detector blind to injected orphan"; fi
# and the reaper must clean it (sweep)
sudo systemctl restart postgresql@16-main >/dev/null 2>&1; pg_healthy
[ "$(orphans)" = "0" ] && ok "mutation: sweep removed injected orphan" || { bad "mutation: injected orphan survived"; sudo rm -f /dev/shm/pg_cuvs_bld_777777_0; }

echo ""
echo "=== RESULT: $PASS passed, $FAIL failed ==="
