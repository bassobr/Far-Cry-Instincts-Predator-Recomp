#!/usr/bin/env python3
"""issue1_pick.py <views.env> <out_dir> <label>=<run_dir> [<label>=<run_dir> ...]: picks, per view of
issue1_views.py, the frame dump in the middle of the view's hold window of each run and writes
<out_dir>/<view>_<label>.jpg (alpha dropped), plus <out_dir>/<view>_pair.jpg (runs side by side, half size) when
more than one run is given. Level time of a dump: from the log time of the nearest FCP_TEST_TELEPORT line (they come
every 0.5 s during a hold). Needs Pillow."""
import os
import re
import sys
from PIL import Image, ImageDraw

env = dict(l.rstrip("\n").split("=", 1) for l in open(sys.argv[1]) if "=" in l)
views = []
for item in env["ISSUE1_VIEWS"].split(","):
    name, win = item.split(":")
    a, b = (float(x) for x in win.split("-"))
    views.append((name, a, b))
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
runs = [a.split("=", 1) for a in sys.argv[3:]]
TP = re.compile(r"^\s*([\d.]+)\s+\d+ FCP_TEST_TELEPORT: level '[^']+' at ([\d.]+) s")
DUMP = re.compile(r"^\s*([\d.]+)\s+\d+ renderer: dump frame (\d+) -> .*[\\/](f_\d+\.png)")
picked = {}
for label, rd in runs:
    tps, dumps = [], []
    for line in open(os.path.join(rd, "predator.log"), errors="replace"):
        m = TP.match(line)
        if m:
            tps.append((float(m.group(1)), float(m.group(2))))
            continue
        m = DUMP.match(line)
        if m:
            dumps.append((float(m.group(1)), int(m.group(2)), m.group(3)))
    lv = []  # (level time, frame, file) for dumps with a teleport within 0.6 s
    for t, n, f in dumps:
        best = min(tps, key=lambda p: abs(p[0] - t)) if tps else None
        if best and abs(best[0] - t) <= 0.6:
            lv.append((best[1] + (t - best[0]), n, f))
    for name, a, b in views:
        mid = (a + b) / 2
        cand = [d for d in lv if a <= d[0] <= b]
        if not cand:
            print(f"{label} {name}: no dump in {a}-{b}")
            continue
        d = min(cand, key=lambda d: abs(d[0] - mid))
        src = os.path.join(rd, "logs", d[2])
        im = Image.open(src).convert("RGB")
        p = os.path.join(out, f"{name}_{label}.jpg")
        im.save(p, quality=90)
        picked.setdefault(name, []).append((label, im))
        print(f"{label} {name}: frame {d[1]} level {d[0]:.2f} s -> {p}")
if len(runs) > 1:
    for name, ims in picked.items():
        w = 960
        tiles = [(l, im.resize((w, int(im.height * w / im.width)))) for l, im in ims]
        h = max(t[1].height for t in tiles)
        sheet = Image.new("RGB", (w * len(tiles), h))
        dr = ImageDraw.Draw(sheet)
        for i, (l, im) in enumerate(tiles):
            sheet.paste(im, (i * w, 0))
            dr.rectangle([i * w, 0, i * w + 8 * len(l) + 60, 14], fill=(0, 0, 0))
            dr.text((i * w + 2, 1), f"{l} {name}", fill=(255, 255, 0))
        sheet.save(os.path.join(out, f"{name}_pair.jpg"), quality=88)
