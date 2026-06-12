#!/usr/bin/env python3
"""runner.py — protocol per-engine runner (one cell × one config).

Sets up the per-engine table, builds the index (phase=build), runs an
iso-recall knob sweep, and measures the chosen operating point (phase=query).
Each phase is wrapped in observe.ResourceSampler and written as one CONTRACT §6
row via observe.write_protocol_row.

CRITICAL (issue #56): pg_cuvs resolves its artifact dir from the `cuvs.index_dir`
GUC. If it does not match the daemon's --index-dir, CREATE INDEX / search
silently fall back to Seq Scan (no GPU, no error) -> catastrophically wrong
latency. So forced-cuvs / auto BOTH `SET cuvs.index_dir` AND assert the query
plan uses the cagra index before any latency row is trusted.

Implemented configs: forced-hnsw, forced-cuvs, forced-seqscan.
(auto / forced-bf are follow-ups — they raise loudly rather than mis-measure.)
"""
import argparse
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANBENCH = os.path.abspath(os.path.join(HERE, "..", "..", "infra", "anbench"))
sys.path.insert(0, ANBENCH)
import observe  # noqa: E402  (stdlib-only; safe to import without numpy)

# numpy + anbench_common are imported lazily inside the functions that need them
# so --selfcheck (pure helpers) runs on a no-numpy box.

CELL_RE = re.compile(r"N(\d+)([kmg]?)_d(\d+)_k(\d+)_r([0-9.]+)$")
MULT = {"": 1, "k": 1000, "m": 1_000_000, "g": 1_000_000_000}

# iso-recall knob sweeps (ascending → pick the minimum value meeting target)
HNSW_EF = [10, 20, 40, 80, 120, 200, 400]
CUVS_K = [50, 100, 200, 400, 800]

TABLES = {
    "forced-hnsw": "t_hnsw",
    "forced-cuvs": "t_cuvs",
    "forced-seqscan": "t_seq",
}


def log(m):
    print(f"[runner] {m}", flush=True)


def parse_cell(cell_id):
    m = CELL_RE.match(cell_id)
    if not m:
        raise ValueError(f"bad cell_id: {cell_id}")
    num, suf, d, k, r = m.groups()
    return dict(N=int(num) * MULT[suf], dim=int(d), k=int(k),
                recall_target=float(r))


# ── table setup (per-engine table; idempotent reuse if N rows already loaded) ─

def setup_table(conn, table, corpus, n, dim, batch=50000):
    import numpy as np
    from anbench_common import read_fbin
    import pgvector.psycopg
    pgvector.psycopg.register_vector(conn)
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass(%s)", (f"public.{table}",))
        if cur.fetchone()[0] is not None:
            cur.execute(f"SELECT count(*) FROM {table}")
            if cur.fetchone()[0] == n:
                log(f"{table} already has {n} rows; reuse")
                return
            cur.execute(f"DROP TABLE {table}")
        cur.execute(f"CREATE TABLE {table} (id bigint, embedding vector({dim}))")
    conn.commit()
    log(f"COPY {n} rows into {table}")
    from pgvector import Vector
    with conn.cursor().copy(
            f"COPY {table} (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int8", "vector"])
        for s in range(0, n, batch):
            e = min(s + batch, n)
            chunk = np.ascontiguousarray(read_fbin(corpus, count=e - s, offset=s))
            for i in range(e - s):
                cp.write_row((s + i, Vector(chunk[i])))
    conn.commit()


# ── plan guard (issue #56): cuvs must NOT seq-scan ───────────────────────────

def explain_uses_index(conn, table, qvec, index_substr):
    from pgvector import Vector
    with conn.cursor() as cur:
        cur.execute(f"EXPLAIN SELECT id FROM {table} ORDER BY embedding <-> %s "
                    f"LIMIT 10", (Vector(qvec),))
        plan = "\n".join(r[0] for r in cur.fetchall())
    return (index_substr in plan) and ("Seq Scan" not in plan), plan


# ── build phase ──────────────────────────────────────────────────────────────

