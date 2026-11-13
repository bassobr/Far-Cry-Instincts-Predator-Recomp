#!/usr/bin/env python3
"""xfat.py: list and extract DAT2 archives (.fat/.dat) of Far Cry Instincts Predator (Xbox 360).

Format (measured on the Predator disc):
  .fat  = "DAT2", u32 count, then count x 16 bytes {u32 offset, u32 stored size,
          u32 unpacked size, u32 name hash}; all little-endian, sorted by hash in ascending order.
  .dat  = raw data. Stored != unpacked size -> 1 marker byte (0x00/0x01) + LZMA "alone" stream
          (13-byte header: props 0x5D, dictionary 8 MiB, u64 unpacked size), otherwise uncompressed.
  Hash  = CRC-32 (zlib/IEEE) over the lowercase path with backslashes and a leading
          backslash, e.g. "\\shaders\\posteffects\\fixed0textures.vsh".

Source: derived from `xfat.py` of Far Cry Instincts Recomp (MIT, same author; original Xbox):
there 12-byte entries {offset, size, hash} without compression. The 360 version has an additional
size field and LZMA; hash function and sort order are the same (same hashes in both versions).

Usage:
  xfat.py list    <archive.fat> [--names names.txt]          list entries (hash, sizes, name, type)
  xfat.py extract <archive.fat> <output> [--names names.txt] [--filter TEXT]
  xfat.py hash    <path> ...                                 compute the hash of a path
Unknown names are stored as _unnamed/<HASH>.<guessed-extension>.
"""
import argparse
import lzma
import os
import struct
import sys
import zlib

MAGIC = b"DAT2"


def name_hash(path):
    p = path.replace("/", "\\").lower()
    if not p.startswith("\\"):
        p = "\\" + p
    return zlib.crc32(p.encode("latin-1")) & 0xFFFFFFFF


class Entry:
    __slots__ = ("index", "offset", "stored", "size", "hash")

    def __init__(self, index, offset, stored, size, h):
        self.index, self.offset, self.stored, self.size, self.hash = index, offset, stored, size, h

    @property
    def compressed(self):
        return self.stored != self.size


def read_fat(path):
    fat = open(path, "rb").read()
    if fat[:4] != MAGIC:
        raise ValueError("%s: not DAT2 (%r)" % (path, fat[:4]))
    n = struct.unpack_from("<I", fat, 4)[0]
    if len(fat) != 8 + 16 * n:
        raise ValueError("%s: length %d does not match %d entries" % (path, len(fat), n))
    return [Entry(i, *struct.unpack_from("<IIII", fat, 8 + 16 * i)) for i in range(n)]


class Archive:
    def __init__(self, fat_path):
        self.fat_path = fat_path
        self.dat_path = fat_path[:-4] + ".dat"
        self.entries = read_fat(fat_path)
        self._f = open(self.dat_path, "rb")

    def raw(self, e):
        self._f.seek(e.offset)
        return self._f.read(e.stored)

    def data(self, e):
        b = self.raw(e)
        if e.compressed:
            # 1 marker byte (0x00 or 0x01, meaning unknown; both unpack the same), then LZMA alone
            b = lzma.decompress(b[1:], format=lzma.FORMAT_ALONE)
            if len(b) != e.size:
                raise ValueError("entry %d: unpacked %d instead of %d" % (e.index, len(b), e.size))
        return b


def load_names(path):
    names = {}
    if path and os.path.exists(path):
        for line in open(path, encoding="latin-1"):
            line = line.strip()
            if line and not line.startswith("#"):
                names.setdefault(name_hash(line), line.replace("/", "\\").lower().lstrip("\\"))
    return names


def guess_ext(b):
    """Coarse type detection from magic bytes (only for entries without a known name)."""
    h = b[:16]
    table = [
        (b"\x1bLua", "luac"), (b"BIK", "bik"), (b"KB2", "bk2"), (b"RIFF", "riff"), (b"DDS ", "dds"),
        (b"<?xml", "xml"), (b"\xef\xbb\xbf<", "xml"), (b"\xff\xfe<", "xml16"), (b"\x89PNG", "png"),
        (b"XPR", "xpr"), (b"CryTek", "cry"), (b"DAT2", "dat2"),
    ]
    for magic, ext in table:
        if h.startswith(magic):
            return ext
    if h[:4] in (b"\x10\x2a\x0e\x00", b"\x10\x2a\x0e\x01", b"\x10\x2a\x11\x00", b"\x10\x2a\x11\x01"):
        return "fxo"  # Xenos shader container (Predator: 0x102A0E00 PS / 0x102A0E01 VS)
    if b and all(c in (9, 10, 13) or 32 <= c < 127 for c in b[:256]):
        return "txt"
    return "bin"


def main(argv=None):
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("fat")
    p.add_argument("--names")
    p = sub.add_parser("extract")
    p.add_argument("fat")
    p.add_argument("out")
    p.add_argument("--names")
    p.add_argument("--filter", default="")
    p = sub.add_parser("hash")
    p.add_argument("paths", nargs="+")
    a = ap.parse_args(argv)

    if a.cmd == "hash":
        for s in a.paths:
            print("%08X  %s" % (name_hash(s), s))
        return 0

    arc = Archive(a.fat)
    names = load_names(a.names)
    if a.cmd == "list":
        known = 0
        for e in arc.entries:
            nm = names.get(e.hash)
            known += nm is not None
            print("%5d %08X %10d %10d %s %s" % (e.index, e.hash, e.stored, e.size,
                                                "L" if e.compressed else "-", nm or "?"))
        print("# %d entries, %d named, stored %d, unpacked %d" % (
            len(arc.entries), known, sum(e.stored for e in arc.entries), sum(e.size for e in arc.entries)),
            file=sys.stderr)
        return 0

    out = os.path.abspath(a.out)
    flt = a.filter.lower()
    for e in arc.entries:
        nm = names.get(e.hash)
        if flt and (nm is None or flt not in nm):
            continue
        b = arc.data(e)
        rel = nm if nm else os.path.join("_unnamed", "%08X.%s" % (e.hash, guess_ext(b)))
        dst = os.path.abspath(os.path.join(out, rel.replace("\\", os.sep)))
        if not dst.startswith(out + os.sep):
            raise ValueError("path leaves the target: %r" % rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(b)
    return 0


if __name__ == "__main__":
    sys.exit(main())
