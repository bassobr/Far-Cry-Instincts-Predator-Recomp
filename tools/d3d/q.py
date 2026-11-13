#!/usr/bin/env python3
"""Queries for mapping the D3D library.

    q.py region <lo> <hi>      functions in the range: size, callers inside/outside, imports, strings
    q.py fn <address> [max]    disassemble a whole function, branch targets named
    q.py dis <address> <n>     n instructions from the address
    q.py callers <address>     callers (site, function, name)
    q.py callees <address>     callees
    q.py words <address> <n>   n data words (with pointer interpretation)
    q.py tbl <address> <n>     n function pointers from the address, with the size of the target function
"""
import sys

import d3dlib


def h(x):
    return int(x, 16)


def region(im, lo, hi):
    for f, e in im.funcs_in(lo, hi):
        sites = im.caller_sites(f)
        inn = sum(1 for s in sites if lo <= s < hi)
        out = len(sites) - inn
        imps = sorted({im.ix.imports[t][2] for _, t, _ in im.callees(f) if t in im.ix.imports})
        strs = [im.ix.strings[s][0][:40] for _, s in im.ix.strings_in(f)][:3]
        ptr = len(im.ix.ptrs.get(f, []))
        print("%08X %5d %s in=%-3d out=%-3d ptr=%-2d %s %s" % (
            f, e - f, "P" if f in im.ix.pseudo else " ", inn, out, ptr, im.name(f) if f in im.names() else "",
            " ".join(imps) + (" | " + repr(strs) if strs else "")))


def fn(im, a, maxn=4000):
    f = im.func_of(a)
    if f is None:
        print("no function")
        return
    lo, hi = im.frange(f)
    print("; %08X-%08X %d bytes %s" % (lo, hi, hi - lo, im.name(f)))
    dis(im, lo, min((hi - lo) // 4, maxn))


def dis(im, a, n):
    for i in range(n):
        x = a + 4 * i
        t = im.dis1(x)
        note = ""
        w = im.u32(x)
        if w >> 26 == 18:
            tg = d3dlib.branch_target(x, w)
            fo = im.func_of(tg)
            if tg in im.ix.imports or tg in im.names() or (fo == tg and (w & 1)):
                note = "; " + im.name(tg)
        print("%08X  %-44s %s" % (x, t, note))


def main():
    im = d3dlib.Img()
    c = sys.argv[1]
    if c == "region":
        region(im, h(sys.argv[2]), h(sys.argv[3]))
    elif c == "fn":
        fn(im, h(sys.argv[2]), int(sys.argv[3]) if len(sys.argv) > 3 else 4000)
    elif c == "dis":
        dis(im, h(sys.argv[2]), int(sys.argv[3]))
    elif c == "callers":
        f = h(sys.argv[2])
        for s in sorted(im.caller_sites(f)):
            fo = im.func_of(s)
            print("%08X in %s %s" % (s, "%08X" % fo if fo else "-", im.name(fo) if fo in im.names() else ""))
    elif c == "callees":
        f = h(sys.argv[2])
        for s, t, k in im.callees(f):
            print("%08X %-2s %08X %s" % (s, k, t, im.name(t)))
    elif c == "words":
        a = h(sys.argv[2])
        for i, w in enumerate(im.words(a, int(sys.argv[3]))):
            fo = im.func_of(w) if 0x82000000 <= w < 0x82980000 else None
            s = im.ix.strings.get(w)
            print("%08X +%-5d %08X %s%s" % (a + 4 * i, 4 * i, w, ("-> fn %s" % im.name(fo)) if fo == w else "",
                                           (" -> %r" % s[0][:50]) if s else ""))
    elif c == "tbl":
        a = h(sys.argv[2])
        for i, w in enumerate(im.words(a, int(sys.argv[3]))):
            r = im.frange(w) if im.func_of(w) == w else None
            print("%3d +%-4d %08X %s %s" % (i, 4 * i, w, ("%d" % (r[1] - r[0])) if r else "?", im.name(w) if w in im.names() else ""))


if __name__ == "__main__":
    main()
