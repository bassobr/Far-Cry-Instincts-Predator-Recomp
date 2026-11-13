"""Generator of the semantic test.

For every encoding form of the catalog (tests/semantics/forms.json, made by forms.py from the game) it
builds small guest functions from the game's own encodings, inputs with edge and random values and the
expected results:
  hw   expected value from the SDK vectors upstream calls hardware tested (tests/unit/ppc/*_cases.h),
  ref  expected value from the reference interpreter (ppcref.py),
  none no expectation (only the differential comparison against XenonRecomp).

  python gen.py <forms.json> <sdk_unit_ppc_dir> <out_dir> [--seed N] [--cases N] [--leaf leaf.toml
                 --image default.bin --json default.json]

Writes into out_dir: sem.bin and sem_syms.txt (functions at 0x82010000, "addr size name"), sem.elf (same
code for XenonRecomp's test mode), cases.bin (runner input), expected.bin (same layout as the runner's
results) and meta.json (per case: form, origin, compare masks). With --leaf, leaf functions of the game
are added at their own addresses (sem_leaf.bin etc.); those files contain game code and stay in the build
folder.
"""
import json
import os
import random
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppcref  # noqa: E402
import xenia_lookup  # noqa: E402
from ppcref import M32, M64, Unsupported  # noqa: E402

BASE = 0x82010000
BUF = 0x10080000          # middle of the runner's 1 MB data window
STACK_TOP = 0x6F000000 + 0x100000 - 0x1000
BLR = 0x4E800020
K_GPR, K_FPR, K_VR, K_CR, K_XER, K_CTR, K_LR, K_VSCR, K_MSR, K_RN = range(10)


def li(rt, v):
    return 0x38000000 | (rt << 21) | (v & 0xFFFF)


# ----------------------------------------------------------------------------------------------------
# Values
# ----------------------------------------------------------------------------------------------------
GPR_EDGES = [0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFF80000000,
             0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0x00000000FFFF8000, 0x8000, 0x7FFF,
             0xFFFFFFFE, 31, 32, 63, 64, 0x1F, 0x3F]


