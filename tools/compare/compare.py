"""Image comparison of the renderer against a reference (Xenia Canary), so that progress becomes measurable.

Pixel equality is not to be expected (water, video, timing, Canary quirks). Hence per image region:
  mae   mean absolute deviation in RGB (0..255)
  dlum  deviation of the mean brightness (test - reference, 0..255): too bright/too dark
  edge  correlation of the edge images (Sobel magnitude, -1..1): same structure, even with a different tone
Plus global PSNR and the share of regions with mae > --bad.

  python -I tools/compare/compare.py pair <test.png> <ref.png> [--out diff.png] [--grid 4x4]
  python -I tools/compare/compare.py match <test.png>... --refs <folder> [--out-dir d] [--grid 4x4]
      finds the most similar reference for each test image (downscaled images, mae), then compares like pair
  python -I tools/compare/compare.py series <test-folder> --refs <folder> --pairs "f_1740=t0074.0,..."

Output per pair: one line with the global values, a table of the regions, optionally an image
(test | reference | amplified difference) to look at. Needs numpy and Pillow.
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image

SIZE = (1280, 720)


def load(path, size=SIZE):
    im = Image.open(path).convert("RGB")
    if im.size != size:
        im = im.resize(size, Image.BILINEAR)
    return np.asarray(im, dtype=np.float32)


def luminance(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def sobel(l):
    gx = np.zeros_like(l)
    gy = np.zeros_like(l)
    gx[1:-1, 1:-1] = (l[:-2, 2:] + 2 * l[1:-1, 2:] + l[2:, 2:]) - (l[:-2, :-2] + 2 * l[1:-1, :-2] + l[2:, :-2])
    gy[1:-1, 1:-1] = (l[2:, :-2] + 2 * l[2:, 1:-1] + l[2:, 2:]) - (l[:-2, :-2] + 2 * l[:-2, 1:-1] + l[:-2, 2:])
    return np.hypot(gx, gy)


def corr(a, b):
    a = a - a.mean()
    b = b - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-6 else (1.0 if np.allclose(a, b) else 0.0)


def compare(test, ref, grid=(4, 4), bad=40.0):
    diff = np.abs(test - ref)
    lt, lr = luminance(test), luminance(ref)
    et, er = sobel(lt), sobel(lr)
    mse = float((diff ** 2).mean())
    psnr = 99.0 if mse < 1e-9 else 10 * np.log10(255.0 ** 2 / mse)
    gx, gy = grid
    h, w = lt.shape
    regions = []
    for j in range(gy):
        for i in range(gx):
            ys, ye = j * h // gy, (j + 1) * h // gy
            xs, xe = i * w // gx, (i + 1) * w // gx
            regions.append({
                "r": j, "c": i,
                "mae": float(diff[ys:ye, xs:xe].mean()),
                "dlum": float(lt[ys:ye, xs:xe].mean() - lr[ys:ye, xs:xe].mean()),
                "edge": corr(et[ys:ye, xs:xe], er[ys:ye, xs:xe]),
            })
    return {
        "mae": float(diff.mean()), "psnr": psnr, "dlum": float(lt.mean() - lr.mean()),
        "edge": corr(et, er), "bad": sum(r["mae"] > bad for r in regions) / len(regions), "regions": regions,
        "diff": diff,
    }


def write_triptych(path, test, ref, diff):
    amp = np.clip(diff.mean(axis=2) * 4, 0, 255)
    heat = np.stack([amp, np.clip(amp * 0.5, 0, 255), np.zeros_like(amp)], axis=2)
    row = np.concatenate([test, ref, heat], axis=1).astype(np.uint8)
    Image.fromarray(row).resize((row.shape[1] // 2, row.shape[0] // 2), Image.BILINEAR).save(path)


def report(name_t, name_r, res, grid):
    print(f"{name_t} ~ {name_r}: mae {res['mae']:.1f}  psnr {res['psnr']:.1f} dB  dlum {res['dlum']:+.1f}  "
          f"edge {res['edge']:.2f}  bad regions {res['bad'] * 100:.0f} %")
    gx, gy = grid
    for j in range(gy):
        cells = [r for r in res["regions"] if r["r"] == j]
        print("    " + "  ".join(f"{c['mae']:5.1f}/{c['dlum']:+5.0f}/{c['edge']:4.2f}" for c in cells))


def thumbs(paths):
    return {p: np.asarray(Image.open(p).convert("RGB").resize((64, 36), Image.BILINEAR), dtype=np.float32) for p in paths}


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p1 = sub.add_parser("pair")
    p1.add_argument("test")
    p1.add_argument("ref")
    p2 = sub.add_parser("match")
    p2.add_argument("tests", nargs="+")
    p2.add_argument("--refs", required=True)
    p3 = sub.add_parser("series")
    p3.add_argument("testdir")
    p3.add_argument("--refs", required=True)
    p3.add_argument("--pairs", required=True)
    for p in (p1, p2, p3):
        p.add_argument("--out", default="")
        p.add_argument("--out-dir", default="")
        p.add_argument("--grid", default="4x4")
        p.add_argument("--bad", type=float, default=40.0)
    a = ap.parse_args()
    grid = tuple(int(v) for v in a.grid.split("x"))

    pairs = []
    if a.cmd == "pair":
        pairs.append((a.test, a.ref))
    elif a.cmd == "match":
        refs = sorted(glob.glob(os.path.join(a.refs, "*.png")))
        rt = thumbs(refs)
        for t in a.tests:
            tt = thumbs([t])[t]
            best = min(refs, key=lambda r: float(np.abs(rt[r] - tt).mean()))
            pairs.append((t, best))
    else:
        for item in a.pairs.split(","):
            t, r = item.split("=")
            tp = os.path.join(a.testdir, t if t.endswith(".png") else t + ".png")
            rp = r if os.path.isabs(r) else os.path.join(a.refs, r if r.endswith(".png") else r + "_canary.png")
            pairs.append((tp, rp))

    summary = []
    for t, r in pairs:
        test, ref = load(t), load(r)
        res = compare(test, ref, grid, a.bad)
        report(os.path.basename(t), os.path.basename(r), res, grid)
        summary.append(res)
        out = a.out if a.cmd == "pair" else (
            os.path.join(a.out_dir, os.path.splitext(os.path.basename(t))[0] + "_cmp.png") if a.out_dir else "")
        if out:
            os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
            write_triptych(out, test, ref, res["diff"])
    if len(summary) > 1:
        print(f"Mean over {len(summary)} pairs: mae {np.mean([s['mae'] for s in summary]):.1f}  "
              f"psnr {np.mean([s['psnr'] for s in summary]):.1f} dB  edge {np.mean([s['edge'] for s in summary]):.2f}  "
              f"bad regions {np.mean([s['bad'] for s in summary]) * 100:.0f} %")
    return 0


if __name__ == "__main__":
    sys.exit(main())
