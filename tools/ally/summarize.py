#!/usr/bin/env python3
"""Summarises Ally runs (folders fetched from %USERPROFILE%\\predator-test\\runs\\<name>), as tools/deck/summarize.py.

  python -I tools/ally/summarize.py [--from 15] [--md] [--threads] <run folder> [...]

Frame and CPU values come from tools/deck/summarize.py (same log lines). The Ally's sensors.csv (tools/ally/run.ps1,
WMI without installed tools) has no GPU clock or APU power: over the span of the counted statistics windows it gives
the GPU 3D utilisation of the process, the effective CPU clock (performance % x base clock), total CPU load, and the
battery discharge rate (0 on mains power). --threads adds the busiest threads (mean CPU % over the counted windows).
"""
import argparse
import collections
import csv
import importlib.util
import os
import re
import statistics

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("deck_summarize", os.path.join(_here, "..", "deck", "summarize.py"))
deck = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(deck)


def threads(folder, start_from, top=6):
    per = collections.defaultdict(list)
    take = False
    with open(os.path.join(folder, "predator.log"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = deck.CPUWIN.search(line)
            if m:
                take = m.group(1) != "-" and float(m.group(2)) >= start_from
                continue
            m = deck.CPUTHR.search(line.rstrip("\n"))
            if take and m:
                per[re.sub(r"\s+", " ", m.group(2)).strip()].append(float(m.group(1)))
    rows = sorted(((statistics.mean(v), k) for k, v in per.items()), reverse=True)[:top]
    return "; ".join(f"{k} {v:.0f}%" for v, k in rows)


def ally_sensors(folder, stamps):
    sens = os.path.join(folder, "sensors.csv")
    out = {"gpu": "-", "cmhz": "-", "cpu": "-", "watt": "-", "batt": "-"}
    if not os.path.exists(sens):
        return out
    with open(sens, newline="", encoding="utf-8-sig") as f:
        rows = [x for x in csv.DictReader(f) if x.get("t")]
    # the sensor clock counts from the process start, as the log clock (within a second)
    part = rows
    if stamps:
        lo, hi = min(stamps) - 5, max(stamps)
        part = [x for x in rows if lo <= float(x["t"]) <= hi] or rows

    def mean(k):
        v = [float(x[k]) for x in part if x.get(k) not in (None, "")]
        return statistics.mean(v) if v else None

    g, c, u, w = mean("gpu_busy"), mean("cpu_mhz_avg"), mean("cpu_util"), mean("batt_w")
    out["gpu"] = f"{g:.0f}" if g is not None else "-"
    out["cmhz"] = f"{c:.0f}" if c is not None else "-"
    out["cpu"] = f"{u:.0f}" if u is not None else "-"
    out["watt"] = f"{w:.1f}" if w else "-"
    b = [x.get("batt_pct") for x in part if x.get("batt_pct")]
    out["batt"] = f"{b[0]}->{b[-1]}" if b else "-"
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="start_from", type=float, default=15.0)
    ap.add_argument("--md", action="store_true")
    ap.add_argument("--threads", action="store_true")
    ap.add_argument("folders", nargs="+")
    a = ap.parse_args()
    rows = []
    for d in a.folders:
        if not os.path.exists(os.path.join(d, "predator.log")):
            continue
        r = deck.summarize(d, a.start_from)
        row = deck.fmt(r)
        row.update(ally_sensors(d, r["stamps"]))
        if a.threads:
            row["threads"] = threads(d, a.start_from)
        rows.append(row)
    cols = ["run", "fps", "windows", "p99", "max", "low1", "gw", "cpf", "main", "render", "load", "under", "gpu",
            "cmhz", "cpu", "watt", "batt"] + (["threads"] if a.threads else [])
    if a.md:
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for r in rows:
            print("| " + " | ".join(str(r[c]) for c in cols) + " |")
    else:
        for r in rows:
            print(", ".join(f"{c} {r[c]}" for c in cols))


if __name__ == "__main__":
    main()