def d(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


def f2d(b32):
    return ppcref.s2d(b32)


FP_EDGES = [0, 1 << 63, 1, (1 << 63) | 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, d(1.0), d(-1.0),
            d(0.5), d(-0.5), d(1.5), d(2.5), d(-2.5), 0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF,
            0x7FF0000000000000, 0xFFF0000000000000, 0x7FF8000000000000, 0xFFF8000000000000,
            0x7FF8000000000123, 0x7FF0000000000001, 0xFFF4000000000ABC, 0x7FF7FFFFFFFFFFFF,
            d(2147483647.0), d(2147483647.5), d(2147483648.0), d(-2147483648.0), d(-2147483648.5),
            d(-2147483649.0), d(4294967296.0), d(9.2233720368547758e18), d(-9.2233720368547758e18),
            d(1e300), d(-1e-300), d(3.0), d(1.0 / 3.0)]
F32_EDGES = [0, 0x80000000, 1, 0x80000001, 0x007FFFFF, 0x00800000, 0x3F800000, 0xBF800000, 0x3F000000,
             0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FC00123,
             0x7F800001, 0xFFA00ABC, 0x7FBFFFFF, 0x4EFFFFFF, 0x4F000000, 0xCF000000, 0xCF000001,
             0x3EAAAAAB, 0x40400000, 0x40490FDB, 0x34000000, 0x00000400]


def rand_gpr(rng):
    t = rng.random()
    if t < 0.3:
        return rng.choice(GPR_EDGES)
    if t < 0.5:
        return ppcref.sext(rng.getrandbits(32), 32) & M64
    if t < 0.65:
        return rng.getrandbits(32)
    if t < 0.75:
        return rng.getrandbits(8) - 128 & M64
    return rng.getrandbits(64)


def rand_f32(rng):
    t = rng.random()
    if t < 0.35:
        return rng.choice(F32_EDGES)
    if t < 0.45:
        return rng.getrandbits(32)                                  # any class, NaN payloads included
    e = rng.choice([rng.randint(1, 254), rng.randint(100, 150), rng.randint(120, 135)])
    return (rng.getrandbits(1) << 31) | (e << 23) | rng.getrandbits(23)


def rand_fpr(rng, single):
    t = rng.random()
    if single and t < 0.75:
        return f2d(rand_f32(rng))
    if t < 0.4:
        return rng.choice(FP_EDGES)
    if t < 0.5:
        return rng.getrandbits(64)
    e = rng.choice([rng.randint(1, 2046), rng.randint(1000, 1050), rng.randint(1018, 1060)])
    return (rng.getrandbits(1) << 63) | (e << 52) | rng.getrandbits(52)


def rand_vr(rng, floaty):
    if floaty:
        return [rand_f32(rng) for _ in range(4)]
    t = rng.random()
    if t < 0.3:
        return [rng.choice([0, M32, 0x80000000, 0x7FFFFFFF, 0x7F7F7F7F, 0x80808080, 0x00FF00FF,
                            0xFFFF0000, 0x0000FFFF, 0x7FFF8000, 0x00010203]) for _ in range(4)]
    return [rng.getrandbits(32) for _ in range(4)]


# ----------------------------------------------------------------------------------------------------
# Forms
# ----------------------------------------------------------------------------------------------------
REG_RE = re.compile(r"\b(r|f|v|cr)(\d+)\b")
SKIP = {
    "bcctrx": "indirect branch through CTR (needs the function table; not a single form test)",
    "mftb": "time base, no fixed result (not tested)",
}
SINGLE_FP = {"faddsx", "fsubsx", "fmulsx", "fdivsx", "fmaddsx", "fmsubsx", "fnmaddsx", "fnmsubsx",
             "fsqrtsx"}
RN_FORMS = {"faddx", "faddsx", "fsubx", "fsubsx", "fmulx", "fmulsx", "fdivx", "fdivsx", "fmaddx",
            "fmaddsx", "fmsubx", "fmsubsx", "fnmaddsx", "fnmsubx", "fnmsubsx", "frspx", "fcfidx",
            "fctidx", "fctiwx", "fsqrtx", "fsqrtsx"}
VMX_FLOAT = re.compile(r"^v(add|sub|mul|madd|nmsub|max|min|cmp(eq|ge|gt|b)fp|rfi|ref|rsqrte|logefp|expte|"
                       r"msum|ct|cf|csx|cux)")
MEM = re.compile(r"^(l|st)(b|h|w|d|f|v)")


def operand_regs(ops):
    out = []
    for kind, num in REG_RE.findall(ops):
        out.append((kind, int(num)))
    return out


class Case:
    __slots__ = ("addr", "flags", "ins", "outs", "mem_in", "mem_out", "form", "origin", "masks", "note",
                 "expected", "group")

    def __init__(self, addr, form, group):
        self.addr, self.form, self.group = addr, form, group
        self.flags = 0
        self.ins, self.outs, self.mem_in, self.mem_out = [], [], [], []
        self.origin, self.masks, self.note, self.expected = "none", {}, "", None


class Gen:
    def __init__(self, seed, per_variant):
        self.rng = random.Random(seed)
        self.per_variant = per_variant
        self.code = {}          # address -> word
        self.funcs = []         # (address, size, name)
        self.leaf_funcs = []    # leaf functions of the game at their own addresses
        self.next = BASE
        self.cases = []
        self.skipped = {}
        self.ref_unsupported = {}

    def add_func(self, words, name):
        a = self.next
        for i, w in enumerate(words):
            self.code[a + 4 * i] = w
        self.funcs.append((a, 4 * len(words), name))
        self.next = a + 4 * len(words) + 4   # one zero word between functions (XenonRecomp's split)
        return a

    # ------------------------------------------------------------------------------------------------
    def state_from(self, case):
        s = ppcref.State()
        s.r[1] = STACK_TOP
        s.nj = (case.flags >> 2) & 1
        s.rn = case.flags & 3
        for kind, idx, w in case.ins:
            if kind == K_GPR:
                s.r[idx] = (w[0] << 32) | w[1]
            elif kind == K_FPR:
                s.f[idx] = (w[0] << 32) | w[1]
            elif kind == K_VR:
                s.v[idx] = list(w)
            elif kind == K_CR:
                s.cr = w[0]
            elif kind == K_XER:
                s.xer = w[0]
            elif kind == K_CTR:
                s.ctr = (w[0] << 32) | w[1]
            elif kind == K_LR:
                s.lr = (w[0] << 32) | w[1]
            elif kind == K_VSCR:
                s.sat, s.nj = w[0] & 1, (w[0] >> 16) & 1
            elif kind == K_MSR:
                s.msr = w[0]
        for a, data in case.mem_in:
            for i, b in enumerate(data):
                s.mem[a + i] = b
        return s

    def outputs_from(self, s, case):
        vals = []
        for kind, idx in case.outs:
            if kind == K_GPR:
                v = s.r[idx]
                vals.append([v >> 32, v & M32, 0, 0])
            elif kind == K_FPR:
                v = s.f[idx]
                vals.append([v >> 32, v & M32, 0, 0])
            elif kind == K_VR:
                vals.append(list(s.v[idx]))
            elif kind == K_CR:
                vals.append([s.cr & M32, 0, 0, 0])
            elif kind == K_XER:
                vals.append([s.xer, 0, 0, 0])
            elif kind == K_CTR:
                vals.append([s.ctr >> 32, s.ctr & M32, 0, 0])
            elif kind == K_LR:
                vals.append([s.lr >> 32, s.lr & M32, 0, 0])
            elif kind == K_VSCR:
                vals.append([(s.nj << 16) | s.sat, 0, 0, 0])
            elif kind == K_MSR:
                vals.append([s.msr, 0, 0, 0])
            elif kind == K_RN:
                vals.append([s.rn, 0, 0, 0])
        mems = [bytes(s.mem.get(a + i, 0) for i in range(n)) for a, n in case.mem_out]
        return vals, mems

    def finish(self, case):
        """Runs the reference unless an expectation is already set (hardware vectors)."""
        if case.expected is None and case.origin != "hw-none":
            s = self.state_from(case)
            try:
                ppcref.run(s, self.code, case.addr)
                case.expected = self.outputs_from(s, case)
                case.origin = "ref"
            except Unsupported as e:
                if str(e) == "trap taken":
                    return            # a taken trap ends the guest, not a test case
                key = "%s: %s" % (case.form, str(e).split(" at ")[0])
                self.ref_unsupported[key] = self.ref_unsupported.get(key, 0) + 1
                case.origin = "none"
        if case.origin == "hw-none":
            case.origin = "none"
        self.cases.append(case)

    # ------------------------------------------------------------------------------------------------
    def reg_inputs(self, case, regs, xname):
        single = xname in SINGLE_FP
        floaty = bool(VMX_FLOAT.match(xname))
        seen = set()
        for kind, n in regs:
            if (kind, n) in seen:
                continue
            seen.add((kind, n))
            if kind == "r":
                v = rand_gpr(self.rng)
                case.ins.append((K_GPR, n, [v >> 32, v & M32, 0, 0]))
            elif kind == "f":
                v = rand_fpr(self.rng, single)
                case.ins.append((K_FPR, n, [v >> 32, v & M32, 0, 0]))
            elif kind == "v":
                case.ins.append((K_VR, n, rand_vr(self.rng, floaty)))
        return seen

    def std_outputs(self, case, seen, extra=()):
        for kind, n in sorted(seen):
            case.outs.append(({"r": K_GPR, "f": K_FPR, "v": K_VR}[kind], n))
        case.outs += [(K_CR, 0), (K_XER, 0)] + list(extra)

    def cr_xer_inputs(self, case):
        cr = self.rng.getrandbits(32)
        case.ins.append((K_CR, 0, [cr, 0, 0, 0]))
        xer = self.rng.choice([0, 0x20000000, 0x80000000, 0xA0000000, 0xE0000000])
        case.ins.append((K_XER, 0, [xer, 0, 0, 0]))

    # ------------------------------------------------------------------------------------------------
    def form_cases(self, form, info):
        reps = list(info["variants"]) + [dict(word=r["word"], ops=r["ops"], key="regclass") for r in
                                         info["regclass"].values()]
        seen_words = set()
        for rep in reps:
            w = rep["word"]
            if w in seen_words:
                continue
            seen_words.add(w)
            xname = xenia_lookup.lookup(w)
            if xname in SKIP:
                self.skipped[form] = SKIP[xname]
                return
            if xname in ("bcx",):
                self.branch_cases(form, w)
            elif xname == "bclrx":
                self.bclr_cases(form, w)
            elif xname == "bx":
                if w & 1:
                    self.call_cases(form)
                else:
                    self.skipped[form] = "unconditional branch to another function (tail call), covered by bl/blr tests"
                return
            elif xname in ("lwarx", "ldarx", "stwcx", "stdcx"):
                self.reserve_cases(form, xname)
                return
            elif MEM.match(xname) or xname in ("dcbz", "dcbz128", "lvsl", "lvsr"):
                self.mem_cases(form, w, rep["ops"], xname)
            else:
                self.alu_cases(form, w, rep["ops"], xname)

    def alu_cases(self, form, w, ops, xname):
        spr = (ppcref.X(w, 16, 20) << 5) | ppcref.X(w, 11, 15)
        if xname == "mtspr" and spr == 8:
            # mtlr: save and restore the return address around it, read it back with mflr r3
            rs = ppcref.RT(w)
            keep = 30 if rs != 30 else 29
            fn = self.add_func([0x7C0802A6 | (keep << 21), w, 0x7C6802A6, 0x7C0803A6 | (keep << 21), BLR], form)
            ops = ops + ",r3"
        else:
            fn = self.add_func([w, BLR], form)
        regs = operand_regs(ops)
        n = self.per_variant * (2 if xname.startswith("f") or VMX_FLOAT.match(xname) else 1)
        for i in range(n):
            c = Case(fn, form, "alu")
            if xname in RN_FORMS:
                c.flags = i & 3
            if VMX_FLOAT.match(xname) or xname.startswith("v"):
                c.flags |= 0 if (i % 5 == 4) else 4    # NJ = 1 (runtime default), every 5th NJ = 0
            else:
                c.flags |= 4
            seen = self.reg_inputs(c, [(k, n_) for k, n_ in regs if k != "cr"], xname)
            self.cr_xer_inputs(c)
            if xname in ("twi", "tdi"):
                # only inputs that do not trap
                pass
            if xname == "mtfsfx":
                for k, idx, val in c.ins:
                    if k == K_FPR:
                        val[0], val[1] = 0xFFF80000, self.rng.choice([0, 1, 2, 3, 0xF8, 0x4])
            extra = [(K_VSCR, 0)] if xname.startswith("v") else []
            if xname == "mtfsfx":
                extra.append((K_RN, 0))
            # MSR: ReXGlue (and Xenia) model mtmsrd/mfmsr as a global interrupt lock, not as the MSR
            # register; only EE as mfmsr reports it is compared
            self.std_outputs(c, seen, extra)
            if xname == "mfmsr":
                c.masks = {i_: [0, 0x8000, 0, 0] for i_, o in enumerate(c.outs) if o[0] == K_GPR}
            if xname == "mffsx":
                c.masks = {i_: [0, 3, 0, 0] for i_, o in enumerate(c.outs) if o[0] == K_FPR}
            if xname == "mfcr" and ppcref.X(w, 11, 11):
                # mfocrf: only the selected field of RT is defined [ISA]
                fxm = ppcref.X(w, 12, 19)
                fm = 0
                for k_ in range(8):
                    if fxm & (0x80 >> k_):
                        fm |= 0xF << (28 - 4 * k_)
                c.masks = {i_: [0, fm, 0, 0] for i_, o in enumerate(c.outs) if o == (K_GPR, ppcref.RT(w))}
            self.finish(c)
        self.hw_cases(form, w, xname)

    # --- hardware vectors -------------------------------------------------------------------------
    def hw_cases(self, form, w, xname):
        hv = self.hw
        if xname in ("frsqrtex", "fresx"):
            table = hv["kScalarReciprocalSqrtEstimateCases" if xname == "frsqrtex" else
                       "kScalarReciprocalEstimateCases"]
            fn = self.add_func([w, BLR], form)
            ft, fb = ppcref.FT(w), ppcref.FB(w)
            for inp, out in table:
                c = Case(fn, form, "hw")
                c.flags = 4
                c.ins.append((K_FPR, fb, [inp >> 32, inp & M32, 0, 0]))
                c.outs.append((K_FPR, ft))
                c.expected = ([[out >> 32, out & M32, 0, 0]], [])
                c.origin = "hw"
                self.cases.append(c)
            return
        vec_est = {"vrefp": "kReciprocalEstimateCases", "vrsqrtefp": "kReciprocalSqrtEstimateCases",
                   "vlogefp": "kLog2EstimateCases", "vexptefp": "kExp2EstimateCases"}
        if xname in vec_est:
            table = hv[vec_est[xname]]
            fn = self.add_func([w, BLR], form)
            dd, _, b, _ = ppcref.regs(w, xname)
            for i in range(0, len(table), 4):
                chunk = [table[(i + k) % len(table)] for k in range(4)]
                c = Case(fn, form, "hw")
                c.flags = 4
                c.ins.append((K_VR, b, [x[0] for x in chunk]))
                c.outs.append((K_VR, dd))
                c.expected = ([[x[1] for x in chunk]], [])
                c.origin = "hw"
                self.cases.append(c)
            fam = {"vrefp": 0, "vrsqrtefp": 1, "vlogefp": 2, "vexptefp": 3}[xname]
            for fam_, ins, outs in hv["kEstimateNJ0Cases"]:
                if fam_ != fam:
                    continue
                c = Case(fn, form, "hw")
                c.flags = 0
                c.ins.append((K_VR, b, list(ins)))
                c.outs.append((K_VR, dd))
                c.expected = ([list(outs)], [])
                c.origin = "hw"
                self.cases.append(c)
            return
        fused = {"vmaddfp": ("madd", "A C B"), "vnmsubfp": ("nmsub", "A C B"),
                 "vmaddfp128": ("madd", "A B D"), "vmaddcfp128": ("madd", "A D B"),
                 "vnmsubfp128": ("nmsub", "A B D")}
        if xname in fused:
            kind, layout = fused[xname]
            dd, a, b, c_ = ppcref.regs(w, xname)
            regmap = {"A": a, "B": b, "C": c_, "D": dd}
            names = layout.split()
            if len({regmap[x] for x in names} | {dd}) < (3 if "D" in names else 4):
                # the game word reuses registers; build a word with distinct registers
                w = self.distinct_fused(w, xname)
                dd, a, b, c_ = ppcref.regs(w, xname)
                regmap = {"A": a, "B": b, "C": c_, "D": dd}
            fn = self.add_func([w, BLR], form)
            table = hv["kMultiplyAddCases" if kind == "madd" else "kNegativeMultiplySubtractCases"]
            for i in range(0, len(table), 4):
                chunk = [table[(i + k) % len(table)] for k in range(4)]
                self._fused_case(fn, form, regmap, names, dd, [x[0] for x in chunk], [x[1] for x in chunk],
                                 [x[2] for x in chunk], [x[3] for x in chunk], 1)
            for neg, ma, mb, mc, mo in hv["kMultiplyAddNJ0Cases"]:
                if neg != (kind == "nmsub"):
                    continue
                self._fused_case(fn, form, regmap, names, dd, ma, mb, mc, mo, 0)
            return
        if xname in ("vmsum3fp128", "vmsum4fp128"):
            dd, a, b, _ = ppcref.regs(w, xname)
            fn = self.add_func([w, BLR], form)
            terms = 3 if xname == "vmsum3fp128" else 4
            for ca, cb, out in hv["kDotProduct%dCases" % terms]:
                c = Case(fn, form, "hw")
                c.flags = 4
                c.ins += [(K_VR, a, list(ca)), (K_VR, b, list(cb))]
                c.outs.append((K_VR, dd))
                c.expected = ([[out] * 4], [])
                c.origin = "hw"
                self.cases.append(c)
            for t, ca, cb, out in hv["kDotProductNJ0Cases"]:
                if t != terms:
                    continue
                c = Case(fn, form, "hw")
                c.flags = 0
                c.ins += [(K_VR, a, list(ca)), (K_VR, b, list(cb))]
                c.outs.append((K_VR, dd))
                c.expected = ([[out] * 4], [])
                c.origin = "hw"
                self.cases.append(c)
            return
        if xname == "vpkd3d128":
            imm = (w >> 16) & 31
            typ, mask, shift = imm >> 2, imm & 3, (w >> 6) & 3
            ww = (w & ~((31 << 11) | 3)) | (1 << 11)  # vB = v1, vD from the game word
            if ppcref.v128_d(ww) == 1:
                ww = (ww & ~(31 << 21)) | (2 << 21)
            fn = self.add_func([ww, BLR], form)
            dd, b = ppcref.v128_d(ww), ppcref.v128_b(ww)
            for grp, t, m, sh, src, exp, satbit in hv["kPackD3DResults"]:
                if (t, m, sh) != (typ, mask, shift):
                    continue
                self._pack_hw(fn, form, dd, b, src, exp, satbit)
            # every other captured combination with a re-encoded word (synthetic encodings)
            if not getattr(self, "_pack_all_done", False):
                self._pack_all_done = True
                combos = sorted({(t, m, sh) for _, t, m, sh, _, _, _ in hv["kPackD3DResults"]})
                for t, m, sh in combos:
                    w2 = (ww & ~((31 << 16) | (3 << 6))) | (((t << 2) | m) << 16) | (sh << 6)
                    fn2 = self.add_func([w2, BLR], form + ":hw%d.%d.%d" % (t, m, sh))
                    for grp, t2, m2, sh2, src, exp, satbit in hv["kPackD3DResults"]:
                        if (t2, m2, sh2) == (t, m, sh):
                            self._pack_hw(fn2, form + ":hw%d.%d.%d" % (t, m, sh), dd, b, src, exp, satbit)

    def _pack_hw(self, fn, form, dd, b, src, exp, satbit):
        c = Case(fn, form, "hw")
        c.flags = 4
        c.ins += [(K_VR, b, list(src)), (K_VR, dd, list(self.hw["kPackD3DInitial"])),
                  (K_VSCR, 0, [1 << 16, 0, 0, 0])]
        c.outs += [(K_VR, dd), (K_VSCR, 0)]
        c.expected = ([list(exp), [(1 << 16) | (1 if satbit else 0), 0, 0, 0]], [])
        c.origin = "hw"
        self.cases.append(c)

    def _fused_case(self, fn, form, regmap, names, dd, la, lb, lc, lo, nj):
        c = Case(fn, form, "hw")
        c.flags = nj << 2
        vals = {names[0]: la, names[1]: lb, names[2]: lc}
        for nm, val in vals.items():
            c.ins.append((K_VR, regmap[nm], list(val)))
        c.outs.append((K_VR, dd))
        c.expected = ([list(lo)], [])
        c.origin = "hw"
        self.cases.append(c)

    def distinct_fused(self, w, xname):
        if xname.endswith("128"):
            # vD = v1, vA = v2, vB = v3 in the VMX128 fields
            w &= ~((31 << 21) | (3 << 2) | (31 << 16) | (1 << 10) | (1 << 5) | (31 << 11) | 3)
            return w | (1 << 21) | (2 << 16) | (3 << 11)
        w &= ~((31 << 21) | (31 << 16) | (31 << 11) | (31 << 6))
        return w | (1 << 21) | (2 << 16) | (3 << 11) | (4 << 6)

    # --- branches -----------------------------------------------------------------------------------
    def branch_cases(self, form, w):
        bc = (w & ~0xFFFC & ~3) | 12           # BD = +12, AA = LK = 0
        fn = self.add_func([bc, li(3, 1), BLR, li(3, 2), BLR], form)
        for i in range(self.per_variant):
            c = Case(fn, form, "branch")
            c.flags = 4
            cr = self.rng.getrandbits(32)
            c.ins.append((K_CR, 0, [cr, 0, 0, 0]))
            ctr = self.rng.choice([0, 1, 2, 3, 0x100000000, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0x100000001])
            c.ins.append((K_CTR, 0, [ctr >> 32, ctr & M32, 0, 0]))
            c.outs += [(K_GPR, 3), (K_CTR, 0), (K_CR, 0)]
            self.finish(c)
        if xenia_lookup.lookup(w) == "bcx" and (ppcref.RA(w) & 3) == 3 and not (ppcref.RT(w) & 16):
            self.nan_branch_cases(form, bc)

    def nan_branch_cases(self, form, bc):
        """bso/bns after fcmpu: the SO bit of the field is the unordered bit."""
        crf = ppcref.RA(bc) >> 2
        fcmpu = 0xFC000000 | (crf << 23) | (1 << 16) | (2 << 11)    # fcmpu crf, f1, f2
        fn = self.add_func([fcmpu, bc, li(3, 1), BLR, li(3, 2), BLR], form + "+fcmpu")
        vals = [d(1.0), d(-1.0), 0, 1 << 63, 0x7FF8000000000000, 0xFFF8000000000001, 0x7FF0000000000001,
                0x7FF0000000000000, 0xFFF0000000000000, 1]
        for a in vals:
            for b in vals:
                c = Case(fn, form + "+fcmpu", "nan-branch")
                c.flags = 4
                c.ins += [(K_FPR, 1, [a >> 32, a & M32, 0, 0]), (K_FPR, 2, [b >> 32, b & M32, 0, 0]),
                          (K_CR, 0, [0xFFFFFFFF, 0, 0, 0])]
                c.outs += [(K_GPR, 3), (K_CR, 0)]
                self.finish(c)

    def bclr_cases(self, form, w):
        if w & 1:
            self.skipped[form] = "blrl: call through LR"
            return
        if (ppcref.RT(w) & 0x14) == 0x14:   # plain blr: every test ends with it
            return
        fn = self.add_func([li(3, 2), w, li(3, 1), BLR], form)
        for i in range(self.per_variant):
            c = Case(fn, form, "branch")
            c.flags = 4
            c.ins.append((K_CR, 0, [self.rng.getrandbits(32), 0, 0, 0]))
            ctr = self.rng.choice([0, 1, 2, 0x100000000, 0xFFFFFFFF])
            c.ins.append((K_CTR, 0, [ctr >> 32, ctr & M32, 0, 0]))
            c.outs += [(K_GPR, 3), (K_CTR, 0)]
            self.finish(c)

    def call_cases(self, form):
        """bl with CR fields, XER[CA] and XER[SO] across the call (calls must preserve them)."""
        mflr12, mtlr12 = 0x7D8802A6, 0x7D8803A6
        mfcr3 = 0x7C600026
        mfxer4 = 0x7C8102A6
        # callee 1 clobbers cr0, cr1, cr6, cr7 and XER[CA]; callee 2 touches nothing
        clob = self.add_func([li(5, -1), 0x2C050000, 0x2C850000, 0x2F050000, 0x2F850000,
                              0x30A5FFFF, BLR], "callee_clobber")   # cmpwi crX,r5,0; addic r5,r5,-1
        quiet = self.add_func([li(6, 7), BLR], "callee_quiet")
        for callee in (clob, quiet):
            start = self.next
            # caller: mflr r12; bl callee; mfcr r3; mfxer r4; mtlr r12; blr
            words = [mflr12, 0, mfcr3, mfxer4, mtlr12, BLR]
            words[1] = 0x48000001 | ((callee - (start + 4)) & 0x03FFFFFC)
            fn = self.add_func(words, form + ("_clobber" if callee == clob else "_quiet"))
            for i in range(self.per_variant):
                c = Case(fn, form, "call")
                c.flags = 4
                c.ins.append((K_CR, 0, [self.rng.getrandbits(32), 0, 0, 0]))
                c.ins.append((K_XER, 0, [self.rng.choice([0, 0x20000000, 0x80000000, 0xA0000000]), 0, 0, 0]))
                c.outs += [(K_GPR, 3), (K_GPR, 4), (K_GPR, 5), (K_GPR, 6), (K_CR, 0), (K_XER, 0)]
                c.masks = {1: [0, M32, 0, 0]}   # mfxer: compare the XER word (high half: see report)
                self.finish(c)

    # --- memory -------------------------------------------------------------------------------------
    def mem_cases(self, form, w, ops, xname):
        regs = operand_regs(ops)
        ra, rb = ppcref.RA(w), ppcref.RB(w)
        d_form = (w >> 26) not in (31, 4)
        if xname in ("ld", "ldu", "lwa", "std", "stdu"):
            disp = ppcref.sext(w & 0xFFFC, 16)
        elif d_form:
            disp = ppcref.SI(w)
        else:
            disp = None
        if d_form and ra == 0:
            self.skipped.setdefault(form + " (RA=0)", "absolute D-form address, not relocatable into the buffer")
            return
        fn = self.add_func([w, BLR], form)
        vec = xname.startswith(("lv", "stv"))
        for i in range(self.per_variant * (2 if vec or xname.startswith(("lf", "stf")) else 1)):
            c = Case(fn, form, "mem")
            c.flags = 4
            align = self.rng.choice([0, 0, 0, 1, 2, 3, 4, 8, 12, 15] if vec or xname.startswith("lvs")
                                    else [0, 0, 0, 0, 1, 2, 3, 4])
            if xname in ("ld", "ldu", "lwa", "std", "stdu", "lfd", "stfd", "lfdx", "stfdx"):
                align &= ~3 if self.rng.random() < 0.7 else 0
            ea = BUF + 0x100 + align
            seen = self.reg_inputs(c, [(k, n) for k, n in regs if k != "cr"], xname)
            if xname.startswith(("lf", "stf")):
                for k, idx, val in c.ins:
                    if k == K_FPR:
                        v = self.rng.choice([rand_fpr(self.rng, True), rand_fpr(self.rng, False)])
                        val[0], val[1] = v >> 32, v & M32
            # place the address
            def setreg(n, v):
                for k, idx, val in c.ins:
                    if k == K_GPR and idx == n:
                        val[0], val[1] = (v >> 32) & M32, v & M32
                        return
                c.ins.append((K_GPR, n, [(v >> 32) & M32, v & M32, 0, 0]))
                seen.add(("r", n))
            if disp is not None:
                setreg(ra, (ea - disp) & M64)
            else:
                if ra == 0:
                    setreg(rb, ea)
                elif ra == rb:
                    ea &= ~1
                    setreg(ra, ea // 2)
                else:
                    k = self.rng.choice([0, 4, 0x40, 0x100, -0x10])
                    setreg(ra, ea - k)
                    setreg(rb, k & M64)
            # value register == base register: the address is the stored value, fine
            region = BUF
            data = bytearray(self.rng.getrandbits(8) for _ in range(0x300))
            if xname.startswith("lf") and self.rng.random() < 0.5:
                pat = self.rng.choice(F32_EDGES) if "s" in xname[2:3] else self.rng.choice(FP_EDGES)
                n = 4 if xname.startswith("lfs") else 8
                off = ea - region
                data[off:off + n] = pat.to_bytes(n, "big")
            c.mem_in.append((region, bytes(data)))
            c.mem_out.append((region, 0x300))
            self.std_outputs(c, seen)
            self.finish(c)

    def reserve_cases(self, form, xname):
        wide = xname in ("ldarx", "stdcx")
        larx = 0x7C0000A8 if wide else 0x7C000028     # ldarx/lwarx r5,0,r4
        stcx = 0x7C0001AD if wide else 0x7C00012D     # stdcx./stwcx. r6,0,r4
        larx |= (5 << 21) | (4 << 11)
        stcx |= (6 << 21) | (4 << 11)
        pair = self.add_func([larx, stcx, BLR], form + "_pair")
        alone = self.add_func([stcx, BLR], form + "_alone")
        for fn, grp in ((pair, "pair"), (alone, "alone")):
            for i in range(self.per_variant // 2):
                c = Case(fn, form, "reserve-" + grp)
                c.flags = 4
                ea = BUF + 0x100 + (8 * self.rng.randint(0, 3))
                v6 = rand_gpr(self.rng)
                c.ins += [(K_GPR, 4, [0, ea, 0, 0]), (K_GPR, 6, [v6 >> 32, v6 & M32, 0, 0]),
                          (K_XER, 0, [self.rng.choice([0, 0x80000000]), 0, 0, 0])]
                data = bytes(self.rng.getrandbits(8) for _ in range(0x300))
                c.mem_in.append((BUF, data))
                c.mem_out.append((BUF, 0x300))
                c.outs += [(K_GPR, 5), (K_CR, 0)]
                if grp == "alone":
                    c.note = "stcx. without a reservation: the ISA does not store"
                self.finish(c)

    # ------------------------------------------------------------------------------------------------
    def special_cases(self):
        """Sequences from the earlier ports and the phase-0 suspects that are not a single form."""
        # Flush to zero between scalar FP and VMX: a VMX add with a denormal, then a scalar add of
        # denormals in the same function (codegen switches the host flush mode).
        vaddfp = 0x1000180A | (1 << 21) | (2 << 16) | (3 << 11)    # vaddfp v1,v2,v3 (re-encoded)
        vaddfp = (0x1000000A) | (1 << 21) | (2 << 16) | (3 << 11)
        fadd = 0xFC00002A | (1 << 21) | (2 << 16) | (3 << 11)      # fadd f1,f2,f3
        fadds = 0xEC00002A | (4 << 21) | (2 << 16) | (3 << 11)     # fadds f4,f2,f3
        for words, name in (([vaddfp, fadd, fadds, BLR], "ftz:vmx_then_fpu"),
                            ([fadd, vaddfp, fadds, BLR], "ftz:fpu_vmx_fpu")):
            fn = self.add_func(words, name)
            for nj in (1, 0):
                for a, b in ((1, 1), (0x000FFFFFFFFFFFFF, 1), (f2d(0x00000001), f2d(0x00400000)),
                             (f2d(0x007FFFFF), f2d(0x80000001))):
                    c = Case(fn, name, "ftz")
                    c.flags = nj << 2
                    c.ins += [(K_FPR, 2, [a >> 32, a & M32, 0, 0]), (K_FPR, 3, [b >> 32, b & M32, 0, 0]),
                              (K_VR, 2, [0x00000001, 0x007FFFFF, 0x3F800000, 0x80400000]),
                              (K_VR, 3, [0x00000001, 0x00000001, 0x00000001, 0x80000001])]
                    c.outs += [(K_FPR, 1), (K_FPR, 4), (K_VR, 1)]
                    self.finish(c)
        # fmadds double rounding: an integer product with 25 significant bits is a tie at single
        # precision; a tiny addend decides the direction [ISA: one rounding of the exact result].
        fmadds = 0xEC00003A | (1 << 21) | (2 << 16) | (3 << 11) | (4 << 6)    # fmadds f1,f2,f4,f3
        fn = self.add_func([fmadds, BLR], "fmadds:tie")
        for i in range(64):
            a = self.rng.randrange(1 << 11, 1 << 12) | 1
            b = self.rng.randrange(1 << 12, 1 << 13) | 1
            while (a * b).bit_length() != 25:
                a = self.rng.randrange(1 << 11, 1 << 12) | 1
                b = self.rng.randrange(1 << 12, 1 << 13) | 1
            tiny = self.rng.choice([1, -1]) * 2.0 ** self.rng.randint(-60, -40)
            c = Case(fn, "fmadds:tie", "fma-tie")
            c.flags = 4
            c.ins += [(K_FPR, 2, list(divmod(d(float(a)), 1 << 32)) + [0, 0]),
                      (K_FPR, 4, list(divmod(d(float(b)), 1 << 32)) + [0, 0]),
                      (K_FPR, 3, list(divmod(d(tiny), 1 << 32)) + [0, 0])]
            c.outs.append((K_FPR, 1))
            self.finish(c)
        self.nan_cases()

    def nan_cases(self):
        """NaN matrix: every pair (triple for fused forms) of QNaN/SNaN with
        payloads and both signs, infinities, zeros and ones, so that the default NaN (inf-inf, 0*inf,
        0/0, sqrt(-x)), the NaN priority (A, B, C) and the sign of a propagated NaN are all exercised.
        Expected values from ppcref.py: ISA Book I 4.3.2/4.6.6 (FPU), AltiVec PEM (VMX; the VMX NaN
        priority is also in the SDK hardware vectors of vmaddfp). Forms are named nan:<form>."""
        D2 = [0x7FF8000000000ABC, 0xFFF8000000000123, 0x7FF4000000000456, 0xFFF0000000000001,
              0x7FF0000000000000, 0xFFF0000000000000, 0, 1 << 63, d(1.0), d(-1.0)]
        # single forms: NaN payloads and values that are singles
        S2 = [0x7FF8002460000000, 0xFFF8000AE0000000, 0x7FF4015780000000, 0xFFF0000020000000,
              0x7FF0000000000000, 0xFFF0000000000000, 0, 1 << 63, d(1.0), d(-1.0)]
        F3 = [0, 1, 2, 4, 6, 8]   # indices into the lists above for the fused forms (6^3 cases)
        A_FORMS = {  # name: (primary 63/59 word with xo, kind)
            "fadd": (0xFC00002A, "ab"), "fsub": (0xFC000028, "ab"), "fdiv": (0xFC000024, "ab"),
            "fmul": (0xFC000032, "ac"), "fmadd": (0xFC00003A, "acb"), "fmsub": (0xFC000038, "acb"),
            "fnmadd": (0xFC00003E, "acb"), "fnmsub": (0xFC00003C, "acb"), "fsqrt": (0xFC00002C, "b"),
        }
        for base_name, (word, kind) in sorted(A_FORMS.items()):
            for single in (False, True):
                name = base_name + ("s" if single else "")
                w = (word & ~0xFC000000) | (0xEC000000 if single else 0xFC000000)
                w |= (1 << 21)                                   # frD = f1
                regs = {"a": 2, "b": 3, "c": 4}
                for k in kind:
                    w |= regs[k] << {"a": 16, "b": 11, "c": 6}[k]
                fn = self.add_func([w, BLR], "nan:" + name)
                vals = S2 if single else D2
                if kind == "b":
                    combos = [(v,) for v in vals + [d(-4.0), d(4.0)]]
                elif len(kind) == 2:
                    combos = [(x, y) for x in vals for y in vals]
                else:
                    combos = [(vals[i], vals[j], vals[k2]) for i in F3 for j in F3 for k2 in F3]
                for combo in combos:
                    c = Case(fn, "nan:" + name, "nan")
                    c.flags = 4   # rn 0, nj 1
                    for k, v in zip(kind, combo):
                        c.ins.append((K_FPR, regs[k], [v >> 32, v & M32, 0, 0]))
                    c.outs.append((K_FPR, 1))
                    self.finish(c)
        # VMX: own encodings for the classic forms, the game's words for the VMX128 forms
        L = [0x7FC00123, 0xFFC00ABC, 0x7F800001, 0xFFA00ABC, 0x7F800000, 0xFF800000, 0, 0x80000000,
             0x3F800000, 0xBF800000, 0x00000001]
        pairs = [(x, y) for x in L for y in L]
        vforms = [("vaddfp", 0x1000000A | (1 << 21) | (2 << 16) | (3 << 11), 1, 2, 3),
                  ("vsubfp", 0x1000004A | (1 << 21) | (2 << 16) | (3 << 11), 1, 2, 3),
                  ("vmaxfp", 0x1000040A | (1 << 21) | (2 << 16) | (3 << 11), 1, 2, 3),
                  ("vminfp", 0x1000044A | (1 << 21) | (2 << 16) | (3 << 11), 1, 2, 3)]
        for f in ("vaddfp128", "vsubfp128", "vmulfp128", "vmaxfp128", "vminfp128"):
            info = self.catalog.get(f) if getattr(self, "catalog", None) else None
            if not info:
                continue
            for rep in info["variants"]:
                rr = operand_regs(rep["ops"])
                if len(rr) == 3 and rr[1] != rr[2]:
                    vforms.append((f, rep["word"], rr[0][1], rr[1][1], rr[2][1]))
                    break
        for name, w, vd, va, vb in vforms:
            fn = self.add_func([w, BLR], "nan:" + name)
            for nj in (1, 0):
                for i in range(0, len(pairs), 4):
                    chunk = pairs[i:i + 4]
                    chunk += [(0x3F800000, 0x3F800000)] * (4 - len(chunk))
                    c = Case(fn, "nan:" + name, "nan")
                    c.flags = nj << 2
                    c.ins += [(K_VR, va, [x for x, _ in chunk]), (K_VR, vb, [y for _, y in chunk]),
                              (K_VSCR, 0, [nj << 16, 0, 0, 0])]
                    c.outs.append((K_VR, vd))
                    self.finish(c)


# ----------------------------------------------------------------------------------------------------
# Leaf functions of the game (addresses from leaf_sample.toml, code from the user's image)
# ----------------------------------------------------------------------------------------------------
IMAGE_BASE = 0x82000000
LEAF_BUF = 0x10000000      # eight 1 KB regions, argument pointers point to their middle
LEAF_STACK = STACK_TOP - 0x1000


class ImageState(ppcref.State):
    """Reference state that also reads the constants of the game image (read only)."""
    image = b""

    def rb(self, a):
        a &= M32
        if a in self.mem:
            return self.mem[a]
        if IMAGE_BASE <= a < IMAGE_BASE + len(self.image):
            return self.image[a - IMAGE_BASE]
        raise Unsupported("read outside the test buffer at %08X" % a)


def chkstk_cases(g, entries, image):
    """__chkstk with fixed frame sizes in r12; the probes read the stack pages below r1."""
    ImageState.image = image
    for ent in entries:
        start, size = ent["address"], ent["size"]
        words = [struct.unpack_from(">I", image, start - IMAGE_BASE + 4 * i)[0] for i in range(size // 4)]
        for i, w in enumerate(words):
            g.code[start + 4 * i] = w
        name = "chkstk_%08X" % start
        g.leaf_funcs.append((start, size, name))
        for r12 in ent["r12"]:
            c = Case(start, name, "chkstk")
            c.flags = 4
            v = r12 & M64
            c.ins.append((K_GPR, 12, [v >> 32, v & M32, 0, 0]))
            g.cr_xer_inputs(c)
            c.mem_in.append((STACK_TOP - 0x11000, bytes(0x11000)))
            c.outs += [(K_GPR, 0), (K_GPR, 1), (K_GPR, 11), (K_GPR, 12), (K_CTR, 0), (K_CR, 0)]
            s = ImageState()
            s.__dict__.update(g.state_from(c).__dict__)
            ppcref.run(s, g.code, start)
            c.expected = g.outputs_from(s, c)
            c.origin = "ref"
            g.cases.append(c)


def leaf_cases(g, sample, image, per_func):
    ImageState.image = image
    for ent in sample:
        start, size = ent["address"], ent["size"]
        words = [struct.unpack_from(">I", image, start - IMAGE_BASE + 4 * i)[0] for i in range(size // 4)]
        for i, w in enumerate(words):
            g.code[start + 4 * i] = w
        name = "leaf_%08X" % start
        g.leaf_funcs.append((start, size, name))
        gprs, fprs, vrs, bases = set(), set(), set(), set()
        for w in words:
            xn = xenia_lookup.lookup(w) or ""
            if xn.startswith("v") or xn.startswith(("lv", "stv")):
                dd, a, b, c = ppcref.regs(w, xn)
                vrs |= {dd, a, b} | ({c} if not xn.endswith("128") else set())
                if xn.startswith(("lv", "stv")):
                    vrs = (vrs - {a, b}) | {dd}
                    bases |= {ppcref.RA(w), ppcref.RB(w)}
            elif xn.startswith("f") or xn.startswith(("lf", "stf")):
                fprs |= {ppcref.FT(w), ppcref.FA(w), ppcref.FB(w), ppcref.FC(w)}
                if xn.startswith(("lf", "stf")):
                    fprs = fprs - {ppcref.FA(w), ppcref.FB(w)} | {ppcref.FT(w)}
                    bases.add(ppcref.RA(w))
            elif xn.startswith(("l", "st")):
                bases.add(ppcref.RA(w))
        for i in range(per_func):
            c = Case(start, name, "leaf")
            c.flags = 4
            for r in range(3, 11):
                if r in bases or g.rng.random() < 0.5:
                    v = LEAF_BUF + 0x400 * (r - 3) + 0x200
                else:
                    v = rand_gpr(g.rng)
                c.ins.append((K_GPR, r, [v >> 32, v & M32, 0, 0]))
            for f in sorted(fprs | set(range(1, 9))):
                v = rand_fpr(g.rng, g.rng.random() < 0.7)
                c.ins.append((K_FPR, f, [v >> 32, v & M32, 0, 0]))
            for v_ in sorted(vrs):
                c.ins.append((K_VR, v_, rand_vr(g.rng, g.rng.random() < 0.7)))
            g.cr_xer_inputs(c)
            for k in range(8):
                c.mem_in.append((LEAF_BUF + 0x400 * k, bytes(g.rng.getrandbits(8) for _ in range(0x400))))
                c.mem_out.append((LEAF_BUF + 0x400 * k, 0x400))
            c.mem_in.append((LEAF_STACK, bytes(0x1000)))
            c.outs += [(K_GPR, r) for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)]
            c.outs += [(K_FPR, f) for f in sorted(fprs | {0, 1, 2, 3})]
            c.outs += [(K_VR, v_) for v_ in sorted(vrs)]
            c.outs += [(K_CR, 0), (K_XER, 0)]
            s = ImageState()
            sref = g.state_from(c)
            s.__dict__.update(sref.__dict__)
            try:
                ppcref.run(s, g.code, start)
                c.expected = g.outputs_from(s, c)
                c.origin = "ref"
            except Unsupported as e:
                key = "%s: %s" % (name, str(e).split(" at ")[0])
                g.ref_unsupported[key] = g.ref_unsupported.get(key, 0) + 1
            g.cases.append(c)


# ----------------------------------------------------------------------------------------------------
# SDK hardware vectors
# ----------------------------------------------------------------------------------------------------
def parse_cases_h(path):
    text = open(path, encoding="utf-8").read()
    out = {}
    for m in re.finditer(r"inline constexpr \w+ (k\w+)\[\] = \{(.*?)\n\};", text, re.S):
        body = m.group(2)
        nums = [int(x, 0) if not x in ("true", "false") else int(x == "true")
                for x in re.findall(r"0x[0-9A-Fa-f]+|\btrue\b|\bfalse\b|\b\d+\b", body)]
        out[m.group(1)] = nums
    m = re.search(r"kPackD3DInitial = \{([^}]*)\}", text)
    if m:
        out["kPackD3DInitial"] = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
    return out


def load_hw(sdk_dir):
    raw = {}
    for name in ("estimate_cases.h", "estimate_nj0_cases.h", "multiply_add_cases.h",
                 "multiply_add_nj0_cases.h", "dot_product_cases.h", "dot_product_nj0_cases.h",
                 "pack_d3d_cases.h"):
        raw.update(parse_cases_h(os.path.join(sdk_dir, name)))

    def chunks(xs, n):
        return [xs[i:i + n] for i in range(0, len(xs), n)]

    hw = {}
    for k in ("kReciprocalEstimateCases", "kReciprocalSqrtEstimateCases", "kLog2EstimateCases",
              "kExp2EstimateCases", "kScalarReciprocalEstimateCases", "kScalarReciprocalSqrtEstimateCases"):
        hw[k] = [tuple(x) for x in chunks(raw[k], 2)]
    hw["kEstimateNJ0Cases"] = [(x[0], x[1:5], x[5:9]) for x in chunks(raw["kEstimateNJ0Cases"], 9)]
    for k in ("kMultiplyAddCases", "kNegativeMultiplySubtractCases"):
        hw[k] = [tuple(x) for x in chunks(raw[k], 4)]
    hw["kMultiplyAddNJ0Cases"] = [(bool(x[0]), x[1:5], x[5:9], x[9:13], x[13:17])
                                  for x in chunks(raw["kMultiplyAddNJ0Cases"], 17)]
    for k in ("kDotProduct3Cases", "kDotProduct4Cases"):
        hw[k] = [(x[0:4], x[4:8], x[8]) for x in chunks(raw[k], 9)]
    hw["kDotProductNJ0Cases"] = [(x[0], x[1:5], x[5:9], x[9]) for x in chunks(raw["kDotProductNJ0Cases"], 10)]
    hw["kPackD3DResults"] = [(x[0], x[1], x[2], x[3], x[4:8], x[8:12], x[12])
                             for x in chunks(raw["kPackD3DResults"], 13)]
    hw["kPackD3DInitial"] = raw["kPackD3DInitial"]
    return hw


# ----------------------------------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------------------------------
def write_case_file(path, cases):
    out = bytearray(b"FCPS" + struct.pack("<II", 1, len(cases)))
    for c in cases:
        out += struct.pack("<IIHHHH", c.addr, c.flags, len(c.ins), len(c.outs), len(c.mem_in), len(c.mem_out))
        for kind, idx, w in c.ins:
            out += struct.pack("<BBH4I", kind, idx, 0, *[x & M32 for x in w])
        for kind, idx in c.outs:
            out += struct.pack("<BBH", kind, idx, 0)
        for a, data in c.mem_in:
            out += struct.pack("<II", a, len(data)) + data + bytes((-len(data)) & 3)
        for a, n in c.mem_out:
            out += struct.pack("<II", a, n)
    open(path, "wb").write(out)


def write_expected(path, cases):
    out = bytearray(b"FCPE" + struct.pack("<I", len(cases)))
    for c in cases:
        if c.expected is None:
            out += struct.pack("<I", 0xFFFFFFFF)
            for _ in c.outs:
                out += bytes(16)
            for a, n in c.mem_out:
                out += bytes(n + ((-n) & 3))
            continue
        vals, mems = c.expected
        out += struct.pack("<I", 0)
        for v in vals:
            out += struct.pack("<4I", *[x & M32 for x in v])
        for m in mems:
            out += m + bytes((-len(m)) & 3)
    open(path, "wb").write(out)


def write_code(g, out_dir, stem):
    """One binary from 0x82000000 (sem.bin, base of fcp_semtool emit) and an ELF with one section per
    block for XenonRecomp's test mode (synthetic block plus each leaf function)."""
    funcs = g.funcs + g.leaf_funcs
    hi = max(a + n for a, n, _ in funcs)
    blob = bytearray(hi - IMAGE_BASE)
    for a, w in g.code.items():
        blob[a - IMAGE_BASE:a - IMAGE_BASE + 4] = struct.pack(">I", w)
    open(os.path.join(out_dir, stem + ".bin"), "wb").write(blob)
    with open(os.path.join(out_dir, stem + "_syms.txt"), "w") as f:
        for a, n, name in sorted(funcs):
            f.write("%08X %X %s\n" % (a, n, re.sub(r"[^\w]", "_", name)))
    segs = [(BASE, bytes(blob[BASE - IMAGE_BASE:g.next - IMAGE_BASE]))]
    for a, n, _ in sorted(g.leaf_funcs):
        segs.append((a, bytes(blob[a - IMAGE_BASE:a - IMAGE_BASE + n])))
    write_elf(os.path.join(out_dir, stem + ".elf"), segs)


def write_elf(path, segments):
    """Big endian ELF32 with one PT_LOAD and one executable section per segment (XenonRecomp test mode)."""
    shstr = b"\0.shstrtab\0" + b"".join(b".text%d\0" % i for i in range(len(segments)))
    ehsize, phsize, shsize = 52, 32, 40
    data_off = ehsize + phsize
    blobs = b""
    sec = []
    for i, (addr, blob) in enumerate(segments):
        sec.append((addr, data_off + len(blobs), len(blob)))
        blobs += blob + bytes((-len(blob)) & 3)
    shstr_off = data_off + len(blobs)
    sh_off = shstr_off + len(shstr) + ((-len(shstr)) & 3)
    nsec = 2 + len(segments)
    base = min(a for a, _ in segments)
    end = max(a + len(b) for a, b in segments)
    eh = struct.pack(">16sHHIIIIIHHHHHH", b"\x7fELF\x01\x02\x01" + bytes(9), 2, 20, 1, base, ehsize, sh_off,
                     0, ehsize, phsize, 1, shsize, nsec, 1)
    ph = struct.pack(">IIIIIIII", 1, data_off, base, base, len(blobs), end - base, 5, 4)
    shdrs = bytes(shsize)
    shdrs += struct.pack(">IIIIIIIIII", 1, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)
    name_off = len(b"\0.shstrtab\0")
    for i, (addr, off, size) in enumerate(sec):
        shdrs += struct.pack(">IIIIIIIIII", name_off, 1, 6, addr, off, size, 0, 0, 4, 0)
        name_off += len(b".text%d\0" % i)
    pad = bytes((-len(shstr)) & 3)
    open(path, "wb").write(eh + ph + blobs + shstr + pad + shdrs)


def main():
    args = sys.argv[1:]
    forms_path, sdk_dir, out_dir = args[0:3]

    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default

    seed = int(opt("--seed", "20261112"))
    per_variant = int(opt("--cases", "40"))
    os.makedirs(out_dir, exist_ok=True)
    catalog = json.load(open(forms_path))
    g = Gen(seed, per_variant)
    g.hw = load_hw(sdk_dir)
    g.catalog = catalog["forms"]
    for form in sorted(catalog["forms"]):
        g.form_cases(form, catalog["forms"][form])
    g.special_cases()
    image_path = None
    if opt("--leaf") and opt("--image") and os.path.exists(opt("--image")):
        import tomllib
        cfg = tomllib.load(open(opt("--leaf"), "rb"))
        image_path = os.path.abspath(opt("--image"))
        image = open(image_path, "rb").read()
        leaf_cases(g, cfg.get("leaf", []), image, int(opt("--leaf-cases", "32")))
        chkstk_cases(g, cfg.get("chkstk", []), image)

    write_code(g, out_dir, "sem")
    write_case_file(os.path.join(out_dir, "cases.bin"), g.cases)
    write_expected(os.path.join(out_dir, "expected.bin"), g.cases)
    meta = {"cases": [[c.form, c.origin, c.group, c.note, {str(k): v for k, v in c.masks.items()},
                       [list(o) for o in c.outs], [list(m) for m in c.mem_out], c.addr, c.flags,
                       [[k, i, w] for k, i, w in c.ins]] for c in g.cases],
            "forms": {f: info["count"] for f, info in catalog["forms"].items()},
            "game": {f: info["game"] for f, info in catalog["forms"].items()},
            "skipped": g.skipped, "ref_unsupported": g.ref_unsupported,
            "functions": len(g.funcs) + len(g.leaf_funcs), "leaf_functions": len(g.leaf_funcs),
            "image": image_path}
    json.dump(meta, open(os.path.join(out_dir, "meta.json"), "w"))
    origins = {}
    for c in g.cases:
        origins[c.origin] = origins.get(c.origin, 0) + 1
    print("gen.py: %d functions, %d cases (%s), %d forms skipped"
          % (len(g.funcs), len(g.cases), ", ".join("%s %d" % kv for kv in sorted(origins.items())),
             len(g.skipped)))


if __name__ == "__main__":
    main()
