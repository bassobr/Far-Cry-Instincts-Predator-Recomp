"""Self check of the reference interpreter against the SDK's PPC test vectors (rexglue/tests/ppc/asm).

Runs every test function of the assembled SDK test binaries (out/build/<cfg>/tests/ppc/bin) in ppcref.py
with the REGISTER_IN/MEMORY_IN values and compares with REGISTER_OUT/MEMORY_OUT. A mismatch is either a
reference error (fixed in ppcref.py) or a disputed vector (listed in the report).

  python check_ref_sdk.py <asm_dir> <bin_dir> [--verbose]
"""
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppcref  # noqa: E402

BASE = 0x82010000


class ZeroMemState(ppcref.State):
    """SDK tests run with zeroed memory (README of Xenia's tests)."""
    def rb(self, a):
        return self.mem.get(a & ppcref.M32, 0)

    def st(self, a, n, v):
        for i in range(n):
            self.mem[(a + i) & ppcref.M32] = (v >> (8 * (n - 1 - i))) & 0xFF


def parse_value(text, is_f):
    text = text.strip()
    if is_f and "." in text:
        return struct.unpack(">Q", struct.pack(">d", float(text)))[0]
    text = re.sub(r"(?i)u?ll?$", "", text)
    return int(text, 0) & ppcref.M64


def parse_tests(path):
    tests = []
    cur = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^(test_\w+):", line)
        if m:
            cur = {"name": m.group(1), "in": [], "out": [], "min": [], "mout": []}
            tests.append(cur)
            continue
        m = re.match(r"\s*#_\s+(REGISTER_IN|REGISTER_OUT|MEMORY_IN|MEMORY_OUT)\s+(\S+)\s*(.*)$", line)
        if not m or cur is None:
            continue
        kind, a, rest = m.groups()
        if kind.startswith("REGISTER"):
            if a.startswith("v"):
                vals = [int(x, 16) for x in rest.strip("[] ").split(",")]
                val = vals
            else:
                val = parse_value(rest, a.startswith("f"))
            cur["in" if kind == "REGISTER_IN" else "out"].append((a, val))
        else:
            data = bytes.fromhex(rest.replace(" ", ""))
            cur["min" if kind == "MEMORY_IN" else "mout"].append((int(a, 16), data))
    return tests


def load_code(bin_path, map_path):
    data = open(bin_path, "rb").read()
    code = {BASE + i: struct.unpack(">I", data[i:i + 4])[0] for i in range(0, len(data) - 3, 4)}
    syms = {}
    for line in open(map_path):
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = BASE + int(p[0], 16)
    return code, syms


def set_reg(s, name, val):
    if name == "cr":
        s.cr = val & ppcref.M32
    elif name[0] == "r":
        s.r[int(name[1:])] = val
    elif name[0] == "f":
        s.f[int(name[1:])] = val
    elif name[0] == "v":
        s.v[int(name[1:])] = list(val)


def get_reg(s, name):
    if name == "cr":
        return s.cr
    if name[0] == "r":
        return s.r[int(name[1:])]
    if name[0] == "f":
        return s.f[int(name[1:])]
    return s.v[int(name[1:])]


def main():
    asm_dir, bin_dir = sys.argv[1:3]
    verbose = "--verbose" in sys.argv
    total = ok = unsupported = 0
    bad = []
    unsup_reasons = {}
    for asm in sorted(glob.glob(os.path.join(asm_dir, "*.s"))):
        stem = os.path.splitext(os.path.basename(asm))[0]
        code, syms = load_code(os.path.join(bin_dir, stem + ".bin"), os.path.join(bin_dir, stem + ".map"))
        for t in parse_tests(asm):
            if not t["out"] and not t["mout"]:
                continue
            total += 1
            s = ZeroMemState()
            s.nj = 0  # the SDK test harness starts from a zeroed PPCContext
            s.r[1] = 0x7F000000  # a stack for tests that call
            for n, v in t["in"]:
                set_reg(s, n, v)
            for a, d in t["min"]:
                for i, b in enumerate(d):
                    s.mem[a + i] = b
            try:
                ppcref.run(s, code, syms[t["name"]])
            except ppcref.Unsupported as e:
                unsupported += 1
                unsup_reasons[str(e)] = unsup_reasons.get(str(e), 0) + 1
                continue
            diffs = []
            for n, v in t["out"]:
                got = get_reg(s, n)
                if n == "cr":
                    got &= ppcref.M32
                    v &= ppcref.M32
                if got != v:
                    diffs.append("%s want %s got %s" % (n, fmt(v), fmt(got)))
            for a, d in t["mout"]:
                got = bytes(s.rb(a + i) for i in range(len(d)))
                if got != d:
                    diffs.append("mem %08X want %s got %s" % (a, d.hex(), got.hex()))
            if diffs:
                bad.append((stem, t["name"], diffs))
            else:
                ok += 1
    print("check_ref_sdk: %d vectors, %d agree, %d disagree, %d unsupported by the reference"
          % (total, ok, len(bad), unsupported))
    for stem, name, diffs in bad:
        print("  DIFF %s/%s: %s" % (stem, name, "; ".join(diffs)))
    if verbose:
        for r, n in sorted(unsup_reasons.items(), key=lambda kv: -kv[1]):
            print("  unsupported %4d %s" % (n, r))
    return 1 if bad else 0


def fmt(v):
    if isinstance(v, list):
        return "[" + ",".join("%08X" % x for x in v) + "]"
    return "%016X" % v


if __name__ == "__main__":
    sys.exit(main())
