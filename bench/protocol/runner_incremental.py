#!/usr/bin/env python3
"""runner_incremental.py — D3 incremental ingest (PGCUVS_MODULE=incremental).

Scope v1: the headline ADR-074 write claim, measured — append ingest throughput
+ per-row INSERT latency for the two write regimes:
  - forced-flat     (W2, read-heavy): each INSERT triggers the GPU index path
                    (cuvsCagraExtend), ADR-074 ~1.77ms/row, HOT-disabled.
  - forced-noindex  (W1, write-heavy = pgvector no-index): plain heap insert.
Frame: write-heavy → no-index, read-heavy → flat. The crossover is the result.

Emits one `phase=maint` row per config: qps = rows/s, p50/p95/p99 = per-row
INSERT latency (µs), dispersion = per-rep spread, params_json.ops_done = rows
appended. VRAM growth from the ResourceSampler. FIFO/upsert/recall-drift/
concurrent-query-during-ingest are follow-ups (noted in HANDOFF).
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANBENCH = os.path.abspath(os.path.join(HERE, "..", "..", "infra", "anbench"))
sys.path.insert(0, ANBENCH)
import observe  # noqa: E402
import runner   # noqa: E402

TABLES = {"forced-flat": "t_inc_flat", "forced-noindex": "t_inc_noidx"}


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
    n, dim, k, target = cell["N"], cell["dim"], cell["k"], cell["recall_target"]
    table = TABLES.get(a.config)
    if table is None:
        raise NotImplementedError(f"incremental config {a.config} not supported")
    is_flat = a.config == "forced-flat"

    # base = first n_base rows; append the next n_app rows one at a time.
    n_app = min(int(os.environ.get("PGCUVS_INC_APPEND", "2000")), n // 2)
    n_base = n - n_app
    corpus = np.ascontiguousarray(read_fbin(a.corpus, count=n))
    queries = np.ascontiguousarray(read_fbin(a.queries))   # for recall-drift

    import psycopg
    from pgvector.psycopg import register_vector
    from pgvector import Vector
    conn = psycopg.connect(dbname=a.dbname, autocommit=True)
    conn.execute("CREATE EXTENSION IF NOT EXISTS vector")
    register_vector(conn)
    if is_flat:
        conn.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
        conn.execute("ALTER EXTENSION pg_cuvs UPDATE")
        conn.execute(f"SET cuvs.index_dir = '{a.index_dir}'")

    cur = conn.cursor()
    cur.execute(f"DROP TABLE IF EXISTS {table} CASCADE")
    # PK on id so fifo/upsert DELETE/UPDATE WHERE id=... use the btree, not a
    # seqscan (a scalar PK is normal for any table; it is NOT a vector index, so
    # the W1 'no vector index' regime still holds).
    cur.execute(f"CREATE TABLE {table} (id bigint PRIMARY KEY, embedding vector({dim}))")
    # load base via COPY
    with conn.cursor().copy(
            f"COPY {table} (id, embedding) FROM STDIN WITH (FORMAT BINARY)") as cp:
        cp.set_types(["int8", "vector"])
        for i in range(n_base):
            cp.write_row((i, Vector(corpus[i])))
    if is_flat:                                   # W2: resident exact GPU index
        cur.execute(f"CREATE INDEX {table}_flat ON {table} USING flat "
                    f"(embedding vector_l2_ops)")
        ok, plan = runner.explain_uses_index(conn, table, corpus[0], "flat")
        if not ok:
            sys.exit(f"[incremental] FAIL: flat not used. plan:\n{plan}")
    scenario = os.environ.get("PGCUVS_INC_SCENARIO", "append")
    runner.log(f"base {n_base} rows loaded ({a.config}); scenario={scenario} ops={n_app}")

    # ── streaming phase: one op per step, timed ──────────────────────────────
    # append : INSERT new row
    # fifo   : INSERT new (head) + DELETE oldest (tail)  → window stays n_base
    # upsert : alternate INSERT new / UPDATE a random existing row
    import random as _rnd
    _rnd.seed(0)
    lat, ops = [], 0
    live = set(range(n_base))                        # current id set (for recall-drift)
    with observe.ResourceSampler(gpu_index=a.gpu_index, daemon_pid=a.daemon_pid) as s:
        t0 = time.perf_counter()
        for j in range(n_app):
            newid = n_base + j
            v = Vector(corpus[newid % n])
            t = time.perf_counter()
            if scenario == "append":
                cur.execute(f"INSERT INTO {table} (id, embedding) VALUES (%s, %s)",
                            (newid, v))
                live.add(newid)
            elif scenario == "fifo":
                cur.execute(f"INSERT INTO {table} (id, embedding) VALUES (%s, %s)",
                            (newid, v))
                cur.execute(f"DELETE FROM {table} WHERE id = %s", (j,))   # tail
                live.add(newid); live.discard(j)
            elif scenario == "upsert":
                if j % 2 == 0:
                    cur.execute(f"INSERT INTO {table} (id, embedding) VALUES (%s, %s)",
                                (newid, v))
                    live.add(newid)
                else:
                    cur.execute(f"UPDATE {table} SET embedding = %s WHERE id = %s",
                                (v, _rnd.randrange(n_base)))
            else:
                sys.exit(f"[incremental] unknown scenario {scenario}")
            lat.append((time.perf_counter() - t) * 1e6)     # µs
            ops += 1
        total = time.perf_counter() - t0
    n_app = ops                                              # ops actually done
    rows_per_s = ops / total if total > 0 else float("nan")
    a_us = np.asarray(lat)
    p50, p95, p99, p999 = (float(np.percentile(a_us, p)) for p in (50, 95, 99, 99.9))
    disp = {"reps": 1, "per_row_us": [round(float(a_us.min()), 1),
                                      round(float(a_us.max()), 1)],
            "total_s": round(total, 2)}

    observe.write_protocol_row(
        a.csv, run_id=a.run_id, date=time.strftime("%Y-%m-%d"), stage=a.stage,
        phase="maint", cell_id=a.cell, config=a.config, system=a.config,
        index_type=("flat" if is_flat else "none"), N=n, dim=dim, k=k,
        recall_target=target, dataset=a.dataset,
        query_set_id=os.path.basename(a.queries), clients=1, warm_state="warm",
        qps=round(rows_per_s, 1), p50_us=round(p50, 1), p95_us=round(p95, 1),
        p99_us=round(p99, 1), p999_us=round(p999, 1),
        reps=1, agg_method="per-op", dispersion=disp, gt_method="n/a",
        stream_op=scenario, ops_done=ops, delta_rows=ops,
        instance_type=a.instance_type, price_usd_hr=a.price_usd_hr,
        cost_model_version=a.cost_model_version,
        runtime_routing_version=a.runtime_routing_version,
        params_json={"n_base": n_base, "scenario": scenario,
                     "regime": "W2-read-heavy" if is_flat else "W1-write-heavy",
                     "ops_per_s": round(rows_per_s, 1)},
        notes=f"{scenario} {ops} ops: {rows_per_s:.0f} ops/s, p99={p99/1000:.2f}ms/op",
        **s.as_dict())

    # ── recall-drift: does the index stay correct after the mutations? ───────
    # append/fifo have a deterministic current embedding (corpus[id % n]); upsert
    # remaps embeddings non-deterministically → skip. Exact GT over the LIVE id
    # set vs the index result: high recall ⇒ inserts searchable + deletes (fifo)
    # excluded. no-index = seqscan = exact (1.0 baseline confirming the GT).
    if scenario in ("append", "fifo"):
        try:
            ids = np.array(sorted(live), dtype=np.int64)
            emb = corpus[ids % n]
            nqd = min(100, len(queries))
            drift, leaked = [], 0
            for qi in range(nqd):
                d = ((emb - queries[qi]) ** 2).sum(1)
                gtset = set(ids[np.argsort(d)[:k]].tolist())
                cur.execute(
                    f"SELECT id FROM {table} ORDER BY embedding <-> %s LIMIT {k}",
                    (Vector(queries[qi]),))
                got = [int(r[0]) for r in cur.fetchall()]
                leaked += sum(1 for g in got if g not in live)   # deleted/absent
                drift.append(len(set(got) & gtset) / k)
            rdrift = float(np.mean(drift))
            runner.log(f"recall-drift {scenario} {a.config} = {rdrift:.4f} over "
                       f"{len(live)} live rows, leaked={leaked}")
            observe.write_protocol_row(
                a.csv, run_id=a.run_id, date=time.strftime("%Y-%m-%d"),
                stage=a.stage, phase="query", cell_id=a.cell, config=a.config,
                system=a.config, index_type=("flat" if is_flat else "none"),
                N=len(live), dim=dim, k=k, recall_target=target, dataset=a.dataset,
                query_set_id=os.path.basename(a.queries), clients=1,
                warm_state="warm", recall_at_k=round(rdrift, 4), reps=1,
                agg_method="recall-drift", gt_method="exact-live",
                stream_op=scenario, ops_done=ops,
                instance_type=a.instance_type, price_usd_hr=a.price_usd_hr,
                cost_model_version=a.cost_model_version,
                runtime_routing_version=a.runtime_routing_version,
                params_json={"scenario": scenario, "live_rows": len(live),
                             "leaked_ids": leaked, "n_queries": nqd},
                notes=f"recall-drift after {scenario}: {rdrift:.4f}, leaked={leaked}")
        except Exception as e:                            # best-effort; never fail
            runner.log(f"recall-drift {a.config} skipped: {e!r}")

    cur.execute(f"DROP TABLE {table} CASCADE")
    conn.close()
    runner.log(f"incremental {a.config} N={n}: {rows_per_s:.0f} rows/s "
               f"p50={p50:.0f}us p99={p99:.0f}us")
    return 0


if __name__ == "__main__":
    main()
