#!/usr/bin/env python3
"""
ef_recall_sweep.py — ef-recall pareto + build-time breakdown for all 4 modes.

Dataset : ~/anbench/data/corpus.fbin   (1M × 1024, Cohere)
Queries : ~/anbench/data/queries_10k.fbin (first N_QUERIES used)
GT      : ~/anbench/data/gt_1000000.npy  (shape [10000, 100], 0-indexed)

Usage (async):
  nohup python3 bench/ef_recall_sweep.py > /tmp/ef_recall.log 2>&1 &
  tail -f /tmp/ef_recall.log

Results → bench/results/ef_recall_sweep.csv
"""

import csv
import io
import os
import struct
import sys
import time
from pathlib import Path

import numpy as np
import psycopg2

# ── config ────────────────────────────────────────────────────────────────────
CORPUS_FBIN  = Path("/home/ubuntu/anbench/data/corpus.fbin")
QUERIES_FBIN = Path("/home/ubuntu/anbench/data/queries_10k.fbin")
GT_NPY       = Path("/home/ubuntu/anbench/data/gt_1000000.npy")
IDX_DIR      = "/tmp/cuvs_indexes"
TABLE        = "ef_bench"
CAGRA_IDX    = "ef_bench_cagra"
N_QUERIES    = 100
K            = 10
EF_VALUES    = [10, 20, 40, 80, 160, 320]
MODES        = ["nsw", "hnsw", "hnswlib", "hnswlib_file"]
RESULTS_CSV  = Path(__file__).parent / "results" / "ef_recall_sweep.csv"
BATCH        = 50_000

PG_COPY_HEADER  = b'PGCOPY\n\xff\r\n\0' + struct.pack('>II', 0, 0)
PG_COPY_TRAILER = struct.pack('>h', -1)

# ── helpers ───────────────────────────────────────────────────────────────────
def ts():
    return time.strftime("%H:%M:%S")

def log(msg):
    print(f"[{ts()}] {msg}", flush=True)

def read_fbin(path, max_vecs=None):
    with open(path, "rb") as f:
        n_vecs, dim = struct.unpack("<II", f.read(8))
        if max_vecs:
            n_vecs = min(n_vecs, max_vecs)
        data = np.frombuffer(f.read(n_vecs * dim * 4), dtype=np.float32)
    return data.reshape(n_vecs, dim)

def pg_row_bytes(row_id, vec):
    id_b  = struct.pack('>i', row_id)
    vec_b = struct.pack('>hh', len(vec), 0) + vec.astype('>f4').tobytes()
    out   = io.BytesIO()
    out.write(struct.pack('>h', 2))
    out.write(struct.pack('>i', len(id_b)));  out.write(id_b)
    out.write(struct.pack('>i', len(vec_b))); out.write(vec_b)
    return out.getvalue()

def load_vectors(conn, table, vecs):
    n, dim = vecs.shape
    log(f"Checking {table} ...")
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) FROM pg_class WHERE relname = %s", (table,))
        exists = cur.fetchone()[0] > 0
        if exists:
            cur.execute(f"SELECT COUNT(*) FROM {table}")
            cnt = cur.fetchone()[0]
            if cnt == n:
                log(f"  {table} already has {n:,} rows — skipping load.")
                return
            log(f"  {table} has {cnt:,} rows (expected {n:,}) — reloading.")
        cur.execute(f"DROP TABLE IF EXISTS {table} CASCADE")
        cur.execute(f"CREATE TABLE {table} (id int, embedding vector({dim}))")
    conn.commit()

    log(f"Loading {n:,} × {dim}d vectors (binary COPY, batch={BATCH:,}) ...")
    for start in range(0, n, BATCH):
        batch = vecs[start:start + BATCH]
        buf = io.BytesIO()
        buf.write(PG_COPY_HEADER)
        for i, v in enumerate(batch):
            buf.write(pg_row_bytes(start + i + 1, v))
        buf.write(PG_COPY_TRAILER)
        buf.seek(0)
        with conn.cursor() as cur:
            cur.copy_expert(f"COPY {table}(id, embedding) FROM STDIN WITH BINARY", buf)
        conn.commit()
        done = min(start + BATCH, n)
        log(f"  {done:,}/{n:,} ({100*done//n}%)")
    log("Load done.")

def drop_hnsw_indexes(conn):
    with conn.cursor() as cur:
        cur.execute("""
            SELECT indexname FROM pg_indexes
            WHERE tablename = %s AND indexname LIKE 'pg_cuvs_hnsw%%'
        """, (TABLE,))
        for (name,) in cur.fetchall():
            cur.execute(f"DROP INDEX IF EXISTS {name}")
    conn.commit()

def recall_at_k(result_ids_1indexed, gt_0indexed, k):
    res = {r - 1 for r in result_ids_1indexed[:k]}
    gts = set(gt_0indexed[:k].tolist())
    return len(res & gts) / k

