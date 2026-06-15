#!/usr/bin/env python3
"""observe.py — phase-scoped resource observer + protocol-CSV writer.

The per-engine runners already produce performance numbers (qps, latency,
recall). This module adds the missing RESOURCE layer (VRAM/energy/CPU/RSS)
and the full protocol §4 observation schema, written one row per
(system, phase, cell).

Key facts that shape the design:
  * GPU is sampled whole-device via `nvidia-smi` subprocess (not pynvml —
    the NVML python binding may be absent on the VM). One call per ~100ms tick.
  * pg_cuvs holds VRAM in a SEPARATE process (systemd unit pg-cuvs-server),
    NOT in the benchmark backend. So VRAM is taken from the *device* delta
    (peak memory.used − baseline), never from any process RSS. RSS tracking
    here is host-memory accounting only.
  * Every nvidia-smi / /proc read is failure-wrapped → None, never raises.
    The whole sampler runs cleanly on macOS (all GPU metrics = None).
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import threading
import time
import uuid

# ── Protocol §4 schema. ORDER MATTERS — this is the CSV contract. ───────────
PROTOCOL_FIELDS = [
    "run_id", "date", "stage", "phase", "cell_id", "config",
    "system", "system_version", "system_commit", "index_type",
    "N", "dim", "k", "recall_target", "dataset", "query_set_id", "seed",
    "clients", "warm_state",
    "build_s", "qps", "p50_us", "p95_us", "p99_us", "p999_us",
    "avg_latency_us", "recall_at_k",
    "peak_vram_mb", "peak_rss_mb", "cpu_core_s", "gpu_s", "energy_j",
    "disk_bytes_written", "wal_bytes",
    "index_bytes_vram", "index_bytes_host", "index_bytes_disk",
    "instance_type", "price_usd_hr", "usd_per_1m_queries",
    "reps", "agg_method", "dispersion",
    "gt_method",
    "cost_model_version", "runtime_routing_version",
    "selectivity", "correlation", "filter_mode",
    "stream_op", "ops_done", "delta_rows",
    "params_json", "notes",
]

PHASES = ("build", "maint", "query")

DEFAULT_PROTOCOL_DIR = os.path.join("bench", "results", "protocol")


# ── GPU sampling (whole device, subprocess nvidia-smi) ──────────────────────

def _nvsmi_sample(gpu_index):
    """Return (mem_used_mb, util_pct, power_w) for one gpu, or all-None on any
    failure (no GPU, no nvidia-smi, parse error). Never raises."""
    try:
        out = subprocess.check_output(
            ["nvidia-smi",
             "--query-gpu=memory.used,utilization.gpu,power.draw",
             "--format=csv,noheader,nounits",
             "-i", str(gpu_index)],
            text=True, stderr=subprocess.DEVNULL)
        line = out.strip().splitlines()[0]
        mem, util, power = (p.strip() for p in line.split(","))
        # any field can be "[N/A]" on power-capped / MIG GPUs → tolerate
        return (_to_float(mem), _to_float(util), _to_float(power))
    except Exception:
        return (None, None, None)


def _to_float(s):
    try:
        return float(s)
    except (ValueError, TypeError):
        return None


# ── Host process accounting via /proc (Linux); macOS-safe fallbacks ─────────

def _proc_rss_kb(pid):
    """VmRSS in KB for one pid from /proc/<pid>/status, or None."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for ln in f:
                if ln.startswith("VmRSS:"):
                    return int(ln.split()[1])  # KB
    except Exception:
        pass
    return None


def _proc_children(pid):
    """Direct child pids of pid via /proc/<pid>/task/*/children, or []."""
    kids = []
    try:
        tdir = f"/proc/{pid}/task"
        for tid in os.listdir(tdir):
            try:
                with open(f"{tdir}/{tid}/children") as f:
                    kids += [int(x) for x in f.read().split()]
            except Exception:
                continue
    except Exception:
        pass
    return kids


def _proc_tree(pid):
    """pid + transitive children (best effort). Empty if /proc absent."""
    if pid is None:
        return []
    seen, stack = set(), [pid]
    while stack:
        p = stack.pop()
        if p in seen:
            continue
        seen.add(p)
        stack += _proc_children(p)
    return list(seen)


