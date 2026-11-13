#!/usr/bin/env python3
"""Build step: takes default.xex from the user's disc image and creates the memory image for the
coverage check.

    python -I tools/prepare_xex.py --iso <image.iso> --disc config/predator/disc.toml --out <folder>
                                   [--stamp <file>]

Writes to <folder>: default.xex (unchanged from the image), default.bin (decrypted
memory image, offset 0 = 0x82000000) and default.json (headers, sections, .pdata, imports). Checks size
and SHA-256 against disc.toml. Everything in <folder> is game code and belongs only in out/ (gitignored).
The image is only read. Needs pycryptodome (toolchain\\venv).
"""
import argparse
import hashlib
import json
import os
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xexunpack  # noqa: E402
import xiso  # noqa: E402


def write_if_changed(path, data):
    """Writes only when the content changed, so that several build folders see the same timestamp."""
    if os.path.isfile(path) and os.path.getsize(path) == len(data):
        with open(path, "rb") as f:
            if f.read() == data:
                return False
    tmp = path + ".tmp"
    with open(tmp, "wb") as o:
        o.write(data)
    os.replace(tmp, path)
    return True


def extract(iso, name):
    size = os.path.getsize(iso)
    with open(iso, "rb") as f:
        partition, root_sector, root_size, _ = xiso.find_partition(f, size)
        entries = []
        xiso.walk(f, partition, root_sector, root_size, "", entries)
        hits = [e for e in entries if not e[3] and e[0].lower() == name.lower()]
        if not hits:
            raise SystemExit(f"{name} not in the image {iso}")
        _, start, length, _ = hits[0]
        f.seek(partition + start * xiso.SECTOR)
        data = f.read(length)
    if len(data) != length:
        raise SystemExit(f"unexpected end of file while reading {name}")
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--stamp")
    a = ap.parse_args()

    if not os.path.isfile(a.iso):
        raise SystemExit(f"disc image not found: {a.iso}\n"
                         "Set the path with the environment variable FCP_GAME_ISO or -DFCP_GAME_ISO=....")
    with open(a.disc, "rb") as f:
        disc = tomllib.load(f)["default_xex"]
    os.makedirs(a.out, exist_ok=True)
    xex = os.path.join(a.out, "default.xex")
    data = extract(a.iso, disc["path"])
    digest = hashlib.sha256(data).hexdigest().upper()
    if len(data) != disc["size"] or digest != disc["sha256"].upper():
        raise SystemExit(f"{disc['path']} does not match the expected disc version: {len(data)} bytes, "
                         f"SHA-256 {digest} (expected {disc['size']} bytes, {disc['sha256']})")
    changed = write_if_changed(xex, data)

    info, image = xexunpack.unpack(xex)
    base = int(info["image_base_address"], 16)
    pe = xexunpack.pe_sections(image)
    stats = xexunpack.pdata_stats(image, base, pe)
    imps = xexunpack.imports(info, image, base)
    for lib in info.get("import_libraries", []):
        lib.pop("record_addresses", None)
    changed |= write_if_changed(os.path.join(a.out, "default.bin"), bytes(image))
    report = json.dumps({"info": info, "pe": pe, "pdata": stats[0] if stats else None, "imports": imps,
                         "pdata_entries": [[e[0], e[1], e[2], e[3]] for e in stats[1]] if stats else []},
                        indent=1)
    changed |= write_if_changed(os.path.join(a.out, "default.json"), report.encode("utf-8"))
    print(f"prepare_xex: {disc['path']} ({len(data)} bytes, SHA-256 ok), image {len(image)} bytes "
          f"from 0x{base:08X} in {a.out} ({'updated' if changed else 'unchanged'})")
    if a.stamp:
        with open(a.stamp, "w", encoding="utf-8") as f:
            f.write(digest + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