# ── main ──────────────────────────────────────────────────────────────────────
def main():
    log("=== ef-recall sweep: pg_cuvs_build_hnsw 4-mode pareto ===")

    log("Reading corpus ...")
    corpus  = read_fbin(CORPUS_FBIN)
    log(f"  corpus  {corpus.shape}")
    queries = read_fbin(QUERIES_FBIN, max_vecs=N_QUERIES)
    log(f"  queries {queries.shape}")
    gt = np.load(GT_NPY)[:N_QUERIES]
    log(f"  gt      {gt.shape}")

    dsn  = os.environ.get("PGDSN", "dbname=contrib_regression")
    conn = psycopg2.connect(dsn)
    with conn.cursor() as cur:
        cur.execute("CREATE EXTENSION IF NOT EXISTS vector")
        cur.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
        cur.execute(f"SET cuvs.index_dir = '{IDX_DIR}'")
        cur.execute("SET client_min_messages = 'warning'")
    conn.commit()

    load_vectors(conn, TABLE, corpus)
    n, dim = corpus.shape

    # ── CAGRA build ───────────────────────────────────────────────────────────
    log("Building CAGRA index ...")
    with conn.cursor() as cur:
        cur.execute(f"DROP INDEX IF EXISTS {CAGRA_IDX}")
    conn.commit()
    t0 = time.perf_counter()
    with conn.cursor() as cur:
        cur.execute(
            f"CREATE INDEX {CAGRA_IDX} ON {TABLE} USING cagra (embedding vector_l2_ops)"
        )
    conn.commit()
    cagra_s = time.perf_counter() - t0
    log(f"CAGRA build: {cagra_s:.1f}s")

    # pgvector native HNSW baseline — measured in Phase 3J (Cohere 1M×1024, A100-40GB)
    # Skipping rebuild to save ~8-10GB disk. Hardcoded from ADR-037.
    native_s = 285.0
    log(f"Native HNSW build: {native_s:.0f}s (Phase 3J measurement, skipping rebuild)")

    # ── per-mode benchmark ────────────────────────────────────────────────────
    all_rows = []
    for mode in MODES:
        log(f"\n── mode={mode} ──────────────────────────────────────────────")
        drop_hnsw_indexes(conn)

        t0 = time.perf_counter()
        with conn.cursor() as cur:
            cur.execute("SET client_min_messages = 'warning'")
            cur.execute(
                "SELECT pg_cuvs_build_hnsw(%s::regclass, %s)",
                (CAGRA_IDX, mode)
            )
        conn.commit()
        build_s = time.perf_counter() - t0
        total_s = cagra_s + build_s
        speedup = native_s / total_s
        log(f"  pg_cuvs_build_hnsw: {build_s:.1f}s  "
            f"total(CAGRA+HNSW): {total_s:.1f}s  "
            f"speedup vs native({native_s:.0f}s): {speedup:.2f}x")

        with conn.cursor() as cur:
            cur.execute("SET enable_cuvs = off")
            cur.execute("SET enable_seqscan = off")
            for ef in EF_VALUES:
                cur.execute(f"SET hnsw.ef_search = {ef}")
                recalls = []
                t_start = time.perf_counter()
                for qi in range(N_QUERIES):
                    vec_str = "[" + ",".join(
                        f"{v:.6f}" for v in queries[qi].tolist()
                    ) + "]"
                    cur.execute(
                        f"SELECT id FROM {TABLE} "
                        f"ORDER BY embedding <-> %s::vector LIMIT %s",
                        (vec_str, K)
                    )
                    result_ids = [r[0] for r in cur.fetchall()]
                    recalls.append(recall_at_k(result_ids, gt[qi], K))
                elapsed = time.perf_counter() - t_start
                qps    = N_QUERIES / elapsed
                recall = float(np.mean(recalls))
                log(f"  ef={ef:4d}: recall@{K}={recall:.4f}  QPS={qps:.1f}")
                all_rows.append({
                    "mode":          mode,
                    "cagra_build_s": round(cagra_s, 1),
                    "hnsw_build_s":  round(build_s, 1),
                    "total_s":       round(total_s, 1),
                    "native_s":      round(native_s, 1),
                    "speedup":       round(speedup, 2),
                    "ef_search":     ef,
                    "recall_at_10":  round(recall, 4),
                    "qps":           round(qps, 1),
                })
            cur.execute("RESET enable_cuvs")
            cur.execute("RESET enable_seqscan")
            cur.execute("RESET hnsw.ef_search")

    # ── summary ───────────────────────────────────────────────────────────────
    print("\n" + "="*80)
    print(f"N={n:,}  dim={dim}  K={K}  native={native_s:.0f}s  CAGRA={cagra_s:.1f}s")
    print(f"{'mode':<15} {'HNSW_s':>7} {'total_s':>8} {'speedup':>8}  "
          f"ef-recall@10 ({'/'.join(str(e) for e in EF_VALUES)})")
    print("-"*80)
    seen = set()
    for r in all_rows:
        m = r["mode"]
        if m in seen:
            continue
        seen.add(m)
        row_data = [x for x in all_rows if x["mode"] == m]
        ef_str = " ".join(f"{x['recall_at_10']:.3f}" for x in row_data)
        print(f"{m:<15} {row_data[0]['hnsw_build_s']:>6.1f}s "
              f"{row_data[0]['total_s']:>7.1f}s "
              f"{row_data[0]['speedup']:>7.2f}x  {ef_str}")

    # ── CSV ───────────────────────────────────────────────────────────────────
    RESULTS_CSV.parent.mkdir(exist_ok=True)
    with open(RESULTS_CSV, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(all_rows[0].keys()))
        w.writeheader()
        w.writerows(all_rows)
    log(f"Results → {RESULTS_CSV}")
    log("DONE")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        log(f"ERROR: {e}")
        import traceback; traceback.print_exc()
        sys.exit(1)
