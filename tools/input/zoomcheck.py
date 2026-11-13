import math, re, sys
# usage: zoomcheck.py <log> <mode fov|tan|off> <sensitivity>
pat = re.compile(r"look #\d+ ([\d.]+) s .*mouse ([+-][\d.]+) ([+-][\d.]+) counts.*gain ([\d.]+), fov ([\d.]+) zoom ([\d.]+) -> ([+-][\d.]+) ([+-][\d.]+) deg")
mode, sens = sys.argv[2], float(sys.argv[3])
n = bad = 0
total = expect = 0.0
fovs = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    t, mx, my, gain, fov, zoom, y, p = map(float, m.groups())
    if mx == 0:
        continue
    normal = math.pi / 2
    if 0.05 < fov < normal - 1e-3:
        f = fov * 2 / math.pi if mode == 'fov' else math.tan(fov / 2) if mode == 'tan' else 1.0
        z = min(max(f * sens, 0.01), 10)
    else:
        z = 1.0
    n += 1
    fovs.append(fov)
    if abs(z - zoom) > 1e-4:
        bad += 1
    total += y
    expect += mx * 0.1 * z
print(f"{mode} x {sens}: {n} frames with motion, fov {min(fovs):.4f}-{max(fovs):.4f} rad, zoom off formula in {bad}; "
      f"yaw {total:+.4f} deg, from formula {expect:+.4f}")
