#!/usr/bin/env python3
"""limiter_check.py: what the output limiter (apu.limiter, UPSTREAM 371) changed, measured from two recordings of one
run: FCP_RECORD_AUDIO (the 6 channel mix before gain and downmix) and FCP_RECORD_AUDIO_OUT (the device output after the
limiter, before mute; stereo or 6 channels).

The downmix of the SDK is computed again from the mix in float32 in the same order (the build has -ffp-contract=off,
so the values are bit for bit those of MixStereo / MixSurround); the output is that downmix delayed by 63 samples
wherever the limiter's gain is 1. The script reports:
  - peak and samples above 1.0 / above the ceiling before the limiter (= what the output would clip without it),
  - peak and samples above 1.0 after it,
  - how many output samples differ from the delayed downmix (bit for bit), as share and as time ranges,
  - RMS of before and after per window (level change).

Usage: limiter_check.py <mix.wav> <out.wav> [--window 10] [--gain 1] [--json out.json]
Needs numpy.
"""
import argparse
import json
import struct
import sys

import numpy as np

DELAY = 63
CEILING = np.float32(0.966)
RATE = 48000


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV")
    pos = 12
    channels = None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            channels = struct.unpack_from("<H", data, body + 2)[0]
        elif cid == b"data":
            n = (len(data) - body) // 4 if size == 0xFFFFFFFF else size // 4
            samples = np.frombuffer(data, dtype="<f4", count=n, offset=body)
            frames = len(samples) // channels
            return samples[:frames * channels].reshape(frames, channels)
        pos = body + size + (size & 1)
    raise SystemExit(f"{path}: no data chunk")


def downmix(mix, channels, gain):
    f = np.float32
    fl, fr, fc, lf, bl, br = (mix[:, i].astype(f) for i in range(6))
    g = f(gain)
    if channels == 2:
        center, surround, lfe, scale = f(0.70710678), f(0.70710678), f(0.0), f(0.58578644)
        s = f(scale * g)
        mid = fc * center + lf * lfe
        left = (fl + mid + bl * surround) * s
        right = (fr + mid + br * surround) * s
        return np.stack([left, right], axis=1)
    if channels == 6:
        return np.stack([fl * g, fr * g, fc * g, lf * g, bl * g, br * g], axis=1)
    raise SystemExit(f"output with {channels} channels not supported")


def db(x):
    return 20.0 * np.log10(x) if x > 0 else float("-inf")


def ranges(mask):
    """[(start, end)] of runs of True."""
    if not mask.any():
        return []
    d = np.diff(np.concatenate([[0], mask.astype(np.int8), [0]]))
    starts, ends = np.where(d == 1)[0], np.where(d == -1)[0]
    return list(zip(starts.tolist(), ends.tolist()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mix")
    ap.add_argument("out")
    ap.add_argument("--window", type=float, default=10.0)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--json")
    a = ap.parse_args()
    mix = read_wav(a.mix)
    out = read_wav(a.out)
    if mix.shape[1] != 6:
        raise SystemExit("the mix recording must have 6 channels")
    ch = out.shape[1]
    pre = downmix(mix, ch, a.gain)
    n = min(len(pre), len(out) - DELAY)
    pre = pre[:n]
    post = out[DELAY:DELAY + n]
    pre_peak_frame = np.abs(pre).max(axis=1)
    differs = (post != pre).any(axis=1)
    result = {
        "frames": int(n), "seconds": n / RATE, "channels": ch,
        "pre_peak": float(pre_peak_frame.max()), "pre_peak_dbfs": db(float(pre_peak_frame.max())),
        "pre_over_1": int((np.abs(pre) > 1.0).sum()), "pre_over_ceiling": int((np.abs(pre) > CEILING).sum()),
        "post_peak": float(np.abs(post).max()), "post_over_1": int((np.abs(post) > 1.0).sum()),
        "changed_frames": int(differs.sum()), "changed_share": float(differs.mean()) if n else 0.0,
    }
    runs = ranges(differs)
    result["changed_ranges"] = len(runs)
    result["changed_ranges_list"] = [(s / RATE, (e - s) / RATE * 1000.0) for s, e in runs[:40]]
    result["longest_change_ms"] = max(((e - s) / RATE * 1000.0 for s, e in runs), default=0.0)
    # outside the changed ranges the output must equal the delayed downmix bit for bit
    result["unchanged_equal"] = bool((post[~differs] == pre[~differs]).all())
    win = int(a.window * RATE)
    rows = []
    for s in range(0, n, win):
        p, q = pre[s:s + win].astype(np.float64), post[s:s + win].astype(np.float64)
        rp, rq = np.sqrt((p * p).mean()), np.sqrt((q * q).mean())
        rows.append({
            "t": s / RATE, "rms_pre_db": db(rp), "rms_post_db": db(rq),
            "peak_pre": float(np.abs(p).max()), "peak_post": float(np.abs(q).max()),
            "over1_pre": int((np.abs(p) > 1.0).sum()), "changed_frames": int(differs[s:s + win].sum()),
        })
    result["windows"] = rows
    print(f"{a.mix} / {a.out}: {n} frames ({n / RATE:.1f} s), output {ch} ch")
    print(f"before the limiter: peak {result['pre_peak']:.4f} ({result['pre_peak_dbfs']:+.2f} dBFS), "
          f"samples > 1.0: {result['pre_over_1']}, > ceiling {float(CEILING)}: {result['pre_over_ceiling']}")
    print(f"after the limiter:  peak {result['post_peak']:.4f}, samples > 1.0: {result['post_over_1']}")
    print(f"changed frames: {result['changed_frames']} ({100 * result['changed_share']:.4f} %) in "
          f"{len(runs)} ranges, longest {result['longest_change_ms']:.1f} ms; unchanged frames bit-equal to the "
          f"delayed downmix: {result['unchanged_equal']}")
    for t, ms in result["changed_ranges_list"][:20]:
        print(f"  change at {t:8.3f} s for {ms:6.1f} ms")
    print(f"{'t [s]':>6} {'RMS pre':>9} {'RMS post':>9} {'peak pre':>9} {'peak post':>9} {'>1 pre':>7} {'changed':>8}")
    for r in rows:
        print(f"{r['t']:6.0f} {r['rms_pre_db']:9.2f} {r['rms_post_db']:9.2f} {r['peak_pre']:9.4f} {r['peak_post']:9.4f} "
              f"{r['over1_pre']:7d} {r['changed_frames']:8d}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(result, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
