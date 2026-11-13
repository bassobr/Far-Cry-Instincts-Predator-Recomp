#!/usr/bin/env python3
"""Checks a linked Windows PE (build step after linking predator.exe).

    python -I tools/check_exe.py <file.exe|dll> [--require-no-high-entropy] [--imports]

Prints machine, size of .text, DllCharacteristics (DYNAMICBASE, HIGH_ENTROPY_VA, NX_COMPAT ...) and the
directly and delay-imported DLLs. With --require-no-high-entropy it exits with code 1
if HIGH_ENTROPY_VA is set (predator.exe is linked with /HIGHENTROPYVA:NO). Reads headers only.
"""
import argparse
import struct
import sys

DLL_CHARS = {
    0x0020: "HIGH_ENTROPY_VA", 0x0040: "DYNAMIC_BASE", 0x0080: "FORCE_INTEGRITY", 0x0100: "NX_COMPAT",
    0x0200: "NO_ISOLATION", 0x0400: "NO_SEH", 0x0800: "NO_BIND", 0x1000: "APPCONTAINER", 0x2000: "WDM_DRIVER",
    0x4000: "GUARD_CF", 0x8000: "TERMINAL_SERVER_AWARE",
}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pe")
    ap.add_argument("--require-no-high-entropy", action="store_true")
    ap.add_argument("--imports", action="store_true", help="print imported DLLs")
    a = ap.parse_args()

    with open(a.pe, "rb") as f:
        d = f.read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{a.pe}: not a PE")
    machine, nsec = struct.unpack_from("<HH", d, pe + 4)
    opt_size = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        raise SystemExit(f"{a.pe}: not PE32+")
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    dll_chars = struct.unpack_from("<H", d, opt + 70)[0]
    ndirs = struct.unpack_from("<I", d, opt + 108)[0]
    dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(ndirs)]
    secs = []
    for i in range(nsec):
        o = opt + opt_size + 40 * i
        name = d[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rsize, rptr = struct.unpack_from("<IIII", d, o + 8)
        secs.append((name, va, vsize, rptr, rsize))

    def rva2off(rva):
        for _, va, vs, rp, rs in secs:
            if va <= rva < va + max(vs, rs):
                return rp + rva - va
        raise ValueError(f"RVA 0x{rva:X} in no section")

    def cstr(off):
        return d[off:d.index(b"\0", off)].decode("latin-1")

    flags = [n for b, n in DLL_CHARS.items() if dll_chars & b]
    code = []
    for i in range(nsec):
        o = opt + opt_size + 40 * i
        if struct.unpack_from("<I", d, o + 36)[0] & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            code.append(f"{secs[i][0]} {secs[i][2]}")
    print(f"{a.pe}: machine 0x{machine:04X}, ImageBase 0x{image_base:X}, code sections (bytes): "
          f"{', '.join(code)}")
    print(f"  DllCharacteristics 0x{dll_chars:04X}: {' '.join(flags)}")

    if a.imports:
        def walk(idx, entry_size, name_off):
            names = []
            if idx >= len(dirs) or not dirs[idx][0]:
                return names
            off = rva2off(dirs[idx][0])
            while True:
                ent = d[off:off + entry_size]
                if len(ent) < entry_size or not any(ent):
                    break
                names.append(cstr(rva2off(struct.unpack_from("<I", ent, name_off)[0])))
                off += entry_size
            return names

        print("  imports:", ", ".join(walk(1, 20, 12)) or "-")
        print("  delay imports:", ", ".join(walk(13, 32, 4)) or "-")

    if a.require_no_high_entropy and dll_chars & 0x20:
        print("ERROR: HIGH_ENTROPY_VA is set (expected /HIGHENTROPYVA:NO)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
