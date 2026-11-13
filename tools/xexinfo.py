#!/usr/bin/env python3
"""Reads the headers of an XEX2 file (Xbox 360) and prints them as text.

    python tools/xexinfo.py <file.xex> [--json]

Headers only: execution info (title ID, media ID, version), file format (encryption, compression),
base address, entry point, static libraries (XDK version), import libraries (number of records),
resources, TLS, security info. The image itself is not decrypted or unpacked here.

Field layout per the public XEX2 documentation (free60.org) and Xenia (xex2_info.h, BSD-3).
"""
import json
import struct
import sys

KEYS = {
    0x000002FF: "resource_info", 0x000003FF: "file_format_info", 0x00000405: "base_reference",
    0x000005FF: "delta_patch_descriptor", 0x000080FF: "bounding_path", 0x00008105: "device_id",
    0x00010001: "original_base_address", 0x00010100: "entry_point", 0x00010201: "image_base_address",
    0x000103FF: "import_libraries", 0x00018002: "checksum_timestamp", 0x00018102: "enabled_for_callcap",
    0x00018200: "enabled_for_fastcap", 0x000183FF: "original_pe_name", 0x000200FF: "static_libraries",
    0x00020104: "tls_info", 0x00020200: "default_stack_size", 0x00020301: "default_fs_cache_size",
    0x00020401: "default_heap_size", 0x00028002: "page_heap_size_and_flags", 0x00030000: "system_flags",
    0x00040006: "execution_info", 0x000401FF: "title_workspace_size", 0x00040201: "game_ratings",
    0x00040310: "lan_key", 0x000404FF: "xbox360_logo", 0x000405FF: "multidisc_media_ids",
    0x000406FF: "alternate_title_ids", 0x00040801: "additional_title_memory", 0x00E10402: "exports_by_name",
}
MODULE_FLAGS = ["title", "exports_to_title", "system_debugger", "dll", "module_patch", "full_patch",
                "delta_patch", "user_mode"]
IMAGE_FLAGS = {0x2: "manufacturing_utility", 0x4: "manufacturing_support_tool", 0x8: "xgd2_media_only",
               0x100: "cardea_key", 0x200: "xeika_key", 0x400: "usermode_title", 0x800: "usermode_system",
               0x10000000: "page_size_4kb", 0x20000000: "region_free", 0x40000000: "revocation_check_optional",
               0x80000000: "revocation_check_required"}
APPROVAL = ["unapproved", "possibly approved", "approved", "expired"]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin-1")


