#!/usr/bin/env python3
"""sound_reads.py: map read accesses to dvdsound.dat (FCP_TRACE_SOUND_READS=1) to the archive entries.

Reads the lines "sound read t=… handle=… off=… len=…" from predator.log, maps each read range via
dvdsound.fat to the entries and classifies the entries by their first 4 bytes, as in the disc data
inventory: `00 18 00 00` = bank (XMA1), `00 0B 00 08` = stream
(presumably Ubi IMA ADPCM), `05 00 00 00` = large block (presumably IMA ADPCM), otherwise "other".
Output: bytes per class and time window, entries with the most bytes read, reads outside
of entries (header, directory).

Usage: python -I tools/audio/sound_reads.py <predator.log> <dvdsound.fat> [--window 10]
"""
import argparse
import bisect
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from xfat import Archive  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

RX = re.compile(r"sound read t=([\d.]+) handle=([0-9A-F]+) off=([0-9A-F]+) len=([0-9A-F]+)")
KINDS = {b"\x00\x18\x00\x00": "Bank (XMA1)", b"\x00\x0b\x00\x08": "Stream 000B0008",
         b"\x05\x00\x00\x00": "Block 05000000"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("fat")
    ap.add_argument("--window", type=float, default=10.0)
    a = ap.parse_args()
    arc = Archive(a.fat)
    entries = sorted(arc.entries, key=lambda e: e.offset)
    starts = [e.offset for e in entries]
    kind = {}
    for e in entries:
        kind[e.index] = KINDS.get(arc.raw_head(e) if hasattr(arc, "raw_head") else head(arc, e), "other")
    per_window = collections.defaultdict(collections.Counter)
    per_entry = collections.Counter()
    first_seen = {}
    outside = 0
    reads = 0
    for line in open(a.log, encoding="utf-8", errors="replace"):
        m = RX.search(line)
        if not m:
            continue
        reads += 1
        t = float(m.group(1))
        off, ln = int(m.group(3), 16), int(m.group(4), 16)
        w = int(t // a.window)
        pos, end = off, off + ln
        while pos < end:
            i = bisect.bisect_right(starts, pos) - 1
            if i < 0 or pos >= entries[i].offset + entries[i].stored:
                nxt = starts[i + 1] if i + 1 < len(starts) else end
                n = min(end, nxt) - pos
                per_window[w]["outside"] += n
                outside += n
                pos += max(n, 1)
                continue
            e = entries[i]
            n = min(end, e.offset + e.stored) - pos
            per_window[w][kind[e.index]] += n
            per_entry[e.index] += n
            first_seen.setdefault(e.index, t)
            pos += n
    classes = ["Bank (XMA1)", "Stream 000B0008", "Block 05000000", "other", "outside"]
    print(f"{reads} reads; entries in the archive: {len(entries)}")
    print()
    print("| t [s] | " + " | ".join(f"{c} [KiB]" for c in classes) + " |")
    print("|---|" + "---:|" * len(classes))
    for w in sorted(per_window):
        c = per_window[w]
        print(f"| {w * a.window:.0f}–{(w + 1) * a.window:.0f} | " +
              " | ".join(f"{c[k] / 1024:.0f}" for k in classes) + " |")
    print()
    print("Entries with the most bytes read:")
    print()
    print("| Entry | Class | Size [MiB] | read [KiB] | first read [s] |")
    print("|---:|---|---:|---:|---:|")
    by_index = {e.index: e for e in entries}
    for idx, n in per_entry.most_common(15):
        e = by_index[idx]
        print(f"| {idx} | {kind[idx]} | {e.stored / 2**20:.1f} | {n / 1024:.0f} | {first_seen[idx]:.1f} |")
    totals = collections.Counter()
    for c in per_window.values():
        totals.update(c)
    print()
    print("Total per class [KiB]: " + ", ".join(f"{k} {totals[k] / 1024:.0f}" for k in classes))
    print(f"Distinct entries read: {len(per_entry)} "
          f"(of which " + ", ".join(f"{k} {sum(1 for i in per_entry if kind[i] == k)}" for k in classes[:4]) + ")")


def head(arc, e):
    arc._f.seek(e.offset)
    return arc._f.read(4)


if __name__ == "__main__":
    sys.exit(main())
