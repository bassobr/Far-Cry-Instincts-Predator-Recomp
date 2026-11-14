"""Frame-time distribution and spike causes from FCP_TRACE_FRAMES (frames.csv) and the matching predator.log.

  python -I tools/frame_stats.py <frames.csv> [<predator.log>] [--from 5] [--dev 4] [--list 12]

Prints for the level frames from --from level seconds on: p50/p95/p99/p99.9/max, frames over 20 and 25 ms,
frame-to-frame change (mean, p99, how many changes over --dev ms), and per spike (frame longer than median + --dev)
the main-thread share that grew most against the median frame:
  cpu      the main thread ran longer (game code; see the spike profile, FCP_PROFILE)
  wait     time in kernel waits (Wait/Delay imports: events, mutants, semaphores, the vblank wait)
  file     file imports (NtReadFile, NtCreateFile, ...)
  mem      memory imports (Mm*, NtAllocateVirtualMemory, ...)
  audio    XMA/XAudio imports
  sync     release/set imports (mutants, events, critical sections)
  offcpu   neither running nor inside an import (preempted, or a host wait outside the imports)
and, from the log, the renderer share of the slow frame that ended within 40 ms before the hitch line (pipe, tex,
buf, draw, gpu, out), as tools/hitch_measure.ps1. The first frames after the level start (level < --from) are listed
separately (loading screen).
"""

import csv
import re
import statistics
import sys


def pct(v, p):
    if not v:
        return 0.0
    s = sorted(v)
    i = min(len(s) - 1, int(len(s) * p))
    return s[i]


def read_frames(path):
    rows = []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    "frame": int(r["frame"]), "wall": float(r["wall_s"]), "level": float(r["level_s"]),
                    "ms": float(r["ms"]), "dt": float(r["game_dt_ms"]), "cpu": float(r["cpu_ms"]),
                    "imp": float(r["import_ms"]), "wait": float(r["wait_ms"]), "file": float(r["file_ms"]),
                    "mem": float(r["mem_ms"]), "audio": float(r["audio_ms"]), "sync": float(r["sync_ms"]),
                    "other": float(r["other_ms"]), "longest": r["longest"], "longest_ms": float(r["longest_ms"]),
                    "lr": r["longest_lr"],
                })
            except (KeyError, ValueError):
                pass
    return rows


RE_T = re.compile(r"^\s*([\d.]+)\s")
RE_SLOW = re.compile(r"renderer: slow frame \d+: ([\d.]+) ms, draws ([\d.]+) ms \(of which pipelines (\d+) / ([\d.]+) ms, "
                     r"textures \d+ / ([\d.]+) ms, buffers ([\d.]+) ms / \d+ KB\), GPU wait ([\d.]+) ms")
RE_HITCH = re.compile(r"hitch: frame (\d+) took ([\d.]+) ms")


def read_log(path):
    """frame -> (renderer category, ms of it, hitch line text)"""
    out = {}
    last = None
    last_t = -1.0
    level_started = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_T.match(line)
            t = float(m.group(1)) if m else -1.0
            if "gametime: level" in line and "started at frame" in line:
                level_started += 1
            s = RE_SLOW.search(line)
            if s:
                wall, dr, pp, tx, bf, gw = (float(s.group(i)) for i in (1, 2, 4, 5, 6, 7))
                last = {"pipe": pp, "tex": tx, "buf": bf, "draw": max(0.0, dr - pp - tx - bf), "gpu": gw,
                        "out": max(0.0, wall - dr - gw - max(0.0, pp - dr))}
                last_t = t
                continue
            h = RE_HITCH.search(line)
            if h:
                frame = int(h.group(1))
                cat, cms = "game", 0.0
                if last and t - last_t <= 0.040:
                    cat = max(last, key=last.get)
                    cms = last[cat]
                    last = None
                out[frame] = (cat, cms, line.strip())
    return out, level_started


