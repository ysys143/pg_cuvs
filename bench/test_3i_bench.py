#!/usr/bin/env python3
"""
test_3i_bench.py — Phase 3I benchmark:
  CAGRA build + pg_cuvs_import_hnsw  vs  pgvector native HNSW build.

비동기 실행 패턴 (SSH 연결 불필요):
  # VM에서 nohup으로 실행:
  nohup python3 bench/test_3i_bench.py --n 1000000 --dim 384 \
      > /tmp/bench_3i_1m.log 2>&1 &
  echo $! > /tmp/bench_3i_1m.pid

  # 진행 폴링 (별도 SSH):
  tail -20 /tmp/bench_3i_1m.log

  # 완료 확인:
  grep 'DONE\|ERROR' /tmp/bench_3i_1m.log | tail -1

측정 항목:
  cagra_build_s, hnsw_import_s, total_gpu_s, hnsw_native_s, speedup,
  p50_us, p95_us, recall_at_k
"""

import argparse
import csv
import os
import random
import sys
import time
from pathlib import Path

import psycopg2

try:
    from tqdm import tqdm
    HAVE_TQDM = True
except ImportError:
    HAVE_TQDM = False

CSV_PATH = Path(__file__).parent / "results" / "hnsw_import_bench.csv"
CSV_FIELDS = [
    "N", "dim", "k",
    "cagra_build_s", "hnsw_import_s", "total_gpu_s",
    "hnsw_native_s", "speedup",
    "p50_us", "p95_us", "recall_at_k",
    # GPU resource config (mirrors work_mem / max_parallel_workers for CPU)
    "max_vram_mb", "shard_count", "cuvs_k", "parallel_fanout",
    "notes",
]


def log(step, total, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}][{step}/{total}] {msg}", flush=True)


def elapsed(fn):
    t0 = time.perf_counter()
    fn()
    return time.perf_counter() - t0


def connect(db):
    dsn = os.environ.get("PGDSN", f"dbname={db}")
    return psycopg2.connect(dsn)


def setup_table(cur, n, dim, idx_dir):
    cur.execute("DROP TABLE IF EXISTS bench3i CASCADE")
    cur.execute(f"CREATE TABLE bench3i (id bigint, embedding vector({dim}))")
    cur.connection.commit()

    batch_size = 2000
    n_batches = (n + batch_size - 1) // batch_size
    rng = random.Random(42)
    nc = max(8, n // 5000)
    centers = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(nc)]

    batches = range(n_batches)
    if HAVE_TQDM:
        batches = tqdm(batches, desc="  inserting", unit="batch",
                       file=sys.stdout, dynamic_ncols=True)

    rows_done = 0
    for bi in batches:
        start_id = bi * batch_size
        end_id = min(start_id + batch_size, n)
        data = []
        for i in range(start_id, end_id):
            c = centers[i % nc]
            vec = "[" + ",".join(f"{v:.4f}" for v in
                                 [c[d] + rng.gauss(0, 0.05) for d in range(dim)]) + "]"
            data.append((i + 1, vec))
        cur.executemany("INSERT INTO bench3i VALUES (%s, %s::vector)", data)
        rows_done += len(data)
        if not HAVE_TQDM and bi % 100 == 0:
            print(f"  {rows_done:,}/{n:,} rows ({100*rows_done/n:.0f}%)", flush=True)

    cur.connection.commit()
    cur.execute(f"SET cuvs.index_dir = '{idx_dir}'")


def build_cagra(cur):
    t = elapsed(lambda: cur.execute(
        "CREATE INDEX bench3i_cagra ON bench3i USING cagra (embedding vector_l2_ops)"))
    cur.connection.commit()
    return t


def import_hnsw(cur):
    cur.execute("CREATE INDEX bench3i_hnsw ON bench3i USING hnsw (embedding vector_l2_ops)")
    cur.connection.commit()
    t = elapsed(lambda: cur.execute(
        "SELECT pg_cuvs_import_hnsw('bench3i_cagra'::regclass, 'bench3i_hnsw'::regclass)"))
    cur.connection.commit()
    return t


def build_hnsw_native(cur):
    cur.execute("DROP INDEX IF EXISTS bench3i_hnsw")
    cur.connection.commit()
    t = elapsed(lambda: cur.execute(
        "CREATE INDEX bench3i_hnsw ON bench3i USING hnsw (embedding vector_l2_ops)"))
    cur.connection.commit()
    return t


