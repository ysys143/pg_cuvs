#!/bin/bash
# run_cohere.sh — Cohere Wikipedia 1024d real embedding benchmark
#
# VM에서 실행. nohup 비동기 패턴 권장:
#
#   nohup bash bench/run_cohere.sh --n 1000000 > /tmp/cohere_bench.log 2>&1 &
#   tail -f /tmp/cohere_bench.log
#   grep -E 'STEP|DONE|FAIL|result' /tmp/cohere_bench.log
#
# 실행 순서:
#   Step 0: Cohere 데이터 다운로드 (skip if already present)
#   Step 1: Ground truth 빌드
#   Step 2: pg_cuvs CAGRA search
#   Step 3: pgvector HNSW sweep
#   Step 4: pgvector IVFFlat sweep
#   Step 5: pg_cuvs_import_hnsw (3I SQL path: CAGRA build + HNSW import + CPU search)
#   Step 6: cagra-hnsw (cuVS lib path: reference for Step 5 comparison)
#   Step 7: pgvectorscale StreamingDiskANN (optional — skip if not installed)
#   Step 8: VectorChord vchordrq (optional — skip if not installed)
#   Step 9: Export JSONL → CSV summary
#
# 결과: bench/results/cohere_N{N}.jsonl  (raw, all sweeps)
#       bench/results/cohere_N{N}_summary.csv  (best-recall-per-system)
#
# 데이터: Cohere/wikipedia-2023-11-embed-multilingual-v3 (EN subset, 1024d)
#   corpus: L2-normalized → L2 거리 == cosine 랭킹
# ─────────────────────────────────────────────────────────────────────────

set -eo pipefail

# ── 기본값 ────────────────────────────────────────────────────────────────
N=1000000
DIM=1024
K=10,100
DATA_DIR="$HOME/anbench/data"
OUT_DIR="$(pwd)/bench/results"
INDEX_DIR=/tmp/cuvs_indexes
CONDA_ENV=cuvs_dev
SKIP_DOWNLOAD=0
SKIP_GT=0
GPU=0   # CUDA_VISIBLE_DEVICES for daemon restart

usage() {
    echo "Usage: $0 [--n N] [--gpu ID] [--data-dir DIR] [--skip-download] [--skip-gt]"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --n)             N="$2"; shift 2;;
        --gpu)           GPU="$2"; shift 2;;
        --data-dir)      DATA_DIR="$2"; shift 2;;
        --skip-download) SKIP_DOWNLOAD=1; shift;;
        --skip-gt)       SKIP_GT=1; shift;;
        -h|--help)       usage;;
        *) echo "Unknown arg: $1"; usage;;
    esac
done

CORPUS="$DATA_DIR/corpus.fbin"
QUERIES="$DATA_DIR/queries_10k.fbin"
GT="$DATA_DIR/gt_${N}.npy"
RUN="$OUT_DIR/cohere_N${N}.jsonl"
mkdir -p "$OUT_DIR" "$INDEX_DIR"

ts()   { date '+%H:%M:%S'; }
log()  { echo "[$(ts)] $*"; }
step() { echo; echo "══════════════════════════════════════════════"; echo "[$(ts)] $*"; echo "══════════════════════════════════════════════"; }
act()  { set +u; source ~/miniforge3/bin/activate "$1"; set -u 2>/dev/null || true; }

# ─────────────────────────────────────────────────────────────────────────
act "$CONDA_ENV"

# ── Step 0: 데이터 다운로드 ───────────────────────────────────────────────
step "STEP 0: Cohere Wikipedia 1024d 다운로드"
if [[ "$SKIP_DOWNLOAD" -eq 1 ]]; then
    log "  skip (--skip-download)"
else
    TOTAL=$(( N + 10000 ))
    log "  corpus=${N}, queries=10000 (total=$TOTAL rows)"
    python infra/anbench/fetch_dataset.py \
        --out-dir "$DATA_DIR" \
        --n-corpus "$N" \
        --n-queries 10000
fi
log "  corpus: $CORPUS"
log "  queries: $QUERIES"

# ── Step 1: Ground truth ──────────────────────────────────────────────────
step "STEP 1: Ground truth (brute-force k=100)"
if [[ "$SKIP_GT" -eq 1 ]] && [[ -f "$GT" ]]; then
    log "  skip (already exists)"
else
    # GT build needs cupy (GPU brute-force) — use cuvs_py env
    act cuvs_py
    python infra/anbench/build_gt.py \
        --corpus "$CORPUS" --queries "$QUERIES" \
        --n "$N" --k 100 --out "$GT"
    act "$CONDA_ENV"
fi
log "  GT: $GT"

# ── daemon 준비 ───────────────────────────────────────────────────────────
restart_daemon() {
    log "  Restarting pg-cuvs-server (CUDA_VISIBLE_DEVICES=$GPU)..."
    sudo systemctl set-environment CUDA_VISIBLE_DEVICES="$GPU"
    sudo systemctl restart pg-cuvs-server
    for i in $(seq 1 20); do
        [ -S /tmp/.s.pg_cuvs ] && { log "  daemon ready (${i}s)"; return 0; }
        sleep 1
    done
    log "  ERROR: daemon did not start"
    return 1
}

stop_daemon() {
    sudo systemctl stop pg-cuvs-server 2>/dev/null || true
    sudo systemctl unset-environment CUDA_VISIBLE_DEVICES 2>/dev/null || true
    sleep 2
}

