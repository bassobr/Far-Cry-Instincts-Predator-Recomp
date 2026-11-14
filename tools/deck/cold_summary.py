#!/usr/bin/env python3
"""Summary of cold first visits (tools/deck/cold.sh) as a Markdown table.

  cold_summary.py <runs_dir> [--match <regex>]

Per run (predator.log, logs/frames.csv from FCP_TRACE_FRAMES=1, env.txt, sensors.csv):
  prewarm    "renderer: prewarm done" (pipelines, ms) and whether the shipped list was used
  level      level frames from level second 0 to the end: spikes = frames longer than the target frame time + 8.3 ms
             (25 ms at 60, 41.7 ms at 30), hitch time = sum of (frame - target) over the spikes, largest frame,
             spikes after level second 10 (past the first picture and the loading)
  pipelines  sum of "new in interval" of the renderer statistics lines after the level start
  cpu        process CPU in the first 10 s of the run (cpustat lines, if present) and APU power mean
"""
import argparse
import csv
import os
import re
import sys

RE_T = re.compile(r"^\s*([\d.]+)\s")
RE_PRE = re.compile(r"renderer: prewarm done: (\d+) pipelines in (\d+) ms")
RE_SHIP = re.compile(r"renderer: shipped pipeline list .*?(checksum ok|not used)")
RE_NEW = re.compile(r"\(new in interval (\d+)\)")
RE_LVL = re.compile(r"gametime: level .*started at frame")


def env_of(d):
    env = {}
    try:
        for line in open(os.path.join(d, "env.txt"), encoding="utf-8", errors="replace"):
            k, _, v = line.strip().partition("=")
            env[k] = v
    except OSError:
        pass
    return env


def apu(d):
    try:
        rows = list(csv.DictReader(open(os.path.join(d, "sensors.csv"), encoding="utf-8")))
    except OSError:
        return None
    w = [float(r["apu_w"]) for r in rows if r.get("apu_w") and (r.get("gpu_busy") or "0") not in ("", "0")]
    return sum(w) / len(w) if w else None


def summarize(d):
    log = os.path.join(d, "predator.log")
    if not os.path.exists(log):
        return None
    env = env_of(d)
    target = float(env.get("FCP_FPS", "30") or 30)
    tms = 1000.0 / target
    r = {"level": env.get("FCP_START_LEVEL", "-"), "fps": int(target),
         "list": "off" if env.get("FCP_RENDER_PIPELINE_SHIPPED") == "0" else "on",
         "prio": "normal" if env.get("FCP_RENDER_PREWARM_LOW_PRIORITY") == "0" else "low"}
    level_t = None
    new = 0
    for line in open(log, encoding="utf-8", errors="replace"):
        m = RE_T.match(line)
        t = float(m.group(1)) if m else None
        p = RE_PRE.search(line)
        if p:
            r["prewarm"] = (int(p.group(1)), int(p.group(2)), t)
        s = RE_SHIP.search(line)
        if s:
            r["shipped"] = s.group(1)
        if level_t is None and RE_LVL.search(line):
            level_t = t
        n = RE_NEW.search(line)
        if n and level_t is not None:
            new += int(n.group(1))
    r["new"] = new if level_t is not None else None
    csvp = os.path.join(d, "logs", "frames.csv")
    if os.path.exists(csvp) and r["level"] != "-":
        ms = []
        with open(csvp, newline="", encoding="utf-8", errors="replace") as f:
            for row in csv.DictReader(f):
                try:
                    lv = float(row["level_s"])
                    if lv > 0:
                        ms.append((lv, float(row["ms"])))
                except (KeyError, ValueError):
                    pass
        lim = tms + 8.3
        sp = [(lv, v) for lv, v in ms if v > lim]
        r.update(frames=len(ms), spikes=len(sp), hitch=sum(v - tms for _, v in sp),
                 largest=max((v for _, v in ms), default=0), late=sum(1 for lv, _ in sp if lv >= 10),
                 late_ms=sum(v - tms for lv, v in sp if lv >= 10))
    r["apu"] = apu(d)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--match", default="")
    a = ap.parse_args()
    print("| Lauf | Level | Ziel | Liste | Vorwärm-Prio | Vorwärmen | Spitzen | Hängerzeit ms | größte ms | Spitzen ab 10 s (ms) | neue Pipelines im Level | APU W |")
    print("|" + "---|" * 12)
    for name in sorted(os.listdir(a.runs)):
        d = os.path.join(a.runs, name)
        if not os.path.isdir(d) or (a.match and not re.search(a.match, name)):
            continue
        r = summarize(d)
        if not r:
            continue
        pw = f"{r['prewarm'][0]} in {r['prewarm'][1] / 1000:.1f} s" if r.get("prewarm") else "-"
        lv = (f"{r['spikes']} | {r['hitch']:.0f} | {r['largest']:.0f} | {r['late']} ({r['late_ms']:.0f})"
              if "spikes" in r else "- | - | - | -")
        apu_s = f"{r['apu']:.1f}" if r.get("apu") else "-"
        print(f"| {name} | {r['level']} | {r['fps']} | {r['list']} | {r['prio']} | {pw} | {lv} | "
              f"{r['new'] if r['new'] is not None else '-'} | {apu_s} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
