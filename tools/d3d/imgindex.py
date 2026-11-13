#!/usr/bin/env python3
"""Index of a decrypted Xbox 360 memory image (PowerPC, big-endian): functions, calls, code and data
references, strings and imports. Moved from the phase-0 string cross-reference tool; the index format is
unchanged, so an index built with that tool loads here as well.

    python tools/d3d/imgindex.py build [--image default] [--work <dir>]

Inputs (read only): <work>/xex/<image>.bin (offset 0 = base 0x82000000) and <image>.json from
tools/xexunpack.py (sections, .pdata, imports). <work> is, in this order: --work, the environment variable
PREDATOR_WORK, <repo>/work if it contains xex/, and otherwise the work/ folder of the main checkout (from the
environment variable FCP_MAIN_CHECKOUT, or the parent of `git rev-parse --git-common-dir`), so a git worktree
reads the image of the main checkout.

The index is written to <repo>/work/phase0/<image>.idx.pkl (folder name kept for compatibility with indexes
built earlier; gitignored, never part of the repository). When loading, a missing local index falls back to
the same path in the main checkout.

Detection:
- Strings: ASCII (>= 3 printable characters + NUL) and UTF-16BE (>= 4 characters + 0x0000) in all
  non-code sections.
- Code references: register tracking per .pdata function: `lis rX,hi` followed by `addi`/`ori`/`addis`
  or a D-form load/store with base rX. After `bl`/`bctrl`, r0 and r3-r12 count as overwritten.
  The tracking is linear (no control flow) and can rarely report wrong pairs.
- Data references: every aligned 32-bit word in the data sections that points into the image.
- Calls: `bl` (direct) and `b` out of the function (tail call), resolved to .pdata functions and
  import thunks.

Import names: optionally from Xenia's `xam_table.inc`/`xboxkrnl_table.inc` (BSD) in <repo>/work/ext/.
Without them, imports are named `xam_<ordinal>`/`xboxkrnl_<ordinal>`.
"""
import argparse
import bisect
import json
import os
import pickle
import re
import struct
import subprocess
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
INDEX_SUBDIR = os.path.join("work", "phase0")
OUT = os.path.join(REPO, INDEX_SUBDIR)
BASE = 0x82000000

# op31 sub-opcodes (XO, 10 bits) whose destination is rA (bits 11-15): logical instructions, shifts, extends
OP31_DEST_RA = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954, 986, 27, 539, 794, 58,
                413}
# op31 sub-opcodes without a GPR destination: stores, compares, cache, mt*
OP31_NO_DEST = {0, 32, 151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 149, 181, 662, 918, 150, 214, 983, 231,
                487, 135, 167, 199, 467, 144, 146, 178, 86, 54, 278, 246, 1014, 470, 4, 68, 598, 854, 982, 725,
                661, 917, 951, 919, 455, 391, 359, 327, 295, 263}
INT_LOADS = {32, 33, 34, 35, 40, 41, 42, 43, 46}
FP_LOADS = {48, 49, 50, 51}
STORES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55}
UPDATE_FORMS = {33, 35, 41, 43, 37, 39, 45, 49, 51, 53, 55}
VOLATILE = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


# ---- locations ----------------------------------------------------------------------------------------

