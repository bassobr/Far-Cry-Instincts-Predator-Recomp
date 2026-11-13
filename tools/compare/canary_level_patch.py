"""Canary patch for reference images of any story level (to compare renderer image errors level by level).

Overwrites, in the memory image of default.xex, the start level of the Instincts campaign (table 0x8205A3FC,
"00_training", 12 bytes incl. null) with the desired level name. "Instincts Story -> Play without saving"
then starts this level, Evolution levels included (otherwise locked in a fresh profile). The campaign stays
Instincts (game mode, abilities); the level itself (geometry, lighting, shaders) is the same.

  python -I tools/compare/canary_level_patch.py --canary-dir third_party/xenia-canary-9369464 --level 03_river
  python -I tools/compare/canary_level_patch.py --canary-dir third_party/xenia-canary-9369464 --off

Writes <canary-dir>/patches/555307DC - FCP Level.patch.toml (Canary format, apply_patches = true).
Module hash F35793FBFB34BD49 = default.xex of the disc without title update (from Canary's log "Module Hash").
"""
import argparse
import os
import struct

TABLE = 0x8205A3FC  # "00_training\0" (followed by "21_dunea\0" at 0x8205A408)
SLOT = 12
HASH = "F35793FBFB34BD49"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--canary-dir", required=True)
    ap.add_argument("--level", default="")
    ap.add_argument("--off", action="store_true")
    a = ap.parse_args()
    pdir = os.path.join(a.canary_dir, "patches")
    path = os.path.join(pdir, "555307DC - FCP Level.patch.toml")
    if a.off:
        if os.path.exists(path):
            os.remove(path)
        print("canary_level_patch: off")
        return 0
    name = a.level.encode("ascii")
    if not name or len(name) >= SLOT:
        raise SystemExit(f"level name must have 1..{SLOT - 1} characters: {a.level!r}")
    data = name + b"\0" * (SLOT - len(name))
    words = struct.unpack(">3I", data)
    os.makedirs(pdir, exist_ok=True)
    lines = [
        'title_name = "FC Instincts Predator"',
        'title_id = "555307DC"',
        f'hash = "{HASH}"',
        "",
        "[[patch]]",
        f'    name = "FCP start level {a.level}"',
        '    desc = "Instincts start level 00_training -> other level (reference images)"',
        '    author = "fcp"',
        "    is_enabled = true",
    ]
    for i, w in enumerate(words):
        lines += ["", "    [[patch.be32]]", f"        address = 0x{TABLE + 4 * i:08X}", f"        value = 0x{w:08X}"]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"canary_level_patch: {path} -> {a.level}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
