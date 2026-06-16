#!/usr/bin/env python3
"""rabitq_spike.py — numpy feasibility spike for a native RaBitQ quantizer.

Question this answers (before writing any CUDA): on our kind of data, does the
RaBitQ (Gao & Long, SIGMOD'24) 1-bit quantizer actually deliver
  low VRAM  AND  high recall (via a small, error-bounded rerank)?
If the math reproduces here, a `rabitq` AM is worth a real build; if not, we
stop now (days, not weeks) — the de-risk pattern we used for ivfpq.

RaBitQ in one screen (IVF + bi-valued codebook + random rotation):
  build, per vector o assigned to centroid c:
    r  = o - c                      residual
    r' = P r                        random *orthogonal* rotation P (shared)
    b  = (r' >= 0)                  1 bit / dim       ← the code
    x̄  = (2b - 1)/sqrt(D)           unit quantized direction
    store:  b (D bits), ||r|| (fp32), dot_xo = <x̄, r'/||r||>  (fp32)
  query q, centroid c:
    s' = P(q - c)
    <q̄,x̄> = <s', x̄> / ||s'||                      bitwise dot, cheap
    <q̄,ō>_est = <q̄,x̄> / dot_xo                    UNBIASED estimate of cosine
    est ||q-o||^2 = ||s'||^2 + ||r||^2 - 2||s'||||r|| <q̄,ō>_est
  theory: Var(<q̄,ō>_est) ≈ (1-dot_xo^2)(1-cos^2)/((D-1) dot_xo^2)
          → a per-vector error bound usable to skip reranking.

Default data is synthetic (clustered unit vectors, dim=1024 like cohere) so the
MATH checks (unbiased + bound coverage) run with no GPU and no corpus. Pass
--corpus/--queries <fbin> to run the recall check on real cohere on the VM.
"""
import argparse
import struct
import sys
import time

import numpy as np


# ── data ─────────────────────────────────────────────────────────────────────

def read_fbin(path, count=None, offset=0):
    with open(path, "rb") as f:
        n, d = struct.unpack("<ii", f.read(8))
        if count is not None:
            n = min(count, n - offset)
        f.seek(8 + offset * d * 4)
        a = np.frombuffer(f.read(n * d * 4), dtype=np.float32)
    return a.reshape(n, d).copy()


def synth(n, dim, nq, n_clusters, seed=0):
    """Clustered unit vectors — non-trivial NN structure, cohere-like (unit norm)."""
    rng = np.random.default_rng(seed)
    centers = rng.standard_normal((n_clusters, dim)).astype(np.float32)
    centers /= np.linalg.norm(centers, axis=1, keepdims=True)
    who = rng.integers(0, n_clusters, n)
    corpus = centers[who] + 0.35 * rng.standard_normal((n, dim)).astype(np.float32)
    whoq = rng.integers(0, n_clusters, nq)
    queries = centers[whoq] + 0.35 * rng.standard_normal((nq, dim)).astype(np.float32)
    corpus /= np.linalg.norm(corpus, axis=1, keepdims=True)
    queries /= np.linalg.norm(queries, axis=1, keepdims=True)
    return corpus.astype(np.float32), queries.astype(np.float32)


# ── IVF (light): a few Lloyd iterations ──────────────────────────────────────

def kmeans(x, k, iters=8, seed=0):
    rng = np.random.default_rng(seed)
    c = x[rng.choice(len(x), k, replace=False)].copy()
    for _ in range(iters):
        # assign by nearest centroid (L2)
        d = (x * x).sum(1)[:, None] - 2 * x @ c.T + (c * c).sum(1)[None, :]
        a = d.argmin(1)
        for j in range(k):
            m = a == j
            if m.any():
                c[j] = x[m].mean(0)
    d = (x * x).sum(1)[:, None] - 2 * x @ c.T + (c * c).sum(1)[None, :]
    return c, d.argmin(1)


def random_rotation(dim, seed=0):
    rng = np.random.default_rng(seed)
    q, r = np.linalg.qr(rng.standard_normal((dim, dim)))
    return (q * np.sign(np.diag(r))).astype(np.float32)   # proper orthogonal


# ── RaBitQ encode / estimate ─────────────────────────────────────────────────

