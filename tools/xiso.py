#!/usr/bin/env python3
"""Lists or extracts an Xbox/Xbox 360 XDVDFS image (XISO, XGD2, XGD3, full Redump ISO).

    python tools/xiso.py <image.iso>                 # list (size, sector, path)
    python tools/xiso.py <image.iso> --out <folder>  # extract everything
    python tools/xiso.py <image.iso> --out <folder> --only default.xex --only "*.xex"

Ported from tools/xiso.ps1 of Far Cry Instincts Recomp (MIT, same author).
The image is only read.
"""
import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048
# start of the game partition: plain XISO, Xbox 1 (Redump), XGD3, XGD2
PARTITION_OFFSETS = (0, 0x18300000, 0x2080000, 0xFD90000)
PARTITION_NAMES = {0: "XISO (game partition only)", 0x18300000: "Xbox 1 (XGD1)", 0x2080000: "XGD3", 0xFD90000: "XGD2"}
MAGIC = b"MICROSOFT*XBOX*MEDIA"


def find_partition(f, size):
    for p in PARTITION_OFFSETS:
        if p + 0x10000 + 0x800 > size:
            continue
        f.seek(p + 0x10000)
        head = f.read(0x800)
        if head[:20] == MAGIC and head[0x7EC:0x800] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", head, 20)
            filetime = struct.unpack_from("<Q", head, 28)[0]
            return p, root_sector, root_size, filetime
    raise SystemExit("no XDVDFS volume found")


def walk(f, partition, sector, size, prefix, out):
    if size == 0:
        return
    f.seek(partition + sector * SECTOR)
    table = f.read(size)
    pending, seen = [0], set()
    while pending:
        off = pending.pop()
        if off + 14 > len(table) or off in seen:
            continue
        seen.add(off)
        left, right = struct.unpack_from("<HH", table, off)
        if left == 0xFFFF:
            continue  # padding
        start, length = struct.unpack_from("<II", table, off + 4)
        attr = table[off + 12]
        name_len = table[off + 13]
        name = table[off + 14:off + 14 + name_len].decode("latin-1")
        if left:
            pending.append(left * 4)
        if right:
            pending.append(right * 4)
        is_dir = bool(attr & 0x10)
        path = prefix + name
        out.append((path, start, length, is_dir))
        if is_dir:
            walk(f, partition, start, length, path + "\\", out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("iso")
    ap.add_argument("--out", help="target folder for extraction")
    ap.add_argument("--only", action="append", default=[], help="glob on the path (may be repeated)")
    args = ap.parse_args()

    size = os.path.getsize(args.iso)
    with open(args.iso, "rb") as f:
        partition, root_sector, root_size, filetime = find_partition(f, size)
        entries = []
        walk(f, partition, root_sector, root_size, "", entries)
        entries.sort(key=lambda e: e[0].lower())

        def wanted(path):
            if not args.only:
                return True
            low = path.lower()
            return any(fnmatch.fnmatch(low, g.lower()) or fnmatch.fnmatch(os.path.basename(low), g.lower())
                       for g in args.only)

        if not args.out:
            files = [e for e in entries if not e[3]]
            print(f"# image: {size} bytes, partition at 0x{partition:X} ({PARTITION_NAMES[partition]}), "
                  f"volume FILETIME 0x{filetime:016X}")
            print(f"# {len(files)} files, {len(entries) - len(files)} folders, "
                  f"{sum(e[2] for e in files)} bytes of payload")
            for path, start, length, is_dir in entries:
                if wanted(path):
                    print(f"{length:12d} {start:8d} {path}{chr(92) if is_dir else ''}")
            return

        os.makedirs(args.out, exist_ok=True)
        count = total = 0
        for path, start, length, is_dir in entries:
            dest = os.path.join(args.out, path)
            if is_dir:
                if not args.only:
                    os.makedirs(dest, exist_ok=True)
                continue
            if not wanted(path):
                continue
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            f.seek(partition + start * SECTOR)
            left = length
            with open(dest, "wb") as o:
                while left > 0:
                    chunk = f.read(min(1 << 20, left))
                    if not chunk:
                        raise SystemExit(f"unexpected end of file: {path}")
                    o.write(chunk)
                    left -= len(chunk)
            count += 1
            total += length
        print(f"{count} files ({total} bytes) extracted to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