def main():
    args = [a for a in sys.argv[1:]]
    opts = {"--from": 5.0, "--dev": 4.0, "--list": 12}
    files = []
    i = 0
    while i < len(args):
        if args[i] in opts:
            opts[args[i]] = type(opts[args[i]])(args[i + 1])
            i += 2
        else:
            files.append(args[i])
            i += 1
    if not files:
        print(__doc__)
        return 2
    rows = read_frames(files[0])
    hitches, starts = ({}, 0)
    if len(files) > 1:
        hitches, starts = read_log(files[1])
    # level frames: level time grows from 0 at the level start (0 before; it restarts at a checkpoint reload)
    level = [r for r in rows if r["level"] >= opts["--from"]]
    early = [r for r in rows if 0 < r["level"] < opts["--from"]]
    if not level:
        print("no level frames")
        return 1
    ms = [r["ms"] for r in level]
    med = statistics.median(ms)
    dev = opts["--dev"]
    diffs = [abs(level[k]["ms"] - level[k - 1]["ms"]) for k in range(1, len(level))
             if level[k]["frame"] == level[k - 1]["frame"] + 1]
    print(f"{files[0]}: {len(level)} level frames from level {opts['--from']:.0f} s "
          f"({level[0]['level']:.0f}..{level[-1]['level']:.0f} s), {len(level) / (sum(ms) / 1000):.2f} fps")
    print(f"  ms p50 {pct(ms, .5):.2f} p95 {pct(ms, .95):.2f} p99 {pct(ms, .99):.2f} p99.9 {pct(ms, .999):.2f} "
          f"max {max(ms):.1f} | mean {statistics.mean(ms):.2f} sd {statistics.pstdev(ms):.2f} | "
          f">20 ms {sum(v > 20 for v in ms)} >25 ms {sum(v > 25 for v in ms)} >33.4 ms {sum(v > 33.4 for v in ms)}")
    print(f"  frame-to-frame |d| mean {statistics.mean(diffs):.2f} p99 {pct(diffs, .99):.2f} max {max(diffs):.1f} | "
          f"|d|>{dev:.0f} ms {sum(d > dev for d in diffs)} | main cpu p50 {pct([r['cpu'] for r in level], .5):.2f} "
          f"p99 {pct([r['cpu'] for r in level], .99):.2f} ms")
    keys = ["cpu", "wait", "file", "mem", "audio", "sync", "offcpu"]

    def parts(r):
        return {"cpu": r["cpu"], "wait": r["wait"], "file": r["file"], "mem": r["mem"], "audio": r["audio"],
                "sync": r["sync"], "offcpu": max(0.0, r["ms"] - r["cpu"] - r["imp"])}

    base = {k: statistics.median([parts(r)[k] for r in level]) for k in keys}
    print("  median frame: " + ", ".join(f"{k} {base[k]:.2f}" for k in keys))
    spikes = [r for r in level if r["ms"] > med + dev]
    by = {}
    by_r = {}
    longest = {}
    for r in spikes:
        p = parts(r)
        grow = {k: p[k] - base[k] for k in keys}
        c = max(grow, key=grow.get)
        by[c] = by.get(c, 0) + 1
        h = hitches.get(r["frame"])
        rc = h[0] if h else "-"
        by_r[rc] = by_r.get(rc, 0) + 1
        if r["longest_ms"] > 2.0:
            k = f"{r['longest']}@{r['lr']}"
            longest[k] = longest.get(k, 0) + 1
        r["_cause"], r["_grow"], r["_render"] = c, grow[c], (h[0] + f" {h[1]:.0f}") if h else "-"
    print(f"  spikes (> median {med:.2f} + {dev:.0f} ms): {len(spikes)} | main-thread share: "
          + ", ".join(f"{k} {v}" for k, v in sorted(by.items(), key=lambda kv: -kv[1]))
          + " | renderer: " + ", ".join(f"{k} {v}" for k, v in sorted(by_r.items(), key=lambda kv: -kv[1])))
    if longest:
        print("  longest import call (> 2 ms) in spikes: "
              + ", ".join(f"{k} {v}" for k, v in sorted(longest.items(), key=lambda kv: -kv[1])[:8]))
    for r in sorted(spikes, key=lambda r: -r["ms"])[: opts["--list"]]:
        print(f"    frame {r['frame']} level {r['level']:.2f} s: {r['ms']:.1f} ms (dt {r['dt']:.1f}) cpu {r['cpu']:.1f} "
              f"wait {r['wait']:.1f} file {r['file']:.1f} mem {r['mem']:.1f} audio {r['audio']:.1f} sync {r['sync']:.1f} "
              f"off {max(0.0, r['ms'] - r['cpu'] - r['imp']):.1f} | cause {r['_cause']} +{r['_grow']:.1f} | "
              f"renderer {r['_render']} | longest {r['longest']} {r['longest_ms']:.1f} lr {r['lr']}")
    if early:
        e = sorted(early, key=lambda r: -r["ms"])[:4]
        print("  first frames after the level start (< %.0f s): " % opts["--from"]
              + ", ".join(f"{r['level']:.2f} s {r['ms']:.0f} ms" for r in e))
    if starts > 1:
        print(f"  level (re)starts in the log: {starts}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