def _proc_cpu_jiffies(pid):
    """utime+stime jiffies for one pid from /proc/<pid>/stat, or None.
    Fields 14 (utime) and 15 (stime) are 1-indexed after the comm field;
    comm may contain spaces/parens so split on the last ')'."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            data = f.read()
        rest = data[data.rindex(")") + 2:].split()
        utime = int(rest[11])   # 14th field overall
        stime = int(rest[12])   # 15th field overall
        return utime + stime
    except Exception:
        return None


def _getrusage_children_rss_kb():
    try:
        import resource
        # ru_maxrss is KB on Linux, bytes on macOS — caller uses Linux path
        return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    except Exception:
        return None


def _getrusage_cpu_s():
    try:
        import resource
        r = resource.getrusage(resource.RUSAGE_CHILDREN)
        s = resource.getrusage(resource.RUSAGE_SELF)
        return (r.ru_utime + r.ru_stime + s.ru_utime + s.ru_stime)
    except Exception:
        return None


class ResourceSampler:
    """Context manager: background thread polls GPU + host every ~100ms.

    VRAM note: pg_cuvs keeps its index in a separate daemon process, so the
    VRAM figure here is the whole-DEVICE delta (peak memory.used minus the
    baseline taken at entry) — it does NOT come from any backend RSS.

    Pass `daemon_pid` for the pg-cuvs-server unit pid so its (host) RSS is
    folded into peak_rss_mb alongside `pid` and its children.

    After the `with` block exits, read attributes:
        peak_vram_mb       device delta (max memory.used − baseline), MB
        peak_vram_mb_raw   raw max memory.used, MB
        energy_j           ∫ power.draw dt   (joules)
        gpu_s              ∫ (util/100) dt   (GPU-busy seconds)
        peak_rss_mb        max tracked-process RSS, MB
        cpu_core_s         Σ (utime+stime) over tracked pids, seconds
        wall_s             wall-clock duration of the block
    Any metric whose source was unavailable is None.
    """

    def __init__(self, gpu_index=0, pid=None, daemon_pid=None, interval=0.1):
        self.gpu_index = gpu_index
        # default to the current process if no pid is given
        self.pid = pid if pid is not None else os.getpid()
        self.daemon_pid = daemon_pid
        self.interval = interval
        self._stop = threading.Event()
        self._thread = None
        # raw sample buffers
        self._t = []            # wall timestamps
        self._vram = []         # memory.used MB
        self._util = []         # utilization %
        self._power = []        # power.draw W
        self._rss = []          # tracked-tree RSS KB
        # cpu jiffies endpoints
        self._cpu_start = None
        self._cpu_end = None
        self._clk_tck = self._safe_clk_tck()
        # results (populated on exit)
        self.peak_vram_mb = None
        self.peak_vram_mb_raw = None
        self.energy_j = None
        self.gpu_s = None
        self.peak_rss_mb = None
        self.cpu_core_s = None
        self.wall_s = None

    @staticmethod
    def _safe_clk_tck():
        try:
            return os.sysconf("SC_CLK_TCK")
        except (ValueError, OSError, AttributeError):
            return 100  # conventional default

    # -- tracked-process helpers --
    def _tracked_pids(self):
        pids = _proc_tree(self.pid)
        if self.daemon_pid is not None:
            pids += _proc_tree(self.daemon_pid)
        # dedupe; if /proc absent both are empty → []
        return list(dict.fromkeys(pids))

    def _sample_rss_kb(self):
        total, saw = 0, False
        for p in self._tracked_pids():
            v = _proc_rss_kb(p)
            if v is not None:
                total += v
                saw = True
        return total if saw else None

    def _sample_cpu_jiffies(self):
        total, saw = 0, False
        for p in self._tracked_pids():
            v = _proc_cpu_jiffies(p)
            if v is not None:
                total += v
                saw = True
        return total if saw else None

    # -- thread loop --
    def _run(self):
        while not self._stop.is_set():
            t = time.perf_counter()
            mem, util, power = _nvsmi_sample(self.gpu_index)
            rss = self._sample_rss_kb()
            self._t.append(t)
            self._vram.append(mem)
            self._util.append(util)
            self._power.append(power)
            self._rss.append(rss)
            self._stop.wait(self.interval)

    def __enter__(self):
        # baseline tick before the workload runs
        self._t0 = time.perf_counter()
        self._cpu_start = self._sample_cpu_jiffies()
        mem, util, power = _nvsmi_sample(self.gpu_index)
        self._baseline_vram = mem
        self._t.append(self._t0)
        self._vram.append(mem)
        self._util.append(util)
        self._power.append(power)
        self._rss.append(self._sample_rss_kb())
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        self.wall_s = time.perf_counter() - self._t0
        self._cpu_end = self._sample_cpu_jiffies()
        self._reduce()
        return False  # never suppress exceptions

    # -- reductions --
    def _reduce(self):
        vram = [v for v in self._vram if v is not None]
        if vram:
            self.peak_vram_mb_raw = max(vram)
            base = self._baseline_vram if self._baseline_vram is not None else min(vram)
            self.peak_vram_mb = max(0.0, self.peak_vram_mb_raw - base)

        self.energy_j = self._integrate(self._power)          # W·s = J
        gpu_busy = [None if u is None else u / 100.0 for u in self._util]
        self.gpu_s = self._integrate(gpu_busy)                 # busy-seconds

        rss = [r for r in self._rss if r is not None]
        if rss:
            self.peak_rss_mb = max(rss) / 1024.0
        else:
            fb = _getrusage_children_rss_kb()
            self.peak_rss_mb = fb / 1024.0 if fb else None

        # CPU: prefer /proc jiffies delta; else getrusage seconds
        if self._cpu_start is not None and self._cpu_end is not None:
            self.cpu_core_s = (self._cpu_end - self._cpu_start) / float(self._clk_tck)
        else:
            self.cpu_core_s = _getrusage_cpu_s()

    def _integrate(self, values):
        """Trapezoidal ∫ values dt over self._t. None entries break the
        integration into segments (paired with their timestamps). Returns None
        if no two consecutive valid samples exist."""
        total, contributed = 0.0, False
        for i in range(1, len(self._t)):
            a, b = values[i - 1], values[i]
            if a is None or b is None:
                continue
            dt = self._t[i] - self._t[i - 1]
            total += 0.5 * (a + b) * dt
            contributed = True
        return total if contributed else None

    def as_dict(self):
        """Resource subset keyed by PROTOCOL_FIELDS names."""
        return {
            "peak_vram_mb": self.peak_vram_mb,
            "peak_rss_mb": self.peak_rss_mb,
            "cpu_core_s": self.cpu_core_s,
            "gpu_s": self.gpu_s,
            "energy_j": self.energy_j,
        }


# ── Postgres-side helpers (lazy psycopg, pure functions on a connection) ────

def wal_lsn(conn):
    """Current WAL position as a bigint (bytes since 0/0)."""
    cur = conn.cursor()
    cur.execute("SELECT pg_current_wal_lsn()::pg_lsn - '0/0'::pg_lsn")
    return int(cur.fetchone()[0])


def wal_delta(conn, before_lsn):
    """wal_bytes since `before_lsn` (captured via wal_lsn before the op)."""
    return wal_lsn(conn) - before_lsn


def index_sizes(conn, index_name):
    """On-disk index size. index_bytes_vram is None — the caller fills it from
    daemon stats if available (VRAM lives in the pg-cuvs-server process)."""
    cur = conn.cursor()
    cur.execute("SELECT pg_relation_size(%s)", (index_name,))
    host = int(cur.fetchone()[0])
    return {
        "index_bytes_host": host,
        "index_bytes_disk": host,
        "index_bytes_vram": None,
    }


# ── Protocol-CSV writer (append-only, header-once) ──────────────────────────

def _coerce(field, value):
    """dict-valued fields → JSON; None/missing → ''."""
    if value is None:
        return ""
    if field in ("params_json", "config") and isinstance(value, (dict, list)):
        return json.dumps(value, separators=(",", ":"))
    return value


def write_protocol_row(csv_path, **row):
    """Append one protocol row. Writes the header on first creation.
    Missing fields → empty string; extra keys ignored."""
    d = os.path.dirname(csv_path)
    if d:
        os.makedirs(d, exist_ok=True)
    need_header = not os.path.exists(csv_path)
    out = {f: _coerce(f, row.get(f)) for f in PROTOCOL_FIELDS}
    with open(csv_path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=PROTOCOL_FIELDS, extrasaction="ignore")
        if need_header:
            w.writeheader()
        w.writerow(out)
        f.flush()
    return csv_path


def protocol_path(stage, base_dir=DEFAULT_PROTOCOL_DIR):
    """Default convention: bench/results/protocol/<stage>.csv"""
    return os.path.join(base_dir, f"{stage}.csv")


def make_run_id(stage, system, cell_id):
    """Stable-ish, collision-resistant id: stage-system-cell + short uuid."""
    return f"{stage}-{system}-{cell_id}-{uuid.uuid4().hex[:8]}"


# ── selftest ────────────────────────────────────────────────────────────────

def _selftest():
    import tempfile

    # (1) sampler around a ~0.5s CPU-busy loop — must not raise on macOS
    with ResourceSampler(gpu_index=0) as s:
        end = time.perf_counter() + 0.5
        x = 0
        while time.perf_counter() < end:
            x += 1  # busy
    assert s.wall_s is not None and s.wall_s >= 0.4, f"wall_s={s.wall_s}"

    have_gpu = s.peak_vram_mb_raw is not None
    if not have_gpu:
        # (c) no-GPU machine: vram/energy/gpu_s must be None, no raise
        assert s.peak_vram_mb is None, s.peak_vram_mb
        assert s.energy_j is None, s.energy_j
        assert s.gpu_s is None, s.gpu_s
        print("[selftest] no GPU detected — vram/energy/gpu_s = None (OK)")
    else:
        print(f"[selftest] GPU present — peak_vram_mb={s.peak_vram_mb}")

    rd = s.as_dict()
    assert set(rd) == {"peak_vram_mb", "peak_rss_mb",
                       "cpu_core_s", "gpu_s", "energy_j"}, rd

    # (2) write ONE row to a temp CSV, read it back
    tmp = tempfile.mkdtemp(prefix="observe_selftest_")
    path = os.path.join(tmp, "build.csv")
    rid = make_run_id("build", "pg_cuvs", "c01")
    write_protocol_row(
        path,
        run_id=rid, date="2026-06-11", stage="build", phase="build",
        cell_id="c01", config={"shard_count": 1},
        system="pg_cuvs", index_type="cagra",
        N=100000, dim=384, k=10, dataset="selftest",
        build_s=1.23, params_json={"cuvs_k": 100}, notes="selftest",
        **rd,
    )

    with open(path, newline="") as f:
        rdr = csv.DictReader(f)
        header = rdr.fieldnames
        rows = list(rdr)

    # (a) every protocol column present in the header
    for col in PROTOCOL_FIELDS:
        assert col in header, f"missing column: {col}"
    assert header == PROTOCOL_FIELDS, "header order mismatch"
    # (b) row round-trips
    assert len(rows) == 1, f"expected 1 row, got {len(rows)}"
    r = rows[0]
    assert r["run_id"] == rid
    assert r["system"] == "pg_cuvs"
    assert r["build_s"] == "1.23"
    assert json.loads(r["config"]) == {"shard_count": 1}
    assert json.loads(r["params_json"]) == {"cuvs_k": 100}
    # (c) GPU-absent metrics serialized as empty
    if not have_gpu:
        assert r["peak_vram_mb"] == "", repr(r["peak_vram_mb"])
        assert r["energy_j"] == "", repr(r["energy_j"])
        assert r["gpu_s"] == "", repr(r["gpu_s"])

    print("SELFTEST OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--selftest", action="store_true",
                    help="run import/sampler/CSV self-checks and exit")
    args = ap.parse_args()
    if args.selftest:
        try:
            _selftest()
        except AssertionError as e:
            print(f"SELFTEST FAILED: {e}", file=sys.stderr)
            sys.exit(1)
        sys.exit(0)
    ap.print_help()


if __name__ == "__main__":
    # make `python3 infra/anbench/observe.py` runnable standalone
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
