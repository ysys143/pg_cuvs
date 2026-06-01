#!/usr/bin/env python3
"""Generate a synthetic dataset for the crossover pilot.

Emits, under --out:
  base.fbin / query.fbin   -- for ground-truth + cuVS-side tools
  base.copy / query.copy   -- psql \\copy text: "<id>\\t[v1,v2,...]" (0-based ids)
                              loadable into the shared `vector` column for BOTH
                              pgvector and pg_cuvs (identical SQL surface).
"""
import argparse
import os
import numpy as np
from common import write_fbin


def write_copy(path, a):
    # numpy C-level per-row text write — feasible for large high-dim cells
    # (1M x 1536 text is ~14 GB; python per-element join is too slow).
    with open(path, "w") as f:
        for i in range(a.shape[0]):
            f.write(f"{i}\t[")
            a[i].tofile(f, sep=",", format="%.6g")
            f.write("]\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100000)
    ap.add_argument("--dim", type=int, default=384)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--dist", choices=["random", "clustered"], default="random")
    ap.add_argument("--out", default="bench/data")
    a = ap.parse_args()

    rng = np.random.default_rng(a.seed)
    if a.dist == "random":
        base = rng.random((a.n, a.dim), dtype=np.float32)
        query = rng.random((a.queries, a.dim), dtype=np.float32)
    else:
        nc = max(8, a.n // 5000)
        centers = rng.random((nc, a.dim), dtype=np.float32)
        base = (centers[rng.integers(0, nc, a.n)] +
                0.05 * rng.standard_normal((a.n, a.dim))).astype(np.float32)
        query = (centers[rng.integers(0, nc, a.queries)] +
                 0.05 * rng.standard_normal((a.queries, a.dim))).astype(np.float32)

    os.makedirs(a.out, exist_ok=True)
    write_fbin(f"{a.out}/base.fbin", base)
    write_fbin(f"{a.out}/query.fbin", query)
    write_copy(f"{a.out}/base.copy", base)
    write_copy(f"{a.out}/query.copy", query)
    print(f"[gen] base={base.shape} query={query.shape} dist={a.dist} -> {a.out}")


if __name__ == "__main__":
    main()
