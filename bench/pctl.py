#!/usr/bin/env python3
"""Read whitespace-separated latency values (ms) on stdin, print p50/p95/p99/avg."""
import sys
import numpy as np

vals = [float(x) for x in sys.stdin.read().split() if x.strip()]
if not vals:
    print("p50=- p95=- p99=- avg=- n=0")
    sys.exit(0)
a = np.array(vals)
print(f"p50={np.percentile(a,50):.3f} p95={np.percentile(a,95):.3f} "
      f"p99={np.percentile(a,99):.3f} avg={a.mean():.3f} n={len(a)}")
