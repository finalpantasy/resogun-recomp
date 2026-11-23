"""Compare two 1920x1080 BGRA frame dumps (the newest display_*.raw in each folder or a file).
Usage: python -I compare-dump.py A B   -> mean abs diff per channel and the share of pixels differing by more than 24."""
import sys, glob, os
def load(p):
    if os.path.isdir(p): p = sorted(glob.glob(os.path.join(p, 'display_*.raw')))[-1]
    return open(p, 'rb').read(), p
import array
a, pa = load(sys.argv[1]); b, pb = load(sys.argv[2])
n = min(len(a), len(b)) // 4
try:
    import numpy as np
    x = np.frombuffer(a[:n*4], np.uint8).reshape(-1, 4)[:, :3].astype(int); y = np.frombuffer(b[:n*4], np.uint8).reshape(-1, 4)[:, :3].astype(int)
    d = abs(x - y); print(f"{pa} vs {pb}: sizes {len(a)} {len(b)} mean {d.mean():.3f} big {(d.max(axis=1) > 24).mean()*100:.3f}%")
except ImportError:
    s = big = 0
    for i in range(0, n*4, 4*7):
        m = max(abs(a[i+k]-b[i+k]) for k in range(3)); s += m; big += m > 24
    c = len(range(0, n*4, 28)); print(f"{pa} vs {pb}: sizes {len(a)} {len(b)} mean-max {s/c:.3f} big {big/c*100:.3f}% (sampled)")
