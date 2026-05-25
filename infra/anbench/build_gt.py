#!/usr/bin/env python3
"""
build_gt.py - exact k-NN ground truth for the query set vs the first N corpus
rows, by GPU brute force (cupy). Vectors are unit-norm, so cosine-NN == L2-NN
== top dot-product; we rank by dot product (one matmul) which matches the L2
search all systems run. Distance math only -- NOT embedding generation.

10M x 1024 (41 GB) does not fit in 40 GB VRAM, so the corpus is streamed in
tiles; we keep a running top-k (by similarity) per query across tiles.

Output: int32 array (n_queries, k) of corpus row ids -> gt_{N}.npy
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from anbench_common import read_fbin, fbin_meta  # noqa: E402


def log(m):
    print(f"[gt] {m}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--n", type=int, required=True, help="use first N corpus rows")
    ap.add_argument("--k", type=int, default=100)
    ap.add_argument("--out", required=True)
    ap.add_argument("--tile", type=int, default=250_000)
    ap.add_argument("--qbatch", type=int, default=1000)
    args = ap.parse_args()

    if os.path.exists(args.out):
        got = np.load(args.out)
        log(f"{args.out} exists shape={got.shape}; skipping")
        return 0

    import cupy as cp

    cn, cdim = fbin_meta(args.corpus)
    N = min(args.n, cn)
    queries = np.ascontiguousarray(read_fbin(args.queries))
    nq, qdim = queries.shape
    assert qdim == cdim, f"dim mismatch {qdim} vs {cdim}"
    k = args.k
    log(f"N={N} dim={cdim} nq={nq} k={k} tile={args.tile} qbatch={args.qbatch}")

    q_gpu = cp.asarray(queries)                      # (nq, dim)
    best_sim = cp.full((nq, k), -cp.inf, dtype=cp.float32)
    best_id = cp.full((nq, k), -1, dtype=cp.int64)

    for ts in range(0, N, args.tile):
        te = min(ts + args.tile, N)
        tile = np.ascontiguousarray(read_fbin(args.corpus, count=te - ts, offset=ts))
        tile_gpu = cp.asarray(tile)                  # (T, dim)
        for qs in range(0, nq, args.qbatch):
            qe = min(qs + args.qbatch, nq)
            sims = q_gpu[qs:qe] @ tile_gpu.T         # (qb, T) dot == similarity
            qb = qe - qs
            kk = min(k, te - ts)
            # top-kk LARGEST per row, in-place on sims (no -sims 8GB copy):
            # argpartition puts the kk largest at the end.
            part = cp.argpartition(sims, sims.shape[1] - kk, axis=1)[:, -kk:]
            cand_sim = cp.take_along_axis(sims, part, axis=1)
            cand_id = (part + ts).astype(cp.int64)
            del sims
            # merge with running best: concat then re-top-k
            msim = cp.concatenate([best_sim[qs:qe], cand_sim], axis=1)
            mid = cp.concatenate([best_id[qs:qe], cand_id], axis=1)
            sel = cp.argpartition(-msim, k - 1, axis=1)[:, :k]
            best_sim[qs:qe] = cp.take_along_axis(msim, sel, axis=1)
            best_id[qs:qe] = cp.take_along_axis(mid, sel, axis=1)
        del tile_gpu
        cp.get_default_memory_pool().free_all_blocks()
        log(f"tile {ts}-{te} done")

    # sort each row by descending similarity (= ascending L2)
    order = cp.argsort(-best_sim, axis=1)
    gt = cp.take_along_axis(best_id, order, axis=1).astype(cp.int32)
    gt_np = cp.asnumpy(gt)
    np.save(args.out, gt_np)
    # sanity: top-1 similarity stats (held-out queries -> < 1.0 expected)
    top1 = cp.asnumpy(cp.take_along_axis(best_sim, order[:, :1], axis=1)).mean()
    log(f"saved {args.out} shape={gt_np.shape} mean_top1_sim={top1:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
