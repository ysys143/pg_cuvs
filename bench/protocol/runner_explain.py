#!/usr/bin/env python3
"""runner_explain.py — Stage B EXPLAIN diagnostic (PGCUVS_MODULE=explain).

For each N, builds the cuvs index, then EXPLAINs the bound KNN query three ways:
default planner choice, forced seqscan, forced cuvs — capturing the planner's
estimated total/startup cost per path and which path it would pick by default.
This shows the cost model's cuvs↔seqscan SWITCH POINT, to compare against the
measured crossover (~20k bound). No execution (EXPLAIN only) — cheap.

Rows are written via observe.write_protocol_row with phase="explain", the cost
estimates carried in params_json (so no second CSV schema is needed).
"""
import argparse
import json
import os
import sys

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
    node = plan
    scan = ("", "")
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
    if "cagra" in (idx or ""):
        return "cuvs"
    if "hnsw" in (idx or ""):
        return "hnsw"
    return nt or "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)        # forced-cuvs (table carrier)
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
    table = "t_cuvs"
    q0 = np.ascontiguousarray(read_fbin(a.queries, count=1))[0]

    import psycopg
    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    conn.execute("ALTER EXTENSION pg_cuvs UPDATE")   # pull flat AM / hw cost
    conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")
    runner.setup_table(conn, table, a.corpus, n, dim)
    if not runner_index_present(conn, f"{table}_cagra"):
        runner.build_index(conn, "forced-cuvs", table, n, a.index_dir)
    conn.execute("ANALYZE " + table)

    def set_mode(seqscan, indexscan):
        conn.execute(f"SET enable_seqscan = {'on' if seqscan else 'off'}")
        conn.execute(f"SET enable_indexscan = {'on' if indexscan else 'off'}")
        conn.execute("SET enable_bitmapscan = off")

    # default planner choice
    set_mode(True, True)
    tot_d, st_d, scan_d = explain_cost(conn, table, q0, k)
    chosen = classify(scan_d)
    # forced seqscan
    set_mode(True, False)
    tot_s, st_s, _ = explain_cost(conn, table, q0, k)
    # forced cuvs
    set_mode(False, True)
    tot_c, st_c, scan_c = explain_cost(conn, table, q0, k)
    conn.close()

    common = dict(
        run_id=a.run_id, date=__import__("time").strftime("%Y-%m-%d"),
        stage=a.stage, cell_id=a.cell, config="auto", system="auto",
        index_type="cagra", N=n, dim=dim, k=k, recall_target=cell["recall_target"],
        dataset=a.dataset, warm_state="warm", clients=1,
        instance_type=a.instance_type, price_usd_hr=a.price_usd_hr,
        cost_model_version=a.cost_model_version,
        runtime_routing_version=a.runtime_routing_version)
    for path, st, tot in [("seqscan", st_s, tot_s), ("cuvs", st_c, tot_c)]:
        observe.write_protocol_row(
            a.csv, **common, phase="explain",
            params_json={"path": path, "est_startup": round(st, 1),
                         "est_total": round(tot, 1), "chosen": chosen, "N": n},
            notes=f"default chose {chosen}")
    runner.log(f"explain N={n} chosen={chosen} "
               f"seqscan_cost={tot_s:.0f} cuvs_cost={tot_c:.0f} "
               f"(switch when seqscan>cuvs)")
    return 0


def runner_index_present(conn, name):
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass(%s)", (name,))
        return cur.fetchone()[0] is not None


def _selfcheck():
    assert classify(("Seq Scan", "")) == "seqscan"
    assert classify(("Index Scan", "t_cuvs_cagra")) == "cuvs"
    assert classify(("Index Scan", "t_hnsw_hnsw")) == "hnsw"
    print("SELFCHECK OK")


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
        sys.exit(0)
    main()