# ── Step 2: pg_cuvs CAGRA search ─────────────────────────────────────────
step "STEP 2: pg_cuvs CAGRA GPU search"
restart_daemon
python infra/anbench/run_pg.py \
    --corpus "$CORPUS" --queries "$QUERIES" --gt "$GT" \
    --n "$N" --system pg_cuvs --out "$RUN" --ks "$K" \
    --dataset "cohere-wiki-en-1024" \
    --index-dir "$INDEX_DIR" || log "  [WARN] pg_cuvs step failed"

# ── Step 3: pgvector HNSW sweep ──────────────────────────────────────────
step "STEP 3: pgvector HNSW (ef_search sweep)"
stop_daemon
python infra/anbench/run_pg.py \
    --corpus "$CORPUS" --queries "$QUERIES" --gt "$GT" \
    --n "$N" --system hnsw --out "$RUN" --ks "$K" \
    --dataset "cohere-wiki-en-1024" || log "  [WARN] hnsw step failed"

# ── Step 4: pgvector IVFFlat sweep ───────────────────────────────────────
step "STEP 4: pgvector IVFFlat (probes sweep)"
python infra/anbench/run_pg.py \
    --corpus "$CORPUS" --queries "$QUERIES" --gt "$GT" \
    --n "$N" --system ivfflat --out "$RUN" --ks "$K" \
    --dataset "cohere-wiki-en-1024" || log "  [WARN] ivfflat step failed"

# ── Step 5: pg_cuvs_import_hnsw (3I SQL path) ────────────────────────────
step "STEP 5: pg_cuvs_import_hnsw SQL path (CAGRA build → pgvector HNSW)"
restart_daemon
python infra/anbench/run_pg_3i.py \
    --corpus "$CORPUS" --queries "$QUERIES" --gt "$GT" \
    --n "$N" --out "$RUN" --ks "$K" \
    --dataset "cohere-wiki-en-1024" \
    --index-dir "$INDEX_DIR" || log "  [WARN] 3I import step failed"
stop_daemon

# ── Step 6: cagra-hnsw lib path (comparison baseline for Step 5) ─────────
step "STEP 6: cagra-hnsw (cuVS lib direct — build-time comparison)"
python infra/anbench/run_cagra_hnsw.py \
    --corpus "$CORPUS" --queries "$QUERIES" --gt "$GT" \
    --n "$N" --out "$RUN" --ks "$K" \
    --dataset "cohere-wiki-en-1024" || log "  [WARN] cagra-hnsw step failed"

# ── Step 7: pgvectorscale StreamingDiskANN (optional) ────────────────────
step "STEP 7: pgvectorscale StreamingDiskANN (optional)"
if psql -d postgres -c "SELECT * FROM pg_available_extensions WHERE name='vectorscale'" \
        -tA 2>/dev/null | grep -q vectorscale; then
    psql -d postgres -v N="$N" -v OUT="$RUN" <<'SQL' || log "  [WARN] pgvectorscale failed"
-- pgvectorscale StreamingDiskANN
SET maintenance_work_mem = '16GB';
DROP INDEX IF EXISTS t_diskann;
\timing on
CREATE INDEX t_diskann ON t USING diskann (embedding vector_l2_ops);
\timing off
-- Note: recall measurement requires Python runner; this just verifies build.
SQL
    log "  pgvectorscale index built (recall sweep via separate run_pgvectorscale.sh)"
else
    log "  skip (pgvectorscale not installed)"
fi

# ── Step 8: VectorChord vchordrq (optional) ───────────────────────────────
step "STEP 8: VectorChord vchordrq (optional)"
if psql -d postgres -c "SELECT * FROM pg_available_extensions WHERE name='vectors'" \
        -tA 2>/dev/null | grep -q vectors; then
    psql -d postgres <<'SQL' || log "  [WARN] VectorChord failed"
SET vchordrq.probes = 5;
DROP INDEX IF EXISTS t_vchordrq;
CREATE INDEX t_vchordrq ON t USING vchordrq (embedding vector_l2_ops);
SQL
    log "  VectorChord index built (recall sweep via separate run_vectorchord.sh)"
else
    log "  skip (VectorChord not installed)"
fi

# ── Step 9: CSV summary ────────────────────────────────────────────────────
step "STEP 9: Export best-recall summary CSV"
python3 - "$RUN" "$OUT_DIR/cohere_N${N}_summary.csv" <<'PYEOF'
import json, sys, csv
from collections import defaultdict

jsonl, out = sys.argv[1], sys.argv[2]
rows = [json.loads(l) for l in open(jsonl) if l.strip()]

# Best recall per (system, dataset, N, k)
best = {}
for r in rows:
    key = (r['system'], r['dataset'], r['N'], r['k'])
    if key not in best or r['recall'] > best[key]['recall']:
        best[key] = r

fields = ['system','dataset','N','dim','metric','k','param_set',
          'build_time_s','index_bytes','recall','qps','p50_ms','p95_ms','p99_ms','notes']
with open(out, 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
    w.writeheader()
    for r in sorted(best.values(), key=lambda x: (x['k'], -x['recall'])):
        w.writerow(r)
print(f"[summary] {len(best)} best-recall rows -> {out}")
PYEOF

step "DONE"
log "Raw results:     $RUN"
log "Summary CSV:     $OUT_DIR/cohere_N${N}_summary.csv"
log ""
log "다음 단계:"
log "  1. bench/results/cohere_N${N}_summary.csv 확인"
log "  2. design/BENCHMARK_CROSSOVER.md §15에 결과 추가"
log "  3. README 벤치 표 synthetic -> real 데이터로 교체 (또는 병기)"
