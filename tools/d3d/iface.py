#!/usr/bin/env python3
"""Interface of the engine to the D3D library: which library functions are called from outside?

    iface.py            table on stdout (Markdown), raw data to work/d3d/iface.tsv

Callers are separated by origin: engine/game (< CRT), CRT/XAPI, D3DX9, other XDK libraries,
XGRAPHC, static initializers at the end of .text. In addition: the engine functions with the most
D3D calls (renderer layer) and the calls through the setter table per state.
"""
import os
from collections import Counter, defaultdict

import d3dlib

# Boundaries measured with bounds.py from strings, imports and call relations per address window (link order at
# the upper end of .text); the D3D9 range is the one of d3dlib.D3D_LO/D3D_HI.
ZONES = [
    (0x820B0000, 0x82620000, "engine"),
    (0x82620000, 0x8264A980, "crt_xapi"),
    (0x8264A980, 0x8272FA00, "d3dx"),
    (0x8272FA00, 0x8274B9F8, "d3d"),
    (0x8274B9F8, 0x8277FB40, "xdk_other"),
    (0x8277FB40, 0x82866000, "xgraphc"),
    (0x82866000, 0x82880000, "init_thunks"),
    (0x82880000, 0x83000000, "bink"),
]


def zone(a):
    for lo, hi, n in ZONES:
        if lo <= a < hi:
            return n
    return "?"


def main():
    im = d3dlib.Img()
    lib = d3dlib.Lib(im, d3dlib.D3D_LO, d3dlib.D3D_HI)
    rows = []
    eng_callers = Counter()
    eng_targets = defaultdict(set)
    for f, e, pd in lib.funcs:
        c = Counter()
        fs = set()
        for s in im.caller_sites(f):
            z = zone(s)
            if z == "d3d":
                continue
            c[z] += 1
            if z == "engine":
                g = im.func_of(s)
                fs.add(g)
                eng_callers[g] += 1
                eng_targets[g].add(f)
        if c:
            rows.append((f, e - f, c, fs))
    rows.sort(key=lambda r: (-r[2]["engine"], -sum(r[2].values()), r[0]))
    p = os.path.join(d3dlib.OUT, "iface.tsv")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("address\tname\tsize\tengine\td3dx\txgraphc\tother\tengine_functions\n")
        for f, sz, c, fs in rows:
            other = sum(v for k, v in c.items() if k not in ("engine", "d3dx", "xgraphc"))
            fh.write("%08X\t%s\t%d\t%d\t%d\t%d\t%d\t%s\n" % (f, im.names().get(f, ""), sz, c["engine"], c["d3dx"], c["xgraphc"],
                                                         other, " ".join("%08X" % g for g in sorted(x for x in fs if x))))
    print("Library functions called from outside: %d (engine: %d functions, %d call sites; D3DX: %d sites; "
          "XGRAPHC: %d sites)" % (
              len(rows), sum(1 for r in rows if r[2]["engine"]), sum(r[2]["engine"] for r in rows),
              sum(r[2]["d3dx"] for r in rows), sum(r[2]["xgraphc"] for r in rows)))
    print()
    print("| Address | Name | Engine | D3DX | XGRAPHC | Other | Engine functions |")
    print("|---|---|---:|---:|---:|---:|---:|")
    for f, sz, c, fs in rows:
        other = sum(v for k, v in c.items() if k not in ("engine", "d3dx", "xgraphc"))
        print("| 0x%08X | %s | %d | %d | %d | %d | %d |" % (f, im.names().get(f, "?"), c["engine"], c["d3dx"], c["xgraphc"], other, len(fs)))
    print()
    print("Engine functions with the most direct D3D calls:")
    for g, n in eng_callers.most_common(45):
        print("  %08X: %d sites, %d different D3D functions" % (g or 0, n, len(eng_targets[g])))
    # distribution of the engine callers per 64K block
    blk = Counter((g or 0) >> 16 for g in eng_callers.elements())
    print("Call sites per 64K block: " + ", ".join("%04Xxxxx: %d" % (b, n) for b, n in sorted(blk.items())))
    print("-> %s" % p)


if __name__ == "__main__":
    main()
