"""Picks the leaf function sample of the semantic test from the user's image (writes addresses only).

A candidate is a .pdata function that
  - ends with blr and has no call, no branch out of the function, no bctr/bctrl/blrl, no sc, no trap,
  - touches memory only through r1 (its own stack, after stwu) or through argument registers r3-r10
    that it never writes before the access, plus loads from absolute image addresses (lis/addi
    constants, read only),
  - stores nothing through absolute addresses and changes no SPR other than LR/CTR,
  - is at most MAX_WORDS long.
Math helpers (FP or VMX arithmetic) are preferred. Output: leaf_sample.toml with [[leaf]] address/size.

  python pick_leaves.py <default.bin> <default.json> <out.toml> [--count N] [--seed S]
"""
import json
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppcref  # noqa: E402
import xenia_lookup  # noqa: E402

IMAGE_BASE = 0x82000000
MAX_WORDS = 160
LOADS = {"lbz", "lbzx", "lbzu", "lhz", "lhzx", "lha", "lhax", "lwz", "lwzx", "lwzu", "lwa", "lwax", "ld", "ldx",
         "ldu", "lfs", "lfsx", "lfd", "lfdx", "lvx", "lvx128", "lvlx", "lvlx128", "lvrx", "lvrx128", "lwbrx"}
STORES = {"stb", "stbx", "stbu", "sth", "sthx", "stw", "stwx", "stwu", "std", "stdx", "stdu", "stfs",
          "stfsx", "stfd", "stfdx", "stfiwx", "stvx", "stvx128", "stvlx", "stvlx128", "stvrx", "stvrx128",
          "stvewx", "stwbrx", "sthbrx"}
FORBIDDEN = {"sc", "twi", "tdi", "tw", "td", "mtmsrd", "mfmsr", "mtcrf", "mfcr", "lwarx", "stwcx", "ldarx",
             "stdcx", "mftb", "dcbz", "dcbz128", "dcbf", "dcbst", "mffsx", "mtfsfx", "mtfsfix", "mtvscr",
             "mfvscr", "bcctrx", "sync", "eieio"}
MATH = ("f", "v")


def gpr_writes(name, w):
    """GPRs an instruction writes (coarse: RT of most forms, RA of logical/update forms)."""
    if name in STORES:
        return {ppcref.RA(w)} if name.endswith("u") else set()
    if name.startswith(("f", "v", "st", "lv", "b", "cmp", "dcb", "mt")) or name in ("lfs", "lfsx", "lfd", "lfdx"):
        return set()
    if name in ("andx", "andcx", "orx", "orcx", "xorx", "norx", "nandx", "eqvx", "ori", "oris", "xori",
                "xoris", "andix", "andisx", "extsbx", "extshx", "extswx", "cntlzwx", "cntlzdx", "rlwinmx",
                "rlwnmx", "rlwimix", "rldiclx", "rldicrx", "rldicx", "rldimix", "slwx", "srwx", "sldx",
                "srdx", "srawx", "srawix", "sradx", "sradix"):
        return {ppcref.RA(w)}
    out = {ppcref.RT(w)}
    if name.endswith("u"):
        out.add(ppcref.RA(w))
    return out


def analyse(data, start, size):
    words = [struct.unpack_from(">I", data, start - IMAGE_BASE + 4 * i)[0] for i in range(size // 4)]
    if not words or words[-1] != 0x4E800020:
        return None
    written = set()
    lis_regs = set()
    ptr = {1, 3, 4, 5, 6, 7, 8, 9, 10}   # registers that hold an argument pointer (plus offsets)
    math = 0
    for i, w in enumerate(words):
        pc = start + 4 * i
        name = xenia_lookup.lookup(w)
        if name is None or name in FORBIDDEN or name not in ppcref.H:
            return None
        if name == "bx":
            if w & 1:
                return None
            tgt = (pc + ppcref.sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF
            if not start <= tgt < start + size:
                return None
        if name == "bcx":
            if w & 3:
                return None
            tgt = (pc + ppcref.sext(w & 0xFFFC, 16)) & 0xFFFFFFFF
            if not start <= tgt < start + size:
                return None
        if name == "bclrx" and w & 1:
            return None
        if name in ("mtspr", "mfspr"):
            spr = (ppcref.X(w, 16, 20) << 5) | ppcref.X(w, 11, 15)
            if spr not in (8, 9) or (name == "mtspr" and spr == 8):
                return None
        if name in LOADS or name in STORES:
            ra = ppcref.RA(w)
            ok_base = ra == 1 or ra in ptr
            if name in LOADS and ra in lis_regs:
                ok_base = True
            if ra == 0 and (w >> 26) != 31:
                return None
            if not ok_base:
                return None
        if name == "addis" and ppcref.RA(w) == 0:
            lis_regs.add(ppcref.RT(w))
            written.add(ppcref.RT(w))
            continue
        if name == "addi" and ppcref.RA(w) in lis_regs:
            lis_regs.add(ppcref.RT(w))
            written.add(ppcref.RT(w))
            continue
        ws = gpr_writes(name, w)
        written |= ws
        lis_regs -= ws
        derived = (name in ("addi", "addis") and ppcref.RA(w) in ptr) or                   (name == "orx" and ppcref.RT(w) == ppcref.RB(w) and ppcref.RT(w) in ptr) or                   (name == "addx" and (ppcref.RA(w) in ptr or ppcref.RB(w) in ptr))
        if name.endswith("u") and name in STORES | LOADS:
            derived = ppcref.RA(w) in ptr
        for r_ in ws:
            if derived:
                ptr.add(r_)
            else:
                ptr.discard(r_)
        if name.startswith(MATH) and name not in ("fmrx",) and not name.startswith(("lv", "stv")):
            math += 1
    return math


def main():
    binpath, jsonpath, out = sys.argv[1:4]
    count = int(sys.argv[sys.argv.index("--count") + 1]) if "--count" in sys.argv else 80
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 20261112
    data = open(binpath, "rb").read()
    entries = json.load(open(jsonpath))["pdata_entries"]
    math_funcs, other = [], []
    for start, length, *_ in entries:
        size = length
        if size // 4 > MAX_WORDS or size < 8:
            continue
        r = analyse(data, start, size)
        if r is None:
            continue
        (math_funcs if r >= 2 else other).append((start, size, r))
    rng = random.Random(seed)
    pick = rng.sample(math_funcs, min(len(math_funcs), count * 3 // 4))
    pick += rng.sample(other, min(len(other), count - len(pick)))
    pick.sort()
    with open(out, "w", encoding="utf-8") as f:
        f.write("# Leaf function sample of the semantic test (tests/semantics/py/pick_leaves.py, seed %d).\n" % seed)
        f.write("# Addresses in default.xex of the disc version; the code is read from the user's image at test\n")
        f.write("# time and never committed. Candidates: %d math helpers, %d other leaf functions.\n"
                % (len(math_funcs), len(other)))
        for start, size, m in pick:
            f.write("[[leaf]]\naddress = 0x%08X\nsize = 0x%X\nmath = %d\n" % (start, size, m))
    print("pick_leaves.py: %d math and %d other candidates, %d picked" % (len(math_funcs), len(other), len(pick)))


if __name__ == "__main__":
    main()
