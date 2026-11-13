#!/usr/bin/env python3
"""Decrypts and unpacks an XEX2 file into its memory image (offset 0 = base address).

    python tools/xexunpack.py <file.xex> <output.bin> [--report <report.json>]

Supported: AES encryption (retail and devkit keys), no/basic compression. LZX is reported
but not unpacked (use XenonRecomp/ReXGlue for that). Prints sections, .pdata statistics and the resolved
imports (library, ordinal, variable/thunk).

The output is game code: it belongs in work/ and never in the repository.
Needs pycryptodome (toolchain\\venv).
"""
import argparse
import json
import struct
import sys

from Crypto.Cipher import AES

import xexinfo

# Publicly documented XEX2 keys (free60.org; also contained in Xenia and XenonRecomp)
RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def unpack(path):
    info = xexinfo.parse(path)
    with open(path, "rb") as f:
        data = f.read()
    pe = data[info["pe_data_offset"]:]
    ff = info["file_format"]
    header = data[:info["pe_data_offset"]]

    # read the raw data of file_format_info once more (block list)
    count = xexinfo.u32(header, 0x14)
    blocks = None
    enc = comp = 0
    for i in range(count):
        key, val = xexinfo.u32(header, 0x18 + i * 8), xexinfo.u32(header, 0x1C + i * 8)
        if key == 0x000003FF:
            size = xexinfo.u32(header, val)
            raw = header[val:val + size]
            enc, comp = xexinfo.u16(raw, 4), xexinfo.u16(raw, 6)
            if comp == 1:
                blocks = [(xexinfo.u32(raw, o), xexinfo.u32(raw, o + 4)) for o in range(8, size, 8)]
    if comp not in (0, 1):
        raise SystemExit(f"compression '{ff['compression']}' is not unpacked here")

    file_key = bytes.fromhex(info["security"]["file_key_encrypted"])
    image_size = int(info["security"]["image_size"], 16)
    candidates = [("retail", RETAIL_KEY), ("devkit", DEVKIT_KEY)] if enc else [("none", None)]
    for name, master in candidates:
        if master is None:
            plain = pe
        else:
            session = AES.new(master, AES.MODE_ECB).decrypt(file_key)
            usable = len(pe) - len(pe) % 16
            plain = AES.new(session, AES.MODE_CBC, iv=bytes(16)).decrypt(pe[:usable])
        if comp == 1:
            out, pos = bytearray(), 0
            for data_size, zero_size in blocks:
                out += plain[pos:pos + data_size]
                out += bytes(zero_size)
                pos += data_size
            image = bytes(out)
        else:
            image = plain
        if image[:2] == b"MZ":
            info["key_used"] = name
            break
    else:
        raise SystemExit("decryption yields no PE (neither the retail nor the devkit key fits)")
    if len(image) < image_size:
        image += bytes(image_size - len(image))
    return info, image[:image_size]


