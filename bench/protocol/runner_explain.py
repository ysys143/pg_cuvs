#!/usr/bin/env python3
"""runner_explain.py — Stage B physics cost-model validation (PGCUVS_MODULE=explain).

v3 (ADR-075): the cost model is now data-movement physics + a hardware probe.
This sweep validates ROUTING, not a constant. For each cell it builds the two GPU
deployments — a `cagra` table and a `flat` table (the planner's real per-query
choice is seqscan↔THE-ONE-INDEX, since a column carries one index) — and EXPLAINs
the bound KNN query under BOTH regimes:

  * physical (cuvs.enable_phys_cost=on, default) — hardware-probed costs
  * legacy   (cuvs.enable_phys_cost=off)         — baked constants (pre-0.5.0)

capturing, per (engine, regime): the planner's chosen path + each path's est
startup/total cost, plus a pg_cuvs_hw_profile() snapshot. EXPLAIN only — cheap,
no execution. The phys-vs-legacy delta is the evidence that physics moves the
seqscan↔GPU crossover to where the hardware/dim actually put it.
"""
import argparse
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANBENCH = os.path.abspath(os.path.join(HERE, "..", "..", "infra", "anbench"))
sys.path.insert(0, ANBENCH)
import observe  # noqa: E402
import runner   # noqa: E402


def explain_cost(conn, table, qvec, k):
    """(total_cost, startup_cost, (scan_node_type, index_name)) for the bound query."""
    from pgvector import Vector
    with conn.cursor() as cur:
        cur.execute(f"EXPLAIN (FORMAT JSON) SELECT id FROM {table} "
                    f"ORDER BY embedding <-> %s LIMIT %s", (Vector(qvec), k))
        plan = cur.fetchone()[0][0]["Plan"]
    total, startup = plan["Total Cost"], plan["Startup Cost"]
    node, scan = plan, ("", "")
    while node is not None:
        nt = node.get("Node Type", "")
        if "Scan" in nt:
            scan = (nt, node.get("Index Name", ""))
            break
        kids = node.get("Plans")
        node = kids[0] if kids else None
    return total, startup, scan


def classify(scan):
    nt, idx = scan
    if "Seq Scan" in nt:
        return "seqscan"
    if "flat" in (idx or ""):
        return "flat"
    if "cagra" in (idx or ""):
        return "cuvs"
    if "hnsw" in (idx or ""):
        return "hnsw"
    if "CuvsTransientBF" in nt:
        return "transient-bf"
    return nt or "?"


def hw_profile(conn):
    """Snapshot pg_cuvs_hw_profile() — the routing-regime witness. {} if absent."""
    try:
        with conn.cursor() as cur:
            cur.execute("SELECT gpu_name, source, probe_status, "
                        "matches_running_daemon, link_bw_bytes_per_us, "
                        "hbm_bw_bytes_per_us, gpu_cagra_lat_us, ipc_rtt_us "
                        "FROM pg_cuvs_hw_profile()")
            r = cur.fetchone()
        if not r:
            return {}
        keys = ["gpu_name", "source", "probe_status", "matches_running_daemon",
                "link_bw", "hbm_bw", "gpu_cagra_lat_us", "ipc_rtt_us"]
        return {k: v for k, v in zip(keys, r)}
    except Exception as e:                       # function missing / older version
        return {"hw_profile_error": str(e)[:80]}


def set_paths(conn, *, seqscan, indexscan):
    conn.execute(f"SET enable_seqscan = {'on' if seqscan else 'off'}")
    conn.execute(f"SET enable_indexscan = {'on' if indexscan else 'off'}")
    conn.execute("SET enable_bitmapscan = off")


