#!/usr/bin/env python3
"""Helper for delimiting statically linked libraries: call relations per address window.

    bounds.py <lo> <hi> [step]

Per window: number of functions, call sites from below ENG_HI (engine/game code, < 0x82620000), calls into
the D3D library, into kernel imports, and the first string a function of the window uses.
A library start shows up as a jump in these columns (the linker places the object files of a .lib together).
"""
import sys

import d3dlib

ENG_HI = 0x82620000


def main():
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    step = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x2000
    im = d3dlib.Img()
    cm = im.callee_map()
    x = lo
    while x < hi:
        fs = im.funcs_in(x, x + step)
        from_eng = from_self = to_d3d = to_imp = to_eng = 0
        s = ""
        for f, e in fs:
            for site in im.caller_sites(f):
                if site < ENG_HI:
                    from_eng += 1
            for site, t, k in cm.get(f, []):
                if d3dlib.D3D_LO <= t < d3dlib.D3D_HI:
                    to_d3d += 1
                elif t in im.ix.imports:
                    to_imp += 1
                elif t < ENG_HI:
                    to_eng += 1
            if not s:
                si = im.ix.strings_in(f)
                if si:
                    s = im.ix.strings[si[0][1]][0][:48]
        print("%08X n=%-3d fromEngine=%-4d ->D3D=%-3d ->Import=%-3d ->Engine=%-3d %r" % (x, len(fs), from_eng, to_d3d, to_imp, to_eng, s))
        x += step


if __name__ == "__main__":
    main()
