#!/usr/bin/env python3
"""shim_runner.py — CPU-shim placeholder measurement.

Writes ONE protocol row via observe.write_protocol_row so the plumbing
(run.sh dispatch -> observe CSV writer + ResourceSampler) is testable without
GPU/PG. The real path is engines/<config>.sh -> adapted run_pg.py, which does
the same import + write but around a real build/query.
"""
import argparse
import datetime
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "infra", "anbench"))
import observe  # noqa: E402


def parse_cell(cell_id):
    """N1k_d1024_k10_r0.95 -> {N, dim, k, recall_target}. Best effort."""
    m = re.match(r"N([0-9]+)([kmg]?)_d(\d+)_k(\d+)_r([0-9.]+)$", cell_id)
    if not m:
        return {}
    num, suf, d, k, r = m.groups()
    mult = {"": 1, "k": 1000, "m": 1000000, "g": 1000000000}[suf]
    return {"N": int(num) * mult, "dim": int(d), "k": int(k),
            "recall_target": float(r)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--cell", required=True)
    ap.add_argument("--config", required=True)
    ap.add_argument("--dataset", default="shim")
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--cost-model-version", default="unset")
    ap.add_argument("--runtime-routing-version", default="unset")
    a = ap.parse_args()

    # exercise the real sampler (no-GPU safe -> vram/energy/gpu_s = None)
    with observe.ResourceSampler() as s:
        pass

    row = dict(
        run_id=a.run_id, date=datetime.date.today().isoformat(),
        stage=a.stage, phase="query", cell_id=a.cell, config=a.config,
        system="shim", index_type="shim", dataset=a.dataset,
        cost_model_version=a.cost_model_version,
        runtime_routing_version=a.runtime_routing_version,
        notes="cpu-shim-placeholder",
        **parse_cell(a.cell), **s.as_dict(),
    )
    observe.write_protocol_row(a.csv, **row)


if __name__ == "__main__":
    main()
