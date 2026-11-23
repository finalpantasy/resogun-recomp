"""Compare two 1920x1080 BGRA frame dumps of the Set Up Tee Shot view.
Usage: python -I compare-dump.py A.raw B.raw
Prints the mean abs diff (0-255) over two sets of regions: HUD (hole badge, par, player cards: static), and scenery
(trees, hills, sky: static but lit by a random time of day / weather, so it can differ between boots).
The golfers on the tee, the ball, the wind arrow and the menu are left out."""
import sys
HUD = [(20, 230, 10, 90), (40, 140, 520, 800)]
SCENERY = [(390, 690, 120, 300), (1200, 1680, 180, 330), (700, 1100, 40, 110)]  # x0, x1, y0, y1
a, b = (open(p, 'rb').read() for p in sys.argv[1:3])
def diff(regions):
    tot = n = 0
    for x0, x1, y0, y1 in regions:
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                o = (y * 1920 + x) * 4
                tot += sum(abs(a[o + k] - b[o + k]) for k in range(3)) / 3; n += 1
    return tot / n
print(f"HUD diff {diff(HUD):.2f}  scenery diff {diff(SCENERY):.2f}")
