"""Reads device dumps (FCP_DUMP_DEVICE, 20,608 bytes, big endian) and shows the known fields of the D3D device
(mapped statically from the D3D library) and King Kong's layout of the register shadows (+11456 ff.).

  python -I tools/boot/devdump.py <device_A.bin> [<device_B.bin> ...]

With two or more dumps also: the number of changed 32-bit words per 64-byte block range.
"""

import struct
import sys

FIELDS = [
    ("+0x000C Referenzzaehler", 0x000C),
    ("+0x2890 Fence-Rueckschreibadresse", 0x2890),
    ("+0x289C aktuelle Fence", 0x289C),
    ("+0x28A4 SQ_GPR_MANAGEMENT", 0x28A4),
    ("+0x2C90 Vertex-Deklaration", 0x2C90),
    ("+0x2D74 RB_DEPTHCONTROL (gemessen)", 0x2D74),
    ("+0x30F4 Indexpuffer", 0x30F4),
    ("+0x30F8 Render-Target 0", 0x30F8),
    ("+0x30FC Render-Target 1", 0x30FC),
    ("+0x3108 Tiefe/Stencil", 0x3108),
    ("+0x310C Stream 0 Offset", 0x310C),
    ("+0x3110 Stream 0 Puffer", 0x3110),
    ("+0x3290 Pixel-Shader", 0x3290),
    ("+0x32A0 Praedikationsmaske", 0x32A0),
    ("+0x35DC PRESENTINTERVAL (Predator)", 0x35DC),
    ("+0x35CC PRESENTINTERVAL-Lage King Kong", 0x35CC),
    ("+0x4FFC Vertex-Shader", 0x4FFC),
    ("+11456 RB_SURFACE_INFO (KK)", 11456),
    ("+11460 RB_COLOR_INFO (KK)", 11460),
    ("+11464 RB_DEPTH_INFO (KK)", 11464),
    ("+11640 RB_BLENDCONTROL0 (KK)", 11640),
    ("+11644 RB_COLORCONTROL (KK)", 11644),
    ("+11648 PA_CL_CLIP_CNTL (KK)", 11648),
    ("+11652 PA_SU_SC_MODE_CNTL (KK)", 11652),
    ("+11656 PA_CL_VTE_CNTL (KK)", 11656),
]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    dumps = [open(p, "rb").read() for p in sys.argv[1:]]
    for path, d in zip(sys.argv[1:], dumps):
        print(f"{path}: {len(d)} bytes")
        u = lambda o: struct.unpack_from(">I", d, o)[0]
        for name, off in FIELDS:
            print(f"  {name:40s} {u(off):08X}")
        print("  +0x3208 Viewport (x,y,w,h,minz,maxz)", struct.unpack_from(">IIIIff", d, 0x3208))
        print("  +0x3220 Scissor (l,t,r,b)        ", struct.unpack_from(">iiii", d, 0x3220))
        gamma = struct.unpack_from(">8I", d, 0x36FC)
        print("  +0x36FC gamma ramp (first 8 words)  ", " ".join(f"{g:08X}" for g in gamma))
    if len(dumps) >= 2:
        a, b = dumps[0], dumps[-1]
        changed = [o for o in range(0, min(len(a), len(b)), 4) if a[o:o + 4] != b[o:o + 4]]
        print(f"changed words {sys.argv[1]} -> {sys.argv[-1]}: {len(changed)}")
        ranges = []
        for o in changed:
            if ranges and o - ranges[-1][1] <= 64:
                ranges[-1][1] = o
            else:
                ranges.append([o, o])
        for lo, hi in ranges[:40]:
            print(f"  +{lo:5d}..+{hi + 3:5d} (0x{lo:04X}..0x{hi + 3:04X})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
