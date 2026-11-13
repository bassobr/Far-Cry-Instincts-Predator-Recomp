#!/usr/bin/env python3
"""Searches D3DFORMAT constants (360 encoding) in the engine code and in data tables.

    formats.py

360 D3DFORMAT (after King Kong Recompiled, docs/d3d-structs.md, MIT): bits 0-5 GPU texture format, 6-7 endian,
8 tiled, 9-16 sign, 17 number format, 18-29 swizzle. Searched are lis+ori constants in the code outside the
XDK libraries and aligned words in .rdata/.data whose upper 14 bits match a known pattern.
Names of the GPU formats after Xenia `xenos.h` (BSD-3), enum TextureFormat.
"""
from collections import Counter, defaultdict

import d3dlib
import iface

TEXFMT = {0: "1_REVERSE", 1: "1", 2: "8", 3: "1_5_5_5", 4: "5_6_5", 5: "6_5_5", 6: "8_8_8_8", 7: "2_10_10_10", 8: "8_A",
          9: "8_B", 10: "8_8", 11: "Cr_Y1_Cb_Y0_REP", 12: "Y1_Cr_Y0_Cb_REP", 13: "16_16_EDRAM", 14: "8_8_8_8_A",
          15: "4_4_4_4", 16: "10_11_11", 17: "11_11_10", 18: "DXT1", 19: "DXT2_3", 20: "DXT4_5",
          21: "16_16_16_16_EDRAM", 22: "24_8", 23: "24_8_FLOAT", 24: "16", 25: "16_16", 26: "16_16_16_16",
          27: "16_EXPAND", 28: "16_16_EXPAND", 29: "16_16_16_16_EXPAND", 30: "16_FLOAT", 31: "16_16_FLOAT",
          32: "16_16_16_16_FLOAT", 33: "32", 34: "32_32", 35: "32_32_32_32", 36: "32_FLOAT", 37: "32_32_FLOAT",
          38: "32_32_32_32_FLOAT", 39: "32_AS_8", 40: "32_AS_8_8", 41: "16_MPEG", 42: "16_16_MPEG",
          43: "8_INTERLACED", 44: "32_AS_8_INTERLACED", 45: "32_AS_8_8_INTERLACED", 46: "16_INTERLACED",
          47: "16_MPEG_INTERLACED", 48: "16_16_MPEG_INTERLACED", 49: "DXN", 50: "8_8_8_8_AS_16_16_16_16",
          51: "DXT1_AS_16_16_16_16", 52: "DXT2_3_AS_16_16_16_16", 53: "DXT4_5_AS_16_16_16_16",
          54: "2_10_10_10_AS_16_16_16_16", 55: "10_11_11_AS_16_16_16_16", 56: "11_11_10_AS_16_16_16_16",
          57: "32_32_32_FLOAT", 58: "DXT3A", 59: "DXT5A", 60: "CTX1", 61: "DXT3A_AS_1_1_1_1",
          62: "8_8_8_8_GAMMA_EDRAM", 63: "2_10_10_10_FLOAT_EDRAM"}

# upper 16 bits that occur in XDK D3DFMT values (swizzle/sign combinations)
HI = {0x1828, 0x2828, 0x1A20, 0x1A22, 0x2DA0, 0x2D20, 0x2DA2, 0x2800, 0x0490, 0x0800, 0x1A28, 0x3828, 0x0028, 0x1820,
      0x2820, 0x1A2A, 0x2D22, 0x0828, 0x1A00, 0x2D00}


def looks(v):
    return (v >> 16) in HI and (v & 0xFE00) in (0x0000, 0xAA00, 0x8200, 0x2800) and (v & 0xFFFF) != 0 and \
        ((v >> 6) & 3) in (0, 1, 2)


def desc(v):
    return "%s%s%s" % (TEXFMT.get(v & 0x3F, "?"), " tiled" if v & 0x100 else " linear", " endian%d" % ((v >> 6) & 3))


def main():
    im = d3dlib.Img()
    code = defaultdict(list)
    for v, refs in im.ix.refs.items():
        if not looks(v):
            continue
        for a, kind in refs:
            if kind == "ori":
                code[v].append(a)
    print("Constants in the code (lis+ori), by zone:")
    for v in sorted(code):
        zs = Counter(iface.zone(a) for a in code[v])
        if not (zs["engine"]):
            continue
        fs = sorted({im.func_of(a) or 0 for a in code[v] if iface.zone(a) == "engine"})
        print("  0x%08X %-34s engine %d sites in %d functions (%s)%s" % (
            v, desc(v), zs["engine"], len(fs), " ".join("%08X" % f for f in fs[:6]),
            "; libraries %d" % (sum(zs.values()) - zs["engine"]) if sum(zs.values()) != zs["engine"] else ""))
    # tables in the data
    print()
    print("Data tables (at least 3 hits in 64 bytes):")
    for s in im.meta["pe"]["sections"]:
        if s["code"] or s["name"] not in (".rdata", ".data"):
            continue
        lo = d3dlib.BASE + s["rva"]
        n = min(s["virtual_size"], s["raw_size"]) // 4
        ws = im.words(lo, n)
        hits = [i for i, w in enumerate(ws) if looks(w)]
        i = 0
        while i < len(hits):
            j = i
            while j + 1 < len(hits) and hits[j + 1] - hits[j] <= 8:
                j += 1
            if j - i >= 2:
                a = lo + 4 * hits[i]
                vals = [ws[k] for k in hits[i:j + 1]]
                refs = im.ix.refs.get(a, [])
                print("  %08X..%08X %d values, code references to the start: %s" % (
                    a, lo + 4 * hits[j], len(vals), " ".join("%08X" % (im.func_of(r[0]) or 0) for r in refs[:4])))
                c = Counter(vals)
                for v, k in sorted(c.items()):
                    print("      0x%08X x%d %s" % (v, k, desc(v)))
            i = j + 1


if __name__ == "__main__":
    main()