def pe_sections(image):
    lfanew = struct.unpack_from("<I", image, 0x3C)[0]
    assert image[lfanew:lfanew + 4] == b"PE\0\0"
    machine, nsec, timestamp = struct.unpack_from("<HHI", image, lfanew + 4)
    opt_size = struct.unpack_from("<H", image, lfanew + 20)[0]
    opt = lfanew + 24
    dirs = {}
    names = ["export", "import", "resource", "exception", "security", "basereloc", "debug", "arch", "globalptr",
             "tls", "load_config", "bound_import", "iat", "delay_import", "com", "reserved"]
    ndirs = struct.unpack_from("<I", image, opt + 92)[0]
    for i in range(min(ndirs, 16)):
        rva, size = struct.unpack_from("<II", image, opt + 96 + i * 8)
        if rva or size:
            dirs[names[i]] = (rva, size)
    sections = []
    table = opt + opt_size
    for i in range(nsec):
        o = table + i * 40
        name = image[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", image, o + 8)
        chars = struct.unpack_from("<I", image, o + 36)[0]
        sections.append({"name": name, "rva": vaddr, "virtual_size": vsize, "raw_size": rsize, "raw_ptr": rptr,
                         "flags": chars, "code": bool(chars & 0x20), "exec": bool(chars & 0x20000000),
                         "write": bool(chars & 0x80000000)})
    return {"machine": f"0x{machine:04X}", "timestamp": timestamp, "directories": dirs, "sections": sections}


def pdata_stats(image, base, pe):
    """Xbox 360 .pdata: 8 bytes per function: BeginAddress, then the bit field
    PrologLength:8, FunctionLength:22 (in instructions), 32Bit:1, ExceptionFlag:1 (from the lowest bit up)."""
    if "exception" not in pe["directories"]:
        return None
    rva, size = pe["directories"]["exception"]
    entries = []
    for o in range(rva, rva + size, 8):
        begin, bits = struct.unpack_from(">II", image, o)
        if begin == 0:
            continue
        prolog = bits & 0xFF
        length = (bits >> 8) & 0x3FFFFF
        entries.append((begin, length * 4, prolog * 4, bits >> 30))
    entries.sort()
    total = sum(e[1] for e in entries)
    return {"rva": f"0x{rva:X}", "size": size, "functions": len(entries), "code_bytes": total,
            "first": f"0x{entries[0][0]:08X}", "last_end": f"0x{entries[-1][0] + entries[-1][1]:08X}",
            "with_exception_flag": sum(1 for e in entries if e[3] & 2),
            "overlaps": sum(1 for a, b in zip(entries, entries[1:]) if a[0] + a[1] > b[0]),
            "largest": [f"0x{e[0]:08X}:{e[1]}" for e in sorted(entries, key=lambda e: -e[1])[:5]]}, entries


def imports(info, image, base):
    result = []
    for lib in info.get("import_libraries", []):
        seen = {}
        for addr in lib["record_addresses"]:
            value = struct.unpack_from(">I", image, addr - base)[0]
            kind = value >> 24  # 0 = variable (address slot), 1 = thunk (jump stub)
            ordinal = value & 0xFFFF
            entry = seen.setdefault(ordinal, {"ordinal": ordinal, "variable": None, "thunk": None})
            entry["thunk" if kind == 1 else "variable"] = f"0x{addr:08X}"
        result.append({"name": lib["name"], "version": lib["version"],
                       "imports": sorted(seen.values(), key=lambda e: e["ordinal"])})
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xex")
    ap.add_argument("out")
    ap.add_argument("--report")
    args = ap.parse_args()

    info, image = unpack(args.xex)
    base = int(info["image_base_address"], 16)
    with open(args.out, "wb") as f:
        f.write(image)
    pe = pe_sections(image)
    stats = pdata_stats(image, base, pe)
    imps = imports(info, image, base)
    for lib in info.get("import_libraries", []):
        lib.pop("record_addresses", None)

    print(f"{args.xex}: key {info['key_used']}, image {len(image)} bytes from 0x{base:08X}, "
          f"entry {info['entry_point']}")
    print(f"PE machine {pe['machine']}, timestamp {pe['timestamp']}")
    print("Directories:", {k: (hex(v[0]), v[1]) for k, v in pe["directories"].items()})
    print(f"{'Section':10} {'Address':>10} {'virt':>9} {'raw':>9}  Flags")
    for s in pe["sections"]:
        attrs = "".join(c for c, on in (("C", s["code"]), ("X", s["exec"]), ("W", s["write"])) if on)
        print(f"{s['name']:10} 0x{base + s['rva']:08X} {s['virtual_size']:9d} {s['raw_size']:9d}  "
              f"0x{s['flags']:08X} {attrs}")
    if stats:
        print(".pdata:", json.dumps(stats[0]))
    for lib in imps:
        funcs = sum(1 for e in lib["imports"] if e["thunk"])
        varis = sum(1 for e in lib["imports"] if not e["thunk"])
        print(f"Imports {lib['name']}: {len(lib['imports'])} ordinals ({funcs} functions, {varis} variables)")
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump({"info": info, "pe": pe, "pdata": stats[0] if stats else None, "imports": imps,
                       "pdata_entries": [[e[0], e[1], e[2], e[3]] for e in stats[1]] if stats else []}, f, indent=1)


if __name__ == "__main__":
    main()
