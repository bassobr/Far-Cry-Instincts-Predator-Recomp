#!/usr/bin/env python3
"""analyze_wav.py: measurements of a mix recording (FCP_RECORD_AUDIO) or any WAV file.

Per window (default 10 s) and channel: RMS and peak in dBFS, silence share, clipping (|x| >= 1 before the
output is clamped), L/R correlation, coarse band energy (shares in percent), jumps (click
candidates) and zero gaps in the middle of the signal. If a `<file>.gaps.tsv` lies next to the recording
(written by the mix: periods in which an open stream had no frame), these underruns are counted per
window as well.

With 6 channels (FL FR C LFE SL SR, SDK order) the SDK's stereo downmix is computed as well
(apu/mix.h StereoWeights: C and surround -3 dB, LFE 0, scale 0.5858), as a stereo device hears it;
correlation, bands and clipping then refer to this downmix.

Usage (needs numpy):
  python -I tools/audio/analyze_wav.py <file.wav> [--window 10] [--start s] [--end s] [--json out.json]
"""
import argparse
import json
import math
import os
import struct
import sys

import numpy as np

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

SILENCE_DB = -60.0          # a block counts as silent if its RMS is below this
BLOCK_S = 0.05              # block length for the silence share
JUMP = 0.25                 # sample-to-sample jump that stands out in a quiet surrounding
ZERO_RUN = 48               # this many exact zeros (1 ms) in the middle of the signal = gap
BANDS = [(0, 150), (150, 1000), (1000, 4000), (4000, 12000), (12000, 24000)]
CH6 = ["FL", "FR", "C", "LFE", "SL", "SR"]


def read_wav(path):
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise SystemExit(f"{path}: not a WAV file")
        fmt = None
        data_off = data_len = None
        while True:
            h = f.read(8)
            if len(h) < 8:
                break
            cid, size = h[:4], struct.unpack("<I", h[4:])[0]
            if cid == b"fmt ":
                fmt = f.read(size)
            elif cid == b"data":
                data_off = f.tell()
                data_len = size
                break
            else:
                f.seek(size + (size & 1), 1)
    if fmt is None or data_off is None:
        raise SystemExit(f"{path}: fmt/data missing")
    tag, ch, rate, _, align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 0xFFFE:
        tag = struct.unpack_from("<H", fmt, 24)[0]
    real = os.path.getsize(path) - data_off
    if data_len in (0, 0xFFFFFFFF) or data_len > real:
        data_len = real  # recording not closed cleanly: up to the end of the file
    if tag == 3 and bits == 32:
        dt = np.float32
        scale = 1.0
    elif tag == 1 and bits == 16:
        dt = np.int16
        scale = 1.0 / 32768.0
    else:
        raise SystemExit(f"{path}: format {tag}/{bits} bit not supported")
    frames = data_len // align
    raw = np.memmap(path, dtype=dt, mode="r", offset=data_off, shape=(frames, ch))
    return raw, rate, ch, scale


def db(x):
    return -math.inf if x <= 0 else 10.0 * math.log10(x)


def fmt_db(x):
    return "-inf" if x == -math.inf or x < -200 else f"{x:.1f}"


def downmix(x6):
    c, s = 0.70710678, 0.70710678
    scale = 0.58578644
    mid = x6[:, 2] * c
    left = (x6[:, 0] + mid + x6[:, 4] * s) * scale
    right = (x6[:, 1] + mid + x6[:, 5] * s) * scale
    return np.stack([left, right], axis=1)


