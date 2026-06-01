#!/usr/bin/env python3
"""Load vectors into PostgreSQL via binary COPY (10-50x faster than text COPY).

Generates synthetic clustered data on-the-fly and streams into PG without
writing a 500GB base.copy file. Supports --smoke for quick validation.

Usage:
  python3 bench/load_binary.py --n 50000000 --dim 384 --queries 1000 \\
    --db bench --host /var/run/postgresql --seed 1234

Requires: numpy, psycopg2
"""
import argparse
import io
import struct
import sys
import numpy as np

# PostgreSQL binary COPY wire format constants
PG_COPY_HEADER = b'PGCOPY\n\xff\r\n\0' + struct.pack('>II', 0, 0)
PG_COPY_TRAILER = struct.pack('>h', -1)


def pg_copy_row(vec: np.ndarray, row_id: int) -> bytes:
    """Encode one (id int4, v vector) row in PG binary COPY format."""
    # id column: int4
    id_bytes = struct.pack('>i', row_id)
    # vector column: pgvector binary wire format (vector_send)
    # = int16 ndim + int16 unused + dim × float32 (all big-endian)
    dim = len(vec)
    vec_header = struct.pack('>hh', dim, 0)
    vec_data = vec.astype('>f4').tobytes()
    vec_bytes = vec_header + vec_data

    buf = io.BytesIO()
    buf.write(struct.pack('>h', 2))               # 2 fields
    buf.write(struct.pack('>i', len(id_bytes)))   # id field len
    buf.write(id_bytes)
    buf.write(struct.pack('>i', len(vec_bytes)))  # vec field len
    buf.write(vec_bytes)
    return buf.getvalue()


def binary_copy_batch(conn, table: str, rows_iter):
    """Write an iterable of (id, vec) pairs via binary COPY."""
    buf = io.BytesIO()
    buf.write(PG_COPY_HEADER)
    for row_id, vec in rows_iter:
        buf.write(pg_copy_row(vec, row_id))
    buf.write(PG_COPY_TRAILER)
    buf.seek(0)
    with conn.cursor() as cur:
        cur.copy_expert(f'COPY {table}(id, v) FROM STDIN WITH BINARY', buf)


def gen_clustered(n: int, dim: int, seed: int, sigma: float = 0.05):
    rng = np.random.default_rng(seed)
    nc = max(8, n // 5000)
    centers = rng.random((nc, dim), dtype=np.float32)
    assignment = rng.integers(0, nc, n)
    noise = (sigma * rng.standard_normal((n, dim))).astype(np.float32)
    return centers[assignment] + noise


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n', type=int, default=50_000_000)
    ap.add_argument('--dim', type=int, default=384)
    ap.add_argument('--queries', type=int, default=1000)
    ap.add_argument('--seed', type=int, default=1234)
    ap.add_argument('--db', default='bench')
    ap.add_argument('--host', default='/var/run/postgresql')
    ap.add_argument('--batch', type=int, default=50_000)
    ap.add_argument('--smoke', action='store_true', help='Load 1000 rows only')
    ap.add_argument('--data', default='/home/jaesolshin/bench/data',
                    help='Output dir for gt-compatible fbin query file')
    a = ap.parse_args()

    if a.smoke:
        a.n = 1000
        a.queries = 100

    import psycopg2, os
    os.makedirs(a.data, exist_ok=True)

    conn = psycopg2.connect(dbname=a.db, host=a.host)
    conn.autocommit = False

    print(f'[load] generating {a.n}×{a.dim} clustered (seed={a.seed})')
    rng = np.random.default_rng(a.seed)
    nc = max(8, a.n // 5000)
    centers = rng.random((nc, a.dim), dtype=np.float32)

    # Load base vectors in batches
    print(f'[load] binary COPY → {a.db}.items  batch={a.batch}')
    total = 0
    for start in range(0, a.n, a.batch):
        sz = min(a.batch, a.n - start)
        assign = rng.integers(0, nc, sz)
        noise = (0.05 * rng.standard_normal((sz, a.dim))).astype(np.float32)
        chunk = centers[assign] + noise

        def rows(start=start, chunk=chunk):
            for i, v in enumerate(chunk):
                yield start + i, v

        binary_copy_batch(conn, 'items', rows())
        total += sz
        if total % 500_000 == 0 or total == a.n:
            conn.commit()
            print(f'  {total}/{a.n}', flush=True)

    conn.commit()

    # Load query vectors
    print(f'[load] generating {a.queries} queries')
    q_assign = rng.integers(0, nc, a.queries)
    q_noise = (0.05 * rng.standard_normal((a.queries, a.dim))).astype(np.float32)
    queries = centers[q_assign] + q_noise

    binary_copy_batch(conn, 'queries',
                      ((i, queries[i]) for i in range(a.queries)))
    conn.commit()

    # Write query.fbin for gt_faiss.py
    qfbin = os.path.join(a.data, 'query.fbin')
    with open(qfbin, 'wb') as f:
        np.array([a.queries, a.dim], dtype=np.int32).tofile(f)
        queries.tofile(f)
    print(f'[load] query.fbin written → {qfbin}')

    loaded = conn.cursor()
    loaded.execute('SELECT count(*) FROM items')
    n_items = loaded.fetchone()[0]
    loaded.execute('SELECT count(*) FROM queries')
    n_queries = loaded.fetchone()[0]
    conn.close()
    print(f'[load] items={n_items} queries={n_queries}  DONE')


if __name__ == '__main__':
    main()
