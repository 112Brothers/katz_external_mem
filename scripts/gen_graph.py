#!/usr/bin/env python3
"""Synthetic directed power-law graph → raw_edges.bin + meta.bin.

Usage: python3 scripts/gen_graph.py [out_dir] [n_nodes] [n_edges] [gamma]
Defaults: data/synthetic  2_000_000  30_000_000  3.5
"""

import numpy as np
import struct
import sys
import os

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "data/synthetic"
N       = int(sys.argv[2])   if len(sys.argv) > 2 else 2_000_000
M       = int(sys.argv[3])   if len(sys.argv) > 3 else 30_000_000
GAMMA   = float(sys.argv[4]) if len(sys.argv) > 4 else 3.5
SEED    = 42
BATCH   = 5_000_000

os.makedirs(OUT_DIR, exist_ok=True)
rng = np.random.default_rng(SEED)

print(f"n={N:,}  m={M:,}  gamma={GAMMA}")

u = rng.random(N)
w = (1.0 - u) ** (-1.0 / (GAMMA - 1.0))
prob = w / w.sum()
print(f"max expected in-degree: {M * prob.max():,.0f}")

raw_path = os.path.join(OUT_DIR, "raw_edges.bin")
with open(raw_path, "wb") as f:
    remaining = M
    written = 0
    while remaining > 0:
        n = min(BATCH, remaining)
        src = rng.integers(0, N, size=n, dtype=np.uint32)
        dst = rng.choice(N, size=n, p=prob).astype(np.uint32)
        np.stack([src, dst], axis=1).astype(np.uint32).tofile(f)
        written += n
        remaining -= n
        print(f"  {written/1e6:.0f}M / {M/1e6:.0f}M edges", flush=True)

with open(os.path.join(OUT_DIR, "meta.bin"), "wb") as f:
    f.write(struct.pack("<I4xQ", N, M))

print(f"done: {os.path.getsize(raw_path)/1e9:.2f} GB")
