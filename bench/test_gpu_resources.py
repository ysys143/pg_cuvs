#!/usr/bin/env python3
"""
test_gpu_resources.py — GPU 자원 파라미터별 E2E 검증 및 벤치마크.

테스트 항목:
  T1. max_vram_mb  : VRAM 예산 제한 → 초과 시 OOM fallback 확인
  T2. shard_count  : 명시적 샤딩(1, 2) → 결과 정합성 + latency 비교
  T3. cuvs_k       : k sweep(10,50,100,200) → recall vs latency tradeoff
  T4. parallel_fanout: shard_count=2에서 parallel(1) vs sequential(0) latency

비동기 실행:
  nohup sudo bash -c 'CUDA_VISIBLE_DEVICES=1 PGDSN=dbname=contrib_regression \
    python3 bench/test_gpu_resources.py' > /tmp/test_gpu_res.log 2>&1 &
  sudo tail -f /tmp/test_gpu_res.log

결과:
  bench/results/gpu_resources_bench.csv
"""

import argparse
import csv
import os
import random
import sys
import subprocess
import time
from pathlib import Path

import psycopg2

try:
    from tqdm import tqdm
    HAVE_TQDM = True
except ImportError:
    HAVE_TQDM = False

CSV_PATH = Path(__file__).parent / "results" / "gpu_resources_bench.csv"
CSV_FIELDS = [
    "test", "N", "dim", "k",
    "max_vram_mb", "shard_count", "cuvs_k", "parallel_fanout",
    "build_s", "p50_us", "p95_us", "recall_at_k", "notes"
]

DAEMON_SERVICE = "pg-cuvs-server"
PG_SERVICE = "postgresql"
IDX_DIR = "/tmp/cuvs_indexes"
DB = os.environ.get("PGDB", "contrib_regression")


def log(step, total, msg):
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}][{step}/{total}] {msg}", flush=True)


def connect():
    dsn = os.environ.get("PGDSN", f"dbname={DB}")
    conn = psycopg2.connect(dsn)
    conn.autocommit = False
    return conn


def restart_daemon(cuda_device=None, max_vram_mb=None):
    """Restart daemon with specific GPU and VRAM settings."""
    print(f"  Restarting daemon (GPU={cuda_device or 'all'} max_vram={max_vram_mb or 'default'})...",
          flush=True)
    subprocess.run(["sudo", "systemctl", "stop", DAEMON_SERVICE], check=False)
    time.sleep(2)

    env_cmds = []
    if cuda_device is not None:
        env_cmds += ["sudo", "systemctl", "set-environment",
                     f"CUDA_VISIBLE_DEVICES={cuda_device}"]
        subprocess.run(env_cmds, check=False)
    else:
        subprocess.run(["sudo", "systemctl", "unset-environment",
                        "CUDA_VISIBLE_DEVICES"], check=False)

    # Override max-vram-mb in service via systemd override
    if max_vram_mb:
        override = (f"[Service]\nExecStart=\n"
                    f"ExecStart=/usr/lib/postgresql/16/bin/pg_cuvs_server "
                    f"--socket /tmp/.s.pg_cuvs --index-dir {IDX_DIR} "
                    f"--max-vram-mb {max_vram_mb}\n"
                    f"ExecStartPost=/bin/sh -c \"for i in $(seq 1 30); do "
                    f"[ -S /tmp/.s.pg_cuvs ] && break; sleep 1; done; chmod 666 /tmp/.s.pg_cuvs\"\n")
        subprocess.run(["sudo", "mkdir", "-p",
                        f"/etc/systemd/system/{DAEMON_SERVICE}.service.d"], check=False)
        proc = subprocess.run(
            ["sudo", "tee",
             f"/etc/systemd/system/{DAEMON_SERVICE}.service.d/override.conf"],
            input=override.encode(), capture_output=True, check=False)
        subprocess.run(["sudo", "systemctl", "daemon-reload"], check=False)
    else:
        subprocess.run(["sudo", "rm", "-f",
                        f"/etc/systemd/system/{DAEMON_SERVICE}.service.d/override.conf"],
                       check=False)
        subprocess.run(["sudo", "systemctl", "daemon-reload"], check=False)

    subprocess.run(["sudo", "systemctl", "start", DAEMON_SERVICE], check=False)
    # Wait for socket
    for i in range(20):
        time.sleep(1)
        if Path("/tmp/.s.pg_cuvs").is_socket():
            print(f"  daemon ready ({i+1}s)", flush=True)
            break


