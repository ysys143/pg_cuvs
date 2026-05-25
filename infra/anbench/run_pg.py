#!/usr/bin/env python3
"""
run_pg.py - Tier A (operational/SQL, per-query). Benchmarks one of:
  pg_cuvs (GPU CAGRA via the sidecar), pgvector HNSW, pgvector IVFFlat.
Loads the first N corpus rows into a PG table (table id == corpus row index, so
recall maps directly), builds the index, runs the 10K query set ONE statement
at a time (the operational pattern), and measures recall + per-query latency.

All use vector_l2_ops on L2-normalized vectors == cosine ranking.

Env: any with numpy + psycopg + pgvector (we add them to cuvs_py).
Connects to a local PG as the ubuntu superuser.
"""
import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anbench_common import (read_fbin, recall_at_k, percentiles_ms, emit_result,  # noqa: E402
                            gpu_mem_used_mb)


def log(m):
    print(f"[pg] {m}", flush=True)


def load_table(conn, corpus_path, n, dim, batch=50000):
    """Create table t(id bigint, embedding vector(dim)) and COPY first n rows.
    Reuses the table if it already has exactly n rows."""
    import pgvector.psycopg
    pgvector.psycopg.register_vector(conn)
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass('public.t')")
        exists = cur.fetchone()[0] is not None
        if exists:
            cur.execute("SELECT count(*) FROM t")
            if cur.fetchone()[0] == n:
                log(f"table t already has {n} rows; reuse")
                return
            cur.execute("DROP TABLE t")
        cur.execute(f"CREATE TABLE t (id bigint, embedding vector({dim}))")
    conn.commit()
    log(f"COPY {n} rows into t")
    t0 = time.perf_counter()
    written = 0
    with conn.cursor().copy("COPY t (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        from pgvector import Vector
        cp.set_types(["int8", "vector"])
        for s in range(0, n, batch):
            e = min(s + batch, n)
            chunk = np.ascontiguousarray(read_fbin(corpus_path, count=e - s, offset=s))
            for i in range(e - s):
                cp.write_row((s + i, Vector(chunk[i])))
            written = e
            if s % (batch * 20) == 0:
                log(f"  copied {written}/{n}")
    conn.commit()
    log(f"COPY done {n} rows in {time.perf_counter()-t0:.1f}s")


def build_index(conn, system, n, index_dir):
    with conn.cursor() as cur:
        if system == "pg_cuvs":
            cur.execute("SET maintenance_work_mem='8GB'")
            cur.execute(f"SET cuvs.index_dir = '{index_dir}'")
            sql = "CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops)"
            name = "t_cagra"
        elif system == "hnsw":
            cur.execute("SET maintenance_work_mem='16GB'")
            cur.execute("SET max_parallel_maintenance_workers=7")
            sql = "CREATE INDEX t_hnsw ON t USING hnsw (embedding vector_l2_ops) WITH (m=16, ef_construction=64)"
            name = "t_hnsw"
        elif system == "ivfflat":
            lists = max(1, int(4 * (n ** 0.5)))
            cur.execute("SET maintenance_work_mem='16GB'")
            sql = f"CREATE INDEX t_ivf ON t USING ivfflat (embedding vector_l2_ops) WITH (lists={lists})"
            name = "t_ivf"
        else:
            raise SystemExit(f"unknown system {system}")
        # Drop ALL ANN indexes first so only the target index exists -- otherwise
        # the planner may pick a leftover index from a previous system on the
        # same table (e.g. choose ivfflat while we think we are testing cagra).
        for nm in ("t_hnsw", "t_ivf", "t_cagra"):
            cur.execute("DROP INDEX IF EXISTS " + nm)
        conn.commit()
        t0 = time.perf_counter()
        cur.execute(sql)
        conn.commit()
        bt = time.perf_counter() - t0
        cur.execute(f"SELECT pg_relation_size('{name}')")
        idx_bytes = cur.fetchone()[0]
    log(f"built {name} in {bt:.1f}s size={idx_bytes/1e6:.0f}MB")
    return bt, idx_bytes


def run_queries(conn, queries, kmax, set_sql, warmup=200, sq_n=2000):
    """Run queries one statement at a time; return (ids array, qps, percentiles).
    set_sql: optional 'SET hnsw.ef_search=...' run per connection."""
    # Inline the query vector as a literal rather than a bind parameter:
    # pg_cuvs's cagra scan crashes the backend when the ORDER BY vector arrives
    # as an extended-protocol Param (works with a Const). Inlining keeps all PG
    # systems on the same path and avoids that pg_cuvs bug.
    nq = len(queries)

    def vec_literal(v):
        return "[" + ",".join(repr(float(x)) for x in v.tolist()) + "]"

    with conn.cursor() as cur:
        if set_sql:
            cur.execute(set_sql)

        def one(i):
            cur.execute(f"SELECT id FROM t ORDER BY embedding <-> "
                        f"'{vec_literal(queries[i])}'::vector LIMIT {kmax}")
            return cur.fetchall()

        for i in range(min(warmup, nq)):
            one(i)
        ids = np.full((nq, kmax), -1, dtype=np.int64)
        lat = []
        t0 = time.perf_counter()
        for i in range(nq):
            t1 = time.perf_counter()
            rows = one(i)
            if i < sq_n:
                lat.append(time.perf_counter() - t1)
            for j, r in enumerate(rows):
                ids[i, j] = r[0]
        total = time.perf_counter() - t0
    return ids, nq / total, percentiles_ms(lat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--system", required=True,
                    choices=["pg_cuvs", "hnsw", "ivfflat", "exact"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--ks", default="10,100")
    ap.add_argument("--dbname", default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--exact-queries", type=int, default=200,
                    help="query subset for the exact seqscan baseline (slow; "
                         "recall is 1.0 by definition)")
    args = ap.parse_args()

    import psycopg

    ks = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt = np.load(args.gt)
    dim = read_fbin(args.corpus, count=1).shape[1]
    nq = len(queries)

    # autocommit from the start: building the cagra index inside an explicit
    # transaction block corrupts the backend so subsequent cagra searches crash
    # the connection. Autocommit (index build + each query as its own txn) is
    # both the working path and the realistic app pattern. (commit() is a no-op
    # under autocommit, so the helpers' commit() calls are harmless.)
    conn = psycopg.connect(dbname=args.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    if args.system == "pg_cuvs":
        conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    conn.commit()

    load_table(conn, args.corpus, args.n, dim)

    if args.system == "exact":
        # exact in-PG baseline: no ANN index -> seqscan computes all distances.
        with conn.cursor() as cur:
            for nm in ("t_hnsw", "t_ivf", "t_cagra"):
                cur.execute("DROP INDEX IF EXISTS " + nm)
        conn.commit()
        bt, idx_bytes, gpu_before, gpu_after = 0.0, 0, float("nan"), float("nan")
        sweeps = [("SET enable_cuvs=off; SET enable_indexscan=off; "
                   "SET enable_bitmapscan=off", "exact-seqscan")]
        set_prefix = None
        eq = min(args.exact_queries, nq)
        qset, gtset, n_rep, sysname = queries[:eq], gt[:eq], eq, "pgvector-exact"
        note = "exact seqscan baseline (query subset)"
    else:
        gpu_before = gpu_mem_used_mb() if args.system == "pg_cuvs" else float("nan")
        bt, idx_bytes = build_index(conn, args.system, args.n, args.index_dir)
        gpu_after = gpu_mem_used_mb() if args.system == "pg_cuvs" else float("nan")
        if args.system == "pg_cuvs":
            sweeps = [(None, "k=100(fixed)")]
            set_prefix = f"SET cuvs.index_dir = '{args.index_dir}'"
            sysname, note = "pg_cuvs", "k hardcoded 100 internally"
        elif args.system == "hnsw":
            sweeps = [(f"SET hnsw.ef_search={v}", f"ef_search={v}")
                      for v in (10, 20, 40, 80, 120, 200, 400)]
            set_prefix, sysname, note = None, "pgvector-hnsw", ""
        else:
            sweeps = [(f"SET ivfflat.probes={v}", f"probes={v}")
                      for v in (1, 4, 8, 16, 32, 64, 128)]
            set_prefix, sysname, note = None, "pgvector-ivfflat", ""
        qset, gtset, n_rep = queries, gt, nq

    for set_sql, label in sweeps:
        full_set = "; ".join(x for x in (set_prefix, set_sql) if x)
        ids, qps, (p50, p95, p99) = run_queries(conn, qset, kmax, full_set or None)
        for k in ks:
            rec = recall_at_k(ids[:, :k], gtset[:, :k], k)
            emit_result(args.out, system=sysname, dataset=args.dataset, N=args.n, dim=dim,
                        metric="cosine(L2-normed)", k=k, param_set=label,
                        build_time_s=round(bt, 3), index_bytes=idx_bytes, host_mem_mb=None,
                        gpu_mem_mb=(round(gpu_after - gpu_before, 1)
                                    if args.system == "pg_cuvs" else None),
                        recall=round(rec, 4), qps=round(qps, 1), p50_ms=round(p50, 3),
                        p95_ms=round(p95, 3), p99_ms=round(p99, 3), n_queries=n_rep, notes=note)
    conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
