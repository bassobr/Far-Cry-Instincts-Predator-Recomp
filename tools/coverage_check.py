#!/usr/bin/env python3
"""Coverage check for the ReXGlue codegen of default.xex (fixed build step).

    python -I tools/coverage_check.py <generated_dir> <image.bin> <image.json> --config <coverage.toml>
                                      [--report <txt>] [--suggest <toml>] [--stamp <file>]
                                      [--base 0x82000000]

Adapted from the phase-0 dry-run tool (idea from Far Cry Instincts Recomp, MIT, same author: "pointers into
code", i.e. every code address the data refers to must be a known entry point) and adjusted to the output of
rexglue-sdk 351bc94d. Checks against the memory image, without ReXGlue internals:

  1. Pointers into code: every 4-byte word in non-executable sections that points to a 4-byte-aligned
     address in a code section; plus code addresses that are formed in the code via lis/addi or lis/ori.
     The target is a registered function (registrar->SetFunction in <proj>_register.cpp) or a
     label (loc_XXXXXXXX: in the generated code) -> ok, otherwise suspicious.
  2. Unreached start points: addresses directly behind an unconditional end (blr, bctr, b) that are neither
     function nor label, contain no padding word and look like a prolog (mflr r12/r0, stwu r1,
     std rN,-x(r1)) or are the target of a pointer from 1.

Candidates = start points from 2. that lie in no .pdata function (inside a .pdata function with the
exception flag they are __except bodies of the parent function). Every candidate that is not in
[coverage].ignore is a missing entry point: exit code 1, the build stops. With --suggest, suggestions for
[functions] are written (check them with a PPC disassembler before adopting them).
"""
import argparse
import bisect
import json
import os
import re
import struct
import sys
import tomllib


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


RE_SET = re.compile(r"SetFunction\(\s*(?:0x)?([0-9A-Fa-f]+)\s*,")
RE_LABEL = re.compile(r"^\s*loc_([0-9A-Fa-f]{8})\s*:")


