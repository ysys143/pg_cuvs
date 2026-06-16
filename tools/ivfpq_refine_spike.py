#!/usr/bin/env python3
"""ivfpq_refine_spike.py — does the REAL cuVS refine() lift ivf_pq recall, and
at what cost? (Handoff "option B".)

Tests the cuVS PRIMITIVE directly via its Python bindings — no daemon, no IPC,
no extension rebuild — the same low-integration approach rabitq_spike.py used
for RaBitQ. Baseline = ivf_pq at pq_dim=dim/2 (the config that gave recall 0.937
on the VM, bench run #30). Then over-fetch k*ratio candidates from ivf_pq and
cuVS refine() with the original vectors, sweeping refine_ratio. Reports recall
+ search/refine latency per ratio so we can compare against RaBitQ (136 B/vec,
recall 0.9675 @ realistic n_probes) on the same cohere slice.

Runs on the VM via engines/spike-ivfpq-refine.sh (configs=spike-ivfpq-refine).
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
    ap.add_argument("--nq", type=int, default=200)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--n-probes", type=int, default=64)
    a = ap.parse_args()

    corpus = read_fbin(a.corpus, count=a.n)
    queries = read_fbin(a.queries, count=a.nq)
    N, D = corpus.shape
    nq = len(queries)
    n_lists = max(16, int(N ** 0.5))
    pq_dim = D // 2                                   # the run #30 baseline config
    print(f"# data={a.corpus}  N={N} dim={D} nq={nq} k={a.k} "
          f"n_lists={n_lists} pq_dim={pq_dim} n_probes={a.n_probes}")

    # cuVS python bindings (fail fast + diagnostic if the env lacks them)
    try:
        import cupy as cp
        import cuvs
        from cuvs.neighbors import ivf_pq, refine
        print(f"# cuvs {getattr(cuvs, '__version__', '?')}  cupy {cp.__version__}")
    except Exception as e:
        print(f"[spike] cuVS python bindings unavailable: {e!r}")
        print("[spike] -> fall back to the C++ wrapper route for option B")
        sys.exit(0)

    # exact GT (numpy brute force, squared L2 — matches ivf_pq sqeuclidean)
    gt = np.argsort(((queries * queries).sum(1)[:, None]
                     - 2 * queries @ corpus.T
                     + (corpus * corpus).sum(1)[None, :]), axis=1)[:, :a.k]

    d_corpus = cp.asarray(corpus)
    d_queries = cp.asarray(queries)

    t0 = time.perf_counter()
    idx = ivf_pq.build(
        ivf_pq.IndexParams(n_lists=n_lists, pq_bits=8, pq_dim=pq_dim,
                           metric="sqeuclidean"),
        d_corpus)
    cp.cuda.runtime.deviceSynchronize()
    print(f"# ivf_pq build: {time.perf_counter()-t0:.1f}s")

    sp = ivf_pq.SearchParams(n_probes=a.n_probes)

    def recall_of(neigh):                              # neigh: (nq,k) host int
        return float(np.mean([len(set(neigh[i]) & set(gt[i])) / a.k
                              for i in range(nq)]))

    print("\n=== cuVS ivf_pq + refine spike ===")
    print(f"  {'refine_ratio':>12} {'recall@'+str(a.k):>9} {'ms/query':>9}")
    for ratio in (1, 2, 4, 8, 16):
        k2 = min(a.k * ratio, N)
        cp.cuda.runtime.deviceSynchronize()
        t = time.perf_counter()
        _, I = ivf_pq.search(sp, idx, d_queries, k2)
        if ratio > 1:
            # cuVS refine: recompute exact distances on the candidates, re-rank
            _, I = refine.refine(d_corpus, d_queries, I, a.k, metric="sqeuclidean")
        cp.cuda.runtime.deviceSynchronize()
        ms = (time.perf_counter() - t) / nq * 1000.0
        neigh = cp.asnumpy(I)[:, :a.k]
        print(f"  {ratio:>12} {recall_of(neigh):>9.4f} {ms:>9.3f}")

    print("\n# note: dataset is device-resident here (refine variant A: f32 in "
          "VRAM, no compression win). The VRAM-preserving variant (host/GDS "
          "dataset) is the follow-up — this run measures the recall lift + "
          "refine latency.")


if __name__ == "__main__":
    main()
