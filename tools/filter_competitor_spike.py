#!/usr/bin/env python3
"""filter_competitor_spike.py — D2 competitor arm: pgvector filtered kNN under
hnsw.iterative_scan, with the p99 tail.

The pg_cuvs side (D-wedge cuvs_filtered_knn, recall=1.0 @ flat latency for
sel>=10%) is already measured in tools/bench_filter_sweep.py /
docs/filter-threshold-experiment.md. This measures the COMPETITOR: pgvector
HNSW + a WHERE filter, swept over hnsw.iterative_scan in {off, strict_order,
relaxed_order} × selectivity. The differentiation thesis:
  - off       → HNSW filters AFTER graph search → returns < k rows → recall cliff
  - relaxed   → keeps scanning → recall recovers but p99 tail blows up
pg_cuvs auto-routing (D-wedge/3O) holds recall=1.0 at a flat tail instead.

Reports recall@k + p50/p99 (ms) per (selectivity, scan_mode). Runs on the VM via
engines/spike-filter.sh (configs=spike-filter), cohere 100k.
"""
import argparse
import struct
import sys
import time

import numpy as np


def read_fbin(path, count=None, offset=0):
    with open(path, "rb") as f:
        n, d = struct.unpack("<ii", f.read(8))
        if count is not None:
            n = min(count, n - offset)
        f.seek(8 + offset * d * 4)
        a = np.frombuffer(f.read(n * d * 4), dtype=np.float32)
    return a.reshape(n, d).copy()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--n", type=int, default=100000)
    ap.add_argument("--nq", type=int, default=300)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--dbname", default="bench")
    a = ap.parse_args()

    corpus = read_fbin(a.corpus, count=a.n)
    queries = read_fbin(a.queries, count=a.nq)
    N, D = corpus.shape
    nq = len(queries)
    print(f"# data={a.corpus} N={N} dim={D} nq={nq} k={a.k}")

    import psycopg
    from pgvector.psycopg import register_vector
    from pgvector import Vector
    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    register_vector(conn)
    pgv = conn.execute("SELECT extversion FROM pg_extension WHERE extname='vector'").fetchone()[0]
    print(f"# pgvector {pgv}")

    # load table (id + embedding), build HNSW once
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS fbench")
    cur.execute(f"CREATE TABLE fbench (id int, embedding vector({D}))")
    with conn.cursor().copy(
            "COPY fbench (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int4", "vector"])
        for i in range(N):
            cp.write_row((i, Vector(corpus[i])))
    cur.execute("CREATE INDEX ON fbench USING hnsw (embedding vector_l2_ops) "
                "WITH (m=16, ef_construction=64)")
    cur.execute("SET hnsw.ef_search = 100")
    print("# hnsw built")

    rng = np.random.default_rng(0)
    SELS = (0.01, 0.05, 0.10, 0.50)
    MODES = ("off", "strict_order", "relaxed_order")

    # does this pgvector support iterative_scan?
    try:
        cur.execute("SET hnsw.iterative_scan = strict_order")
        has_iter = True
    except Exception as e:
        has_iter = False
        print(f"# hnsw.iterative_scan unsupported ({e!r}) — 'off' only")
        MODES = ("off",)

    print(f"\n{'sel':>5} {'scan_mode':>14} {'recall@'+str(a.k):>9} "
          f"{'p50_ms':>8} {'p99_ms':>8} {'n<k':>5}")
    for sel in SELS:
        nf = max(a.k, int(N * sel))
        fset = np.sort(rng.choice(N, nf, replace=False))          # tenant subset
        fset_l = fset.tolist()
        # exact-filtered GT per query (numpy over the filtered rows)
        sub = corpus[fset]
        gt = []
        for qi in range(nq):
            d = ((sub - queries[qi]) ** 2).sum(1)
            gt.append(set(fset[np.argsort(d)[:a.k]].tolist()))
        for mode in MODES:
            if has_iter:
                cur.execute(f"SET hnsw.iterative_scan = {mode}")
            lat, recs, short = [], [], 0
            for qi in range(nq):
                t = time.perf_counter()
                cur.execute(
                    "SELECT id FROM fbench WHERE id = ANY(%s) "
                    "ORDER BY embedding <-> %s LIMIT %s",
                    (fset_l, Vector(queries[qi]), a.k))
                got = [r[0] for r in cur.fetchall()]
                lat.append((time.perf_counter() - t) * 1000.0)
                if len(got) < a.k:
                    short += 1
                recs.append(len(set(got) & gt[qi]) / a.k)
            p50, p99 = np.percentile(lat, 50), np.percentile(lat, 99)
            print(f"{sel*100:4.0f}% {mode:>14} {np.mean(recs):9.4f} "
                  f"{p50:8.2f} {p99:8.2f} {short:5d}")

    cur.execute("DROP TABLE fbench")
    conn.close()
    print("\n# vs pg_cuvs D-wedge (filter-threshold-experiment.md): recall=1.0 @ "
          "sel>=10% all correlations, ~1.3-2.8ms flat. Competitor recall/p99 above.")


if __name__ == "__main__":
    main()