def build_index(conn, config, table, n, index_dir):
    with conn.cursor() as cur:
        cur.execute("SET maintenance_work_mem='16GB'")
        cur.execute("SET max_parallel_maintenance_workers=7")
        if config == "forced-hnsw":
            name = f"{table}_hnsw"
            sql = (f"CREATE INDEX {name} ON {table} USING hnsw "
                   f"(embedding vector_l2_ops) WITH (m=16, ef_construction=64)")
            index_type = "hnsw"
        elif config == "forced-cuvs":
            cur.execute(f"SET cuvs.index_dir = '{index_dir}'")  # CRITICAL
            name = f"{table}_cagra"
            sql = (f"CREATE INDEX {name} ON {table} USING cagra "
                   f"(embedding vector_l2_ops)")
            index_type = "cagra"
        elif config == "forced-seqscan":
            return None, "none", 0.0, 0
        else:
            raise NotImplementedError(f"config {config} not implemented yet")
        cur.execute(f"DROP INDEX IF EXISTS {name}")
        conn.commit()
        t0 = time.perf_counter()
        cur.execute(sql)
        conn.commit()
        bt = time.perf_counter() - t0
        cur.execute("SELECT pg_relation_size(%s)", (name,))
        idx_bytes = cur.fetchone()[0]
    log(f"built {name} in {bt:.1f}s host_size={idx_bytes/1e6:.0f}MB")
    return name, index_type, bt, idx_bytes


# ── query phase ──────────────────────────────────────────────────────────────

def vec_literal(v):
    return "[" + ",".join(repr(float(x)) for x in v.tolist()) + "]"


def run_queries(conn, table, queries, kmax, set_sql, index_dir, config,
                warmup=200, lat_cap=2000):
    import numpy as np
    from pgvector import Vector
    # Bind the query vector as a parameter (NOT an inline literal). Inlining a
    # 1024-d vector as SQL text costs ~1ms of parse per query — a large fraction
    # of CAGRA's fast path — and is not how apps query. Binding + psycopg's
    # auto-prepare (after warmup) is the realistic steady state and isolates the
    # engine cost. The cuvs/hnsw index AMs both support a parameterized order-by.
    sql = f"SELECT id FROM {table} ORDER BY embedding <-> %s LIMIT {kmax}"
    with conn.cursor() as cur:
        # plan-forcing GUCs are session-level (set once in main); here we only
        # apply the per-sweep knob.
        if set_sql:
            cur.execute(set_sql)
        nq = len(queries)

        def one(i):
            cur.execute(sql, (Vector(queries[i]),))
            return cur.fetchall()

        for i in range(min(warmup, nq)):
            one(i)
        ids = np.full((nq, kmax), -1, dtype=np.int64)
        lat = []
        t0 = time.perf_counter()
        for i in range(nq):
            t1 = time.perf_counter()
            rows = one(i)
            if i < lat_cap:
                lat.append(time.perf_counter() - t1)
            for j, r in enumerate(rows):
                ids[i, j] = r[0]
        total = time.perf_counter() - t0
    return ids, nq / total, lat   # lat = per-query latencies (seconds)


def _pctls_us(lat):
    """(p50, p95, p99, p999) in microseconds from a latency list (seconds)."""
    import numpy as np
    a = np.asarray(lat, dtype=np.float64) * 1e6
    if a.size == 0:
        return (float("nan"),) * 4
    return tuple(float(np.percentile(a, p)) for p in (50, 95, 99, 99.9))


def _agg_reps(per_rep_us, per_rep_qps):
    """Median across reps for each percentile + qps; dispersion = per-metric
    [min,max] across reps (JSON). per_rep_us: list of (p50,p95,p99,p999) µs."""
    import json
    import numpy as np
    arr = np.asarray(per_rep_us, dtype=np.float64)        # (reps, 4)
    med = [float(np.median(arr[:, i])) for i in range(4)]
    qps = float(np.median(per_rep_qps))
    disp = {"reps": len(per_rep_qps),
            "p50_us": [round(float(arr[:, 0].min()), 1), round(float(arr[:, 0].max()), 1)],
            "p99_us": [round(float(arr[:, 2].min()), 1), round(float(arr[:, 2].max()), 1)],
            "qps": [round(min(per_rep_qps), 1), round(max(per_rep_qps), 1)]}
    return med, qps, json.dumps(disp, separators=(",", ":"))


