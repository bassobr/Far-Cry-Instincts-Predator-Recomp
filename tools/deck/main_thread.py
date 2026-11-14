#!/usr/bin/env python3
"""Main thread per frame from predator.log (FCP_CPU_STATS, FCP_FPS_STATS), as tools/perf_measure.ps1 computes it.

  main_thread.py <runs_dir> [--match <regex>] [--from 15]

Per run: frames/s (mean of the framestat windows from level second --from on), main thread load (thread "guest
826444F0") and its ms per frame (10 * load % / fps), render thread load (guest 82314A18), worst p99. The last line per
group (run name without the trailing _<n>) is the mean over its runs.
"""
import argparse
import os
import re
import sys

CPUW = re.compile(r"cpu: window [\d.]+ s \| process [\d.]+% .*\| level (\S+) ([\d.]+) s")
CPUT = re.compile(r"cpu:\s+([\d.]+)% \(user.*\) (.+) \[\d+\]$")
FS = re.compile(r"framestat: target \S+ \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ p99 ([\d.]+) .*\| level (\S+) ([\d.]+) s")


def summarize(path, start):
    fps, p99, main, render, take = [], [], [], [], False
    for line in open(path, encoding="utf-8", errors="replace"):
        m = CPUW.search(line)
        if m:
            take = m.group(1) != "-" and float(m.group(2)) >= start
            continue
        m = CPUT.search(line.rstrip())
        if m and take:
            if "guest 826444F0" in m.group(2):
                main.append(float(m.group(1)))
            elif "guest 82314A18" in m.group(2):
                render.append(float(m.group(1)))
            continue
        m = FS.search(line)
        if m and m.group(3) != "-" and float(m.group(4)) >= start:
            fps.append(float(m.group(1)))
            p99.append(float(m.group(2)))
    if not fps or not main:
        return None
    f = sum(fps) / len(fps)
    mm = sum(main) / len(main)
    return {"fps": f, "main": mm, "main_ms": 10.0 * mm / f, "render": sum(render) / len(render) if render else 0,
            "p99": max(p99)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--match", default="")
    ap.add_argument("--from", dest="start", type=float, default=15.0)
    a = ap.parse_args()
    groups = {}
    print("| Lauf | Bilder/s | Hauptthread % | Hauptthread ms/Bild | Render-Thread % | p99 max ms |")
    print("|---|---:|---:|---:|---:|---:|")
    for name in sorted(os.listdir(a.runs)):
        p = os.path.join(a.runs, name, "predator.log")
        if not os.path.exists(p) or (a.match and not re.search(a.match, name)):
            continue
        r = summarize(p, a.start)
        if not r:
            continue
        print(f"| {name} | {r['fps']:.1f} | {r['main']:.1f} | {r['main_ms']:.2f} | {r['render']:.1f} | {r['p99']:.1f} |")
        groups.setdefault(re.sub(r"_[^_]+$", "", name), []).append(r)
    for g, rs in groups.items():
        n = len(rs)
        print(f"| **{g}** (Mittel {n}) | {sum(r['fps'] for r in rs) / n:.1f} | {sum(r['main'] for r in rs) / n:.1f} | "
              f"{sum(r['main_ms'] for r in rs) / n:.2f} | {sum(r['render'] for r in rs) / n:.1f} | - |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