def main_checkout():
    """Main checkout of the repository (differs from REPO in a git worktree), or None."""
    env = os.environ.get("FCP_MAIN_CHECKOUT")
    if env:
        return env
    try:
        r = subprocess.run(["git", "-C", REPO, "rev-parse", "--git-common-dir"], capture_output=True, text=True,
                           timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0 or not r.stdout.strip():
        return None
    common = r.stdout.strip()
    if not os.path.isabs(common):
        common = os.path.join(REPO, common)
    return os.path.dirname(os.path.normpath(common))


def default_work():
    """Folder with xex/<image>.bin and .json (see the module docstring for the order)."""
    env = os.environ.get("PREDATOR_WORK")
    if env:
        return env
    local = os.path.join(REPO, "work")
    if os.path.isdir(os.path.join(local, "xex")):
        return local
    main = main_checkout()
    if main:
        cand = os.path.join(main, "work")
        if os.path.isdir(os.path.join(cand, "xex")):
            return cand
    return local


DEFAULT_WORK = default_work()


def index_path(image):
    """Path of the index to load: the local one, else the one in the main checkout, else the local path."""
    local = os.path.join(OUT, image + ".idx.pkl")
    if os.path.exists(local):
        return local
    main = main_checkout()
    if main:
        cand = os.path.join(main, INDEX_SUBDIR, image + ".idx.pkl")
        if os.path.exists(cand):
            return cand
    return local


def load_image(work, image):
    with open(os.path.join(work, "xex", image + ".bin"), "rb") as f:
        data = f.read()
    with open(os.path.join(work, "xex", image + ".json"), encoding="utf-8") as f:
        meta = json.load(f)
    return data, meta


# ---- index build ----------------------------------------------------------------------------------------

def import_names(ext_dir):
    names = {}
    for lib, fn in (("xam.xex", "xam_table.inc"), ("xboxkrnl.exe", "xboxkrnl_table.inc")):
        p = os.path.join(ext_dir, fn)
        if not os.path.exists(p):
            continue
        for m in re.finditer(r"XE_EXPORT\(\s*\w+\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*(\w+)", open(p, encoding="utf-8").read()):
            names[(lib, int(m.group(1), 16))] = m.group(2)
    return names


def find_strings(data, sections):
    """ASCII and UTF-16BE strings in all non-code sections: {address: (text, 'a'|'w')}."""
    out = {}
    ascii_re = re.compile(rb"[\x20-\x7e\t\r\n]{3,}\x00")
    wide_re = re.compile(rb"(?:\x00[\x20-\x7e]){4,}\x00\x00")
    for s in sections:
        if s["code"] or s["name"] in (".pdata", ".reloc", ".XBLD", ".XEXID"):
            continue
        lo, hi = s["rva"], s["rva"] + min(s["virtual_size"], s["raw_size"])
        chunk = data[lo:hi]
        for m in ascii_re.finditer(chunk):
            out[BASE + lo + m.start()] = (m.group()[:-1].decode("latin-1"), "a")
        for m in wide_re.finditer(chunk):
            a = lo + m.start()
            if a & 1:
                continue
            out[BASE + a] = (m.group()[:-2].decode("utf-16-be"), "w")
    return out


def pseudo_funcs(data, meta, funcs):
    """Code without a .pdata entry (leaf functions, XAPI wrappers): pseudo functions in the gaps.
    Starts = targets of `bl` inside a gap and the first non-zero word after zero padding."""
    gaps = []
    bl_targets = set()
    for s in meta["pe"]["sections"]:
        if not s["code"]:
            continue
        lo, hi = BASE + s["rva"], BASE + s["rva"] + s["virtual_size"]
        prev = lo
        for f, e in funcs:
            if e <= lo or f >= hi:
                continue
            if f > prev:
                gaps.append((prev, f))
            prev = max(prev, e)
        if prev < hi:
            gaps.append((prev, hi & ~3))
        words = struct.unpack_from(">%dI" % ((hi - lo) // 4), data, lo - BASE)
        for i, w in enumerate(words):
            if w >> 26 == 18 and (w & 3) == 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                bl_targets.add((lo + i * 4 + li) & 0xFFFFFFFF)
    new = set()
    for a, b in gaps:
        zero = True
        for x in range(a, b, 4):
            w = struct.unpack_from(">I", data, x - BASE)[0]
            if w == 0:
                zero = True
                continue
            if zero or x in bl_targets:
                new.add(x)
            zero = False
    pseudo = []
    gs = sorted(gaps)
    gstarts = [g[0] for g in gs]
    news = sorted(new)
    for i, x in enumerate(news):
        g = gs[bisect.bisect_right(gstarts, x) - 1]
        end = g[1]
        if i + 1 < len(news) and news[i + 1] < end:
            end = news[i + 1]
        # cut off zero padding at the end
        while end > x and struct.unpack_from(">I", data, end - 4 - BASE)[0] == 0:
            end -= 4
        if end > x:
            pseudo.append((x, end))
    return pseudo


def scan_code(data, meta):
    """Code references (value -> [(instruction, kind)]), calls (target -> [instruction]) over all of .text/BINK."""
    funcs = sorted((e[0], e[0] + e[1]) for e in meta["pdata_entries"])
    pseudo = pseudo_funcs(data, meta, funcs)
    funcs = sorted(funcs + pseudo)
    starts = [f[0] for f in funcs]
    code_secs = [s for s in meta["pe"]["sections"] if s["code"]]
    refs = defaultdict(list)
    calls = defaultdict(list)
    jumps = defaultdict(list)
    fstarts = set(starts)
    for s in code_secs:
        lo = BASE + s["rva"]
        hi = lo + s["virtual_size"]
        reg = {}
        fi = bisect.bisect_right(starts, lo) - 1
        cur_end = funcs[fi][1] if fi >= 0 else lo
        cur_start = funcs[fi][0] if fi >= 0 else lo
        words = struct.unpack_from(">%dI" % ((hi - lo) // 4), data, lo - BASE)
        for i, w in enumerate(words):
            a = lo + i * 4
            if a in fstarts:
                reg = {}
                fi = bisect.bisect_right(starts, a) - 1
                cur_start, cur_end = funcs[fi]
            op = w >> 26
            rd = (w >> 21) & 31
            ra = (w >> 16) & 31
            imm = w & 0xFFFF
            if op == 15:  # addis / lis
                if ra == 0:
                    reg[rd] = (imm << 16) & 0xFFFFFFFF
                elif ra in reg:
                    reg[rd] = (reg[ra] + (s16(imm) << 16)) & 0xFFFFFFFF
                else:
                    reg.pop(rd, None)
            elif op == 14:  # addi / li
                if ra != 0 and ra in reg:
                    v = (reg[ra] + s16(imm)) & 0xFFFFFFFF
                    refs[v].append((a, "addi"))
                    reg[rd] = v
                else:
                    reg.pop(rd, None)
            elif op == 24:  # ori rA,rS,uimm
                if rd in reg:
                    v = reg[rd] | imm
                    if imm:
                        refs[v].append((a, "ori"))
                    reg[ra] = v
                else:
                    reg.pop(ra, None)
            elif op in INT_LOADS or op in FP_LOADS or op in STORES:
                if ra != 0 and ra in reg:
                    v = (reg[ra] + s16(imm)) & 0xFFFFFFFF
                    refs[v].append((a, "st" if op in STORES else "ld"))
                if op in UPDATE_FORMS:
                    reg.pop(ra, None)
                if op in INT_LOADS:
                    if op == 46:  # lmw
                        for r in range(rd, 32):
                            reg.pop(r, None)
                    else:
                        reg.pop(rd, None)
            elif op == 58:  # ld/ldu/lwa
                if ra != 0 and ra in reg:
                    refs[(reg[ra] + s16(imm & 0xFFFC)) & 0xFFFFFFFF].append((a, "ld"))
                reg.pop(rd, None)
            elif op == 62:  # std
                if ra != 0 and ra in reg:
                    refs[(reg[ra] + s16(imm & 0xFFFC)) & 0xFFFFFFFF].append((a, "st"))
            elif op == 18:  # b / bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (li if w & 2 else a + li) & 0xFFFFFFFF
                if w & 1:
                    calls[t].append(a)
                    for r in VOLATILE:
                        reg.pop(r, None)
                elif not (cur_start <= t < cur_end):
                    jumps[t].append(a)
            elif op == 19:
                xo = (w >> 1) & 0x3FF
                if xo == 528 and (w & 1):  # bcctrl
                    for r in VOLATILE:
                        reg.pop(r, None)
            elif op == 31:
                xo = (w >> 1) & 0x3FF
                if xo == 444 and rd == ((w >> 11) & 31):  # mr
                    if rd in reg:
                        reg[ra] = reg[rd]
                    else:
                        reg.pop(ra, None)
                elif xo in OP31_DEST_RA:
                    reg.pop(ra, None)
                elif xo not in OP31_NO_DEST:
                    reg.pop(rd, None)
                    if xo in (55, 119, 311, 375, 567, 631, 759, 53, 181, 437, 309):  # update forms
                        reg.pop(ra, None)
            elif op in (20, 21, 23, 25, 26, 27, 28, 29, 30):
                if op == 25 and rd in reg:  # oris
                    reg[ra] = reg[rd] | (imm << 16)
                else:
                    reg.pop(ra, None)
            elif op in (7, 8, 12, 13):
                reg.pop(rd, None)
    return funcs, set(p[0] for p in pseudo), dict(refs), dict(calls), dict(jumps)


def scan_data_ptrs(data, meta):
    """Aligned 32-bit words in data sections that point into the image: value -> [location]."""
    size = len(data)
    ptrs = defaultdict(list)
    for s in meta["pe"]["sections"]:
        if s["code"] or s["name"] in (".pdata", ".reloc", ".XBLD", ".XEXID"):
            continue
        lo, n = s["rva"], min(s["virtual_size"], s["raw_size"]) // 4
        for i, v in enumerate(struct.unpack_from(">%dI" % n, data, lo)):
            if BASE <= v < BASE + size:
                ptrs[v].append(BASE + lo + i * 4)
    return dict(ptrs)


def build_index(data, meta, image, ext_dir=None, log=sys.stderr):
    """Index as a dict (the content of <image>.idx.pkl)."""
    names = import_names(ext_dir or os.path.join(REPO, "work", "ext"))
    print("Strings ...", file=log)
    strings = find_strings(data, meta["pe"]["sections"])
    print("  %d" % len(strings), file=log)
    print("Code ...", file=log)
    funcs, pseudo, refs, calls, jumps = scan_code(data, meta)
    print("  %d functions (%d without .pdata), %d referenced values, %d call targets"
          % (len(funcs), len(pseudo), len(refs), len(calls)), file=log)
    print("Data pointers ...", file=log)
    ptrs = scan_data_ptrs(data, meta)
    # Substrings: references into the middle of a detected string (padding bytes before it, suffix merging by
    # the linker) become entries of their own, so that e.g. "_time" behind "`\tH" is found.
    extra = {}
    for a, (t, k) in strings.items():
        if k != "a":
            continue
        for o in range(1, len(t)):
            if a + o in strings:
                break
            if any(kind in ("addi", "ori") for _, kind in refs.get(a + o, ())) or (a + o) in ptrs:
                extra[a + o] = (t[o:], "a")
    strings.update(extra)
    print("  %d substrings added" % len(extra), file=log)
    imports = {}
    for lib in meta["imports"]:
        for imp in lib["imports"]:
            nm = names.get((lib["name"], imp["ordinal"]), "%s_%d" % (lib["name"].split(".")[0], imp["ordinal"]))
            if imp.get("thunk"):
                imports[int(imp["thunk"], 16)] = (lib["name"], imp["ordinal"], nm, "fn")
            imports[int(imp["variable"], 16)] = (lib["name"], imp["ordinal"], nm, "var")
    return {"image": image, "funcs": funcs, "pseudo": pseudo, "strings": strings, "refs": refs, "calls": calls,
            "jumps": jumps, "ptrs": ptrs, "imports": imports}


def build(work, image, out_dir=OUT):
    data, meta = load_image(work, image)
    idx = build_index(data, meta, image)
    os.makedirs(out_dir, exist_ok=True)
    p = os.path.join(out_dir, image + ".idx.pkl")
    with open(p, "wb") as f:
        pickle.dump(idx, f, protocol=4)
    print("Index: %s" % p, file=sys.stderr)
    return p


# ---- index access ---------------------------------------------------------------------------------------

def containing(funcs, fstarts, a):
    i = bisect.bisect_right(fstarts, a) - 1
    if i >= 0 and funcs[i][0] <= a < funcs[i][1]:
        return funcs[i][0]
    return None


class Index:
    def __init__(self, image="default", path=None):
        with open(path or index_path(image), "rb") as f:
            self.__dict__.update(pickle.load(f))
        self.fstarts = [f[0] for f in self.funcs]
        self.fset = set(self.fstarts)
        self.str_addrs = sorted(self.strings)

    def func_of(self, a):
        return containing(self.funcs, self.fstarts, a)

    def func_range(self, f):
        i = bisect.bisect_left(self.fstarts, f)
        return self.funcs[i] if i < len(self.funcs) and self.funcs[i][0] == f else None

    def string_refs(self, a):
        """(code refs, data refs) of a string. Code refs only via address formation (addi/ori), not load/store."""
        cr = [(x, kind) for x, kind in self.refs.get(a, []) if kind in ("addi", "ori")]
        return cr, list(self.ptrs.get(a, []))

    def strings_in(self, f):
        """Strings a function uses through code references: [(instruction, string address)]."""
        r = self.func_range(f)
        if not r:
            return []
        if not hasattr(self, "_by_insn"):
            self._by_insn = {}
            for a in self.str_addrs:
                for (x, k) in self.string_refs(a)[0]:
                    self._by_insn.setdefault(x, a)
        return sorted((x, s) for x, s in self._by_insn.items() if r[0] <= x < r[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=DEFAULT_WORK)
    ap.add_argument("--image", default="default")
    ap.add_argument("--out", default=OUT, help="folder for <image>.idx.pkl (default: <repo>/work/phase0)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("build")
    args = ap.parse_args()
    if args.cmd == "build":
        build(args.work, args.image, args.out)


if __name__ == "__main__":
    main()