def knob_sweep(config):
    if config == "forced-hnsw":
        return [(f"SET hnsw.ef_search={v}", v) for v in HNSW_EF]
    if config == "forced-cuvs":
        return [(f"SET cuvs.k={v}", v) for v in CUVS_K]
    if config == "forced-seqscan":
        return [(None, None)]  # exact, single point
    raise NotImplementedError(config)


def choose_iso_recall(conn, table, queries, gt, k, target, config, index_dir):
    """Sweep the knob; return (chosen_set_sql, knob, achieved_recall, ids, met)
    for the MINIMUM knob meeting target (else the max knob, met=False)."""
    from anbench_common import recall_at_k
    best = None
    for set_sql, knob in knob_sweep(config):
        ids, _, _ = run_queries(conn, table, queries, k, set_sql, index_dir,
                                config, lat_cap=0)
        rec = recall_at_k(ids[:, :k], gt[:, :k], k)
        log(f"  sweep {config} knob={knob} recall@{k}={rec:.4f}")
        if rec >= target:
            return set_sql, knob, rec, True
        best = (set_sql, knob, rec)
    return best[0], best[1], best[2], False


# ── main ──────────────────────────────────────────────────────────────────────

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
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--daemon-pid", type=int, default=None)
    ap.add_argument("--instance-type", default=None)
    ap.add_argument("--price-usd-hr", default=None)
    ap.add_argument("--cost-model-version", default="unset")
    ap.add_argument("--runtime-routing-version", default="unset")
    ap.add_argument("--dbname", default="bench")
    ap.add_argument("--gpu-index", type=int, default=0)
    a = ap.parse_args()

    import numpy as np
    from anbench_common import read_fbin, recall_at_k
    cell = parse_cell(a.cell)
    n, dim, k, target = cell["N"], cell["dim"], cell["k"], cell["recall_target"]
    table = TABLES.get(a.config)
    if table is None:
        raise NotImplementedError(f"config {a.config} not implemented")

    # Build a RUNNER-OWNED GT keyed to (corpus, queries, N) rather than trusting the
    # pre-built gt_<N>.npy of uncertain provenance — those gave random recall (=k/N)
    # at N=100k because their neighbor ids did not match the first-N corpus id space
    # this runner loads (issue #56). build_gt.py is the same GPU brute force that
    # produced the proven §16 GT, so a runner-built GT is exact by construction.
    gt_path = os.path.join(a.gt_dir, f"gt_runner_{n}.npy")
    if not os.path.exists(gt_path):
        log(f"GT {gt_path} missing — building via build_gt.py (N={n}, k=100)")
        import subprocess
        r = subprocess.run([sys.executable, os.path.join(ANBENCH, "build_gt.py"),
                            "--corpus", a.corpus, "--queries", a.queries,
                            "--n", str(n), "--k", "100", "--out", gt_path])
        if r.returncode != 0 or not os.path.exists(gt_path):
            sys.exit(f"[runner] GT build failed for N={n} ({gt_path})")

    import psycopg
    queries = np.ascontiguousarray(read_fbin(a.queries))
    gt = np.load(gt_path)
    dpid = a.daemon_pid

    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    if a.config in ("forced-cuvs",):
        conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")

    # Session-level plan forcing. A 'forced' config measures THAT engine's path
    # even where the planner would pick another at small N (e.g. at N=1k the cuvs
    # startup cost > a 1k-row seq scan, so without this the planner seq-scans and
    # the cuvs guard fails). Stage B measures the planner's actual choice separately.
    if a.config in ("forced-hnsw", "forced-cuvs"):
        conn.execute("SET enable_seqscan = off")
        conn.execute("SET enable_bitmapscan = off")
    if a.config == "forced-cuvs":
        conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")
    if a.config == "forced-seqscan":
        conn.execute("SET enable_cuvs = off")
        conn.execute("SET enable_indexscan = off")
        conn.execute("SET enable_bitmapscan = off")

    setup_table(conn, table, a.corpus, n, dim)

    def common_row(**extra):
        base = dict(
            run_id=a.run_id, date=time.strftime("%Y-%m-%d"), stage=a.stage,
            cell_id=a.cell, config=a.config, system=a.config,
            N=n, dim=dim, k=k, recall_target=target, dataset=a.dataset,
            query_set_id=os.path.basename(a.queries), clients=1, warm_state="warm",
            reps=a.reps, agg_method=f"median-of-{a.reps}", gt_method="exact-fbin",
            instance_type=a.instance_type, price_usd_hr=a.price_usd_hr,
            cost_model_version=a.cost_model_version,
            runtime_routing_version=a.runtime_routing_version,
        )
        base.update(extra)
        return base

    # ── build phase (resource-sampled) ───────────────────────────────────────
    before_lsn = observe.wal_lsn(conn)
    with observe.ResourceSampler(gpu_index=a.gpu_index, daemon_pid=dpid) as s:
        name, index_type, bt, idx_host = build_index(conn, a.config, table, n,
                                                      a.index_dir)
    wal_b = observe.wal_delta(conn, before_lsn)
    sizes = observe.index_sizes(conn, name) if name else {
        "index_bytes_host": 0, "index_bytes_disk": 0, "index_bytes_vram": None}
    observe.write_protocol_row(
        a.csv, **common_row(phase="build", index_type=index_type,
                            build_s=round(bt, 3), wal_bytes=wal_b,
                            params_json={"build": "m16_ef64" if index_type == "hnsw"
                                         else index_type},
                            **sizes, **s.as_dict()))

    # ── plan guard for cuvs (issue #56) ──────────────────────────────────────
    if a.config == "forced-cuvs":
        conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")
        ok, plan = explain_uses_index(conn, table, queries[0], "cagra")
        if not ok:
            sys.exit(f"[runner] FAIL: cuvs plan is not using the cagra index "
                     f"(seq-scan fallback — check cuvs.index_dir / daemon). plan:\n{plan}")
        log("plan guard OK — cagra index in use")

    # ── iso-recall knob selection (cheap: recall-only sweep on a 2k subset) ───
    qcap_sweep = min(2000, len(queries))
    set_sql, knob, _, met = choose_iso_recall(conn, table, queries[:qcap_sweep],
                                              gt[:qcap_sweep], k, target,
                                              a.config, a.index_dir)
    # ── measured query point: reps × full query set → median + dispersion ────
    qcap = min(10000, len(queries))
    qfin, gtfin = queries[:qcap], gt[:qcap]
    per_rep, per_qps, last_ids = [], [], None
    with observe.ResourceSampler(gpu_index=a.gpu_index, daemon_pid=dpid) as s:
        for _ in range(max(1, a.reps)):
            ids, qps, lat = run_queries(conn, table, qfin, k, set_sql,
                                        a.index_dir, a.config, lat_cap=qcap)
            per_rep.append(_pctls_us(lat))
            per_qps.append(qps)
            last_ids = ids
    (p50, p95, p99, p999), qps, disp = _agg_reps(per_rep, per_qps)
    rec = recall_at_k(last_ids[:, :k], gtfin[:, :k], k)
    observe.write_protocol_row(
        a.csv, **common_row(
            phase="query", index_type=index_type,
            qps=round(qps, 1), p50_us=round(p50, 1), p95_us=round(p95, 1),
            p99_us=round(p99, 1), p999_us=round(p999, 1),
            recall_at_k=round(rec, 4), dispersion=disp,
            params_json={"knob": knob, "iso_recall_met": met, "n_queries": qcap},
            notes="" if met else "recall target NOT met at sweep ceiling",
            **s.as_dict()))
    conn.close()
    log(f"done {a.cell} {a.config}: recall={rec:.4f} qps={qps:.0f} "
        f"p50={p50:.0f}us p99={p99:.0f}us reps={a.reps}")


def _selfcheck():
    assert parse_cell("N1k_d1024_k10_r0.95") == dict(
        N=1000, dim=1024, k=10, recall_target=0.95)
    assert parse_cell("N1m_d1024_k100_r0.99")["N"] == 1_000_000
    assert [kn for _, kn in knob_sweep("forced-hnsw")] == HNSW_EF
    assert knob_sweep("forced-seqscan") == [(None, None)]
    print("SELFCHECK OK")


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        _selfcheck()
        sys.exit(0)
    main()