def encode(corpus, centroids, assign, P):
    """Return per-vector: bits(bool NxD), r_norm(N), dot_xo(N), r_rot(NxD kept for GT only)."""
    D = corpus.shape[1]
    r = corpus - centroids[assign]
    r_rot = r @ P.T                                  # rotate residual
    r_norm = np.linalg.norm(r_rot, axis=1)
    bits = r_rot >= 0
    # dot_xo = <x̄, ō> = ||r_rot||_1 / (sqrt(D) * ||r_rot||)
    dot_xo = np.abs(r_rot).sum(1) / (np.sqrt(D) * np.maximum(r_norm, 1e-12))
    return bits, r_norm.astype(np.float32), dot_xo.astype(np.float32), r_rot


def estimate_cos(s_rot, s_norm, bits, dot_xo):
    """Unbiased estimate of <q̄, ō> for every vector vs one query residual s_rot."""
    D = s_rot.shape[0]
    # <s', x̄> = (1/sqrt(D)) * sum (2b-1) s'_i = (1/sqrt(D)) (2 b·s' - sum s')
    bdot = bits @ s_rot                              # sum over b_i=1 of s'_i
    sx = (2.0 * bdot - s_rot.sum()) / np.sqrt(D)
    q_x = sx / max(s_norm, 1e-12)                    # <q̄, x̄>
    return q_x / np.maximum(dot_xo, 1e-12)           # <q̄, ō>_est