def scan_generated(gendir):
    funcs, labels, files = set(), set(), 0
    for f in sorted(os.listdir(gendir)):
        p = os.path.join(gendir, f)
        if not f.endswith(".cpp"):
            continue
        files += 1
        reg = f.endswith("_register.cpp")
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if reg:
                    m = RE_SET.search(line)
                    if m:
                        funcs.add(int(m.group(1), 16))
                        continue
                m = RE_LABEL.match(line)
                if m:
                    labels.add(int(m.group(1), 16))
    return funcs, labels, files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gendir")
    ap.add_argument("image")
    ap.add_argument("json")
    ap.add_argument("--config")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x82000000)
    ap.add_argument("--report")
    ap.add_argument("--suggest")
    ap.add_argument("--stamp")
    a = ap.parse_args()

    ignore = set()
    if a.config:
        with open(a.config, "rb") as f:
            cfg = tomllib.load(f).get("coverage", {})
        ignore = {int(x) for x in cfg.get("ignore", [])}

    with open(a.image, "rb") as f:
        data = f.read()
    with open(a.json, encoding="utf-8") as f:
        info = json.load(f)
    code, other = [], []
    for s in info["pe"]["sections"]:
        lo = a.base + s["rva"]
        hi = lo + s["virtual_size"]
        (code if s.get("exec") else other).append((s["name"], lo, hi))

    funcs, labels, nfiles = scan_generated(a.gendir)
    out = []

    def emit(line=""):
        out.append(line)

    if not funcs:
        emit(f"ERROR: no SetFunction entries found in {a.gendir} (output format changed?)")
        print("\n".join(out))
        return 2

    def in_code(x):
        return any(lo <= x < hi for _, lo, hi in code)

    # 1. pointers into code from data sections
    ptr_ok_func = ptr_ok_label = 0
    suspicious = {}
    for name, lo, hi in other:
        if name in (".pdata", ".reloc", ".XBLD", ".XEXID", ".idata"):
            continue
        for ea in range(lo, min(hi, a.base + len(data)) - 3, 4):
            v = be32(data, ea - a.base)
            if v & 3 or not in_code(v):
                continue
            if v in funcs:
                ptr_ok_func += 1
            elif v in labels:
                ptr_ok_label += 1
            else:
                suspicious.setdefault(v, []).append((name, ea))

    # 1b. code addresses formed in the code via lis/addi (or lis/ori)
    code_ptr_ok = 0
    code_ptr_sus = {}
    for name, lo, hi in code:
        for ea in range(lo, hi, 4):
            w = be32(data, ea - a.base)
            if (w >> 26) != 15 or ((w >> 16) & 31) != 0:  # lis = addis rD,0,imm
                continue
            rd = (w >> 21) & 31
            hi16 = (w & 0xFFFF) << 16
            for k in range(1, 9):
                e2 = ea + 4 * k
                if e2 >= hi:
                    break
                w2 = be32(data, e2 - a.base)
                op2 = w2 >> 26
                if op2 == 14 and ((w2 >> 16) & 31) == rd:  # addi rX, rd, lo
                    v = (hi16 + ((w2 & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                elif op2 == 24 and ((w2 >> 21) & 31) == rd:  # ori rA, rd, lo
                    v = hi16 | (w2 & 0xFFFF)
                else:
                    if ((w2 >> 21) & 31) == rd and op2 in (14, 15, 32, 34, 40, 58):
                        break  # rd overwritten
                    continue
                if not (v & 3) and in_code(v):
                    first = be32(data, v - a.base)
                    if not (first & 3) and in_code(first):
                        break  # jump table directly in the code (entries are code addresses)
                    if v in funcs:
                        code_ptr_ok += 1
                    elif v not in labels:
                        code_ptr_sus.setdefault(v, []).append(ea)
                break

    # 2. unreached start points behind an unconditional end
    prolog_hits = []
    for name, lo, hi in code:
        for ea in range(lo + 4, hi, 4):
            prev = be32(data, ea - 4 - a.base)
            w = be32(data, ea - a.base)
            uncond = prev == 0x4E800020 or prev == 0x4E800420 or ((prev >> 26) == 18 and (prev & 1) == 0)
            if not uncond or w == 0 or ea in funcs or ea in labels:
                continue
            is_prolog = w in (0x7D8802A6, 0x7C0802A6) or (w >> 16) == 0x9421 or \
                ((w >> 26) == 62 and ((w >> 16) & 31) == 1 and (w & 3) == 0 and (w & 0x8000))
            is_target = ea in suspicious or ea in code_ptr_sus
            if is_prolog or is_target:
                prolog_hits.append((ea, is_prolog, is_target))

    pd = sorted((e[0], e[1], e[3]) for e in info["pdata_entries"])
    starts = [p[0] for p in pd]

    def pdata_of(x):
        i = bisect.bisect_right(starts, x) - 1
        if i >= 0 and pd[i][0] <= x < pd[i][0] + pd[i][1]:
            return pd[i]
        return None

    eh_bodies = [ea for ea, _, _ in prolog_hits if pdata_of(ea) and (pdata_of(ea)[2] & 2)]
    in_pdata_other = [ea for ea, _, _ in prolog_hits if pdata_of(ea) and not (pdata_of(ea)[2] & 2)]
    cands = [(ea, p, s) for ea, p, s in prolog_hits if not pdata_of(ea)]
    missing = [(ea, p, s) for ea, p, s in cands if ea not in ignore]
    ignored = [ea for ea, _, _ in cands if ea in ignore]
    stale_ignores = sorted(ignore - {ea for ea, _, _ in cands})

    emit(f"Generated .cpp files: {nfiles}; functions registered: {len(funcs)}; labels: {len(labels)}")
    emit(f"Pointers into code (data sections): to function {ptr_ok_func}, to label {ptr_ok_label}, "
         f"without match {sum(len(v) for v in suspicious.values())} references to {len(suspicious)} targets")
    emit(f"Code addresses via lis/addi in code: to function {code_ptr_ok}, without match "
         f"{sum(len(v) for v in code_ptr_sus.values())} sites to {len(code_ptr_sus)} targets")
    for v in sorted(code_ptr_sus):
        emit(f"  0x{v:08X}  <- {len(code_ptr_sus[v])}x, e.g. code 0x{code_ptr_sus[v][0]:08X}")
    emit(f"Unreached start points behind an unconditional end: {len(prolog_hits)} "
         f"(prolog pattern {sum(1 for _, p, _ in prolog_hits if p)}, pointer target "
         f"{sum(1 for _, _, s in prolog_hits if s)})")
    emit(f"  in .pdata function with exception flag (__except body): {len(eh_bodies)}")
    emit(f"  in .pdata function without exception flag: {len(in_pdata_other)}")
    emit(f"  outside every .pdata function (candidates): {len(cands)}, of which in ignore {len(ignored)}")
    for ea, p, s in cands:
        tag = " (ignore)" if ea in ignore else ""
        emit(f"    0x{ea:08X}  prolog={p} pointer_target={s}{tag}")
    if stale_ignores:
        emit("Note: ignore entries without a candidate (can be removed): " +
             ", ".join(f"0x{x:08X}" for x in stale_ignores))
    if missing:
        emit(f"ERROR: {len(missing)} missing entry points. Add them to config/predator/default.toml under [functions] "
             f"(suggestions: --suggest) or, once checked, ignore them in config/predator/coverage.toml.")
    else:
        emit("OK: no missing entry points")

    text = "\n".join(out)
    print(text)
    if a.report:
        os.makedirs(os.path.dirname(os.path.abspath(a.report)), exist_ok=True)
        with open(a.report, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if a.suggest:
        os.makedirs(os.path.dirname(os.path.abspath(a.suggest)), exist_ok=True)
        with open(a.suggest, "w", encoding="utf-8") as f:
            f.write("# Suggestions from tools/coverage_check.py for [functions] (start address only).\n")
            for ea, p, s in missing:
                f.write(f"0x{ea:08X} = {{}}  # prolog={p} pointer_target={s}\n")
    if missing:
        return 1
    if a.stamp:
        os.makedirs(os.path.dirname(os.path.abspath(a.stamp)), exist_ok=True)
        with open(a.stamp, "w", encoding="utf-8") as f:
            f.write(f"funcs={len(funcs)} labels={len(labels)} candidates={len(cands)} ignored={len(ignored)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