def measure_latency(cur, n_queries, k, dim):
    cur.execute("SET enable_cuvs=off; SET enable_seqscan=off")
    cur.connection.commit()
    latencies = []
    rng = random.Random(123)
    queries = range(n_queries)
    if HAVE_TQDM:
        queries = tqdm(queries, desc="  latency", unit="q", file=sys.stdout,
                       dynamic_ncols=True)
    for _ in queries:
        vec = "[" + ",".join(f"{rng.gauss(0,1):.4f}" for _ in range(dim)) + "]"
        t0 = time.perf_counter()
        cur.execute("SELECT id FROM bench3i ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        cur.fetchall()
        latencies.append((time.perf_counter() - t0) * 1e6)
    cur.execute("RESET enable_seqscan; SET enable_cuvs=on")
    cur.connection.commit()
    latencies.sort()
    return latencies[len(latencies) // 2], latencies[int(len(latencies) * 0.95)]


def measure_recall(cur, n_queries, k, dim):
    rng = random.Random(456)
    hits = 0
    queries = range(n_queries)
    if HAVE_TQDM:
        queries = tqdm(queries, desc="  recall", unit="q", file=sys.stdout,
                       dynamic_ncols=True)
    for _ in queries:
        vec = "[" + ",".join(f"{rng.gauss(0,1):.4f}" for _ in range(dim)) + "]"
        cur.execute("SET enable_cuvs=off; SET enable_seqscan=on")
        cur.execute("SELECT id FROM bench3i ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        bf = {r[0] for r in cur.fetchall()}
        cur.execute("SET enable_seqscan=off")
        cur.execute("SELECT id FROM bench3i ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        hnsw = {r[0] for r in cur.fetchall()}
        hits += len(bf & hnsw)
    cur.execute("SET enable_cuvs=on; RESET enable_seqscan")
    cur.connection.commit()
    return hits / (n_queries * k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100_000)
    ap.add_argument("--dim", type=int, default=4)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--n-queries", type=int, default=100)
    ap.add_argument("--n-recall", type=int, default=50)
    ap.add_argument("--idx-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--db", default="contrib_regression")
    # GPU resource parameters (analogous to work_mem/max_parallel_workers for CPU)
    ap.add_argument("--max-vram-mb", type=int, default=0,
                    help="VRAM limit per GPU in MB (0=daemon default, typically 40000)")
    ap.add_argument("--shard-count", type=int, default=0,
                    help="cuvs.shard_count (0=auto, 1=single GPU, >=2 multi-GPU)")
    ap.add_argument("--cuvs-k", type=int, default=100,
                    help="cuvs.k — GPU top-k candidate list size (analogous to ef_search)")
    ap.add_argument("--parallel-fanout", type=int, default=1,
                    help="cuvs.parallel_fanout (1=concurrent shard dispatch, 0=sequential)")
    args = ap.parse_args()

    TOTAL = 6
    print(f"\n=== Phase 3I benchmark: N={args.n:,}, dim={args.dim}, k={args.k} ===",
          flush=True)
    print(f"GPU config: max_vram_mb={args.max_vram_mb or 'daemon-default'} "
          f"shard_count={args.shard_count or 'auto'} "
          f"cuvs_k={args.cuvs_k} parallel_fanout={args.parallel_fanout}", flush=True)

    conn = connect(args.db)
    conn.autocommit = False
    cur = conn.cursor()
    cur.execute("CREATE EXTENSION IF NOT EXISTS vector")
    cur.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    cur.execute("SET client_min_messages = warning")
    # Apply GPU resource GUCs (mirrors SET work_mem / max_parallel_workers for CPU)
    cur.execute(f"SET cuvs.shard_count = {args.shard_count}")
    cur.execute(f"SET cuvs.k = {args.cuvs_k}")
    cur.execute(f"SET cuvs.parallel_fanout = {bool(args.parallel_fanout)}")
    conn.commit()

    log(1, TOTAL, f"Setup table N={args.n:,}")
    setup_table(cur, args.n, args.dim, args.idx_dir)
    print(f"  → inserted {args.n:,} rows", flush=True)

    log(2, TOTAL, "CAGRA build")
    cagra_s = build_cagra(cur)
    print(f"  → {cagra_s:.1f}s", flush=True)

    log(3, TOTAL, "HNSW import")
    import_s = import_hnsw(cur)
    total_gpu = cagra_s + import_s
    print(f"  → import {import_s:.1f}s  total_gpu {total_gpu:.1f}s", flush=True)

    log(4, TOTAL, "pgvector native HNSW (baseline)")
    native_s = build_hnsw_native(cur)
    speedup = native_s / total_gpu if total_gpu > 0 else float("nan")
    print(f"  → {native_s:.1f}s  speedup={speedup:.1f}x", flush=True)

    log(5, TOTAL, f"Query latency ({args.n_queries} queries)")
    p50, p95 = measure_latency(cur, args.n_queries, args.k, args.dim)
    print(f"  → p50={p50:.0f}us  p95={p95:.0f}us", flush=True)

    log(6, TOTAL, f"Recall ({args.n_recall} queries)")
    recall = measure_recall(cur, args.n_recall, args.k, args.dim)
    print(f"  → recall@{args.k}={recall:.4f}", flush=True)

    cur.execute("DROP TABLE bench3i CASCADE")
    conn.commit()
    cur.close()
    conn.close()

    CSV_PATH.parent.mkdir(exist_ok=True)
    write_header = not CSV_PATH.exists()
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if write_header:
            w.writeheader()
        w.writerow({
            "N": args.n, "dim": args.dim, "k": args.k,
            "cagra_build_s": round(cagra_s, 2),
            "hnsw_import_s": round(import_s, 2),
            "total_gpu_s": round(total_gpu, 2),
            "hnsw_native_s": round(native_s, 2),
            "speedup": round(speedup, 2),
            "p50_us": round(p50, 0),
            "p95_us": round(p95, 0),
            "recall_at_k": round(recall, 4),
            "max_vram_mb": args.max_vram_mb or "daemon-default",
            "shard_count": args.shard_count or "auto",
            "cuvs_k": args.cuvs_k,
            "parallel_fanout": args.parallel_fanout,
            "notes": "",
        })

    print(f"\n=== DONE ===", flush=True)
    print(f"  N={args.n:,}  GPU={total_gpu:.1f}s  native={native_s:.1f}s  "
          f"speedup={speedup:.1f}x  p50={p50:.0f}us  recall={recall:.4f}",
          flush=True)
    print(f"  → {CSV_PATH}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\nERROR: {e}", flush=True)
        raise