def band_shares(mono, rate):
    n = 4096
    if len(mono) < n:
        return [0.0] * len(BANDS)
    hop = n
    frames = (len(mono) - n) // hop + 1
    win = np.hanning(n).astype(np.float32)
    acc = np.zeros(n // 2 + 1)
    for i in range(0, frames, max(1, frames // 200)):  # at most ~200 blocks per window
        seg = mono[i * hop:i * hop + n] * win
        acc += np.abs(np.fft.rfft(seg)) ** 2
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    total = acc[1:].sum()
    if total <= 0:
        return [0.0] * len(BANDS)
    return [100.0 * acc[(freqs >= lo) & (freqs < hi)].sum() / total for lo, hi in BANDS]


def zero_gaps(x):
    """Runs of >= ZERO_RUN exact zeros that lie between non-silent sections (per channel)."""
    count = 0
    for c in range(x.shape[1]):
        z = x[:, c] == 0.0
        if not z.any() or z.all():
            continue
        d = np.diff(np.concatenate(([0], z.view(np.int8), [0])))
        starts = np.flatnonzero(d == 1)
        ends = np.flatnonzero(d == -1)
        for s, e in zip(starts, ends):
            if e - s >= ZERO_RUN and s > 0 and e < len(z):
                before = np.abs(x[max(0, s - 480):s, c]).max()
                after = np.abs(x[e:e + 480, c]).max()
                if before > 1e-3 and after > 1e-3:
                    count += 1
    return count


def jumps(x):
    """Click candidates: single samples that jump out of their neighbours by > JUMP while
    the neighbours are close to each other (|2x[n] - x[n-1] - x[n+1]| > 2*JUMP, |x[n+1] - x[n-1]| < JUMP/2).
    Steep but continuous transients (gunshots) are not counted this way."""
    count = 0
    for c in range(x.shape[1]):
        v = x[:, c].astype(np.float64)
        if len(v) < 3:
            continue
        spike = np.abs(2 * v[1:-1] - v[:-2] - v[2:])
        flat = np.abs(v[2:] - v[:-2])
        count += int(((spike > 2 * JUMP) & (flat < JUMP / 2)).sum())
    return count


def load_gaps(path, rate):
    p = path + ".gaps.tsv"
    if not os.path.exists(p):
        return None
    out = []
    with open(p, encoding="utf-8") as f:
        next(f, None)
        for line in f:
            parts = line.split("\t")
            if len(parts) >= 2:
                out.append(float(parts[1]))
    return out


def analyze(path, window, start, end):
    raw, rate, ch, scale = read_wav(path)
    total_s = raw.shape[0] / rate
    gaps = load_gaps(path, rate)
    end = total_s if end is None else min(end, total_s)
    names = CH6 if ch == 6 else (["L", "R"] if ch == 2 else [f"ch{i}" for i in range(ch)])
    rows = []
    t = start
    while t < end - 1e-9:
        t1 = min(end, t + window)
        a, b = int(t * rate), int(t1 * rate)
        x = np.asarray(raw[a:b], dtype=np.float32) * scale
        n = len(x)
        if n == 0:
            break
        row = {"t0": round(t, 2), "t1": round(t1, 2)}
        ms = (x.astype(np.float64) ** 2).mean(axis=0)
        row["rms_db"] = {names[c]: db(ms[c]) for c in range(ch)}
        row["peak_db"] = {names[c]: db(float(np.abs(x[:, c]).max()) ** 2) for c in range(ch)}
        row["over_unity"] = int((np.abs(x) >= 1.0).sum())
        st = x if ch <= 2 else downmix(x)
        if st.shape[1] == 1:
            st = np.concatenate([st, st], axis=1)
        row["stereo_rms_db"] = db(float((st.astype(np.float64) ** 2).mean()))
        row["stereo_peak_db"] = db(float(np.abs(st).max()) ** 2)
        row["stereo_clip"] = int((np.abs(st) >= 1.0).sum())
        blk = max(1, int(BLOCK_S * rate))
        nb = n // blk
        if nb:
            e = (st[:nb * blk].astype(np.float64) ** 2).reshape(nb, blk, 2).mean(axis=(1, 2))
            row["silence_pct"] = 100.0 * float((10 * np.log10(np.maximum(e, 1e-30)) < SILENCE_DB).mean())
        else:
            row["silence_pct"] = 100.0
        l, r = st[:, 0].astype(np.float64), st[:, 1].astype(np.float64)
        if l.std() > 1e-7 and r.std() > 1e-7:
            row["lr_corr"] = float(np.corrcoef(l, r)[0, 1])
        else:
            row["lr_corr"] = None
        row["bands_pct"] = band_shares(((l + r) * 0.5).astype(np.float32), rate)
        row["zero_gaps"] = zero_gaps(x)
        row["jumps"] = jumps(st)
        if gaps is not None:
            row["underrun_periods"] = sum(1 for g in gaps if t <= g < t1)
        rows.append(row)
        t = t1
    return {"file": os.path.basename(path), "rate": rate, "channels": ch, "seconds": round(total_s, 2),
            "names": names, "windows": rows}


def print_table(res):
    print(f"# {res['file']}: {res['rate']} Hz, {res['channels']} channels, {res['seconds']} s")
    print()
    names = res["names"]
    hdr = ["t [s]"] + [f"RMS {n}" for n in names] + ["Peak", "Stereo RMS", "Silence %", "L/R corr.",
                                                     ">=1.0", "<150", "150-1k", "1k-4k", "4k-12k",
                                                     ">12k", "Zero gaps", "Jumps", "Underruns"]
    print("| " + " | ".join(hdr) + " |")
    print("|" + "|".join(["---"] + ["---:"] * (len(hdr) - 1)) + "|")
    for w in res["windows"]:
        peak = max(w["peak_db"].values())
        corr = "–" if w["lr_corr"] is None else f"{w['lr_corr']:.2f}"
        cells = [f"{w['t0']:.0f}–{w['t1']:.0f}"] + [fmt_db(w["rms_db"][n]) for n in names] + [
            fmt_db(peak), fmt_db(w["stereo_rms_db"]), f"{w['silence_pct']:.0f}", corr,
            str(w["over_unity"] + w["stereo_clip"])] + [f"{b:.0f}" for b in w["bands_pct"]] + [
            str(w["zero_gaps"]), str(w["jumps"]), str(w.get("underrun_periods", "–"))]
        print("| " + " | ".join(cells) + " |")
    print()
    print("RMS/peak in dBFS; silence = share of 50 ms blocks below -60 dBFS (stereo); bands in % of the "
          "energy (mid L+R); >=1.0 = values the output clamps; zero gaps = >=1 ms of exact zeros "
          "between signal; jumps = click candidates; underruns from .gaps.tsv.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--window", type=float, default=10.0)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--json")
    a = ap.parse_args()
    res = analyze(a.wav, a.window, a.start, a.end)
    print_table(res)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1, default=lambda v: None if v == -math.inf else v)


if __name__ == "__main__":
    sys.exit(main())
