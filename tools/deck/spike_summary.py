#!/usr/bin/env python3
"""Compact table of the warm spike runs on the Deck (tools/deck/series_v012.sh part 2).

  spike_summary.py <runs_dir> [--match <regex>] [--from 5]

Per run (logs/frames.csv from FCP_TRACE_FRAMES=1, predator.log with REX_VSYNC_STATS=1):
  frames   fps, p99, p99.9, max, frames over 1.5 target frame times
  spikes   frames longer than median + 4 ms by the longest import call inside them (as tools/frame_stats.py):
           input = XamInputGetState/SetState, vblank = NtWaitForSingleObjectEx at 0x82649650 (the frame wait),
           lock = RtlEnterCriticalSection, mem = Mm*, none = no import call over 2 ms (time outside the imports)
  big      frames over 50 ms with level second and the longest import call
  vsync    blanks per 10 s (min-max), late mean (max of the windows), worst late, windows' count over 2 / 4 ms
Under Wine QueryThreadCycleTime returns 0: the main thread's CPU time is missing, so "outside the imports" also holds
the time the thread computed.
"""
import argparse
import csv
import os
import re
import statistics
import sys

RE_VS = re.compile(r"vsync: (\d+) blanks in [\d.]+ s, late mean ([\d.]+) ms, max ([\d.]+) ms, >2 ms (\d+), >4 ms (\d+)")


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * p))] if s else 0.0


def kind(r):
    if r["longest_ms"] <= 2.0:
        return "none"
    n = r["longest"]
    if n.startswith("XamInput"):
        return "input"
    if n == "NtWaitForSingleObjectEx" and r["lr"].upper().endswith("82649650"):
        return "vblank"
    if n.startswith("RtlEnterCritical"):
        return "lock"
    if n.startswith("Mm") or "VirtualMemory" in n:
        return "mem"
    return n


def summarize(d, start):
    p = os.path.join(d, "logs", "frames.csv")
    if not os.path.exists(p):
        return None
    rows = []
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                if float(r["level_s"]) >= start:
                    rows.append({"level": float(r["level_s"]), "ms": float(r["ms"]), "longest": r["longest"],
                                 "longest_ms": float(r["longest_ms"]), "lr": r["longest_lr"]})
            except (KeyError, ValueError):
                pass
    if not rows:
        return None
    ms = [r["ms"] for r in rows]
    med = statistics.median(ms)
    sp = [r for r in rows if r["ms"] > med + 4]
    kinds = {}
    for r in sp:
        k = kind(r)
        kinds[k] = kinds.get(k, 0) + 1
    big = sorted((r for r in rows if r["ms"] > 50), key=lambda r: r["level"])
    vs = []
    for line in open(os.path.join(d, "predator.log"), encoding="utf-8", errors="replace"):
        m = RE_VS.search(line)
        if m:
            vs.append(tuple(float(x) for x in m.groups()))
    return {"fps": len(ms) / (sum(ms) / 1000), "p99": pct(ms, .99), "p999": pct(ms, .999), "max": max(ms),
            "long": sum(v > 1.5 * med for v in ms), "n": len(ms), "spikes": len(sp), "kinds": kinds, "big": big, "vs": vs}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--match", default="")
    ap.add_argument("--from", dest="start", type=float, default=5.0)
    a = ap.parse_args()
    print("| Lauf | Bilder/s | p99 | p99,9 | max ms | > 1,5 × Median | Spitzen (> Median + 4 ms) nach längstem Import | Bilder > 50 ms (Levelsekunde ms Import) | Vblanks/10 s | spät Mittel / max ms | Fenster > 2 / > 4 ms |")
    print("|" + "---|" * 11)
    for name in sorted(os.listdir(a.runs)):
        d = os.path.join(a.runs, name)
        if not os.path.isdir(d) or (a.match and not re.search(a.match, name)):
            continue
        r = summarize(d, a.start)
        if not r:
            continue
        kinds = ", ".join(f"{k} {v}" for k, v in sorted(r["kinds"].items(), key=lambda kv: -kv[1]))
        big = "; ".join(f"{b['level']:.1f} {b['ms']:.0f} {kind(b)}" for b in r["big"]) or "-"
        vs = r["vs"]
        if vs:
            vsc = f"{min(v[0] for v in vs):.0f}-{max(v[0] for v in vs):.0f} | {max(v[1] for v in vs):.2f} / {max(v[2] for v in vs):.1f} | {sum(v[3] for v in vs):.0f} / {sum(v[4] for v in vs):.0f}"
        else:
            vsc = "- | - | -"
        print(f"| {name} | {r['fps']:.1f} | {r['p99']:.1f} | {r['p999']:.1f} | {r['max']:.0f} | {r['long']} | {r['spikes']}: {kinds} | {big} | {vsc} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