def sweep_engine(conn, table, qvec, k):
    """Return dict: default chosen + per-path cost (seqscan vs the index)."""
    set_paths(conn, seqscan=True, indexscan=True)            # default planner
    tot_d, st_d, scan_d = explain_cost(conn, table, qvec, k)
    set_paths(conn, seqscan=True, indexscan=False)           # forced seqscan
    tot_s, st_s, _ = explain_cost(conn, table, qvec, k)
    set_paths(conn, seqscan=False, indexscan=True)           # forced index
    tot_i, st_i, scan_i = explain_cost(conn, table, qvec, k)
    return dict(chosen=classify(scan_d), idx_path=classify(scan_i),
                est_total_default=round(tot_d, 1),
                seq_startup=round(st_s, 1), seq_total=round(tot_s, 1),
                idx_startup=round(st_i, 1), idx_total=round(tot_i, 1))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)        # vestigial (table carrier)
    ap.add_argument("--cell", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--stage", required=True)
    ap.add_argument("--dataset", default="cohere-1m")
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--gt-dir", required=True)
    ap.add_argument("--index-dir", default="/tmp/cuvs_indexes")
    ap.add_argument("--baseline", default="same-box")
    ap.add_argument("--reps", type=int, default=1)
    ap.add_argument("--daemon-pid", type=int, default=None)
    ap.add_argument("--instance-type", default=None)
    ap.add_argument("--price-usd-hr", default=None)
    ap.add_argument("--cost-model-version", default="unset")
    ap.add_argument("--runtime-routing-version", default="unset")
    ap.add_argument("--dbname", default="bench")
    ap.add_argument("--gpu-index", type=int, default=0)
    a = ap.parse_args()

    import numpy as np
    from anbench_common import read_fbin
    cell = runner.parse_cell(a.cell)
    n, dim, k = cell["N"], cell["dim"], cell["k"]
    q0 = np.ascontiguousarray(read_fbin(a.queries, count=1))[0]

    import psycopg
    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    conn.execute("ALTER EXTENSION pg_cuvs UPDATE")           # pull flat AM / hw cost
    conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")

    # Build the two GPU deployments (one index each — the realistic per-query choice).
    deployments = [("t_cuvs", "forced-cuvs", "cagra"),
                   ("t_flat", "forced-flat", "flat")]
    for table, cfg, suffix in deployments:
        runner.setup_table(conn, table, a.corpus, n, dim)
        if not _present(conn, f"{table}_{suffix}"):
            runner.build_index(conn, cfg, table, n, a.index_dir)
        conn.execute("ANALYZE " + table)

    hp = hw_profile(conn)
    runner.log(f"hw_profile: source={hp.get('source')} "
               f"probe_status={hp.get('probe_status')} "
               f"matches_daemon={hp.get('matches_running_daemon')}")

    common = dict(
        run_id=a.run_id, date=time.strftime("%Y-%m-%d"), stage=a.stage,
        cell_id=a.cell, config="auto", system="auto", N=n, dim=dim, k=k,
        recall_target=cell["recall_target"], dataset=a.dataset,
        warm_state="warm", clients=1, instance_type=a.instance_type,
        price_usd_hr=a.price_usd_hr, cost_model_version=a.cost_model_version,
        runtime_routing_version=a.runtime_routing_version)

    for table, cfg, suffix in deployments:
        engine = "flat" if suffix == "flat" else "cuvs"
        for phys in (True, False):                            # physical vs legacy
            conn.execute(f"SET cuvs.enable_phys_cost = {'on' if phys else 'off'}")
            r = sweep_engine(conn, table, q0, k)
            observe.write_protocol_row(
                a.csv, **common, phase="explain", index_type=suffix,
                params_json={"engine": engine, "phys_cost": phys,
                             "hw_source": hp.get("source"),
                             "hw_probe_status": hp.get("probe_status"),
                             "hw_matches_daemon": hp.get("matches_running_daemon"),
                             "dim": dim, "N": n, **r},
                notes=f"{engine} phys={phys} chose={r['chosen']} "
                      f"seq={r['seq_total']} idx={r['idx_total']}")
            runner.log(f"explain N={n} dim={dim} {engine} phys={phys} "
                       f"chose={r['chosen']} seq_total={r['seq_total']} "
                       f"idx_total={r['idx_total']} (switch when seq>idx)")
    conn.close()
    return 0


def _present(conn, name):
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass(%s)", (name,))
        return cur.fetchone()[0] is not None


def _selfcheck():
    assert classify(("Seq Scan", "")) == "seqscan"
    assert classify(("Index Scan", "t_cuvs_cagra")) == "cuvs"
    assert classify(("Index Scan", "t_flat_flat")) == "flat"
    assert classify(("Index Scan", "t_hnsw_hnsw")) == "hnsw"
    assert classify(("Custom Scan (CuvsTransientBF)", "")) == "transient-bf"
    print("SELFCHECK OK")


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
        sys.exit(0)
    main()