# ── main spike ───────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus")
    ap.add_argument("--queries")
    ap.add_argument("--n", type=int, default=20000)
    ap.add_argument("--nq", type=int, default=200)
    ap.add_argument("--dim", type=int, default=1024)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args()

    if a.corpus:
        corpus = read_fbin(a.corpus, count=a.n)
        queries = read_fbin(a.queries, count=a.nq)
        a.dim = corpus.shape[1]
        src = f"fbin {a.corpus}"
    else:
        corpus, queries = synth(a.n, a.dim, a.nq, max(16, int(a.n ** 0.5) // 4), a.seed)
        src = "synthetic clustered"
    N, D = corpus.shape
    nq = len(queries)
    n_lists = max(16, int(N ** 0.5))
    print(f"# data={src}  N={N} dim={D} nq={nq} k={a.k} n_lists={n_lists}")

    t0 = time.time()
    centroids, assign = kmeans(corpus, n_lists, seed=a.seed)
    P = random_rotation(D, a.seed)
    bits, r_norm, dot_xo, r_rot = encode(corpus, centroids, assign, P)
    print(f"# build (kmeans+rotate+encode): {time.time()-t0:.1f}s")

    # exact ground truth (brute force) for recall
    gt = np.argsort(((queries * queries).sum(1)[:, None]
                     - 2 * queries @ corpus.T
                     + (corpus * corpus).sum(1)[None, :]), axis=1)[:, :a.k]

    # ── recall: realistic IVF probing (top-n_probes lists) × rerank budget ──
    # Probing a SUBSET of lists adds the real IVF-miss that "probe all" hides;
    # small budgets (0.1/0.5%) find where reranking stops recovering recall.
    z = 2.576                                        # 99% normal two-sided
    BUDGETS = (0.001, 0.005, 0.01, 0.02, 0.05)
    NPROBES = sorted({n_lists} | {p for p in (64, 32, 16, 8) if p < n_lists}, reverse=True)
    recall_grid = {(p, b): [] for p in NPROBES for b in BUDGETS}
    std_errs = []
    bias_acc = []
    cov_hits = cov_tot = 0

    for qi in range(nq):
        q = queries[qi]
        # estimate cosine + distance for ALL vectors (single-list-per-vector,
        # rotate q residual per centroid)
        est_cos = np.empty(N, np.float32)
        true_cos = np.empty(N, np.float32)
        s_rot_by_c = {}
        for j in range(n_lists):
            s = q - centroids[j]
            s_rot_by_c[j] = (s @ P.T, np.linalg.norm(s))
        for j in range(n_lists):
            m = np.where(assign == j)[0]
            if m.size == 0:
                continue
            s_rot, s_norm = s_rot_by_c[j]
            est_cos[m] = estimate_cos(s_rot, s_norm, bits[m], dot_xo[m])
            tc = (r_rot[m] @ s_rot) / (np.maximum(r_norm[m], 1e-12) * max(s_norm, 1e-12))
            true_cos[m] = tc
        # standardized error vs theoretical std (tests unbiased + variance form)
        sd = (1.0 / np.maximum(dot_xo, 1e-9)) * np.sqrt(
            np.clip((1 - dot_xo ** 2) * (1 - np.clip(true_cos, -1, 1) ** 2)
                    / (D - 1), 0, None))
        good = sd > 1e-9
        se = (est_cos[good] - true_cos[good]) / sd[good]
        std_errs.append(se)
        bias_acc.append(est_cos[good] - true_cos[good])
        cov_hits += int((np.abs(se) <= z).sum())
        cov_tot += int(good.sum())

        # --- RaBitQ estimated distance, then realistic IVF probing + rerank ---
        sres = q - centroids[assign]                 # (N,D) query residual per vec
        s_norm_all = np.linalg.norm(sres, axis=1)
        est_d2 = s_norm_all ** 2 + r_norm ** 2 - 2 * s_norm_all * r_norm * est_cos
        order_rq = np.argsort(est_d2)                 # est-ranked, all N
        cprobe = np.argsort(((centroids - q) ** 2).sum(1))   # nearest centroids first
        for nprobe in NPROBES:
            keep = np.isin(assign, cprobe[:nprobe])   # vectors in the probed lists
            cand = order_rq[keep[order_rq]]           # est-ranked ∩ probed lists
            for budget in BUDGETS:                    # rerank top-(budget% of retrieved)
                R = max(a.k, int(len(cand) * budget))
                rer = cand[:R]
                exact = ((queries[qi] - corpus[rer]) ** 2).sum(1)
                top = rer[np.argsort(exact)[:a.k]]
                recall_grid[(nprobe, budget)].append(len(set(top) & set(gt[qi])) / a.k)

    se = np.concatenate(std_errs)
    bias = np.concatenate(bias_acc)
    coverage = cov_hits / cov_tot
    bytes_per_vec = D / 8 + 8                          # bits + 2 fp32 scalars

    print("\n=== RaBitQ spike results ===")
    print(f"(a) unbiasedness   : mean(est-true cos) = {bias.mean():+.5f}  "
          f"(|.|<0.002 ⇒ PASS)   -> {'PASS' if abs(bias.mean())<0.002 else 'FAIL'}")
    print(f"    standardized   : mean={se.mean():+.3f} std={se.std():.3f}  "
          f"(std∈[0.85,1.15] ⇒ variance-form OK) -> "
          f"{'PASS' if 0.85<=se.std()<=1.15 else 'FAIL'}")
    print(f"(b) bound coverage : P(|z|<=2.576) = {coverage:.4f}  "
          f"(>=0.99 ⇒ PASS)      -> {'PASS' if coverage>=0.99 else 'FAIL'}")
    print(f"(c) recall@{a.k} — rows = n_probes (of {n_lists} lists), "
          f"cols = rerank budget (% of retrieved):")
    print("      n_probes " + "".join(f"{b*100:7.2f}%" for b in BUDGETS))
    for p in NPROBES:
        row = "".join(f"{np.mean(recall_grid[(p, b)]):8.4f}" for b in BUDGETS)
        label = f"{p} (all)" if p == n_lists else str(p)
        print(f"      {label:>8} {row}")
    # two distinct axes: probe-all isolates the QUANTIZER; n_probes is IVF tuning.
    realistic = max((p for p in NPROBES if p != n_lists), default=n_lists)
    pa = float(np.mean(recall_grid[(n_lists, 0.05)]))
    rl = float(np.mean(recall_grid[(realistic, 0.05)]))
    print(f"    quantizer (probe-all) recall@5% = {pa:.4f}  -> "
          f"{'PASS >=0.95' if pa >= 0.95 else 'FAIL <0.95'}  (is RaBitQ ranking good?)")
    print(f"    realistic n_probes={realistic} recall@5% = {rl:.4f}  "
          f"(IVF miss, not a RaBitQ fault — raise n_probes; 136 B codes make probes cheap)")
    pq_bytes = D // 2 + 8                              # ivfpq pq_dim/2 codes + scalars
    print(f"(d) storage  : RaBitQ {bytes_per_vec:.0f} B/vec  vs  ivfpq(pq_dim/2) "
          f"~{pq_bytes} B  vs  raw f32 {D*4} B  -> {D*4/bytes_per_vec:.1f}x vs raw, "
          f"{pq_bytes/bytes_per_vec:.1f}x smaller than ivfpq")


if __name__ == "__main__":
    main()