def version(v):
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"XEX2":
        raise SystemExit(f"{path}: not XEX2 (magic {data[:4]!r})")
    info = {"file": path, "file_size": len(data), "magic": "XEX2"}
    flags = u32(data, 4)
    info["module_flags"] = [n for i, n in enumerate(MODULE_FLAGS) if flags & (1 << i)]
    info["pe_data_offset"] = u32(data, 8)
    sec = u32(data, 0x10)
    count = u32(data, 0x14)
    opt, unknown = {}, []
    for i in range(count):
        key, val = u32(data, 0x18 + i * 8), u32(data, 0x1C + i * 8)
        size_code = key & 0xFF
        if size_code == 0x00:
            raw, value = None, val
        elif size_code == 0x01:
            raw, value = None, val
        elif size_code == 0xFF:
            raw, value = data[val:val + u32(data, val)], None
        else:
            raw, value = data[val:val + size_code * 4], None
        name = KEYS.get(key)
        if name is None:
            unknown.append(f"0x{key:08X}")
            continue
        opt[name] = (raw, value)
    info["unknown_optional_headers"] = unknown
    info["optional_headers"] = sorted(opt)

    for name in ("original_base_address", "entry_point", "image_base_address", "default_stack_size",
                 "default_fs_cache_size", "default_heap_size", "system_flags", "additional_title_memory"):
        if name in opt:
            info[name] = f"0x{opt[name][1]:08X}"

    if "execution_info" in opt:
        b = opt["execution_info"][0]
        info["execution_info"] = {
            "media_id": f"{u32(b, 0):08X}", "version": version(u32(b, 4)), "base_version": version(u32(b, 8)),
            "title_id": f"{u32(b, 12):08X}", "platform": b[16], "executable_table": b[17],
            "disc_number": b[18], "disc_count": b[19], "savegame_id": f"{u32(b, 20):08X}",
        }
    if "file_format_info" in opt:
        b = opt["file_format_info"][0]
        enc, comp = u16(b, 4), u16(b, 6)
        ff = {"encryption": {0: "none", 1: "normal (AES)"}.get(enc, enc),
              "compression": {0: "none", 1: "basic (zero blocks)", 2: "normal (LZX)", 3: "delta"}.get(comp, comp)}
        if comp == 1:
            blocks = [(u32(b, o), u32(b, o + 4)) for o in range(8, len(b), 8)]
            ff["basic_blocks"] = len(blocks)
            ff["data_bytes"] = sum(d for d, z in blocks)
            ff["zero_bytes"] = sum(z for d, z in blocks)
        elif comp == 2:
            ff["lzx_window_size"] = f"0x{u32(b, 8):X}"
            ff["first_block_size"] = u32(b, 12)
        info["file_format"] = ff
    if "original_pe_name" in opt:
        info["original_pe_name"] = cstr(opt["original_pe_name"][0][4:])
    if "bounding_path" in opt:
        info["bounding_path"] = cstr(opt["bounding_path"][0][4:])
    if "checksum_timestamp" in opt:
        b = opt["checksum_timestamp"][0]
        info["pe_checksum"] = f"0x{u32(b, 0):08X}"
        info["pe_timestamp"] = u32(b, 4)
    if "tls_info" in opt:
        b = opt["tls_info"][0]
        info["tls"] = {"slot_count": u32(b, 0), "raw_data_address": f"0x{u32(b, 4):08X}",
                       "data_size": u32(b, 8), "raw_data_size": u32(b, 12)}
    if "resource_info" in opt:
        b = opt["resource_info"][0]
        info["resources"] = [{"name": cstr(b[o:o + 8]), "address": f"0x{u32(b, o + 8):08X}", "size": u32(b, o + 12)}
                             for o in range(4, len(b) - 15, 16)]
    if "static_libraries" in opt:
        b = opt["static_libraries"][0]
        libs = []
        for o in range(4, len(b) - 15, 16):
            q = u16(b, o + 14)
            libs.append({"name": cstr(b[o:o + 8]),
                         "version": f"{u16(b, o + 8)}.{u16(b, o + 10)}.{u16(b, o + 12)}.{q & 0xFF}",
                         "approval": APPROVAL[(q >> 13) & 3]})
        info["static_libraries"] = libs
    if "import_libraries" in opt:
        b = opt["import_libraries"][0]
        strings_size, lib_count = u32(b, 4), u32(b, 8)
        names = [s.decode("latin-1") for s in b[12:12 + strings_size].split(b"\0") if s]
        o = 12 + strings_size
        libs = []
        for _ in range(lib_count):
            size = u32(b, o)
            rec_count = u16(b, o + 0x26)
            libs.append({"name": names[u16(b, o + 0x24) & 0xFF], "version": version(u32(b, o + 0x1C)),
                         "min_version": version(u32(b, o + 0x20)), "records": rec_count,
                         "record_addresses": [u32(b, o + 0x28 + i * 4) for i in range(rec_count)]})
            o += size
        info["import_libraries"] = libs

    s = data[sec:]
    image_flags = u32(s, 0x10C)
    info["security"] = {
        "header_size": u32(s, 0), "image_size": f"0x{u32(s, 4):X}",
        "image_flags": [n for bit, n in IMAGE_FLAGS.items() if image_flags & bit] or f"0x{image_flags:X}",
        "load_address": f"0x{u32(s, 0x110):08X}",
        "import_table_count": u32(s, 0x128),
        "media_id_xgd2": s[0x140:0x150].hex(),
        "file_key_encrypted": s[0x150:0x160].hex(),
        "export_table": f"0x{u32(s, 0x160):08X}",
        "game_regions": f"0x{u32(s, 0x178):08X}",
        "allowed_media_types": f"0x{u32(s, 0x17C):08X}",
        "page_descriptor_count": u32(s, 0x180),
    }
    return info


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    for path in args:
        info = parse(path)
        if "--json" in sys.argv:
            print(json.dumps(info, indent=2))
            continue
        for lib in info.get("import_libraries", []):
            lib.pop("record_addresses")
        print(json.dumps(info, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
