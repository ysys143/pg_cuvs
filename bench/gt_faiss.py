#!/usr/bin/env python3
"""Compute exact L2 ground truth using faiss-gpu.

Reads base vectors from PostgreSQL (avoids storing 72GB fbin on disk)
and query vectors from query.fbin, writes gt.ibin.

Usage:
  python3 bench/gt_faiss.py --db bench --data /home/jaesolshin/bench/data \\
    --k 100 --dim 384 --n 50000000

Requires: numpy, psycopg2, faiss-gpu
  conda install -c pytorch faiss-gpu   (if not installed)
"""
import argparse
import os
import sys
import numpy as np


def read_fbin(path):
    with open(path, 'rb') as f:
        n, d = np.fromfile(f, np.int32, 2)
        return np.fromfile(f, np.float32, n * d).reshape(n, d)


def write_ibin(path, a):
    a = np.ascontiguousarray(a, dtype=np.int32)
    with open(path, 'wb') as f:
        np.array(a.shape, dtype=np.int32).tofile(f)
        a.tofile(f)


def fetch_base_pg(db: str, host: str, n: int, dim: int, batch: int = 100_000):
    """Stream base vectors from PostgreSQL in row-id order."""
    import psycopg2
    conn = psycopg2.connect(dbname=db, host=host)
    base = np.empty((n, dim), dtype=np.float32)
    with conn.cursor('base_cursor') as cur:
        cur.itersize = batch
        cur.execute('SELECT id, v FROM items ORDER BY id')
        for row_id, v in cur:
            arr = np.frombuffer(bytes.fromhex(v[1:-1].replace(',', '')), dtype='>f4') \
                  if isinstance(v, str) else np.array(v, dtype=np.float32)
            base[row_id] = arr
    conn.close()
    return base


