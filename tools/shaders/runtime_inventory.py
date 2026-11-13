#!/usr/bin/env python3
"""runtime_inventory.py: inventory of the shader sources that Predator compiles or assembles itself at runtime.

Sources:
  1. Text files in common.fat/.dat (named and unnamed entries) that look like HLSL or D3D assembler:
     type, #include targets (resolved by name hash), queried macros (#ifdef/#ifndef/#if defined).
  2. ASCII strings in the decrypted image default.bin (offset 0 = 0x82000000) that contain shader code or
     compiler switches.

Usage: runtime_inventory.py <disc-folder> <default.bin> [--names names.txt] [--out report.txt] [--dump folder]
--dump writes the EXE strings found as files to <folder> (only into work/, derived game data).
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from xfat import Archive, load_names, name_hash  # noqa: E402

BASE = 0x82000000
ASM_TOKENS = re.compile(r"^\s*(x?[vp]s\.[1-3]\.[0-9x]|x?vs_[1-3]_[0-9x]|mov\s+o|tex\s+t\d|dp[34]\s|mad\s|DECLARE_)", re.M | re.I)
HLSL_TOKENS = re.compile(r"\b(float[234](x[234])?|sampler(2D|CUBE|3D)?|tex2D|technique|uniform|struct)\b")
MACRO_TEST = re.compile(r"^\s*#\s*(?:ifdef|ifndef)\s+(\w+)|defined\s*\(\s*(\w+)\s*\)|^\s*#\s*if\s+(\w+)\s*$", re.M)
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def classify(text):
    asm = len(ASM_TOKENS.findall(text))
    hlsl = len(HLSL_TOKENS.findall(text))
    if asm == 0 and hlsl < 3:
        return None
    return "asm" if asm >= hlsl else "hlsl"


def macros(text):
    out = set()
    for m in MACRO_TEST.finditer(text):
        out.add(next(g for g in m.groups() if g))
    return out


def resolve_include(inc, src_dir, hashes):
    """Test the possible paths of an #include against the hashes of common.fat."""
    inc = inc.replace("/", "\\")
    cands = [inc, src_dir + "\\" + inc, "shaders\\" + inc]
    for c in cands:
        h = name_hash(c)
        if h in hashes:
            return c.lower()
    return None


def scan_common(disc, names):
    arc = Archive(os.path.join(disc, "common.fat"))
    hashes = {e.hash: e for e in arc.entries}
    found = []
    for e in arc.entries:
        if e.size > 200000:
            continue
        b = arc.data(e)
        if not b or not all(c in (9, 10, 13) or 32 <= c < 127 for c in b[:512]):
            continue
        text = b.decode("latin-1")
        kind = classify(text)
        nm = names.get(e.hash)
        if kind is None and not (nm and re.search(r"\.(vsh|psh|fx|vsa|psa)(\.h)?$|shader", nm)):
            continue
        found.append((e, nm, kind or "?", text))
    rows = []
    for e, nm, kind, text in found:
        d = nm.rsplit("\\", 1)[0] if nm and "\\" in nm else "shaders"
        incs = []
        for inc in INCLUDE.findall(text):
            r = resolve_include(inc, d, hashes)
            incs.append((inc, r is not None))
        rows.append({"hash": "%08X" % e.hash, "name": nm or "?", "kind": kind, "size": e.size, "includes": incs,
                     "macros": sorted(macros(text)), "entry": sorted(set(re.findall(r"\b(\w+)(?:VS|PS)\s*\(", text)))})
    return rows


def scan_exe(path, dump):
    img = open(path, "rb").read()
    rows = []
    for m in re.finditer(rb"[\x09\x0a\x0d\x20-\x7e]{24,}", img):
        s = m.group().decode("latin-1")
        kind = classify(s)
        key = re.search(r"FC_XENON|_XBOX_SHADER|SHADOW_DISABLED|%sPS|%sVS|vs_[123]_[0x]|ps_[123]_[0x]|"
                        r"x?ps\.1\.[1-4]|x?vs\.1\.1|D3DXSHADER|\.fx\b|\.vsh\b|\.psh\b", s)
        if kind is None and not key:
            continue
        rows.append({"addr": BASE + m.start(), "len": len(s), "kind": kind or "string", "text": s})
    if dump:
        os.makedirs(dump, exist_ok=True)
        for r in rows:
            if r["kind"] in ("hlsl", "asm"):
                with open(os.path.join(dump, "%08X.%s.txt" % (r["addr"], r["kind"])), "w", encoding="latin-1") as f:
                    f.write(r["text"])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disc")
    ap.add_argument("image")
    ap.add_argument("--names")
    ap.add_argument("--out")
    ap.add_argument("--dump")
    a = ap.parse_args()
    names = load_names(a.names)
    out = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    rows = scan_common(a.disc, names)
    print("== common.dat: %d shader sources" % len(rows), file=out)
    allm = collections.Counter()
    for r in sorted(rows, key=lambda r: r["name"]):
        allm.update(r["macros"])
        inc = ", ".join("%s%s" % (i, "" if ok else " (missing)") for i, ok in r["includes"])
        print("%s %-55s %-4s %6d  entry points %s  macros %s  includes %s" % (
            r["hash"], r["name"], r["kind"], r["size"], ",".join(r["entry"]) or "-", ",".join(r["macros"]) or "-",
            inc or "-"), file=out)
    print("Macros over all sources:", dict(allm.most_common()), file=out)
    ex = scan_exe(a.image, a.dump)
    print("\n== default.bin: %d strings with shader code or switches" % len(ex), file=out)
    for r in ex:
        t = r["text"].replace("\r", "").replace("\n", "\\n")
        print("0x%08X %5d %-6s %s" % (r["addr"], r["len"], r["kind"], t[:150]), file=out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
