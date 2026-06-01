#!/usr/bin/env python3
"""Score recall@k from a "<qid> <neighbor_id>" results file vs gt.ibin.

Robust to '|' or whitespace separators (psql -A defaults to '|'). qids are
0-based and align with gt rows.
"""
import argparse
from common import read_ibin, recall_at_k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gt", required=True)
    ap.add_argument("--results", required=True)
    ap.add_argument("--k", type=int, default=10)
    a = ap.parse_args()

    gt = read_ibin(a.gt)[:, : a.k]
    pred = {}
    with open(a.results) as f:
        for line in f:
            parts = line.replace("|", " ").split()
            if len(parts) < 2:
                continue
            qid, nid = parts[0], parts[1]
            pred.setdefault(int(qid), []).append(int(nid))
    print(f"{recall_at_k(pred, gt):.4f}")


if __name__ == "__main__":
    main()
