#!/usr/bin/env python3
"""Compute exact L2 ground truth (brute force) for the pilot dataset."""
import argparse
from common import read_fbin, write_ibin, brute_force_l2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="bench/data")
    ap.add_argument("--k", type=int, default=100)
    a = ap.parse_args()

    base = read_fbin(f"{a.data}/base.fbin")
    query = read_fbin(f"{a.data}/query.fbin")
    gt = brute_force_l2(base, query, a.k)
    write_ibin(f"{a.data}/gt.ibin", gt)
    print(f"[gt] base={base.shape} query={query.shape} k={a.k} -> {a.data}/gt.ibin")


if __name__ == "__main__":
    main()
