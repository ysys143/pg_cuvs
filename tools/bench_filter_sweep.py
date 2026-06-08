#!/usr/bin/env python3
"""
bench_filter_sweep.py — D-wedge post-filter selectivity × correlation benchmark.

Measures GPU-side latency and result-count for cuvs_filtered_knn() across:
  selectivity : fraction of N rows in the filter (1%, 5%, 10%, 25%, 50%)
  correlation : how well filter rows cluster near the query
                0.0 = random sample  (worst case for post-filter recall)
                0.5 = half spatial   (mixed)
                1.0 = spatially closest rows (best case for recall)

Output: TSV to stdout, captured into docs/filter-threshold-experiment.md.

Usage (on VM, inside cuvs_dev conda env):
  psql -U postgres -d contrib_regression -c "CREATE EXTENSION IF NOT EXISTS pg_cuvs"
  python3 tools/bench_filter_sweep.py | tee /tmp/sweep_results.tsv
"""
import subprocess, sys, io
import numpy as np

# ── Config ──────────────────────────────────────────────────────────────────
N          = 200_000
DIM        = 128
K          = 10
OVERFETCH  = 4        # D's current cuvs.k = K * OVERFETCH
REPS       = 5        # median over this many queries per cell
DB         = "contrib_regression"
SELS       = [0.01, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.50]
CORRS      = [0.0, 0.5, 1.0]
RNG_SEED   = 42

QUERY_VEC  = np.full(DIM, 0.5, dtype=np.float32)

# ── psql helpers ─────────────────────────────────────────────────────────────
PREAMBLE = """
SET cuvs.index_dir = '/tmp/cuvs_indexes';
SET cuvs.search_mode = brute_force;
SET cuvs.k = %d;
""" % (K * OVERFETCH)

def psql(sql, check=True, with_preamble=False):
    full = (PREAMBLE + sql) if with_preamble else sql
    r = subprocess.run(
        ["sudo", "-u", "postgres", "psql", "-d", DB, "-t", "-A", "-q", "-F", "\t"],
        input=full, capture_output=True, text=True
    )
    if check and r.returncode != 0:
        sys.exit(f"psql error:\n{r.stderr}\nSQL: {sql[:200]}")
    return r.stdout.strip()

