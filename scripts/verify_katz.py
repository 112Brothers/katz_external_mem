#!/usr/bin/env python3
"""Compare katz_scores.csv against a Python reference implementation.

Usage: python3 scripts/verify_katz.py <data_dir> <alpha> [tol] [max_iter]
"""

import struct, sys, os, numpy as np

def load_meta(path):
    raw = open(path, 'rb').read(16)
    n, = struct.unpack('<I', raw[0:4])
    m, = struct.unpack('<Q', raw[8:16])
    return n, m

def katz_reference(edges, n, alpha, tol=1e-6, max_iter=500):
    src, dst = edges[:, 0].astype(np.intp), edges[:, 1].astype(np.intp)
    x = np.zeros(n)
    for it in range(max_iter):
        y = np.zeros(n)
        np.add.at(y, dst, x[src])
        x_new = alpha * y + 1.0
        diff = np.linalg.norm(x_new - x) / np.sqrt(n)
        x = x_new
        if diff < tol:
            print(f"  reference: {it+1} iters, final residual {diff:.2e}")
            return x
    print(f"  reference: did not converge in {max_iter} iters")
    return x

if len(sys.argv) < 3:
    print(f"Usage: {sys.argv[0]} <data_dir> <alpha> [tol] [max_iter]")
    sys.exit(1)

datadir  = sys.argv[1]
alpha    = float(sys.argv[2])
tol      = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-6
max_iter = int(sys.argv[4])   if len(sys.argv) > 4 else 500

n, m = load_meta(os.path.join(datadir, 'meta.bin'))
print(f"n={n}  m={m}  alpha={alpha}")

raw = open(os.path.join(datadir, 'rev_edges.bin'), 'rb').read()
edges = np.frombuffer(raw, dtype=np.uint32).reshape(-1, 2).copy()

ref = katz_reference(edges, n, alpha, tol, max_iter)

scores_path = os.path.join(datadir, 'katz_scores.csv')
cpp = np.zeros(n)
with open(scores_path) as f:
    next(f)
    for line in f:
        v, s = line.split(',')
        cpp[int(v)] = float(s)

diff = np.abs(ref - cpp)
corr = np.corrcoef(ref, cpp)[0, 1]
print(f"max diff:    {diff.max():.2e}")
print(f"mean diff:   {diff.mean():.2e}")
print(f"pearson r:   {corr:.10f}")
ok = diff.max() < max(1e-4, ref.max() * 1e-5)
print("PASS" if ok else "FAIL")
