#!/usr/bin/env python3
"""Summary of frame pacing runs (tools/deck/pacing.sh) as a Markdown table.

  pacing_summary.py <runs_dir> [--from 10] [--match <regex>]

Per run (folder with predator.log, env.txt, sensors.csv) from level second --from on:
  game    framestat lines: fps, worst p99 of the 5-s windows, long frames per minute (over 1.5 target frame times,
          "long:" field; older logs: ">25:", oldest: the bins from 30 ms)
  display presentstat lines (present wait): frames shown per second, share of frames by refreshes on the display
          (1/2/3/4+), cadence changes per second (a frame that stays a different number of refreshes than the one
          before: judder), long frames per minute (more refreshes than the target needs), latency present->display
  pacing  "on n" when display pacing was active, refresh measured, APU power mean (sensors.csv)
"""
import argparse
import math
import os
import re
import sys

FS = re.compile(r"^\s*([\d.]+)\s+\d+ framestat: target (\S+) \| (\d+) frames in ([\d.]+) s = [\d.]+ fps \| ms mean [\d.]+ "
                r"p50 [\d.]+ p99 ([\d.]+) max ([\d.]+) .*\| level (\S+) ([\d.]+) s \| <8:(\d+) 8-12:(\d+) 12-20:(\d+) "
                r"20-30:(\d+) 30-40:(\d+) 40-60:(\d+) >=60:(\d+)(?: >25:(\d+))?(?: long:(\d+))?")
PS = re.compile(r"^\s*([\d.]+)\s+\d+ presentstat: (\d+) on the display in ([\d.]+) s = [\d.]+/s \| refresh ([\d.]+) ms \| "
                r"pacing (on, n |off )(\d+) \| display ms p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) \| refreshes per frame "
                r"1:(\d+) 2:(\d+) 3:(\d+) 4\+:(\d+) \| cadence changes (\d+) \| latency ms mean ([\d.]+) p99 ([\d.]+)")


def env_of(d):
    env = {}
    try:
        for line in open(os.path.join(d, "env.txt"), encoding="utf-8", errors="replace"):
            k, _, v = line.strip().partition("=")
            env[k] = v
    except OSError:
        pass
    return env


def apu_watts(d):
    try:
        rows = open(os.path.join(d, "sensors.csv"), encoding="utf-8").read().splitlines()
    except OSError:
        return None, None
    head = rows[0].split(",")
    w, hz = [], []
    for r in rows[1:]:
        c = r.split(",")
        if len(c) != len(head):
            continue
        row = dict(zip(head, c))
        try:
            if float(row.get("gpu_busy", 0) or 0) > 0:
                w.append(float(row["apu_w"]))
            if row.get("refresh_hz"):
                hz.append(row["refresh_hz"])
        except ValueError:
            pass
    return (sum(w) / len(w) if w else None), (max(set(hz), key=hz.count) if hz else None)


def summarize(d, start):
    log = os.path.join(d, "predator.log")
    if not os.path.exists(log):
        return None
    fs, ps, level0 = [], [], None
    for line in open(log, encoding="utf-8", errors="replace"):
        m = FS.match(line)
        if m:
            t, lv, lt = float(m.group(1)), m.group(7), float(m.group(8))
            if lv != "-" and level0 is None:
                level0 = t - lt
            fs.append((t, m))
            continue
        m = PS.match(line)
        if m:
            ps.append((float(m.group(1)), m))
    if level0 is None:
        return None
    t_from = level0 + start
    row = {"level": None}
    frames = secs = over = spikes = 0
    p99 = 0.0
    for t, m in fs:
        if m.group(7) == "-" or t - float(m.group(4)) < t_from - 0.5:
            continue
        row["level"] = m.group(7)
        row["target"] = m.group(2)
        frames += int(m.group(3))
        secs += float(m.group(4))
        p99 = max(p99, float(m.group(5)))
        spikes += int(m.group(15))  # frames of 60 ms or more: hitches whatever the target
        # long frames (over 1.5 target periods); older logs: over 25 ms, oldest: the bins from 30 ms
        if m.group(17) is not None:
            over += int(m.group(17))
        elif m.group(16) is not None:
            over += int(m.group(16))
        else:
            over += int(m.group(13)) + int(m.group(14)) + int(m.group(15))
    if not secs:
        return None
    row.update(fps=frames / secs, p99=p99, over25=over / secs * 60, spikes=spikes / secs * 60, secs=secs)
    shown = dsecs = changes = 0
    ref = [0, 0, 0, 0]
    lat = []
    refresh, pacing = None, []
    for t, m in ps:
        if t - float(m.group(3)) < t_from - 0.5:
            continue
        shown += int(m.group(2))
        dsecs += float(m.group(3))
        for i in range(4):
            ref[i] += int(m.group(10 + i))
        changes += int(m.group(14))
        lat.append((float(m.group(15)), int(m.group(2))))
        refresh = float(m.group(4))
        pacing.append(m.group(6) if m.group(5).startswith("on") else "off")
    if dsecs:
        n_ref = sum(ref) or 1
        target = row.get("target", "")
        want = None
        if refresh and target.isdigit():
            want = 1000.0 / float(target) / refresh
        # long frame: more refreshes than the target needs (60 on 90 Hz needs 1.5: 2 is normal, 3 is long)
        need = math.ceil(want - 0.05) if want else 1
        long_frames = sum(ref[i] for i in range(4) if i + 1 > need)
        row.update(shown=shown / dsecs, ref=[100.0 * r / n_ref for r in ref], changes=changes / dsecs,
                   long=long_frames / dsecs * 60, latency=sum(a * b for a, b in lat) / max(1, sum(b for _, b in lat)),
                   refresh=1000.0 / refresh if refresh else None,
                   pacing=max(set(pacing), key=pacing.count) if pacing else "-")
    row["apu"], row["gs_hz"] = apu_watts(d)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs")
    ap.add_argument("--from", dest="start", type=float, default=10.0)
    ap.add_argument("--match", default="")
    a = ap.parse_args()
    out = ["| Lauf | Ziel | Takt | Bilder/s | p99 ms | Spiel lang/min | >=60 ms/min | angezeigt/s | Refreshes 1/2/3/4+ % | Taktwechsel/s | "
           "lange Bilder/min | Latenz ms | Hz | APU W |", "|" + "---|" * 14]
    for name in sorted(os.listdir(a.runs)):
        d = os.path.join(a.runs, name)
        if not os.path.isdir(d) or (a.match and not re.search(a.match, name)):
            continue
        r = summarize(d, a.start)
        if not r:
            continue
        env = env_of(d)
        mode = env.get("FCP_FPS_PACING", "-")
        if "shown" in r:
            disp = (f"{r['shown']:.1f} | {'/'.join(f'{x:.0f}' for x in r['ref'])} | {r['changes']:.1f} | {r['long']:.0f} | "
                    f"{r['latency']:.1f} | {r['refresh']:.1f} |" if r.get('refresh') else "- | - | - | - | - | - |")
            mode += f" ({'on ' + r['pacing'] if r['pacing'] != 'off' else 'off'})" if mode == "display" else ""
        else:
            disp = "- | - | - | - | - | - |"
        apu = f"{r['apu']:.1f}" if r.get("apu") else "-"
        out.append(f"| {name} | {r.get('target', '-')} | {mode} | {r['fps']:.1f} | {r['p99']:.1f} | {r['over25']:.0f} | {r['spikes']:.1f} | {disp} {apu} |")
    print("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
