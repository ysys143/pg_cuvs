#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pandas>=2.0", "matplotlib>=3.7", "seaborn>=0.13"]
# ///
"""plot.py — reproducible Stage-A figures from a protocol results CSV.

Reads a CONTRACT §6 / observe.PROTOCOL_FIELDS CSV (the columns it needs:
config, phase, N, build_s, qps, p50_us, p95_us, p99_us, recall_at_k) and emits
the standard Stage-A figures. Repeated cells (reps) are aggregated by median.

Run reproducibly (no manual venv):
    uv run bench/protocol/plot.py --csv results/protocol/A.csv --out figs/
"""
import argparse
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

# config id -> display label / colour
ENGINES = {"forced-hnsw": ("HNSW", "#e0691a"),
           "forced-cuvs": ("CAGRA", "#1f6fe0"),
           "forced-cuvs-bf": ("GPU-BF", "#11a36b"),
           "forced-cuvs-bf-batch": ("GPU-BF+batch", "#7b2fe0")}
SUB = "cohere 1024d · k=10 · iso-recall@10=0.95 · same-box A100 · c=1"


def _pivot(dfq, value):
    """median over reps → DataFrame indexed by N, columns = config."""
    p = dfq.pivot_table(index="N", columns="config", values=value,
                        aggfunc="median")
    return p.sort_index()


