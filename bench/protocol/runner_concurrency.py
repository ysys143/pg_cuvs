#!/usr/bin/env python3
"""runner_concurrency.py — QPS@p99 vs concurrency (Phase 3, PGCUVS_MODULE=concurrency).

Reuses runner.py (table/index/knob/guard), then drives load with pgbench and reads
the per-transaction `--log` for p50/p95/p99 UNDER LOAD. One protocol row per
concurrency level `c ∈ CLIENTS`. Single-client latency understates the GPU's
batching edge; this is the load-bearing differentiation experiment.

Forcing + knob are pushed to every pgbench connection via PGOPTIONS (startup GUCs),
so the planner uses the engine's index at the same iso-recall operating point as the
single-client runner. Templated on bench/bf_microbatch_concurrency.sh.
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANBENCH = os.path.abspath(os.path.join(HERE, "..", "..", "infra", "anbench"))
sys.path.insert(0, ANBENCH)
import observe  # noqa: E402
import runner   # noqa: E402  reuse setup_table/build_index/choose_iso_recall/etc.

CLIENTS = [1, 8, 32, 64, 128]
KNOB_GUC = {"forced-hnsw": "hnsw.ef_search", "forced-cuvs": "cuvs.k"}


def index_exists(conn, name):
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass(%s)", (name,))
        return cur.fetchone()[0] is not None


def ensure_index(conn, config, table, n, index_dir):
    """Reuse the index from a prior Stage A run if present; else build it."""
    suffix = {"forced-hnsw": "_hnsw", "forced-cuvs": "_cagra"}.get(config)
    itype = {"forced-hnsw": "hnsw", "forced-cuvs": "cagra"}.get(config)
    name = f"{table}{suffix}"
    if index_exists(conn, name):
        runner.log(f"reuse existing index {name}")
        return name, itype
    return runner.build_index(conn, config, table, n, index_dir)[:2]


def load_query_table(conn, queries, dim):
    """Materialize the query set as a PG table so pgbench can pick a random one
    per transaction (realistic — not the same query repeatedly)."""
    from pgvector import Vector
    import pgvector.psycopg
    pgvector.psycopg.register_vector(conn)
    nq = len(queries)
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass('public.cbench_queries')")
        if cur.fetchone()[0] is not None:
            cur.execute("SELECT count(*) FROM cbench_queries")
            if cur.fetchone()[0] == nq:
                return
            cur.execute("DROP TABLE cbench_queries")
        cur.execute(f"CREATE TABLE cbench_queries (qid int primary key, v vector({dim}))")
    conn.commit()
    with conn.cursor().copy(
            "COPY cbench_queries (qid, v) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int4", "vector"])
        for i in range(nq):
            cp.write_row((i + 1, Vector(queries[i])))
    conn.commit()


def _pctl(svals, p):
    """Linear-interpolated percentile of a pre-sorted list (no numpy)."""
    if not svals:
        return float("nan")
    if len(svals) == 1:
        return svals[0]
    idx = (p / 100.0) * (len(svals) - 1)
    lo = int(idx)
    frac = idx - lo
    if lo + 1 < len(svals):
        return svals[lo] * (1 - frac) + svals[lo + 1] * frac
    return svals[lo]


def pgbench_pctls(logdir):
    """(p50,p95,p99,p999) µs + txn count from pgbench --log files.
    Log line: client_id txn_no time script_no time_epoch time_us → field 3 = latency µs."""
    lat = []
    for f in glob.glob(os.path.join(logdir, "pgb.*")):
        with open(f) as fh:
            for ln in fh:
                p = ln.split()
                if len(p) >= 3:
                    try:
                        lat.append(float(p[2]))
                    except ValueError:
                        pass
    if not lat:
        return (float("nan"),) * 4, 0
    lat.sort()
    return tuple(_pctl(lat, p) for p in (50, 95, 99, 99.9)), len(lat)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", required=True)
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
    ap.add_argument("--reps", type=int, default=1)        # accepted, unused
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
    n, dim, k, target = cell["N"], cell["dim"], cell["k"], cell["recall_target"]
    table = runner.TABLES.get(a.config)
    if table is None or a.config not in KNOB_GUC:
        raise NotImplementedError(f"concurrency config {a.config} not supported")

    gt_path = os.path.join(a.gt_dir, f"gt_runner_{n}.npy")
    if not os.path.exists(gt_path):
        sys.exit(f"[concurrency] missing GT {gt_path} — run Stage A for N={n} first")
    queries = np.ascontiguousarray(read_fbin(a.queries))
    gt = np.load(gt_path)
    nq = len(queries)

    import psycopg
    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    if a.config == "forced-cuvs":
        conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    conn.execute("SET enable_seqscan = off")
    conn.execute("SET enable_bitmapscan = off")
    if a.config == "forced-cuvs":
        conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")

    runner.setup_table(conn, table, a.corpus, n, dim)
    name, index_type = ensure_index(conn, a.config, table, n, a.index_dir)
    if a.config == "forced-cuvs":
        ok, plan = runner.explain_uses_index(conn, table,
                                             runner.vec_literal(queries[0]), "cagra")
        if not ok:
            sys.exit(f"[concurrency] FAIL: cuvs seq-scan fallback. plan:\n{plan}")
    set_sql, knob, _, met = runner.choose_iso_recall(
        conn, table, queries[:2000], gt[:2000], k, target, a.config, a.index_dir)
    load_query_table(conn, queries, dim)
    conn.close()

    # startup GUCs for every pgbench connection (force the index + iso-recall knob)
    opts = ["-c", "enable_seqscan=off", "-c", "enable_bitmapscan=off",
            "-c", f"{KNOB_GUC[a.config]}={knob}"]
    if a.config == "forced-cuvs":
        opts += ["-c", f"cuvs.index_dir={a.index_dir}"]
    pgoptions = " ".join(opts)
    qsql = (f"\\set qid random(1, {nq})\n"
            f"SELECT id FROM {table} ORDER BY embedding <-> "
            f"(SELECT v FROM cbench_queries WHERE qid = :qid) LIMIT {k};\n")
    dur = int(os.environ.get("PGCUVS_PGBENCH_SECONDS", "25"))

    common = dict(
        run_id=a.run_id, date=time.strftime("%Y-%m-%d"), stage=a.stage,
        cell_id=a.cell, config=a.config, system=a.config, index_type=index_type,
        N=n, dim=dim, k=k, recall_target=target, dataset=a.dataset,
        query_set_id=os.path.basename(a.queries), warm_state="warm",
        gt_method="exact-fbin", instance_type=a.instance_type,
        price_usd_hr=a.price_usd_hr, cost_model_version=a.cost_model_version,
        runtime_routing_version=a.runtime_routing_version)

    for c in CLIENTS:
        td = tempfile.mkdtemp(prefix="pgb_")
        sqlf = os.path.join(td, "q.sql")
        with open(sqlf, "w") as f:
            f.write(qsql)
        j = min(c, os.cpu_count() or 4)
        cmd = ["pgbench", "-n", "-c", str(c), "-j", str(j), "-T", str(dur),
               "-f", sqlf, "--log", f"--log-prefix={td}/pgb", a.dbname]
        env = dict(os.environ, PGOPTIONS=pgoptions)
        with observe.ResourceSampler(gpu_index=a.gpu_index,
                                     daemon_pid=a.daemon_pid) as s:
            out = subprocess.run(cmd, env=env, capture_output=True, text=True)
        tps = float("nan")
        for ln in out.stdout.splitlines():
            if ln.strip().startswith("tps ="):
                try:
                    tps = float(ln.split("=", 1)[1].split("(")[0].strip())
                except ValueError:
                    pass
        (p50, p95, p99, p999), ntx = pgbench_pctls(td)
        observe.write_protocol_row(
            a.csv, **common, phase="query", clients=c,
            qps=round(tps, 1), p50_us=round(p50, 1), p95_us=round(p95, 1),
            p99_us=round(p99, 1), p999_us=round(p999, 1),
            reps=1, agg_method=f"pgbench-{dur}s",
            params_json={"knob": knob, "iso_recall_met": met,
                         "pgbench_txns": ntx, "clients": c},
            notes="" if out.returncode == 0 else f"pgbench rc={out.returncode}",
            **s.as_dict())
        runner.log(f"conc {a.config} N={n} c={c} tps={tps:.0f} "
                   f"p99={p99:.0f}us txns={ntx} rc={out.returncode}")
    return 0


def _selfcheck():
    import tempfile
    td = tempfile.mkdtemp()
    # synthetic pgbench log: latency µs in field 3
    with open(os.path.join(td, "pgb.123"), "w") as f:
        for i in range(1000):
            f.write(f"0 {i} {1000 + i} 0 1700000000 0\n")
    (p50, p95, p99, p999), n = pgbench_pctls(td)
    assert n == 1000 and 1490 < p50 < 1510, (p50, n)
    assert KNOB_GUC["forced-cuvs"] == "cuvs.k"
    print("SELFCHECK OK")


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
        sys.exit(0)
    main()
