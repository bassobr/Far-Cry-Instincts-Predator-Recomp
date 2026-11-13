#!/usr/bin/env python3
"""Summarises Deck runs (folders fetched from ~/predator-test/runs/<name>) like tools/perf_measure.ps1 does on the PC.

  python -I tools/deck/summarize.py [--from 15] <run folder> [...]

Per run: frames/s (mean of the FCP_FPS_STATS windows from level second --from on), worst p99, lowest 1 % low,
game/wall, CPU per frame of the process, main and render thread CPU, load time (dev start to "gametime: level ...
started"), audio underruns, and from sensors.csv over the same span the mean GPU clock, GPU busy, APU power, peak
temperatures. Prints one line per run, or a Markdown table with --md.
"""
import argparse
import csv
import os
import re
import statistics

FRAMESTAT = re.compile(r"framestat: target \S+ \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ "
                       r"p99 ([\d.]+) max ([\d.]+) \| 1% low ([\d.]+) fps \| game/wall ([\d.]+) \| level (\S+) ([\d.]+) s")
CPUWIN = re.compile(r"cpu: window [\d.]+ s \| process [\d.]+% .*\| level (\S+) ([\d.]+) s")
CPUTHR = re.compile(r"cpu:\s+([\d.]+)% \(user.*\) (.+) \[\d+\]$")
CPUTOT = re.compile(r"cpu total level: .* \| ([\d.]+) ms CPU per frame")
LOAD = re.compile(r"gametime: level .* started at frame \d+, wall ([\d.]+) s")
UNDER = re.compile(r"fcp audio: .*\| total underruns=(\d+)")
STAMP = re.compile(r"^\s*([\d.]+)\s")


def summarize(folder, start_from):
    log = os.path.join(folder, "predator.log")
    r = {"name": os.path.basename(folder.rstrip("/\\")), "fps": [], "p99": [], "max": [], "low": [], "gw": set(),
         "main": [], "render": [], "load": None, "cpf": None, "under": "-", "stamps": []}
    take = False
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = LOAD.search(line)
            if m and r["load"] is None:
                r["load"] = float(m.group(1))
            m = CPUWIN.search(line)
            if m:
                take = m.group(1) != "-" and float(m.group(2)) >= start_from
            else:
                m = CPUTHR.search(line)
                if take and m:
                    who = m.group(2)
                    if "guest 826444F0" in who:
                        r["main"].append(float(m.group(1)))
                    if "guest 82314A18" in who:
                        r["render"].append(float(m.group(1)))
            m = FRAMESTAT.search(line)
            if m and m.group(6) != "-" and float(m.group(7)) >= start_from:
                r["fps"].append(float(m.group(1))); r["p99"].append(float(m.group(2))); r["max"].append(float(m.group(3)))
                r["low"].append(float(m.group(4))); r["gw"].add(m.group(5))
                s = STAMP.match(line)
                if s:
                    r["stamps"].append(float(s.group(1)))
            m = CPUTOT.search(line)
            if m:
                r["cpf"] = float(m.group(1))
            m = UNDER.search(line)
            if m:
                r["under"] = m.group(1)
    sens = os.path.join(folder, "sensors.csv")
    r["sclk"] = r["busy"] = r["watt"] = r["tgpu"] = r["tcpu"] = r["cmhz"] = None
    if os.path.exists(sens):
        with open(sens, newline="") as f:
            rows = [x for x in csv.DictReader(f) if x.get("sclk_mhz")]
        # the sensor clock counts from the Proton start, the log clock from the game's start (about 2 s later): the
        # rows over the span of the counted frame statistics windows (5 s each)
        if r["stamps"]:
            lo, hi = min(r["stamps"]) - 5 + 2, max(r["stamps"]) + 2
            part = [x for x in rows if lo <= float(x["t"]) <= hi] or rows
        else:
            part = rows

        def mean(k):
            v = [float(x[k]) for x in part if x.get(k) not in (None, "")]
            return statistics.mean(v) if v else None

        def peak(k):
            v = [float(x[k]) for x in part if x.get(k) not in (None, "")]
            return max(v) if v else None

        r["sclk"], r["busy"], r["watt"], r["cmhz"] = mean("sclk_mhz"), mean("gpu_busy"), mean("apu_w"), mean("cpu_mhz_avg")
        r["tgpu"], r["tcpu"] = peak("gpu_temp_c"), peak("cpu_temp_c")
    return r


def fmt(r):
    f = statistics.mean(r["fps"]) if r["fps"] else 0.0
    main = statistics.mean(r["main"]) if r["main"] else 0.0
    rend = statistics.mean(r["render"]) if r["render"] else 0.0
    return {
        "run": r["name"], "fps": f"{f:.1f}", "windows": len(r["fps"]),
        "p99": f"{max(r['p99']):.1f}" if r["p99"] else "-", "max": f"{max(r['max']):.1f}" if r["max"] else "-",
        "low1": f"{min(r['low']):.1f}" if r["low"] else "-", "gw": ",".join(sorted(r["gw"])) or "-",
        "cpf": f"{r['cpf']:.2f}" if r["cpf"] else "-",
        "main": f"{main:.0f}% / {10.0 * main / f:.2f} ms" if f else "-", "render": f"{rend:.0f}%",
        "load": f"{r['load']:.1f}" if r["load"] is not None else "-", "under": r["under"],
        "sclk": f"{r['sclk']:.0f}" if r["sclk"] else "-", "busy": f"{r['busy']:.0f}" if r["busy"] is not None else "-",
        "watt": f"{r['watt']:.1f}" if r["watt"] else "-", "cmhz": f"{r['cmhz']:.0f}" if r["cmhz"] else "-",
        "temp": f"{r['tgpu']:.0f}/{r['tcpu']:.0f}" if r["tgpu"] else "-",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="start_from", type=float, default=15.0)
    ap.add_argument("--md", action="store_true")
    ap.add_argument("folders", nargs="+")
    a = ap.parse_args()
    rows = [fmt(summarize(d, a.start_from)) for d in a.folders]
    if a.md:
        cols = ["run", "fps", "p99", "max", "low1", "gw", "cpf", "main", "render", "load", "under", "sclk", "busy",
                "watt", "cmhz", "temp"]
        print("| " + " | ".join(cols) + " |")
        print("|" + "---|" * len(cols))
        for r in rows:
            print("| " + " | ".join(str(r[c]) for c in cols) + " |")
    else:
        for r in rows:
            print(", ".join(f"{k} {v}" for k, v in r.items()))


if __name__ == "__main__":
    main()
