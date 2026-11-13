#!/usr/bin/env python3
"""vdecl_scan.py: find static vertex declarations (D3DVERTEXELEMENT9 arrays) in the EXE image.

An element is 12 bytes (big-endian): u16 stream, u16 offset, u32 type, u8 method, u8 usage, u8 usageIndex, u8 0.
The end is D3DDECL_END = {0xFF, 0, D3DDECLTYPE_UNUSED (0xFFFFFFFF), 0, 0, 0}. The search looks for the end element,
then walks backwards as long as plausible elements precede it (stream < 16, method 0, usage < 14, index < 16, offset
ascending per stream). On the 360 the type encodes the GPU format (bits 0-5 VertexFormat, plus sign,
normalisation, endian, swizzle); here it is printed raw and with bits 0-5.

Usage: vdecl_scan.py <default.bin>
Candidates only: declarations that the game builds from model data at runtime are missing here.
"""
import collections
import struct
import sys

BASE = 0x82000000
USAGE = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
         "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]
END = struct.pack(">HHIBBBB", 0xFF, 0, 0xFFFFFFFF, 0, 0, 0, 0)


def main():
    img = open(sys.argv[1], "rb").read()
    decls = []
    o = img.find(END)
    while o >= 0:
        els = []
        p = o - 12
        while p >= 0:
            st, off, typ, meth, use, idx, pad = struct.unpack_from(">HHIBBBB", img, p)
            if st >= 16 or meth != 0 or use >= 14 or idx >= 16 or pad != 0 or typ in (0, 0xFFFFFFFF) or off > 256:
                break
            els.insert(0, (st, off, typ, use, idx))
            p -= 12
        if els and all(a[1] <= b[1] or a[0] != b[0] for a, b in zip(els, els[1:])):
            decls.append((BASE + p + 12, els))
        o = img.find(END, o + 4)
    types = collections.Counter()
    for addr, els in decls:
        print("0x%08X: %s" % (addr, ", ".join("s%d+%d %s%d type %08X(f%d)" % (s, off, USAGE[u], i, t, t & 0x3F)
                                              for s, off, t, u, i in els)))
        for s, off, t, u, i in els:
            types["%08X" % t] += 1
    print("%d declarations; types: %s" % (len(decls), dict(types.most_common())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