def psql_copy_stdin(copy_sql, data_bytes):
    proc = subprocess.Popen(
        ["sudo", "-u", "postgres", "psql", "-d", DB, "-c", copy_sql],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    _, err = proc.communicate(data_bytes)
    if proc.returncode != 0:
        sys.exit(f"COPY error: {err.decode()}")

# ── Setup ────────────────────────────────────────────────────────────────────
print("=== filter threshold sweep  N=%d  dim=%d  k=%d  overfetch=%d ===" %
      (N, DIM, K, OVERFETCH), file=sys.stderr)

exists = psql("SELECT 1 FROM pg_class WHERE relname='_bench_sweep'")
if exists:
    print("reusing existing _bench_sweep table (rebuilding index with brute_force GUC)...",
          file=sys.stderr)
    rng = np.random.default_rng(RNG_SEED)
    vecs = rng.random((N, DIM), dtype=np.float64).astype(np.float32)
    psql("DROP INDEX IF EXISTS _bench_sweep_idx")
    psql("CREATE INDEX _bench_sweep_idx ON _bench_sweep USING cagra (v vector_l2_ops)",
         with_preamble=True)
    print("index rebuilt", file=sys.stderr)
else:
    psql("""
    CREATE UNLOGGED TABLE _bench_sweep (
        rid  int  NOT NULL,
        v    vector(%d) NOT NULL
    )""" % DIM)

    # ── Generate + load vectors ───────────────────────────────────────────────
    print("generating %d x %d vectors..." % (N, DIM), file=sys.stderr)
    rng  = np.random.default_rng(RNG_SEED)
    vecs = rng.random((N, DIM), dtype=np.float64).astype(np.float32)

    print("building COPY buffer...", file=sys.stderr)
    buf = io.BytesIO()
    for i in range(N):
        row = ("%d\t[%s]\n" % (i, ",".join("%.4f" % x for x in vecs[i]))).encode()
        buf.write(row)
    data = buf.getvalue()

    print("loading via COPY (%d bytes)..." % len(data), file=sys.stderr)
    psql_copy_stdin("COPY _bench_sweep (rid, v) FROM STDIN", data)
    cnt = psql("SELECT count(*) FROM _bench_sweep")
    print("loaded %s rows" % cnt, file=sys.stderr)

    # ── Build index ───────────────────────────────────────────────────────────
    print("building brute_force CAGRA index...", file=sys.stderr)
    psql("CREATE INDEX _bench_sweep_idx ON _bench_sweep USING cagra (v vector_l2_ops)",
         with_preamble=True)
    print("index built", file=sys.stderr)

# ── Fetch ctids ────────────────────────────────────────────────────────────────
print("fetching ctids...", file=sys.stderr)
rows = psql("SELECT rid, ctid FROM _bench_sweep ORDER BY rid").split("\n")
rid_to_ctid = {}
for row in rows:
    if "\t" not in row:
        continue
    rid_s, ctid_s = row.split("\t")
    rid_to_ctid[int(rid_s)] = ctid_s

def ctid_to_bigint(ctid):
    s = ctid.strip("()")
    blk, off = s.split(",")
    return (int(blk) << 16) | int(off)

all_rids   = np.arange(N)
all_bigints = np.array([ctid_to_bigint(rid_to_ctid[r]) for r in all_rids], dtype=np.int64)

# Spatial ordering by L2 distance to QUERY_VEC
dists = np.sum((vecs - QUERY_VEC) ** 2, axis=1)
spatial_order = np.argsort(dists)   # ascending: closest first

def make_filter_bigints(sel, corr):
    n_f = max(K, int(N * sel))
    if corr == 0.0:
        chosen = rng.choice(N, n_f, replace=False)
    elif corr == 1.0:
        chosen = spatial_order[:n_f]
    else:
        n_sp  = int(n_f * corr)
        n_rnd = n_f - n_sp
        sp    = spatial_order[:n_sp]
        pool  = np.setdiff1d(all_rids, sp)
        rd    = rng.choice(pool, n_rnd, replace=False)
        chosen = np.concatenate([sp, rd])
    return np.sort(all_bigints[chosen])

def fmt_bigint_array(bints):
    return "ARRAY[%s]::bigint[]" % ",".join(str(b) for b in bints)

query_lit = "[%s]" % ",".join("%.4f" % x for x in QUERY_VEC)

# ── Benchmark sweep ───────────────────────────────────────────────────────────
HDR = "selectivity\tcorrelation\tn_filter\tn_results\trecall\tmed_ms\tmin_ms\tmax_ms"
print("\n" + HDR, file=sys.stderr)
print(HDR)

def run_query(arr_sql):
    """Run filtered kNN REPS times; return (last_n, latencies_list)."""
    latencies = []
    last_n    = 0
    for _ in range(REPS):
        sql = """
WITH t0 AS (SELECT clock_timestamp() AS ts)
SELECT (SELECT count(*) FROM cuvs_filtered_knn(
         '_bench_sweep_idx'::regclass,
         '%s'::vector(%d),
         %s,
         %d
     )) AS n,
     extract(epoch from clock_timestamp() - t0.ts)*1000 AS ms
FROM t0""" % (query_lit, DIM, arr_sql, K)
        out = psql(sql, with_preamble=True)
        # last non-empty line that contains a tab is the result row
        lines = [l for l in out.splitlines() if "\t" in l]
        if lines:
            n_s, ms_s = lines[-1].split("\t", 1)
            last_n = int(n_s)
            latencies.append(float(ms_s))
    return last_n, latencies

def emit(sel_label, corr_label, n_filter, last_n, latencies):
    if not latencies:
        return
    med = float(np.median(latencies))
    mn  = float(np.min(latencies))
    mx  = float(np.max(latencies))
    recall = last_n / K
    line = "%s\t%s\t%d\t%d\t%.2f\t%.2f\t%.2f\t%.2f" % (
        sel_label, corr_label, n_filter, last_n, recall, med, mn, mx)
    print(line)
    print(line, file=sys.stderr)

# Unfiltered BF baseline (NULL filter)
print("measuring unfiltered BF baseline...", file=sys.stderr)
last_n, lats = run_query("NULL::bigint[]")
emit("unfiltered", "n/a", 0, last_n, lats)

# Selectivity × correlation sweep
for sel in SELS:
    for corr in CORRS:
        bigints = make_filter_bigints(sel, corr)
        arr_sql = fmt_bigint_array(bigints)
        last_n, lats = run_query(arr_sql)
        emit("%.2f" % sel, "%.1f" % corr, len(bigints), last_n, lats)

# ── Cleanup ───────────────────────────────────────────────────────────────────
psql("DROP TABLE _bench_sweep CASCADE")
print("\ndone", file=sys.stderr)
