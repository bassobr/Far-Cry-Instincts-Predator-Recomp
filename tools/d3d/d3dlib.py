"""Shared helpers for mapping the statically linked D3D9 library in default.xex.

Builds on the image index of tools/d3d/imgindex.py (work/phase0/default.idx.pkl; run
`python tools/d3d/imgindex.py build` first). Reads the image read-only (in a git worktree from the main
checkout, see imgindex.py). Output goes to work/d3d/ of the current checkout (gitignored).
"""
import bisect
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
import imgindex  # noqa: E402

BASE = 0x82000000
OUT = os.path.join(REPO, "work", "d3d")
os.makedirs(OUT, exist_ok=True)

TEXT_LO = 0x820B0000
TEXT_HI = 0x820B0000 + 8118636


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Img:
    def __init__(self, image="default"):
        self.ix = imgindex.Index(image)
        self.data, self.meta = imgindex.load_image(imgindex.DEFAULT_WORK, image)
        self.funcs = self.ix.funcs
        self.fstarts = self.ix.fstarts
        self._md = None
        self._names = None
        # callers per target (bl and tail b) as function starts
        self._callee_map = None

    def u32(self, a):
        return struct.unpack_from(">I", self.data, a - BASE)[0]

    def u16(self, a):
        return struct.unpack_from(">H", self.data, a - BASE)[0]

    def f32(self, a):
        return struct.unpack_from(">f", self.data, a - BASE)[0]

    def words(self, a, n):
        return struct.unpack_from(">%dI" % n, self.data, a - BASE)

    def func_of(self, a):
        return self.ix.func_of(a)

    def frange(self, f):
        return self.ix.func_range(f)

    def fwords(self, f):
        r = self.frange(f)
        return self.words(r[0], (r[1] - r[0]) // 4)

    def funcs_in(self, lo, hi):
        i = bisect.bisect_left(self.fstarts, lo)
        j = bisect.bisect_left(self.fstarts, hi)
        return self.funcs[i:j]

    def callee_map(self):
        """function -> sorted list (site, target, kind) of all bl and tail b."""
        if self._callee_map is None:
            m = {}
            for t, srcs in self.ix.calls.items():
                for s in srcs:
                    f = self.func_of(s)
                    if f is not None:
                        m.setdefault(f, []).append((s, t, "bl"))
            for t, srcs in self.ix.jumps.items():
                for s in srcs:
                    f = self.func_of(s)
                    if f is not None:
                        m.setdefault(f, []).append((s, t, "b"))
            for f in m:
                m[f].sort()
            self._callee_map = m
        return self._callee_map

    def callees(self, f):
        return self.callee_map().get(f, [])

    def caller_sites(self, f):
        return self.ix.calls.get(f, []) + self.ix.jumps.get(f, [])

    def md(self):
        if self._md is None:
            import capstone
            self._md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
        return self._md

    def dis1(self, a):
        w = self.data[a - BASE:a - BASE + 4]
        ins = list(self.md().disasm(w, a))
        if ins:
            return "%s %s" % (ins[0].mnemonic, ins[0].op_str)
        return ".long 0x%08X" % struct.unpack(">I", w)[0]

    def names(self):
        """Names from config/predator/d3d_symbols.toml, if present: address -> name."""
        if self._names is None:
            self._names = load_symbols()
        return self._names

    def name(self, a):
        if a in self.ix.imports:
            return self.ix.imports[a][2]
        n = self.names().get(a)
        return n if n else "sub_%08X" % a


def load_symbols(path=None):
    path = path or os.path.join(REPO, "config", "predator", "d3d_symbols.toml")
    out = {}
    if not os.path.exists(path):
        return out
    import re
    addr = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r'\s*address\s*=\s*0x([0-9A-Fa-f]+)', line)
        if m:
            addr = int(m.group(1), 16)
        m = re.match(r'\s*name\s*=\s*"([^"]+)"', line)
        if m and addr is not None:
            out[addr] = m.group(1)
            addr = None
    return out


def branch_target(a, w):
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if w & 2 else a + li) & 0xFFFFFFFF


def refine_funcs(im, lo, hi):
    """Function starts in [lo, hi): .pdata functions unchanged; code without .pdata is split behind every
    unconditional return or branch (blr, bctr, b), unless a conditional branch of the current piece targets
    beyond that point. In addition, bl targets, tail call targets and data pointers count as starts.
    Returns a sorted list (start, end, has_pdata)."""
    pd = sorted((e[0], e[0] + e[1]) for e in im.meta["pdata_entries"] if lo <= e[0] < hi)
    out = [(a, b, True) for a, b in pd]
    gaps = []
    prev = lo
    for a, b in pd:
        if a > prev:
            gaps.append((prev, a))
        prev = max(prev, b)
    if prev < hi:
        gaps.append((prev, hi))
    forced = set()
    for src in (im.ix.calls, im.ix.ptrs):
        for t in src:
            if lo <= t < hi and t % 4 == 0:
                forced.add(t)
    for a, b in gaps:
        start = None
        reach = 0
        x = a
        while x < b:
            w = im.u32(x)
            if start is None:
                if w == 0:
                    x += 4
                    continue
                start = x
                reach = 0
            elif x in forced and x > reach:
                out.append((start, x, False))
                start = x
                reach = 0
            op = w >> 26
            if op == 16:  # bc
                bd = s16(w & 0xFFFC)
                t = (bd if w & 2 else x + bd) & 0xFFFFFFFF
                if a <= t < b and t > reach:
                    reach = t
            elif op == 18 and not (w & 1):
                t = branch_target(x, w)
                if start <= t < b and x < t < x + 0x400 and t > reach and t not in forced:
                    reach = t
            ends = (w == 0x4E800020 or w == 0x4E800420 or (op == 18 and not (w & 1))) and x >= reach
            if w == 0 and x >= reach:
                out.append((start, x, False))
                start = None
            elif ends:
                out.append((start, x + 4, False))
                start = None
            x += 4
        if start is not None:
            out.append((start, b, False))
    out.sort()
    return out


class Lib:
    """Refined function list of a range with the mapping address -> function."""

    def __init__(self, im, lo, hi):
        self.im, self.lo, self.hi = im, lo, hi
        self.funcs = refine_funcs(im, lo, hi)
        self.starts = [f[0] for f in self.funcs]

    def func_of(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.funcs[i][0] <= a < self.funcs[i][1]:
            return self.funcs[i][0]
        return None

    def rng(self, f):
        i = bisect.bisect_left(self.starts, f)
        return self.funcs[i][:2]


# Library range of D3D9 (measured with tools/d3d/bounds.py): starts at 0x8272FA00, which builds vertex
# declarations from an FVF (D3DDECLTYPE constants) and is called by SetFVF, right after the end of inflate_fast
# (D3DX9); ends behind 0x8274B820 (writes the base registers into the command buffer), followed by XamSession*
# wrappers. All call sites of the Vd* imports lie inside this range.
D3D_LO, D3D_HI = 0x8272FA00, 0x8274B9F8