def fetch_base_pg_fast(db: str, host: str, n: int, dim: int):
    """Faster: use COPY TO STDOUT to read vectors as binary."""
    import psycopg2, struct, io
    conn = psycopg2.connect(dbname=db, host=host)
    base = np.empty((n, dim), dtype=np.float32)
    buf = io.BytesIO()
    with conn.cursor() as cur:
        cur.copy_expert('COPY (SELECT v FROM items ORDER BY id) TO STDOUT', buf)
    conn.close()
    # Each row is text: "[v1,v2,...]\n"
    buf.seek(0)
    for i, line in enumerate(buf):
        vals = line.strip()[1:-1].split(b',')
        base[i] = [float(v) for v in vals]
    return base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--db', default='bench')
    ap.add_argument('--host', default='/var/run/postgresql')
    ap.add_argument('--data', default='/home/jaesolshin/bench/data')
    ap.add_argument('--k', type=int, default=100)
    ap.add_argument('--dim', type=int, default=384)
    ap.add_argument('--n', type=int, default=50_000_000)
    ap.add_argument('--gpu', type=int, default=0)
    ap.add_argument('--seed', type=int, default=1234, help='Regen seed (skip PG read)')
    ap.add_argument('--regen', action='store_true', help='Regen base from seed instead of reading PG (fast for large N)')
    ap.add_argument('--smoke', action='store_true', help='Use n=1000 for quick test')
    a = ap.parse_args()

    if a.smoke:
        a.n = 1000

    query_path = os.path.join(a.data, 'query.fbin')
    gt_path = os.path.join(a.data, 'gt.ibin')

    # For small N, use numpy brute-force (smoke tests / CI)
    if a.n <= 10000:
        print(f'[gt] small N={a.n} → numpy brute-force GT (no faiss needed)')
        queries = read_fbin(query_path)
        nq = queries.shape[0]
        import psycopg2
        conn = psycopg2.connect(dbname=a.db, host=a.host)
        base = np.empty((a.n, a.dim), dtype=np.float32)
        with conn.cursor() as cur:
            cur.execute('SELECT v FROM items ORDER BY id')
            for i, (v,) in enumerate(cur.fetchall()):
                # pgvector returns text like "[0.35,0.57,...]" via psycopg2
                base[i] = np.fromstring(str(v).strip('[]'), sep=',', dtype=np.float32)
        conn.close()
        bb = (base * base).sum(1)
        gt = np.empty((nq, a.k), np.int32)
        for i in range(nq):
            dq = bb - 2.0 * (base @ queries[i]) + (queries[i] @ queries[i])
            idx = np.argpartition(dq, a.k)[:a.k]
            gt[i] = idx[np.argsort(dq[idx])]
        write_ibin(query_path.replace('query.fbin', 'gt.ibin'), gt)
        print(f'[gt] gt.ibin written (numpy)  shape={gt.shape}')
        return

    try:
        import faiss
    except ImportError:
        print('[gt] faiss not found. Install: conda install -c pytorch faiss-gpu -n cuvs_dev')
        sys.exit(1)

    queries = read_fbin(query_path)
    nq, d = queries.shape
    assert d == a.dim, f'query dim {d} != expected {a.dim}'
    print(f'[gt] queries={queries.shape}')

    # Base vectors: regen from seed (fast) or read from PG (slow)
    if a.regen:
        print(f'[gt] regenerating {a.n}×{a.dim} base in batches (seed={a.seed})')
        rng = np.random.default_rng(a.seed)
        # nc and GBATCH must match load_binary.py exactly so the RNG sequence is identical
        nc = max(8, a.n // 5000)
        centers = rng.random((nc, a.dim), dtype=np.float32)
        # Add to faiss in batches to avoid 144GB float64 spike (50M×384×8B)
        index_cpu = faiss.IndexFlatL2(a.dim)
        try:
            res = faiss.StandardGpuResources()
            index = faiss.index_cpu_to_gpu(res, a.gpu, index_cpu)
            print(f'[gt] using faiss-gpu')
        except Exception:
            index = index_cpu
            print(f'[gt] using faiss-cpu')
        GBATCH = 50_000  # must match load_binary.py --batch default
        for start in range(0, a.n, GBATCH):
            sz = min(GBATCH, a.n - start)
            assign_b = rng.integers(0, nc, sz)
            noise_b = (0.05 * rng.standard_normal((sz, a.dim))).astype(np.float32)
            chunk = centers[assign_b] + noise_b
            index.add(chunk)
            if start % 10_000_000 == 0:
                print(f'  [{start+sz}/{a.n}]', flush=True)
        queries = read_fbin(query_path)
        nq = queries.shape[0]
        print(f'[gt] searching k={a.k} for {nq} queries...')
        _, I = index.search(queries, a.k)
        write_ibin(gt_path, I.astype(np.int32))
        print(f'[gt] gt.ibin written (regen+faiss)  shape={I.shape}')
        return
    else:
        print(f'[gt] reading {a.n}×{a.dim} base vectors from PG...')
        import psycopg2
        conn = psycopg2.connect(dbname=a.db, host=a.host)
        base = np.empty((a.n, a.dim), dtype=np.float32)
        BATCH = 200_000
        with conn.cursor() as cur:
            offset = 0
            while offset < a.n:
                cur.execute(
                    'SELECT v FROM items ORDER BY id LIMIT %s OFFSET %s',
                    (BATCH, offset)
                )
                rows = cur.fetchall()
                if not rows:
                    break
                for i, (v,) in enumerate(rows):
                    base[offset + i] = np.fromstring(str(v).strip('[]'), sep=',', dtype=np.float32)
                offset += len(rows)
                if offset % 1_000_000 == 0:
                    print(f'  {offset}/{a.n}', flush=True)
        conn.close()
    print(f'[gt] base loaded/generated: {base.shape}')

    # faiss brute-force: try GPU first, fall back to CPU
    index_cpu = faiss.IndexFlatL2(a.dim)
    try:
        res = faiss.StandardGpuResources()
        index = faiss.index_cpu_to_gpu(res, a.gpu, index_cpu)
        print(f'[gt] using faiss-gpu (GPU {a.gpu})')
    except Exception:
        index = index_cpu
        print(f'[gt] faiss-gpu unavailable, using faiss-cpu ({a.dim}d)')
    index.add(base)
    print(f'[gt] searching k={a.k} for {nq} queries...')
    _, I = index.search(queries, a.k)
    I = I.astype(np.int32)

    write_ibin(gt_path, I)
    print(f'[gt] gt.ibin written → {gt_path}  shape={I.shape}')

    if a.smoke:
        # Verify against numpy brute-force for first 10 queries
        bb = (base * base).sum(1)
        hits = 0
        for qi in range(min(10, nq)):
            dq = bb - 2.0 * (base @ queries[qi]) + (queries[qi] @ queries[qi])
            ref = set(np.argpartition(dq, a.k)[:a.k].tolist())
            hits += len(ref & set(I[qi].tolist()))
        print(f'[gt] smoke recall vs numpy: {hits/(10*a.k):.4f} (expect ~1.0)')


if __name__ == '__main__':
    main()