def fig_ratio(dfq, out):
    fig, ax = plt.subplots(figsize=(8, 5))
    for pct, ls in [("p50_us", "-"), ("p95_us", "--"), ("p99_us", ":")]:
        p = _pivot(dfq, pct)
        if "forced-hnsw" not in p or "forced-cuvs" not in p:
            continue
        r = p["forced-cuvs"] / p["forced-hnsw"]
        ax.plot(r.index, r.values, ls, marker="o", lw=2.4,
                label=pct.replace("_us", ""))
    ax.axhline(1.0, color="#333", lw=1.4, ls=(0, (4, 4)))
    ax.text(ax.get_xlim()[1], 1.0, "  CAGRA = HNSW", va="center", fontsize=9)
    ax.set_xscale("log")
    ax.set_xlabel("N (corpus size, log)")
    ax.set_ylabel("latency ratio  CAGRA / HNSW   (<1 = CAGRA faster)")
    ax.set_title("CAGRA / HNSW single-query latency ratio")
    ax.legend(title="percentile")
    fig.text(0.5, 0.005, SUB, ha="center", fontsize=8, color="#555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(os.path.join(out, "ratio.png"), dpi=150)
    plt.close(fig)


def fig_latency(dfq, out):
    fig, ax = plt.subplots(figsize=(8, 5))
    for cfg, (lab, col) in ENGINES.items():
        for pct, ls in [("p95_us", "-"), ("p99_us", "--")]:
            p = _pivot(dfq, pct)
            if cfg not in p:
                continue
            ax.plot(p.index, p[cfg].values / 1000.0, ls, marker="o", lw=2.4,
                    color=col, label=f"{lab} {pct.replace('_us','')}")
    ax.set_xscale("log")
    ax.set_xlabel("N (corpus size, log)")
    ax.set_ylabel("latency (ms)")
    ax.set_title("Single-query latency — CAGRA stays flat, HNSW climbs")
    ax.legend()
    fig.text(0.5, 0.005, SUB, ha="center", fontsize=8, color="#555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(os.path.join(out, "latency.png"), dpi=150)
    plt.close(fig)


def fig_qps(dfq, out):
    qps = _pivot(dfq, "qps")
    rec = _pivot(dfq, "recall_at_k")
    fig, ax = plt.subplots(figsize=(8, 5))
    for cfg, (lab, col) in ENGINES.items():
        if cfg not in qps:
            continue
        ax.plot(qps.index, qps[cfg].values, "-", marker="o", lw=2.4,
                color=col, label=lab)
        for N in qps.index:
            q = qps.loc[N, cfg]
            r = rec.loc[N, cfg] if cfg in rec.columns else float("nan")
            dy = 14 if cfg == "forced-hnsw" else -18
            ax.annotate(f"{q:.0f}\nr{r:.3f}", (N, q), textcoords="offset points",
                        xytext=(0, dy), ha="center", fontsize=8, color=col)
    ax.set_xscale("log")
    ax.set_xlabel("N (corpus size, log)")
    ax.set_ylabel("QPS")
    ax.set_title("Throughput vs corpus size (labels: QPS / recall@10)")
    ax.legend()
    fig.text(0.5, 0.005,
             "single client c=1 (= 1/mean-latency); concurrent-load QPS is Stage D4 · " + SUB,
             ha="center", fontsize=8, color="#555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(os.path.join(out, "qps.png"), dpi=150)
    plt.close(fig)


def fig_build(dfb, out):
    p = _pivot(dfb, "build_s")
    fig, ax = plt.subplots(figsize=(8, 5))
    for cfg, (lab, col) in ENGINES.items():
        if cfg not in p:
            continue
        ax.plot(p.index, p[cfg].values, "-", marker="o", lw=2.4,
                color=col, label=lab)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("N (corpus size, log)")
    ax.set_ylabel("index build time (s, log)")
    ax.set_title("Index build time — CAGRA 4–13× faster")
    ax.legend()
    fig.text(0.5, 0.005, SUB, ha="center", fontsize=8, color="#555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(os.path.join(out, "build.png"), dpi=150)
    plt.close(fig)


def fig_concurrency(dfq, out):
    """QPS vs concurrent clients (labels: p99 ms). Shows the single-daemon ceiling."""
    d = dfq.dropna(subset=["qps"])
    fig, ax = plt.subplots(figsize=(8, 5))
    for cfg, (lab, col) in ENGINES.items():
        g = d[d["config"] == cfg].sort_values("clients")
        if g.empty:
            continue
        ax.plot(g["clients"], g["qps"], "-", marker="o", lw=2.4, color=col, label=lab)
        for _, r in g.iterrows():
            ax.annotate(f"p99 {r['p99_us']/1000:.0f}ms", (r["clients"], r["qps"]),
                        textcoords="offset points",
                        xytext=(0, 12 if cfg == "forced-cuvs" else -16),
                        ha="center", fontsize=8, color=col)
    ax.set_xscale("log", base=2)
    ax.set_xlabel("concurrent clients (pgbench -c, log2)")
    ax.set_ylabel("QPS (sustained, pgbench -T)")
    ax.set_title("Throughput under concurrency — single-daemon ceiling")
    ax.legend()
    fig.text(0.5, 0.005,
             "N=1M · k=10 · iso-recall@10=0.95 · same-box A100 · vector bound from a table (realistic) · " + SUB.split(' · ', 1)[1],
             ha="center", fontsize=8, color="#555")
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    fig.savefig(os.path.join(out, "concurrency.png"), dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", default="figs")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sns.set_theme(style="whitegrid", context="talk", font_scale=0.7)

    df = pd.read_csv(a.csv)
    df = df[df["config"].isin(ENGINES)]
    dfq = df[df["phase"] == "query"].copy()
    dfb = df[df["phase"] == "build"].copy()

    # concurrency data (varying `clients`) → the load-version figure; else Stage-A.
    if "clients" in dfq and dfq["clients"].nunique() > 1:
        fig_concurrency(dfq, a.out)
        print(f"wrote figures to {a.out}/: concurrency.png")
        return
    fig_ratio(dfq, a.out)
    fig_latency(dfq, a.out)
    fig_qps(dfq, a.out)
    if not dfb.empty:
        fig_build(dfb, a.out)
    print(f"wrote figures to {a.out}/: ratio.png latency.png qps.png build.png")


if __name__ == "__main__":
    main()
