"""Shared helpers for the pg_cuvs crossover benchmark harness.

File formats (DiskANN-compatible, also used by the Phase 3B spike):
  fbin = [int32 n][int32 d][float32 n*d]   -- vectors
  ibin = [int32 n][int32 k][int32  n*k]    -- neighbor-id matrix (ground truth)

See design/BENCHMARK_CROSSOVER.md for how these feed the pilot.
"""
import numpy as np


def read_fbin(path):
    with open(path, "rb") as f:
        n, d = np.fromfile(f, np.int32, 2)
        a = np.fromfile(f, np.float32, n * d).reshape(n, d)
    return a


def write_fbin(path, a):
    a = np.ascontiguousarray(a, dtype=np.float32)
    with open(path, "wb") as f:
        np.array(a.shape, dtype=np.int32).tofile(f)
        a.tofile(f)


def read_ibin(path):
    with open(path, "rb") as f:
        n, k = np.fromfile(f, np.int32, 2)
        a = np.fromfile(f, np.int32, n * k).reshape(n, k)
    return a


def write_ibin(path, a):
    a = np.ascontiguousarray(a, dtype=np.int32)
    with open(path, "wb") as f:
        np.array(a.shape, dtype=np.int32).tofile(f)
        a.tofile(f)


def brute_force_l2(base, query, k):
    """Exact L2 top-k ground truth. O(nq * n * d); fine for pilot sizes.
    For N>=10M consider a GPU/batched variant (noted in BENCHMARK_CROSSOVER.md)."""
    bb = (base * base).sum(1)
    gt = np.empty((query.shape[0], k), np.int32)
    for i in range(query.shape[0]):
        dq = bb - 2.0 * (base @ query[i]) + (query[i] @ query[i])
        idx = np.argpartition(dq, k)[:k]
        gt[i] = idx[np.argsort(dq[idx])]
    return gt


def recall_at_k(pred, gt):
    """pred: dict{qid(int 0-based) -> list[id]}; gt: ndarray (nq, k)."""
    nq, k = gt.shape
    hit = 0
    for q in range(nq):
        hit += len(set(pred.get(q, [])) & set(gt[q].tolist()))
    return hit / (nq * k)
