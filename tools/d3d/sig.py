#!/usr/bin/env python3
"""Short signature of functions: accesses through the argument registers, calls, notable constants.

    sig.py <lo> <hi> [--ext]     all functions in the range (--ext: only those called from outside the D3D range)
    sig.py @<address> ...        single functions

One line per function: address, size, callers inside/outside, then
  a3: W+off R+off ...   accesses through r3 (and copies), likewise a4..a7
  C: call targets (named if known)   K: constants (lis/ori pairs, large li)
The register tracking is linear and only knows mr/addi copies; it is a search tool, not a proof.
"""
import sys

import d3dlib
from d3dlib import s16

LOADS = {32: "", 33: "", 34: "b", 35: "b", 40: "h", 41: "h", 42: "h", 43: "h", 48: "f", 49: "f", 50: "d", 51: "d"}
STORES = {36: "", 37: "", 38: "b", 39: "b", 44: "h", 45: "h", 52: "f", 53: "f", 54: "d", 55: "d"}


def sig(im, lo, hi, maxitems=14):
    ws = im.words(lo, (hi - lo) // 4)
    alias = {3: (3, 0), 4: (4, 0), 5: (5, 0), 6: (6, 0), 7: (7, 0)}
    acc = {3: [], 4: [], 5: [], 6: [], 7: []}
    calls = []
    consts = []
    lis = {}
    for i, w in enumerate(ws):
        a = lo + 4 * i
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):  # mr ra <- rd
            if rd in alias:
                alias[ra] = alias[rd]
            else:
                alias.pop(ra, None)
            lis.pop(ra, None)
            continue
        if op == 14:
            if ra == 0:
                alias.pop(rd, None)
                lis.pop(rd, None)
                v = s16(imm)
                if v > 0x400 or v < -1:
                    consts.append("%X" % (v & 0xFFFFFFFF))
            elif ra in alias:
                alias[rd] = (alias[ra][0], alias[ra][1] + s16(imm))
            else:
                if ra in lis:
                    consts.append("%08X" % (((lis[ra] << 16) + s16(imm)) & 0xFFFFFFFF))
                alias.pop(rd, None)
            continue
        if op == 15:
            if ra == 0:
                lis[rd] = imm
            alias.pop(rd, None)
            continue
        if op == 24:
            if rd in lis:
                consts.append("%08X" % ((lis[rd] << 16) | imm))
            alias.pop(ra, None)
            continue
        if op in LOADS or op in STORES or op in (58, 62):
            off = s16(imm & 0xFFFC) if op in (58, 62) else s16(imm)
            if ra in alias:
                base, d = alias[ra]
                kind = "R" if (op in LOADS or op == 58) else "W"
                suf = "q" if op in (58, 62) else (LOADS.get(op) if op in LOADS else STORES.get(op))
                acc[base].append("%s%s%X" % (kind, suf, d + off))
            elif ra in lis and (op in LOADS or op in STORES):
                consts.append("[%08X]" % (((lis[ra] << 16) + off) & 0xFFFFFFFF))
            if op in LOADS or op == 58:
                alias.pop(rd, None)
                lis.pop(rd, None)
            continue
        if op == 18:
            t = d3dlib.branch_target(a, w)
            if (w & 1) or not (lo <= t < hi):
                if not (0x8262BFB0 <= t < 0x8262C060):
                    calls.append(im.name(t).replace("sub_", ""))
                if w & 1:
                    for r in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0):
                        alias.pop(r, None)
                        lis.pop(r, None)
            continue
        if op == 19 and ((w >> 1) & 0x3FF) == 528 and (w & 1):
            calls.append("(ind)")
            for r in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0):
                alias.pop(r, None)
            continue
        # other instructions: roughly discard the destination
        if op in (20, 21, 23, 25, 26, 27, 28, 29, 30):
            alias.pop(ra, None)
            lis.pop(ra, None)
        elif op == 31:
            alias.pop(rd, None)
            alias.pop(ra, None)
        elif op in (7, 8, 12, 13):
            alias.pop(rd, None)

    def uniq(xs):
        seen, out = set(), []
        for x in xs:
            if x not in seen:
                seen.add(x)
                out.append(x)
        return out

    parts = []
    for r in (3, 4, 5, 6, 7):
        u = uniq(acc[r])
        if u:
            parts.append("a%d:%s%s" % (r, " ".join(u[:maxitems]), " +%d" % (len(u) - maxitems) if len(u) > maxitems else ""))
    c = uniq(calls)
    if c:
        parts.append("C:" + " ".join(c[:10]) + (" +%d" % (len(c) - 10) if len(c) > 10 else ""))
    k = uniq(consts)
    if k:
        parts.append("K:" + " ".join(k[:8]))
    return " | ".join(parts)


def main():
    im = d3dlib.Img()
    lib = d3dlib.Lib(im, d3dlib.D3D_LO, d3dlib.D3D_HI)
    args = sys.argv[1:]
    ext = "--ext" in args
    args = [a for a in args if not a.startswith("--")]
    todo = []
    if args and args[0].startswith("@"):
        for a in args:
            f = int(a[1:], 16)
            if lib.func_of(f) == f:
                todo.append((f, lib.rng(f)[1]))
            else:
                todo.append(im.frange(im.func_of(f)))
    else:
        lo, hi = int(args[0], 16), int(args[1], 16)
        l2 = lib if (lo >= lib.lo and hi <= lib.hi) else d3dlib.Lib(im, lo, hi)
        todo = [(f, e) for f, e, _ in l2.funcs if lo <= f < hi]
    for f, e in todo:
        sites = im.caller_sites(f)
        inn = sum(1 for s in sites if lib.lo <= s < lib.hi)
        out = len(sites) - inn
        if ext and not out:
            continue
        nm = im.names().get(f, "")
        print("%08X %4d i%d o%d p%d %s%s" % (f, e - f, inn, out, len(im.ix.ptrs.get(f, [])), (nm + " " if nm else ""), sig(im, f, e)))


if __name__ == "__main__":
    main()
