#!/usr/bin/env python3
"""colour_blind_check.py: checks [graphics] colour_blind on dumps of one frame. With FCP_DUMP_FRAMES=<dir>/f%d.png,
FCP_DUMP_FRAME_AT=<frames> and FCP_DUMP_PRESENT=1 the renderer writes per frame the front buffer as the game drew it
(f<n>_front.png, gamma ramp applied on the CPU) and the shown picture (f<n>.png, the swapchain after the final blit).
With the window as large as the picture both have the same pixels. The script computes the expected picture from the
front buffer with the mode's matrix (the same as src/renderer/core/colour_blind.h: Machado 2009 + Fidaner, on linear
colour with gamma 2.2) and compares it with the shown one; for off the shown picture must equal the front buffer.
It also prints the matrix applied to a few test colours.

Usage: colour_blind_check.py <mode> <shown.png> <front.png> [--json out.json]   (mode: off|protanopia|deuteranopia|tritanopia)
Needs numpy and Pillow.
"""
import argparse
import json
import sys

import numpy as np
from PIL import Image

SIMULATE = {
    "protanopia": [0.152286, 1.052583, -0.204868, 0.114503, 0.786281, 0.099216, -0.003882, -0.048116, 1.051998],
    "deuteranopia": [0.367322, 0.860646, -0.227968, 0.280085, 0.672501, 0.047413, -0.011820, 0.042940, 0.968881],
    "tritanopia": [1.255528, -0.076749, -0.178779, -0.078411, 0.930809, 0.147602, 0.004733, 0.691367, 0.303900],
}
SHIFT = {
    "protanopia": [0, 0, 0, 0.7, 1, 0, 0.7, 0, 1],
    "deuteranopia": [0, 0, 0, 0.7, 1, 0, 0.7, 0, 1],
    "tritanopia": [1, 0, 0.7, 0, 1, 0.7, 0, 0, 0],
}
TEST_COLOURS = {"red": (255, 0, 0), "green": (0, 255, 0), "blue": (0, 0, 255), "yellow": (255, 255, 0),
                "orange": (255, 128, 0), "purple": (128, 0, 255), "grey": (128, 128, 128), "white": (255, 255, 255)}


def matrix(mode):
    if mode == "off":
        return np.eye(3, dtype=np.float32)
    sim = np.array(SIMULATE[mode], dtype=np.float32).reshape(3, 3)
    shift = np.array(SHIFT[mode], dtype=np.float32).reshape(3, 3)
    return (np.eye(3, dtype=np.float32) + shift @ (np.eye(3, dtype=np.float32) - sim)).astype(np.float32)


def apply(m, rgb8):
    lin = np.power(np.clip(rgb8.astype(np.float32) / 255.0, 0, 1), 2.2)
    out = np.clip(lin @ m.T, 0, 1)
    return np.power(out, 1 / 2.2) * 255.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["off", "protanopia", "deuteranopia", "tritanopia"])
    ap.add_argument("shown")
    ap.add_argument("front")
    ap.add_argument("--json")
    a = ap.parse_args()
    shown = np.asarray(Image.open(a.shown).convert("RGB")).astype(np.int32)
    front = np.asarray(Image.open(a.front).convert("RGB")).astype(np.int32)
    if shown.shape != front.shape:
        raise SystemExit(f"sizes differ: shown {shown.shape}, front {front.shape} (window as large as the picture?)")
    m = matrix(a.mode)
    res = {"mode": a.mode, "pixels": int(shown.shape[0] * shown.shape[1]), "matrix": m.round(4).tolist()}
    res["shown_equals_front"] = bool((shown == front).all())
    res["shown_vs_front_max"] = int(np.abs(shown - front).max())
    res["shown_vs_front_mean"] = float(np.abs(shown - front).mean())
    expected = apply(m, front)
    d = np.abs(shown - np.rint(expected))
    res["vs_expected_max"] = int(d.max())
    res["vs_expected_mean"] = float(d.mean())
    res["vs_expected_within_1"] = float((d.max(axis=2) <= 1).mean())
    res["vs_expected_within_2"] = float((d.max(axis=2) <= 2).mean())
    tests = {}
    for name, c in TEST_COLOURS.items():
        tests[name] = [int(round(v)) for v in apply(m, np.array([c]))[0]]
    res["test_colours"] = tests
    # the pixels where the mode changes the most (what the frame shows, expected and seen)
    change = np.abs(np.rint(expected) - front).sum(axis=2)
    flat = np.argsort(change.ravel())[::-1][:5]
    samples = []
    for i in flat:
        y, x = divmod(int(i), shown.shape[1])
        samples.append({"x": x, "y": y, "front": front[y, x].tolist(), "expected": np.rint(expected[y, x]).astype(int).tolist(),
                        "shown": shown[y, x].tolist()})
    res["most_changed"] = samples
    print(f"{a.mode}: {res['pixels']} pixels; shown == front: {res['shown_equals_front']} (max {res['shown_vs_front_max']}, "
          f"mean {res['shown_vs_front_mean']:.3f})")
    print(f"  vs. expected (front x matrix): max {res['vs_expected_max']}, mean {res['vs_expected_mean']:.3f}, "
          f"within 1: {100 * res['vs_expected_within_1']:.3f} %, within 2: {100 * res['vs_expected_within_2']:.3f} %")
    print("  matrix:", res["matrix"])
    print("  test colours:", ", ".join(f"{k} {TEST_COLOURS[k]} -> {tuple(v)}" for k, v in tests.items()))
    for s in samples:
        print(f"  most changed ({s['x']},{s['y']}): front {s['front']} expected {s['expected']} shown {s['shown']}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
