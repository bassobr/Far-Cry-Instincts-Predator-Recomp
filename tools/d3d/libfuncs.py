#!/usr/bin/env python3
"""Function list of a library range with features, as TSV to work/d3d/.

    libfuncs.py [lo hi] [--out name]

Columns: start, size, pdata, call sites inside/outside (bl+b), data pointers, imports, features.
Features (static, per function):
  ring     writes through the command buffer pointer: `lwz rP,0(rDev)` ... `stwu rX,4(rP)` or `stw rP,0(rDev)`
  kick     calls the segment advance (address KICK, 0x82738618)
  t3:XX    loads a PM4 type-3 header (lis 0xC0nn + ori opcode<<8) with opcode XX
  reg:XXXX loads a constant that looks like a Xenos register index (li/ori 0x2000..0x23FF, 0x4000..0x4FFF)
  pend     ORs a 64-bit word at +16..+48 (dirty mask)
  devoffs  number of different offsets > 0x400 read/written through r3 (or a copy); planned, not computed yet
Options starting with -- are ignored (--out is not implemented; the file name follows the range).
"""
import sys

import d3dlib
from d3dlib import s16

KICK = 0x82738618


def features(im, lo, hi):
    ws = im.words(lo, (hi - lo) // 4)
    feats = []
    t3 = set()
    regs = set()
    ring = False
    kick = False
    pend = False
    lis = {}
    ldoffs = {}
    stwu4 = 0
    for i, w in enumerate(ws):
        a = lo + 4 * i
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            lis[rd] = imm
        elif op == 24:  # ori ra <- rd | imm
            if rd in lis:
                v = (lis[rd] << 16) | imm
                if (v >> 24) == 0xC0 and (v & 0xFE) == 0 and ((v >> 8) & 0x7F) != 0:  # bit 0 = predicate
                    t3.add((v >> 8) & 0x7F)
                elif (v >> 16) < 0x100 and (0x2000 <= imm <= 0x23FF or 0x4000 <= imm <= 0x4FFF or 0x0500 <= imm <= 0x05FF):
                    regs.add(imm)
                if ra != rd:
                    lis.pop(ra, None)
        elif op == 14 and ra == 0:
            v = imm
            if 0x2000 <= v <= 0x23FF or 0x4000 <= v <= 0x4FFF:
                regs.add(v)
            lis.pop(rd, None)
        elif op == 37 and s16(imm) == 4:  # stwu rX,4(rP)
            stwu4 += 1
        elif op == 18 and (w & 1):
            if d3dlib.branch_target(a, w) == KICK:
                kick = True
        elif op == 58 and (w & 3) == 0:  # ld
            ldoffs[(rd)] = (ra, s16(imm & 0xFFFC))
        elif op == 62 and (w & 3) == 0:  # std
            k = ldoffs.get(rd)
            off = s16(imm & 0xFFFC)
            if 0 <= off <= 0x30:
                pend = True
    if stwu4 >= 2:
        ring = True
    if ring:
        feats.append("ring%d" % stwu4)
    if kick:
        feats.append("kick")
    for o in sorted(t3):
        feats.append("t3:%02X" % o)
    for r in sorted(regs)[:8]:
        feats.append("reg:%04X" % r)
    if pend:
        feats.append("pend")
    return feats


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    lo, hi = (int(args[0], 16), int(args[1], 16)) if len(args) >= 2 else (d3dlib.D3D_LO, d3dlib.D3D_HI)
    name = "libfuncs_%08X_%08X.tsv" % (lo, hi)
    im = d3dlib.Img()
    lib = d3dlib.Lib(im, lo, hi)
    rows = []
    for f, e, pd in lib.funcs:
        sites = im.caller_sites(f)
        inn = sum(1 for s in sites if lo <= s < hi)
        out = len(sites) - inn
        imps = set()
        for t, srcs in im.ix.calls.items():
            if t in im.ix.imports and any(f <= s < e for s in srcs):
                imps.add(im.ix.imports[t][2])
        ptr = len(im.ix.ptrs.get(f, []))
        strs = []
        rows.append((f, e - f, int(pd), inn, out, ptr, ",".join(sorted(imps)), " ".join(features(im, f, e)),
                     im.names().get(f, "")))
    import os
    p = os.path.join(d3dlib.OUT, name)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("start\tsize\tpdata\tinside\toutside\tpointers\timports\tfeatures\tname\n")
        for r in rows:
            fh.write("%08X\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\n" % r)
    npd = sum(r[2] for r in rows)
    print("%d functions (%d with .pdata, %d without) in %08X-%08X, %d called from outside (%d sites) -> %s" % (
        len(rows), npd, len(rows) - npd, lo, hi, sum(1 for r in rows if r[4]), sum(r[4] for r in rows), p))


if __name__ == "__main__":
    main()
