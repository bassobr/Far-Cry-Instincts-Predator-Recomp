"""Canary reference at any place of a level: a modified copy of the level archive with another start point.

Copies <level>.dat/.fat from the extracted disc (work/disc) into a Canary disc folder (a folder of hard links to
work/disc, see docs/phase1/issue-1-grafik.md 10.3; the two files are replaced by real copies first, so the shared
originals stay untouched), replaces text in the mission XML (start point Respawn0, optionally mercenaries and
creature generators off) and appends the changed XML uncompressed to the .dat; the .fat entry then points there
(stored size = unpacked size = uncompressed, tools/xfat.py).

  python -I tools/compare/canary_mod_level.py --disc work/disc --out work/canarydisc --level 06_cliffs
      --entry DAC16E78 --xml work/l06/_unnamed/DAC16E78.txt --respawn "651.104,327.318,177.300" --no-enemies
  (--xml: the entry already extracted with tools/xfat.py extract; --entry: its name hash)

Game data stays under work/ (gitignored); nothing of it is printed.
"""
import argparse
import os
import shutil
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--level", required=True)
    ap.add_argument("--entry", required=True, help="name hash of the mission XML entry (hex)")
    ap.add_argument("--xml", required=True, help="the entry, extracted (decompressed)")
    ap.add_argument("--respawn", required=True, help="x,y,z for Respawn0")
    ap.add_argument("--no-enemies", action="store_true")
    a = ap.parse_args()
    rel = os.path.join("levels", a.level.upper())
    src_dir = os.path.join(a.disc, rel)
    dst_dir = os.path.join(a.out, rel)
    for ext in (".dat", ".fat"):
        name = a.level + ext
        dst = os.path.join(dst_dir, name)
        if os.path.exists(dst):
            os.remove(dst)  # a hard link to the shared original: never write through it
        shutil.copyfile(os.path.join(src_dir, name), dst)
    xml = open(a.xml, "rb").read()
    key = b'Name="Respawn0" Pos="'
    at = xml.find(key)
    if at < 0:
        raise SystemExit("Respawn0 not found")
    end = xml.find(b'"', at + len(key))
    xml = xml[: at + len(key)] + a.respawn.encode("ascii") + xml[end:]
    if a.no_enemies:
        xml = xml.replace(b'EntityClass="Merc"', b'EntityClass="Merx"')
        xml = xml.replace(b'EntityClass="CreatureGenerator"', b'EntityClass="CreatureGeneratoX"')
    dat = os.path.join(dst_dir, a.level + ".dat")
    fat = os.path.join(dst_dir, a.level + ".fat")
    offset = os.path.getsize(dat)
    with open(dat, "ab") as f:
        f.write(xml)
    raw = bytearray(open(fat, "rb").read())
    if raw[:4] != b"DAT2":
        raise SystemExit("not a DAT2 table")
    count = struct.unpack_from("<I", raw, 4)[0]
    want = int(a.entry, 16)
    for i in range(count):
        o = 8 + 16 * i
        off, stored, unpacked, h = struct.unpack_from("<IIII", raw, o)
        if h == want:
            struct.pack_into("<IIII", raw, o, offset, len(xml), len(xml), h)
            open(fat, "wb").write(raw)
            print(f"canary_mod_level: {a.level} entry {want:08X} -> offset {offset}, {len(xml)} bytes uncompressed "
                  f"(was {stored}/{unpacked}); Respawn0 {a.respawn}{', enemies off' if a.no_enemies else ''}")
            return 0
    raise SystemExit("entry not found")


if __name__ == "__main__":
    main()
