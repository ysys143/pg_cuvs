#!/usr/bin/env python3
"""
run_pg_3i.py — Phase 3I SQL path benchmark on real embedding data.

Tests the actual pg_cuvs_import_hnsw() SQL pipeline:
  1. Load corpus into table t (same as run_pg.py)
  2. CREATE INDEX t_cagra USING cagra   (GPU build, writes .hnsw sidecar)
  3. CREATE INDEX t_hnsw  USING hnsw    (empty target)
  4. SELECT pg_cuvs_import_hnsw(...)    (CAGRA sidecar -> pgvector pages)
  5. Search via pgvector HNSW (CPU, no GPU needed at query time)

This is DISTINCT from run_cagra_hnsw.py which uses the cuVS Python library
directly and bypasses PostgreSQL entirely.

Env: cuvs_dev (has psycopg + pgvector).
"""
import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anbench_common import (  # noqa: E402
    read_fbin, recall_at_k, percentiles_ms, emit_result, gpu_mem_used_mb,
)


def log(m):
    print(f"[3i] {m}", flush=True)


def load_table(conn, corpus_path, n, dim, batch=50_000):
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
            cur.execute("DROP TABLE t CASCADE")
        cur.execute(f"CREATE TABLE t (id bigint, embedding vector({dim}))")
    conn.commit()
    log(f"COPY {n} rows ...")
    t0 = time.perf_counter()
    from pgvector import Vector
    with conn.cursor().copy("COPY t (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int8", "vector"])
        for s in range(0, n, batch):
            e = min(s + batch, n)
            chunk = np.ascontiguousarray(read_fbin(corpus_path, count=e - s, offset=s))
            for i in range(e - s):
                cp.write_row((s + i, Vector(chunk[i])))
            if s % (batch * 10) == 0:
                log(f"  {e}/{n}")
    conn.commit()
    log(f"COPY done in {time.perf_counter()-t0:.1f}s")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus",  required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt",      required=True)
    ap.add_argument("--n",       type=int, required=True)
    ap.add_argument("--out",     required=True)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--ks",      default="10,100")
    ap.add_argument("--dbname",  default="postgres")
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    # ef_search sweep for the imported HNSW
    ap.add_argument("--ef", default="16,32,64,128,256,512")
    ap.add_argument("--max-queries", type=int, default=2000)
    args = ap.parse_args()

    import psycopg
    ks  = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    efs  = [int(x) for x in args.ef.split(",")]
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt      = np.load(args.gt)
    dim     = read_fbin(args.corpus, count=1).shape[1]
    nq      = min(args.max_queries, len(queries))
    queries = queries[:nq]
    gt      = gt[:nq]

    conn = psycopg.connect(dbname=args.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")

    load_table(conn, args.corpus, args.n, dim)

    # ── Step 1: CAGRA build ───────────────────────────────────────────────
    log("Step 1: CAGRA build")
    conn.execute("DROP INDEX IF EXISTS t_cagra")
    # Keep t_hnsw if it already exists to avoid rebuilding (490s at N=1M×1024)
    conn.execute(f"SET cuvs.index_dir = '{args.index_dir}'")
    # cpu_hnsw_fallback=on tells the daemon to also serialize the .hnsw sidecar
    # alongside the .cagra file — required for pg_cuvs_import_hnsw to work.
    conn.execute("SET cuvs.cpu_hnsw_fallback = on")
    conn.execute("SET maintenance_work_mem = '8GB'")
    gpu_before = gpu_mem_used_mb()
    t0 = time.perf_counter()
    conn.execute("CREATE INDEX t_cagra ON t USING cagra (embedding vector_l2_ops)")
    t_cagra = time.perf_counter() - t0
    gpu_after = gpu_mem_used_mb()
    log(f"CAGRA build: {t_cagra:.1f}s  gpu {gpu_before:.0f}->{gpu_after:.0f}MB")

    # ── Step 1b: Trigger HNSW sidecar serialization via one dummy search ──
    # The daemon serializes .hnsw lazily on the FIRST search with
    # use_cpu_hnsw=1 (sent when cuvs.cpu_hnsw_fallback=on).
    # Without this, pg_cuvs_import_hnsw() cannot find the .hnsw sidecar.
    log("Step 1b: Trigger HNSW sidecar creation (dummy search with cpu_hnsw_fallback)")
    conn.execute("SET enable_seqscan = off")
    sample = read_fbin(args.queries, count=1)
    vec_s = "[" + ",".join(repr(float(x)) for x in sample[0].tolist()) + "]"
    with conn.cursor() as cur:
        cur.execute(
            f"SELECT id FROM t ORDER BY embedding <-> '{vec_s}'::vector LIMIT 1"
        )
        cur.fetchall()
    log("Step 1b: HNSW sidecar should now exist on disk")

    # ── Step 2: HNSW target relation ─────────────────────────────────────
    # Skip rebuild if t_hnsw already exists (saves ~490s at N=1M×1024).
    # pg_cuvs_import_hnsw truncates and rewrites the index anyway.
    with conn.cursor() as chk:
        chk.execute("SELECT to_regclass('public.t_hnsw')")
        hnsw_exists = chk.fetchone()[0] is not None
    if hnsw_exists:
        log("Step 2: t_hnsw already exists; skipping rebuild (reuse for import)")
        t_hnsw_create = 0.0
    else:
        log("Step 2: Create HNSW target (m=16 ef=64)")
        t0 = time.perf_counter()
        conn.execute(
            "CREATE INDEX t_hnsw ON t USING hnsw (embedding vector_l2_ops) "
            "WITH (m=16, ef_construction=64)"
        )
        t_hnsw_create = time.perf_counter() - t0
        log(f"HNSW create: {t_hnsw_create:.1f}s")

    # ── Step 3: pg_cuvs_import_hnsw ──────────────────────────────────────
    log("Step 3: pg_cuvs_import_hnsw (CAGRA sidecar -> pgvector pages)")
    t0 = time.perf_counter()
    conn.execute(
        "SELECT pg_cuvs_import_hnsw('t_cagra'::regclass, 't_hnsw'::regclass)"
    )
    t_import = time.perf_counter() - t0
    t_total  = t_cagra + t_import
    log(f"import: {t_import:.1f}s  total (build+import): {t_total:.1f}s")

    # HNSW index size
    with conn.cursor() as cur:
        cur.execute("SELECT pg_relation_size('t_hnsw')")
        idx_bytes = cur.fetchone()[0]
    log(f"HNSW index size: {idx_bytes/1e6:.0f} MB")

    # ── Step 4: Sweep ef_search over imported HNSW ───────────────────────
    log("Step 4: Search sweep over imported HNSW")

    def vec_literal(v):
        return "[" + ",".join(repr(float(x)) for x in v.tolist()) + "]"

    with conn.cursor() as cur:
        # Force index scan (planner may seqscan for small N)
        cur.execute("SET enable_seqscan = off")
        cur.execute("SET enable_cuvs = off")  # don't route to GPU daemon
        # warmup
        for i in range(min(50, nq)):
            cur.execute(
                f"SELECT id FROM t ORDER BY embedding <-> "
                f"'{vec_literal(queries[i])}'::vector LIMIT {kmax}"
            )
            cur.fetchall()

        for ef in efs:
            if ef < kmax:
                continue
            cur.execute(f"SET hnsw.ef_search = {ef}")
            ids = np.full((nq, kmax), -1, dtype=np.int64)
            lat = []
            for i in range(nq):
                t1 = time.perf_counter()
                cur.execute(
                    f"SELECT id FROM t ORDER BY embedding <-> "
                    f"'{vec_literal(queries[i])}'::vector LIMIT {kmax}"
                )
                rows = cur.fetchall()
                lat.append(time.perf_counter() - t1)
                for j, r in enumerate(rows):
                    ids[i, j] = r[0]
            p50, p95, p99 = percentiles_ms(lat)
            qps = nq / sum(lat)
            for k in ks:
                rec = recall_at_k(ids[:, :k], gt[:, :k], k)
                emit_result(
                    args.out,
                    system="pg_cuvs_import_hnsw",
                    dataset=args.dataset,
                    N=args.n, dim=dim,
                    metric="cosine(L2-normed)", k=k,
                    param_set=f"ef={ef}",
                    build_time_s=round(t_total, 3),
                    index_bytes=idx_bytes,
                    host_mem_mb=None,
                    gpu_mem_mb=round(gpu_after - gpu_before, 1),
                    recall=round(rec, 4),
                    qps=round(qps, 1),
                    p50_ms=round(p50, 3),
                    p95_ms=round(p95, 3),
                    p99_ms=round(p99, 3),
                    n_queries=nq,
                    notes=(f"cagra_build={t_cagra:.1f}s "
                           f"import={t_import:.1f}s total={t_total:.1f}s"),
                )

    conn.close()
    log(f"DONE. Results -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
