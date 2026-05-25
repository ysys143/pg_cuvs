#!/usr/bin/env python3
"""
run_faiss.py - faiss baselines (Tier B). --gpu (IVFFlat + IVFPQ, env faiss_gpu)
or --cpu (IVFFlat + HNSW, env faiss_cpu). METRIC_L2 on unit-norm vectors ==
cosine ranking. Reports batched QPS + single-query p50/p95/p99.
"""
import argparse
import math
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anbench_common import (read_fbin, recall_at_k, percentiles_ms,  # noqa: E402
                            gpu_mem_used_mb, host_mem_mb, emit_result)


def log(m):
    print(f"[faiss] {m}", flush=True)


def time_search(index, queries, k, sq_n):
    index.search(queries[:256], k)  # warm
    t0 = time.perf_counter()
    _, I = index.search(queries, k)
    batched = time.perf_counter() - t0
    lat = []
    for i in range(sq_n):
        t1 = time.perf_counter()
        index.search(queries[i:i + 1], k)
        lat.append(time.perf_counter() - t1)
    return I, len(queries) / batched, percentiles_ms(lat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dataset", default="cohere-wiki-en-1024")
    ap.add_argument("--ks", default="10,100")
    ap.add_argument("--mode", choices=["gpu", "cpu"], required=True)
    ap.add_argument("--single-query-n", type=int, default=2000)
    ap.add_argument("--bf-queries", type=int, default=2000,
                    help="query subset for the exact brute-force baseline "
                         "(recall is 1.0 by definition; caps slow CPU flat)")
    args = ap.parse_args()

    import faiss

    ks = [int(x) for x in args.ks.split(",")]
    kmax = max(ks)
    corpus = np.ascontiguousarray(read_fbin(args.corpus, count=args.n))
    queries = np.ascontiguousarray(read_fbin(args.queries))
    gt = np.load(args.gt)
    n, d = corpus.shape
    nq = len(queries)
    sq_n = min(args.single_query_n, nq)
    nlist = int(4 * math.sqrt(n))  # ~4*sqrt(N) lists
    log(f"mode={args.mode} N={n} dim={d} nlist={nlist}")

    def run(system, index, build_time, sweep_attr, sweep_vals, gpu_mb):
        for v in sweep_vals:
            setattr_path(index, sweep_attr, v)
            I, qps, (p50, p95, p99) = time_search(index, queries, kmax, sq_n)
            for k in ks:
                rec = recall_at_k(I[:, :k], gt[:, :k], k)
                emit_result(args.out, system=system, dataset=args.dataset, N=n, dim=d,
                            metric="cosine(L2-normed)", k=k, param_set=f"{sweep_attr}={v}",
                            build_time_s=round(build_time, 3), index_bytes=None,
                            host_mem_mb=round(host_mem_mb(), 1), gpu_mem_mb=gpu_mb,
                            recall=round(rec, 4), qps=round(qps, 1), p50_ms=round(p50, 3),
                            p95_ms=round(p95, 3), p99_ms=round(p99, 3), n_queries=nq, notes="")

    def run_flat(system, index, build_time, gpu_mb):
        # exact brute force: recall is 1.0 by definition; use a query subset
        # (cheap proof + bounds slow CPU flat). One operating point.
        bfq = min(args.bf_queries, nq)
        I, qps, (p50, p95, p99) = time_search(index, queries[:bfq], kmax, min(sq_n, bfq))
        for k in ks:
            rec = recall_at_k(I[:, :k], gt[:bfq, :k], k)
            emit_result(args.out, system=system, dataset=args.dataset, N=n, dim=d,
                        metric="cosine(L2-normed)", k=k, param_set="exact",
                        build_time_s=round(build_time, 3), index_bytes=None,
                        host_mem_mb=round(host_mem_mb(), 1), gpu_mem_mb=gpu_mb,
                        recall=round(rec, 4), qps=round(qps, 1), p50_ms=round(p50, 3),
                        p95_ms=round(p95, 3), p99_ms=round(p99, 3), n_queries=bfq,
                        notes="exact brute force baseline")

    if args.mode == "gpu":
        res = faiss.StandardGpuResources()
        gpu_before = gpu_mem_used_mb()
        # IVFFlat
        cfg = faiss.GpuIndexIVFFlatConfig()
        ivf = faiss.GpuIndexIVFFlat(res, d, nlist, faiss.METRIC_L2, cfg)
        t0 = time.perf_counter(); ivf.train(corpus); ivf.add(corpus)
        bt = time.perf_counter() - t0
        run("faiss-gpu-ivfflat", ivf, bt, "nprobe", [1, 4, 8, 16, 32, 64, 128, 256],
            round(gpu_mem_used_mb() - gpu_before, 1))
        del ivf
        # IVFPQ (m subquantizers divide d; 1024/16=64)
        m_pq = 64 if d % 64 == 0 else 8
        cfgp = faiss.GpuIndexIVFPQConfig()
        # m=64 subquantizers @ 8 bits needs 64 KB of PQ lookup tables in shared
        # memory, over the A100's 48 KB/block limit. fp16 tables halve that to
        # 32 KB so the search kernel fits.
        cfgp.useFloat16LookupTables = True
        ivfpq = faiss.GpuIndexIVFPQ(res, d, nlist, m_pq, 8, faiss.METRIC_L2, cfgp)
        t0 = time.perf_counter(); ivfpq.train(corpus); ivfpq.add(corpus)
        bt = time.perf_counter() - t0
        run("faiss-gpu-ivfpq", ivfpq, bt, "nprobe", [1, 4, 8, 16, 32, 64, 128, 256],
            round(gpu_mem_used_mb() - gpu_before, 1))
        del ivfpq
        # exact GPU brute force (recall 1.0 baseline)
        flat = faiss.GpuIndexFlatL2(res, d, faiss.GpuIndexFlatConfig())
        t0 = time.perf_counter(); flat.add(corpus); bft = time.perf_counter() - t0
        run_flat("faiss-gpu-flat", flat, bft, round(gpu_mem_used_mb() - gpu_before, 1))
    else:
        # CPU IVFFlat
        quant = faiss.IndexFlatL2(d)
        ivf = faiss.IndexIVFFlat(quant, d, nlist, faiss.METRIC_L2)
        t0 = time.perf_counter(); ivf.train(corpus); ivf.add(corpus)
        bt = time.perf_counter() - t0
        run("faiss-cpu-ivfflat", ivf, bt, "nprobe", [1, 4, 8, 16, 32, 64, 128],
            float("nan"))
        # CPU HNSW
        hnsw = faiss.IndexHNSWFlat(d, 32, faiss.METRIC_L2)
        hnsw.hnsw.efConstruction = 64
        t0 = time.perf_counter(); hnsw.add(corpus)
        bt = time.perf_counter() - t0
        run("faiss-cpu-hnsw", hnsw, bt, "hnsw.efSearch", [16, 32, 64, 128, 256],
            float("nan"))
        # exact CPU brute force (recall 1.0 baseline; query subset for speed)
        flat = faiss.IndexFlatL2(d)
        t0 = time.perf_counter(); flat.add(corpus); bft = time.perf_counter() - t0
        run_flat("faiss-cpu-flat", flat, bft, float("nan"))
    return 0


def setattr_path(obj, path, val):
    """setattr supporting dotted paths like 'hnsw.efSearch'."""
    parts = path.split(".")
    for p in parts[:-1]:
        obj = getattr(obj, p)
    setattr(obj, parts[-1], val)


if __name__ == "__main__":
    sys.exit(main())
