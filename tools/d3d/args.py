#!/usr/bin/env python3
"""Constant arguments at call sites: args.py <target> [zone]

For every call site (bl/b) of the target, the last assignments to r3..r10 in the same function piece are
searched backwards (at most 48 instructions, up to the previous call). Constants (li, lis+ori/addi) are shown as
numbers, copies as `=rN`, loads as `[off(rN)]`, everything else as `?`. zone: engine | d3dx | all (default engine).
"""
import sys

import d3dlib
import iface
from d3dlib import s16


def arg_values(im, site, f):
    vals = {}
    need = set(range(3, 11))
    lo = max(f, site - 4 * 48)
    a = site - 4
    pend_ori = {}
    while a >= lo and need:
        w = im.u32(a)
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 18 and (w & 1):
            break
        if op == 19 and ((w >> 1) & 0x3FF) == 528 and (w & 1):
            break
        tgt = None
        val = None
        if op == 14:
            tgt = rd
            if ra == 0:
                val = "0x%X" % (s16(imm) & 0xFFFFFFFF)
            else:
                val = "r%d%+d" % (ra, s16(imm))
                if rd in need and ra == rd:
                    pend_ori[rd] = ("addi", s16(imm))
                    a -= 4
                    continue
        elif op == 15 and ra == 0:
            tgt = rd
            hi = imm << 16
            if rd in pend_ori:
                k, v = pend_ori.pop(rd)
                val = "0x%08X" % ((hi | v) if k == "ori" else ((hi + v) & 0xFFFFFFFF))
            else:
                val = "0x%08X" % hi
        elif op == 24:
            tgt = ra
            if ra == rd and ra in need:
                pend_ori[ra] = ("ori", imm)
                a -= 4
                continue
            val = "?"
        elif op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == ((w >> 11) & 31):
            tgt = ra
            val = "=r%d" % rd
        elif op in (32, 33, 34, 35, 40, 41, 42, 43):
            tgt = rd
            val = "[%d(r%d)]" % (s16(imm), ra)
        elif op in (20, 21, 23, 25, 26, 27, 28, 29):
            tgt = ra
            val = "?"
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954):
                tgt = ra
            elif xo not in (151, 183, 215, 407, 439, 663, 695, 0, 32, 4, 68):
                tgt = rd
            val = "?"
        elif op in (7, 8, 12, 13):
            tgt = rd
            val = "?"
        if tgt in need:
            if tgt in pend_ori:
                val = "?"
                pend_ori.pop(tgt)
            vals[tgt] = val
            need.discard(tgt)
        a -= 4
    return vals


def main():
    im = d3dlib.Img()
    t = int(sys.argv[1], 16)
    zone = sys.argv[2] if len(sys.argv) > 2 else "engine"
    for s in sorted(im.caller_sites(t)):
        z = iface.zone(s)
        if zone != "all" and z != zone:
            continue
        f = im.func_of(s) or s
        v = arg_values(im, s, f)
        print("%08X in %08X  %s" % (s, f, "  ".join("r%d=%s" % (r, v.get(r, "-")) for r in range(3, 11))))


if __name__ == "__main__":
    main()