def setup_table(cur, n, dim):
    cur.execute("DROP TABLE IF EXISTS res_test CASCADE")
    cur.execute(f"CREATE TABLE res_test (id bigint, embedding vector({dim}))")
    cur.connection.commit()

    batch = 2000
    rng = random.Random(42)
    nc = max(8, n // 5000)
    centers = [[rng.gauss(0, 1) for _ in range(dim)] for _ in range(nc)]
    batches = range((n + batch - 1) // batch)
    if HAVE_TQDM:
        batches = tqdm(batches, desc="  inserting", file=sys.stdout, dynamic_ncols=True)

    done = 0
    for bi in batches:
        s, e = bi * batch, min((bi + 1) * batch, n)
        data = []
        for i in range(s, e):
            c = centers[i % nc]
            vec = "[" + ",".join(f"{c[d] + rng.gauss(0,0.05):.4f}" for d in range(dim)) + "]"
            data.append((i + 1, vec))
        cur.executemany("INSERT INTO res_test VALUES (%s, %s::vector)", data)
        done += len(data)
        if not HAVE_TQDM and bi % 50 == 0:
            print(f"  {done:,}/{n:,} rows", flush=True)
    cur.connection.commit()
    cur.execute(f"SET cuvs.index_dir = '{IDX_DIR}'")


def build_cagra(cur, shard_count=0):
    cur.execute(f"SET cuvs.shard_count = {shard_count}")
    t0 = time.perf_counter()
    cur.execute("CREATE INDEX res_cagra ON res_test USING cagra (embedding vector_l2_ops)")
    cur.connection.commit()
    return time.perf_counter() - t0


def measure_query(cur, n_queries, k, dim, cuvs_k, parallel_fanout):
    cur.execute(f"SET cuvs.k = {cuvs_k}")
    cur.execute(f"SET cuvs.parallel_fanout = {bool(parallel_fanout)}")
    cur.execute("SET enable_seqscan = off")
    cur.connection.commit()
    rng = random.Random(123)
    lats = []
    for _ in range(n_queries):
        vec = "[" + ",".join(f"{rng.gauss(0,1):.4f}" for _ in range(dim)) + "]"
        t0 = time.perf_counter()
        cur.execute("SELECT id FROM res_test ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        cur.fetchall()
        lats.append((time.perf_counter() - t0) * 1e6)
    cur.execute("RESET enable_seqscan; RESET cuvs.k; RESET cuvs.parallel_fanout")
    cur.connection.commit()
    lats.sort()
    return lats[len(lats) // 2], lats[int(len(lats) * 0.95)]


def measure_recall(cur, n_queries, k, dim, cuvs_k):
    rng = random.Random(456)
    hits = 0
    cur.execute(f"SET cuvs.k = {cuvs_k}")
    cur.connection.commit()
    for _ in range(n_queries):
        vec = "[" + ",".join(f"{rng.gauss(0,1):.4f}" for _ in range(dim)) + "]"
        cur.execute("SET enable_cuvs=off; SET enable_seqscan=on")
        cur.execute("SELECT id FROM res_test ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        bf = {r[0] for r in cur.fetchall()}
        cur.execute("SET enable_seqscan=off; SET enable_cuvs=on")
        cur.execute("SELECT id FROM res_test ORDER BY embedding <-> %s LIMIT %s", (vec, k))
        hnsw = {r[0] for r in cur.fetchall()}
        hits += len(bf & hnsw)
    cur.execute("RESET enable_seqscan; RESET enable_cuvs; RESET cuvs.k")
    cur.connection.commit()
    return hits / (n_queries * k)


def write_csv(row):
    CSV_PATH.parent.mkdir(exist_ok=True)
    hdr = not CSV_PATH.exists()
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        if hdr:
            w.writeheader()
        w.writerow(row)
    print(f"  → {CSV_PATH}", flush=True)


def drop_index(cur):
    cur.execute("DROP INDEX IF EXISTS res_cagra")
    cur.connection.commit()


# ═══════════════════════════════════════════════════════════════════════════

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=100_000)
    ap.add_argument("--dim", type=int, default=384)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--n-queries", type=int, default=100)
    ap.add_argument("--n-recall", type=int, default=50)
    ap.add_argument("--gpu", type=str, default="1",
                    help="CUDA device index for daemon (default: 1)")
    args = ap.parse_args()

    TOTAL = 4
    print(f"\n=== GPU Resource Parameter Tests: N={args.n:,} dim={args.dim} k={args.k} GPU={args.gpu} ===",
          flush=True)

    # ── T1: max_vram_mb ──────────────────────────────────────────────────
    log(1, TOTAL, "T1: max_vram_mb — VRAM 예산 제한 테스트")

    # T1a: 충분한 VRAM (40GB) → 정상 동작
    restart_daemon(cuda_device=args.gpu, max_vram_mb=40000)
    conn = connect()
    cur = conn.cursor()
    cur.execute("CREATE EXTENSION IF NOT EXISTS vector"); cur.execute("CREATE EXTENSION IF NOT EXISTS pg_cuvs")
    conn.commit()
    setup_table(cur, args.n, args.dim)
    build_s = build_cagra(cur, shard_count=0)
    p50, p95 = measure_query(cur, args.n_queries, args.k, args.dim, 100, 1)
    recall = measure_recall(cur, args.n_recall, args.k, args.dim, 100)
    print(f"  T1a max_vram_mb=40000 → build={build_s:.1f}s p50={p50:.0f}us recall={recall:.4f}", flush=True)
    write_csv({"test": "T1a-vram-40gb", "N": args.n, "dim": args.dim, "k": args.k,
               "max_vram_mb": 40000, "shard_count": "auto", "cuvs_k": 100, "parallel_fanout": 1,
               "build_s": round(build_s,2), "p50_us": round(p50,0), "p95_us": round(p95,0),
               "recall_at_k": round(recall,4), "notes": f"gpu={args.gpu}"})
    drop_index(cur)

    # T1b: 제한된 VRAM (2048MB) → build 시 메모리 제약 동작 확인
    restart_daemon(cuda_device=args.gpu, max_vram_mb=2048)
    cur.execute("SET cuvs.index_dir = '/tmp/cuvs_indexes'")
    try:
        build_s2 = build_cagra(cur, shard_count=0)
        p50b, p95b = measure_query(cur, args.n_queries, args.k, args.dim, 100, 1)
        recallb = measure_recall(cur, args.n_recall, args.k, args.dim, 100)
        note = f"gpu={args.gpu} vram_constrained"
        print(f"  T1b max_vram_mb=2048 → build={build_s2:.1f}s p50={p50b:.0f}us recall={recallb:.4f}", flush=True)
        write_csv({"test": "T1b-vram-2gb", "N": args.n, "dim": args.dim, "k": args.k,
                   "max_vram_mb": 2048, "shard_count": "auto", "cuvs_k": 100, "parallel_fanout": 1,
                   "build_s": round(build_s2,2), "p50_us": round(p50b,0), "p95_us": round(p95b,0),
                   "recall_at_k": round(recallb,4), "notes": note})
    except Exception as e:
        print(f"  T1b max_vram_mb=2048 → OOM/Error (expected): {e}", flush=True)
        write_csv({"test": "T1b-vram-2gb", "N": args.n, "dim": args.dim, "k": args.k,
                   "max_vram_mb": 2048, "shard_count": "auto", "cuvs_k": 100, "parallel_fanout": 1,
                   "build_s": "OOM", "p50_us": "NA", "p95_us": "NA",
                   "recall_at_k": "NA", "notes": f"gpu={args.gpu} OOM as expected"})
        conn.rollback()
    drop_index(cur)

    # ── T2: shard_count ──────────────────────────────────────────────────
    log(2, TOTAL, "T2: shard_count — 명시적 샤딩 (1 vs 2 GPU)")
    restart_daemon(cuda_device=None, max_vram_mb=40000)  # both GPUs
    cur.execute("SET cuvs.index_dir = '/tmp/cuvs_indexes'")

    for sc in [1, 2]:
        build_s = build_cagra(cur, shard_count=sc)
        p50, p95 = measure_query(cur, args.n_queries, args.k, args.dim, 100, 1)
        recall = measure_recall(cur, args.n_recall, args.k, args.dim, 100)
        print(f"  T2 shard_count={sc} → build={build_s:.1f}s p50={p50:.0f}us recall={recall:.4f}", flush=True)
        write_csv({"test": f"T2-shard-{sc}", "N": args.n, "dim": args.dim, "k": args.k,
                   "max_vram_mb": 40000, "shard_count": sc, "cuvs_k": 100, "parallel_fanout": 1,
                   "build_s": round(build_s,2), "p50_us": round(p50,0), "p95_us": round(p95,0),
                   "recall_at_k": round(recall,4), "notes": "both_gpus"})
        drop_index(cur)

    # ── T3: cuvs_k sweep ────────────────────────────────────────────────
    log(3, TOTAL, "T3: cuvs_k sweep — recall vs latency tradeoff")
    build_s = build_cagra(cur, shard_count=1)
    print(f"  T3 CAGRA built in {build_s:.1f}s", flush=True)

    for ck in [10, 50, 100, 200]:
        p50, p95 = measure_query(cur, args.n_queries, args.k, args.dim, ck, 1)
        recall = measure_recall(cur, args.n_recall, args.k, args.dim, ck)
        print(f"  T3 cuvs_k={ck} → p50={p50:.0f}us recall={recall:.4f}", flush=True)
        write_csv({"test": f"T3-cuvsk-{ck}", "N": args.n, "dim": args.dim, "k": args.k,
                   "max_vram_mb": 40000, "shard_count": 1, "cuvs_k": ck, "parallel_fanout": 1,
                   "build_s": round(build_s,2), "p50_us": round(p50,0), "p95_us": round(p95,0),
                   "recall_at_k": round(recall,4), "notes": ""})
    drop_index(cur)

    # ── T4: parallel_fanout ──────────────────────────────────────────────
    log(4, TOTAL, "T4: parallel_fanout — shard_count=2에서 parallel vs sequential")
    build_s = build_cagra(cur, shard_count=2)
    print(f"  T4 CAGRA sharded build in {build_s:.1f}s", flush=True)

    for pf in [0, 1]:
        label = "parallel" if pf else "sequential"
        p50, p95 = measure_query(cur, args.n_queries, args.k, args.dim, 100, pf)
        recall = measure_recall(cur, args.n_recall, args.k, args.dim, 100)
        print(f"  T4 parallel_fanout={pf} ({label}) → p50={p50:.0f}us recall={recall:.4f}", flush=True)
        write_csv({"test": f"T4-fanout-{label}", "N": args.n, "dim": args.dim, "k": args.k,
                   "max_vram_mb": 40000, "shard_count": 2, "cuvs_k": 100, "parallel_fanout": pf,
                   "build_s": round(build_s,2), "p50_us": round(p50,0), "p95_us": round(p95,0),
                   "recall_at_k": round(recall,4), "notes": "both_gpus"})
    drop_index(cur)

    cur.execute("DROP TABLE IF EXISTS res_test CASCADE")
    conn.commit()
    cur.close()
    conn.close()

    # Restore daemon to default
    restart_daemon(cuda_device=None, max_vram_mb=None)

    print(f"\n=== ALL DONE → {CSV_PATH} ===", flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"\nERROR: {e}", flush=True)
        raise
