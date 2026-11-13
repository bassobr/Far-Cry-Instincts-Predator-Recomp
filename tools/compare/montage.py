"""Contact sheet from several images (for a quick look at dumps and reference images).

  python -I tools/compare/montage.py <out.png> <image>... [--width 400] [--cols 3]
Each image is scaled to --width and labelled with its file name. Needs Pillow.
"""
import argparse
import os

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--width", type=int, default=400)
    ap.add_argument("--cols", type=int, default=3)
    a = ap.parse_args()
    tiles = []
    for p in a.images:
        im = Image.open(p).convert("RGB")
        h = round(im.height * a.width / im.width)
        im = im.resize((a.width, h), Image.BILINEAR)
        tiles.append((os.path.basename(p), im))
    th = max(t[1].height for t in tiles) + 14
    rows = (len(tiles) + a.cols - 1) // a.cols
    sheet = Image.new("RGB", (a.cols * a.width, rows * th), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(tiles):
        x, y = (i % a.cols) * a.width, (i // a.cols) * th
        sheet.paste(im, (x, y + 14))
        d.text((x + 2, y + 1), name, fill=(255, 255, 0))
    sheet.save(a.out)


if __name__ == "__main__":
    main()
