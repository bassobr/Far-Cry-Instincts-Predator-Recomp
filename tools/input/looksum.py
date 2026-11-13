import re, sys
# usage: looksum.py <log> [t0-t1 ...]  sums the mouse degrees of the look trace per script-time window
pat = re.compile(r"look #\d+ ([\d.]+) s frame \d+ dt ([\d.]+) ms:.*gain ([\d.]+), fov ([\d.]+) zoom ([\d.]+) -> ([+-][\d.]+) ([+-][\d.]+) deg")
wins = [tuple(map(float, w.split('-'))) for w in sys.argv[2:]] or [(0, 1e9)]
sums = [[0.0, 0.0, 0, 0.0, 1e9, 0.0] for _ in wins]  # yaw, pitch, frames, gain max, first, last
dts = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    t, dt, gain, fov, zoom, y, p = map(float, m.groups())
    dts.append(dt)
    for w, s in zip(wins, sums):
        if w[0] <= t < w[1] and (y or p):
            s[0] += y; s[1] += p; s[2] += 1; s[3] = max(s[3], gain); s[4] = min(s[4], t); s[5] = max(s[5], t)
for w, s in zip(wins, sums):
    print(f"{w[0]:6.2f}-{w[1]:6.2f}: yaw {s[0]:+10.4f} pitch {s[1]:+10.4f} frames {s[2]:4d} max gain {s[3]:.4f} span {s[4]:.3f}-{s[5]:.3f}")
mid = sorted(dts)[len(dts)//2] if dts else 0
print(f"look inputs {len(dts)}, median dt {mid:.2f} ms")
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    if 'input summary' in line or 'fps:' in line.lower() and 'p99' in line:
        print(line.strip()[:260])
