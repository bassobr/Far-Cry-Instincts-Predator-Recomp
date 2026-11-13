"""Reference interpreter for the semantic test: a deliberately simple PowerPC (Xenon) model.

Independent of both recompilers: decoding uses Xenia's opcode table (xenia_lookup.py, BSD 3-Clause), the
semantics are written from the documents named per rule below. Rule sources:
  [ISA]   Power ISA 2.0x Book I (fixed point, branch, floating point chapters), PowerPC 64-bit mode,
  [PEM]   AltiVec Technology Programming Environments Manual (VMX), incl. Java/non-Java mode,
  [XEN]   Xenia (xenia-canary, BSD 3-Clause) for the Xenon specific VMX128 forms and choices without a
          document (vpermwi128, vrlimi128, vpkd3d128/vupkd3d128 field layout, lvlx/lvrx/stvlx/stvrx,
          dcbz128, 32 bit CR0 compare of record forms),
  [SDK]   test vectors shipped with the SDK (tests/ppc/asm, tests/unit/ppc/*_cases.h; upstream calls the
          latter "hardware tested"), used where the documents leave the result open (estimates, D3D packs,
          fctiw high word).
Unsupported forms raise Unsupported; the generator then only runs the differential comparison.

Floating point is exact: values are converted to fractions, combined exactly and rounded once.
"""
from fractions import Fraction
import math

import xenia_lookup


class Unsupported(Exception):
    pass


M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def X(w, a, b):  # bit field in IBM numbering, as Xenia's ExtractBits
    return (w >> (31 - b)) & ((1 << (b - a + 1)) - 1)


# ----------------------------------------------------------------------------------------------------
# IEEE formats
# ----------------------------------------------------------------------------------------------------
class Fmt:
    def __init__(self, ebits, mbits):
        self.e, self.m = ebits, mbits
        self.bias = (1 << (ebits - 1)) - 1
        self.emax = self.bias
        self.emin = 1 - self.bias
        self.prec = mbits + 1
        self.width = 1 + ebits + mbits
        self.sign = 1 << (self.width - 1)
        self.expmask = ((1 << ebits) - 1) << mbits
        self.manmask = (1 << mbits) - 1
        self.qbit = 1 << (mbits - 1)

    def isnan(self, b):
        return (b & self.expmask) == self.expmask and (b & self.manmask) != 0

    def issnan(self, b):
        return self.isnan(b) and not (b & self.qbit)

    def isinf(self, b):
        return (b & ~self.sign) == self.expmask

    def iszero(self, b):
        return (b & ~self.sign) == 0

    def isdenorm(self, b):
        return (b & self.expmask) == 0 and (b & self.manmask) != 0

    def neg(self, b):
        return bool(b & self.sign)

    def inf(self, s):
        return (self.sign if s else 0) | self.expmask

    def value(self, b):
        """Finite value as Fraction (sign of zero lost)."""
        e = (b & self.expmask) >> self.m
        m = b & self.manmask
        if e == 0:
            v = Fraction(m, 1 << (self.m - self.emin))
        else:
            v = Fraction((1 << self.m) | m) * Fraction(2) ** (e - self.bias - self.m)
        return -v if self.neg(b) else v

    def round(self, x, rn, sign_if_zero=0):
        """Rounds the exact value x once [ISA 4.3.6]. rn: 0 nearest even, 1 zero, 2 +inf, 3 -inf."""
        if x == 0:
            return self.sign if sign_if_zero else 0
        s = 1 if x < 0 else 0
        a = -x if s else x
        e = a.numerator.bit_length() - a.denominator.bit_length()
        if Fraction(2) ** e > a:
            e -= 1
        q = max(e, self.emin) - self.m          # exponent of the unit in the last place
        scaled = a / (Fraction(2) ** q)
        n = scaled.numerator // scaled.denominator
        r = scaled - n
        up = False
        if r:
            if rn == 0:
                up = r > Fraction(1, 2) or (r == Fraction(1, 2) and (n & 1))
            elif rn == 2:
                up = not s
            elif rn == 3:
                up = bool(s)
        if up:
            n += 1
        if n >> self.prec:                       # carried into the next binade
            n >>= 1
            q += 1
        if n == 0:
            return self.sign if s else 0
        if n.bit_length() < self.prec:           # subnormal
            biased = 0
        else:
            biased = q + self.m + self.bias
        if biased >= (1 << self.e) - 1:          # overflow
            if rn == 0 or (rn == 2 and not s) or (rn == 3 and s):
                return self.inf(s)
            return (self.sign if s else 0) | (self.expmask - (1 << self.m)) | self.manmask
        return (self.sign if s else 0) | (biased << self.m) | (n & self.manmask)


D = Fmt(11, 52)
S = Fmt(8, 23)
DNAN = 0x7FF8000000000000          # default QNaN of the FPU [ISA 4.3.2]
SNAN32 = 0x7FC00000                # default QNaN of VMX [PEM]


def s2d(b):
    """Single bits to double bits, exact (lfs, VMX lanes as doubles) [ISA lfs: no quieting]."""
    sgn = (b >> 31) << 63
    e = (b >> 23) & 0xFF
    m = b & 0x7FFFFF
    if e == 0xFF:
        return sgn | 0x7FF0000000000000 | (m << 29)
    if e == 0:
        if m == 0:
            return sgn
        return D.round(S.value(b), 0)
    return sgn | ((e - 127 + 1023) << 52) | (m << 29)


def d2s_store(b):
    """stfs conversion [ISA 4.6.3 'Floating-Point Store Single']: no rounding, bits are taken."""
    e = (b >> 52) & 0x7FF
    if e > 896 or (b & ~(1 << 63)) == 0 or e == 0x7FF:
        return ((b >> 32) & 0xC0000000) | ((b >> 29) & 0x3FFFFFFF)
    if 874 <= e <= 896:  # denormalise: shift the implicit one in, truncate
        sign = (b >> 63) << 31
        frac = (1 << 52) | (b & ((1 << 52) - 1))
        shift = 897 - e
        frac >>= shift
        return sign | ((frac >> 29) & 0x7FFFFF)
    raise Unsupported("stfs of a double below the single range: undefined by the ISA")


def quiet_d(b):
    return b | (1 << 51)


def frsp_bits(b, rn):
    """Round a double register value to single precision, result as double bits [ISA frsp]."""
    if D.isnan(b):
        return quiet_d(b) & ~((1 << 29) - 1)
    if D.isinf(b) or D.iszero(b):
        return b
    return s2d(S.round(D.value(b), rn))


# ----------------------------------------------------------------------------------------------------
# Machine state
# ----------------------------------------------------------------------------------------------------
class State:
    def __init__(self):
        self.r = [0] * 32
        self.f = [0] * 32
        self.v = [[0, 0, 0, 0] for _ in range(128)]
        self.cr = 0
        self.so = self.ov = self.ca = 0
        self.ctr = 0
        self.lr = 0
        self.rn = 0
        self.nj = 1           # VSCR[NJ]; the SDK runtime starts every thread with nj = 1
        self.sat = 0
        self.msr = 0x200A000
        self.reserve = None
        self.mem = {}         # address -> byte (only bytes the test set up or wrote)
        self.fpscr_hi = 0     # FPSCR bits other than RN as the guest last wrote them

    # memory
    def rb(self, a):
        a &= M32
        if a not in self.mem:
            raise Unsupported("read outside the test buffer at %08X" % a)
        return self.mem[a]

    def ld(self, a, n):
        v = 0
        for i in range(n):
            v = (v << 8) | self.rb(a + i)
        return v

    def st(self, a, n, v):
        for i in range(n):
            ad = (a + i) & M32
            if ad not in self.mem:
                raise Unsupported("write outside the test buffer at %08X" % ad)
            self.mem[ad] = (v >> (8 * (n - 1 - i))) & 0xFF

    # condition register
    def crf(self, n):
        return (self.cr >> (28 - 4 * n)) & 0xF

    def setcrf(self, n, v):
        sh = 28 - 4 * n
        self.cr = (self.cr & ~(0xF << sh)) | ((v & 0xF) << sh)

    def crbit(self, b):
        return (self.cr >> (31 - b)) & 1

    def setcrbit(self, b, v):
        self.cr = (self.cr & ~(1 << (31 - b))) | ((v & 1) << (31 - b))

    def cmp_field(self, n, a, b, signed_):
        if a < b:
            c = 8
        elif a > b:
            c = 4
        else:
            c = 2
        self.setcrf(n, c | self.so)

    def rc0(self, v):
        # [XEN] record forms compare the low 32 bits (Xenia PPCHIRBuilder::UpdateCR truncates);
        # the 64 bit mode text of the ISA would compare all 64 bits, see the report.
        self.cmp_field(0, sext(v, 32), 0, True)

    @property
    def xer(self):
        return (self.so << 31) | (self.ov << 30) | (self.ca << 29)

    @xer.setter
    def xer(self, v):
        self.so, self.ov, self.ca = (v >> 31) & 1, (v >> 30) & 1, (v >> 29) & 1


# ----------------------------------------------------------------------------------------------------
# Integer helpers
# ----------------------------------------------------------------------------------------------------
def rotl32(x, n):
    x &= M32
    n &= 31
    return ((x << n) | (x >> (32 - n))) & M32 if n else x


def rotl64(x, n):
    x &= M64
    n &= 63
    return ((x << n) | (x >> (64 - n))) & M64 if n else x


def mask64(mb, me):
    """MASK(mb, me) in IBM numbering, wraps when mb > me [ISA 3.3.13]."""
    def ones(a, b):
        return ((1 << (64 - a)) - 1) ^ ((1 << (63 - b)) - 1)
    if mb <= me:
        return ones(mb, me)
    return (ones(0, me) | ones(mb, 63)) & M64


def rot32dup(x, n):
    r = rotl32(x, n)
    return (r << 32) | r


# ----------------------------------------------------------------------------------------------------
# Instruction semantics
# ----------------------------------------------------------------------------------------------------
H = {}


def op(*names):
    def deco(fn):
        for n in names:
            H[n] = fn
        return fn
    return deco


def RT(w): return X(w, 6, 10)
def RA(w): return X(w, 11, 15)
def RB(w): return X(w, 16, 20)
def RCF(w): return X(w, 21, 25)
def RCB(w): return w & 1
def OE(w): return X(w, 21, 21)
def SI(w): return sext(w & 0xFFFF, 16)
def UI(w): return w & 0xFFFF


def ra0(s, w):
    return s.r[RA(w)] if RA(w) else 0


def set_ov(s, ov):
    s.ov = ov
    if ov:
        s.so = 1


def finish_xo(s, w, res, ov=None):
    s.r[RT(w)] = res & M64
    if OE(w) and ov is not None:
        set_ov(s, ov)
    if RCB(w):
        s.rc0(res)


def ov32_add(a, b, r):
    return int(((a ^ r) & (b ^ r) & 0x80000000) != 0)


# --- add/sub [ISA 3.3.8]. Xenon titles run with MSR[SF] = 0 (SDK runtime MSR 0x200A000, as Xenia):
# 32 bit mode, so CA is the carry out of bit 32, OV the 32 bit overflow and CR0 a 32 bit compare, while the
# registers keep their full 64 bit results.
def c32(*xs):
    return int(sum(x & M32 for x in xs) >> 32)


def ov32(a, b, r):
    return int(((a ^ r) & (b ^ r) & 0x80000000) != 0)


@op("addi")
def _addi(s, w):
    s.r[RT(w)] = (ra0(s, w) + SI(w)) & M64


@op("addis")
def _addis(s, w):
    s.r[RT(w)] = (ra0(s, w) + (SI(w) << 16)) & M64


@op("addicx")
def _addic(s, w):
    a = s.r[RA(w)]
    res = (a + SI(w)) & M64
    s.ca = c32(a, SI(w))
    s.r[RT(w)] = res
    if X(w, 5, 5):  # addic. (primary opcode 13)
        s.rc0(res)


@op("subficx")
def _subfic(s, w):
    na = (~s.r[RA(w)]) & M64
    s.r[RT(w)] = (na + SI(w) + 1) & M64
    s.ca = c32(na, SI(w), 1)


def _addlike(s, w, a, b, cin, set_ca):
    r = (a + b + cin) & M64
    if set_ca:
        s.ca = c32(a, b, cin)
    finish_xo(s, w, r, ov32(a, b, r))


@op("addx")
def _add(s, w): _addlike(s, w, s.r[RA(w)], s.r[RB(w)], 0, False)
@op("addcx")
def _addc(s, w): _addlike(s, w, s.r[RA(w)], s.r[RB(w)], 0, True)
@op("addex")
def _adde(s, w): _addlike(s, w, s.r[RA(w)], s.r[RB(w)], s.ca, True)
@op("addzex")
def _addze(s, w): _addlike(s, w, s.r[RA(w)], 0, s.ca, True)
@op("addmex")
def _addme(s, w): _addlike(s, w, s.r[RA(w)], M64, s.ca, True)
@op("subfx")
def _subf(s, w): _addlike(s, w, (~s.r[RA(w)]) & M64, s.r[RB(w)], 1, False)
@op("subfcx")
def _subfc(s, w): _addlike(s, w, (~s.r[RA(w)]) & M64, s.r[RB(w)], 1, True)
@op("subfex")
def _subfe(s, w): _addlike(s, w, (~s.r[RA(w)]) & M64, s.r[RB(w)], s.ca, True)
@op("subfzex")
def _subfze(s, w): _addlike(s, w, (~s.r[RA(w)]) & M64, 0, s.ca, True)
@op("subfmex")
def _subfme(s, w): _addlike(s, w, (~s.r[RA(w)]) & M64, M64, s.ca, True)


@op("negx")
def _neg(s, w):
    a = s.r[RA(w)]
    finish_xo(s, w, (-a) & M64, int((a & M32) == 0x80000000))


# --- multiply/divide [ISA 3.3.8.1]
@op("mulli")
def _mulli(s, w):
    s.r[RT(w)] = (sext(s.r[RA(w)], 64) * SI(w)) & M64


@op("mullwx")
def _mullw(s, w):
    p = sext(s.r[RA(w)], 32) * sext(s.r[RB(w)], 32)
    finish_xo(s, w, p & M64, int(not (-(1 << 31) <= p < (1 << 31))))


@op("mulhwx")
def _mulhw(s, w):
    p = sext(s.r[RA(w)], 32) * sext(s.r[RB(w)], 32)
    # high word; the upper 32 bits of RT are undefined [ISA]; Xenia sign extends
    finish_xo(s, w, sext(p >> 32, 32) & M64)


@op("mulhwux")
def _mulhwu(s, w):
    p = (s.r[RA(w)] & M32) * (s.r[RB(w)] & M32)
    finish_xo(s, w, (p >> 32) & M32)


@op("mulldx")
def _mulld(s, w):
    p = sext(s.r[RA(w)], 64) * sext(s.r[RB(w)], 64)
    finish_xo(s, w, p & M64, int(not (-(1 << 63) <= p < (1 << 63))))


@op("mulhdx")
def _mulhd(s, w):
    p = sext(s.r[RA(w)], 64) * sext(s.r[RB(w)], 64)
    finish_xo(s, w, (p >> 64) & M64)


@op("mulhdux")
def _mulhdu(s, w):
    finish_xo(s, w, ((s.r[RA(w)] * s.r[RB(w)]) >> 64) & M64)


def _trunc_div(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


@op("divwx")
def _divw(s, w):
    a, b = sext(s.r[RA(w)], 32), sext(s.r[RB(w)], 32)
    if b == 0 or (a == -(1 << 31) and b == -1):
        # undefined [ISA]; SDK and Xenia give 0 (instr_divw.s)
        finish_xo(s, w, 0, 1)
        return
    finish_xo(s, w, _trunc_div(a, b) & M32, 0)


@op("divwux")
def _divwu(s, w):
    a, b = s.r[RA(w)] & M32, s.r[RB(w)] & M32
    if b == 0:
        finish_xo(s, w, 0, 1)
        return
    finish_xo(s, w, a // b, 0)


@op("divdx")
def _divd(s, w):
    a, b = sext(s.r[RA(w)], 64), sext(s.r[RB(w)], 64)
    if b == 0 or (a == -(1 << 63) and b == -1):
        finish_xo(s, w, 0, 1)
        return
    finish_xo(s, w, _trunc_div(a, b) & M64, 0)


@op("divdux")
def _divdu(s, w):
    a, b = s.r[RA(w)], s.r[RB(w)]
    if b == 0:
        finish_xo(s, w, 0, 1)
        return
    finish_xo(s, w, a // b, 0)


# --- logical [ISA 3.3.11]
def logical(s, w, val):
    s.r[RA(w)] = val & M64
    if RCB(w):
        s.rc0(val)


@op("andx")
def _and(s, w): logical(s, w, s.r[RT(w)] & s.r[RB(w)])
@op("andcx")
def _andc(s, w): logical(s, w, s.r[RT(w)] & ~s.r[RB(w)])
@op("orx")
def _or(s, w): logical(s, w, s.r[RT(w)] | s.r[RB(w)])
@op("orcx")
def _orc(s, w): logical(s, w, s.r[RT(w)] | (~s.r[RB(w)] & M64))
@op("xorx")
def _xor(s, w): logical(s, w, s.r[RT(w)] ^ s.r[RB(w)])
@op("norx")
def _nor(s, w): logical(s, w, ~(s.r[RT(w)] | s.r[RB(w)]))
@op("nandx")
def _nand(s, w): logical(s, w, ~(s.r[RT(w)] & s.r[RB(w)]))
@op("eqvx")
def _eqv(s, w): logical(s, w, ~(s.r[RT(w)] ^ s.r[RB(w)]))


@op("ori")
def _ori(s, w): s.r[RA(w)] = s.r[RT(w)] | UI(w)
@op("oris")
def _oris(s, w): s.r[RA(w)] = s.r[RT(w)] | (UI(w) << 16)
@op("xori")
def _xori(s, w): s.r[RA(w)] = s.r[RT(w)] ^ UI(w)
@op("xoris")
def _xoris(s, w): s.r[RA(w)] = s.r[RT(w)] ^ (UI(w) << 16)


@op("andix")
def _andi(s, w):
    v = s.r[RT(w)] & UI(w)
    s.r[RA(w)] = v
    s.rc0(v)


@op("andisx")
def _andis(s, w):
    v = s.r[RT(w)] & (UI(w) << 16)
    s.r[RA(w)] = v
    s.rc0(v)


@op("extsbx")
def _extsb(s, w): logical(s, w, sext(s.r[RT(w)], 8))
@op("extshx")
def _extsh(s, w): logical(s, w, sext(s.r[RT(w)], 16))
@op("extswx")
def _extsw(s, w): logical(s, w, sext(s.r[RT(w)], 32))


@op("cntlzwx")
def _cntlzw(s, w):
    x = s.r[RT(w)] & M32
    logical(s, w, 32 - x.bit_length())


@op("cntlzdx")
def _cntlzd(s, w):
    logical(s, w, 64 - s.r[RT(w)].bit_length())


# --- rotate/shift [ISA 3.3.13]
def SH(w): return X(w, 16, 20)
def MB(w): return X(w, 21, 25)
def ME(w): return X(w, 26, 30)


@op("rlwinmx")
def _rlwinm(s, w):
    logical(s, w, rot32dup(s.r[RT(w)], SH(w)) & mask64(MB(w) + 32, ME(w) + 32))


@op("rlwnmx")
def _rlwnm(s, w):
    logical(s, w, rot32dup(s.r[RT(w)], s.r[RB(w)] & 31) & mask64(MB(w) + 32, ME(w) + 32))


@op("rlwimix")
def _rlwimi(s, w):
    m = mask64(MB(w) + 32, ME(w) + 32)
    logical(s, w, (rot32dup(s.r[RT(w)], SH(w)) & m) | (s.r[RA(w)] & ~m))


def md_sh(w): return X(w, 16, 20) | (X(w, 30, 30) << 5)
def md_mb(w): return X(w, 21, 25) | (X(w, 26, 26) << 5)  # mb5 || mb0:4 -> value = mb0:4 + 32*mb5


def md_m(w):
    raw = X(w, 21, 26)
    return ((raw & 1) << 5) | (raw >> 1)


@op("rldiclx")
def _rldicl(s, w):
    logical(s, w, rotl64(s.r[RT(w)], md_sh(w)) & mask64(md_m(w), 63))


@op("rldicrx")
def _rldicr(s, w):
    logical(s, w, rotl64(s.r[RT(w)], md_sh(w)) & mask64(0, md_m(w)))


@op("rldicx")
def _rldic(s, w):
    sh = md_sh(w)
    logical(s, w, rotl64(s.r[RT(w)], sh) & mask64(md_m(w), 63 - sh))


@op("rldimix")
def _rldimi(s, w):
    sh = md_sh(w)
    m = mask64(md_m(w), 63 - sh)
    logical(s, w, (rotl64(s.r[RT(w)], sh) & m) | (s.r[RA(w)] & ~m))


@op("slwx")
def _slw(s, w):
    n = s.r[RB(w)] & 63
    logical(s, w, 0 if n > 31 else (s.r[RT(w)] << n) & M32)


@op("srwx")
def _srw(s, w):
    n = s.r[RB(w)] & 63
    logical(s, w, 0 if n > 31 else (s.r[RT(w)] & M32) >> n)


@op("sldx")
def _sld(s, w):
    n = s.r[RB(w)] & 127
    logical(s, w, 0 if n > 63 else (s.r[RT(w)] << n) & M64)


@op("srdx")
def _srd(s, w):
    n = s.r[RB(w)] & 127
    logical(s, w, 0 if n > 63 else s.r[RT(w)] >> n)


def _sra(s, w, x, n, width):
    v = sext(x, width)
    r = v >> n if n < width else (-1 if v < 0 else 0)
    lost = v - (r << n if n < width else r * 0) if n < width else (0 if v >= 0 else 1)
    s.ca = int(v < 0 and (n >= width and v != 0 or (n < width and (v & ((1 << n) - 1)) != 0)))
    logical(s, w, r & M64)


@op("srawx")
def _sraw(s, w):
    n = s.r[RB(w)] & 63
    _sra(s, w, s.r[RT(w)], min(n, 32), 32)


@op("srawix")
def _srawi(s, w):
    _sra(s, w, s.r[RT(w)], SH(w), 32)


@op("sradx")
def _srad(s, w):
    n = s.r[RB(w)] & 127
    _sra(s, w, s.r[RT(w)], min(n, 64), 64)


@op("sradix")
def _sradi(s, w):
    _sra(s, w, s.r[RT(w)], md_sh(w), 64)


# --- compare [ISA 3.3.9]
@op("cmp")
def _cmp(s, w):
    L = X(w, 10, 10)
    a, b = s.r[RA(w)], s.r[RB(w)]
    bits = 64 if L else 32
    s.cmp_field(X(w, 6, 8), sext(a, bits), sext(b, bits), True)


@op("cmpl")
def _cmpl(s, w):
    L = X(w, 10, 10)
    m = M64 if L else M32
    s.cmp_field(X(w, 6, 8), s.r[RA(w)] & m, s.r[RB(w)] & m, False)


@op("cmpi")
def _cmpi(s, w):
    L = X(w, 10, 10)
    bits = 64 if L else 32
    s.cmp_field(X(w, 6, 8), sext(s.r[RA(w)], bits), SI(w), True)


@op("cmpli")
def _cmpli(s, w):
    L = X(w, 10, 10)
    m = M64 if L else M32
    s.cmp_field(X(w, 6, 8), s.r[RA(w)] & m, UI(w), False)


# --- condition register logic [ISA 2.4.3]
def _crop(fn):
    def h(s, w):
        a, b = s.crbit(X(w, 11, 15)), s.crbit(X(w, 16, 20))
        s.setcrbit(X(w, 6, 10), fn(a, b) & 1)
    return h


H["crand"] = _crop(lambda a, b: a & b)
H["cror"] = _crop(lambda a, b: a | b)
H["crxor"] = _crop(lambda a, b: a ^ b)
H["crnand"] = _crop(lambda a, b: ~(a & b))
H["crnor"] = _crop(lambda a, b: ~(a | b))
H["creqv"] = _crop(lambda a, b: ~(a ^ b))
H["crandc"] = _crop(lambda a, b: a & ~b)
H["crorc"] = _crop(lambda a, b: a | ~b)


@op("mcrf")
def _mcrf(s, w):
    s.setcrf(X(w, 6, 8), s.crf(X(w, 11, 13)))


@op("mfcr")
def _mfcr(s, w):
    if X(w, 11, 11):  # mfocrf: only the selected field is defined [ISA]; Xenia copies the full CR
        s.r[RT(w)] = s.cr
    else:
        s.r[RT(w)] = s.cr


@op("mtcrf")
def _mtcrf(s, w):
    fxm = X(w, 12, 19)
    v = s.r[RT(w)] & M32
    for i in range(8):
        if fxm & (0x80 >> i):
            s.setcrf(i, v >> (28 - 4 * i))


# --- special purpose registers
@op("mfspr")
def _mfspr(s, w):
    spr = X(w, 16, 20) << 5 | X(w, 11, 15)
    if spr == 8:
        s.r[RT(w)] = s.lr
    elif spr == 9:
        s.r[RT(w)] = s.ctr
    elif spr == 1:
        s.r[RT(w)] = s.xer
    else:
        raise Unsupported("mfspr %d" % spr)


@op("mtspr")
def _mtspr(s, w):
    spr = X(w, 16, 20) << 5 | X(w, 11, 15)
    v = s.r[RT(w)]
    if spr == 8:
        s.lr = v
    elif spr == 9:
        s.ctr = v
    elif spr == 1:
        s.xer = v & 0xE0000000
    else:
        raise Unsupported("mtspr %d" % spr)


@op("mfmsr")
def _mfmsr(s, w):
    s.r[RT(w)] = s.msr


@op("mtmsrd")
def _mtmsrd(s, w):
    # L=1: only EE and RI change [ISA Book III]
    if X(w, 15, 15):
        s.msr = (s.msr & ~0x8002) | (s.r[RT(w)] & 0x8002)
    else:
        raise Unsupported("mtmsrd L=0")


# --- no-ops for the test (cache hints, barriers) [ISA Book II]
for _n in ("dcbt", "dcbtst", "dcbf", "dcbst", "sync", "eieio", "isync", "icbi"):
    H[_n] = lambda s, w: None


@op("dcbz", "dcbz128")
def _dcbz(s, w):
    n = 128 if xenia_lookup.lookup(w) == "dcbz128" else 32   # [XEN] dcbz128 clears 128 bytes
    ea = (ra0(s, w) + s.r[RB(w)]) & M32 & ~(n - 1)
    for i in range(n):
        s.st(ea + i, 1, 0)


@op("twi", "tdi")
def _trap_imm(s, w):
    to = RT(w)
    a = s.r[RA(w)]
    bits = 64 if xenia_lookup.lookup(w) == "tdi" else 32
    sa, ua = sext(a, bits), a & ((1 << bits) - 1)
    b = SI(w)
    ub = b & ((1 << bits) - 1)
    hit = ((to & 16) and sa < b) or ((to & 8) and sa > b) or ((to & 4) and sa == b) or \
          ((to & 2) and ua < ub) or ((to & 1) and ua > ub)
    if hit:
        raise Unsupported("trap taken")


# --- loads/stores [ISA 3.3.2/3.3.3]
def ea_d(s, w): return (ra0(s, w) + SI(w)) & M64
def ea_ds(s, w): return (ra0(s, w) + sext(w & 0xFFFC, 16)) & M64
def ea_x(s, w): return (ra0(s, w) + s.r[RB(w)]) & M64


def _load(n, signed_=False, mode="d", update=False, rev=False):
    def h(s, w):
        ea = {"d": ea_d, "ds": ea_ds, "x": ea_x}[mode](s, w)
        v = s.ld(ea, n)
        if rev:
            v = int.from_bytes(v.to_bytes(n, "big"), "little")
        if signed_:
            v = sext(v, 8 * n) & M64
        s.r[RT(w)] = v
        if update:
            s.r[RA(w)] = ea
    return h


def _store(n, mode="d", update=False, rev=False):
    def h(s, w):
        ea = {"d": ea_d, "ds": ea_ds, "x": ea_x}[mode](s, w)
        v = s.r[RT(w)] & ((1 << (8 * n)) - 1)
        if rev:
            v = int.from_bytes(v.to_bytes(n, "big"), "little")
        s.st(ea, n, v)
        if update:
            s.r[RA(w)] = ea
    return h


H.update({
    "lbz": _load(1), "lbzu": _load(1, update=True), "lbzx": _load(1, mode="x"),
    "lhz": _load(2), "lhzx": _load(2, mode="x"), "lha": _load(2, True), "lhax": _load(2, True, "x"),
    "lwz": _load(4), "lwzu": _load(4, update=True), "lwzx": _load(4, mode="x"),
    "lwa": _load(4, True, "ds"), "lwax": _load(4, True, "x"),
    "ld": _load(8, mode="ds"), "ldu": _load(8, mode="ds", update=True), "ldx": _load(8, mode="x"),
    "lwbrx": _load(4, mode="x", rev=True), "lhbrx": _load(2, mode="x", rev=True),
    "stb": _store(1), "stbu": _store(1, update=True), "stbx": _store(1, "x"),
    "sth": _store(2), "sthx": _store(2, "x"), "sthbrx": _store(2, "x", rev=True),
    "stw": _store(4), "stwu": _store(4, update=True), "stwx": _store(4, "x"),
    "stwux": _store(4, "x", update=True), "stwbrx": _store(4, "x", rev=True),
    "std": _store(8, "ds"), "stdu": _store(8, "ds", update=True), "stdx": _store(8, "x"),
})


@op("lwarx", "ldarx")
def _larx(s, w):
    n = 8 if xenia_lookup.lookup(w) == "ldarx" else 4
    ea = ea_x(s, w)
    v = s.ld(ea, n)
    s.reserve = (ea & M32, n)
    s.r[RT(w)] = v


@op("stwcx", "stdcx")
def _stcx(s, w):
    n = 8 if xenia_lookup.lookup(w) == "stdcx" else 4
    ea = ea_x(s, w) & M32
    ok = s.reserve is not None and s.reserve[0] == ea
    if ok:
        s.st(ea, n, s.r[RT(w)] & ((1 << (8 * n)) - 1))
    s.reserve = None
    s.setcrf(0, (2 if ok else 0) | s.so)


# floating point loads/stores
@op("lfs", "lfsx")
def _lfs(s, w):
    ea = ea_x(s, w) if xenia_lookup.lookup(w) == "lfsx" else ea_d(s, w)
    s.f[RT(w)] = s2d(s.ld(ea, 4))


@op("lfd", "lfdx")
def _lfd(s, w):
    ea = ea_x(s, w) if xenia_lookup.lookup(w) == "lfdx" else ea_d(s, w)
    s.f[RT(w)] = s.ld(ea, 8)


@op("stfs", "stfsx")
def _stfs(s, w):
    ea = ea_x(s, w) if xenia_lookup.lookup(w) == "stfsx" else ea_d(s, w)
    s.st(ea, 4, d2s_store(s.f[RT(w)]))


@op("stfd", "stfdx")
def _stfd(s, w):
    ea = ea_x(s, w) if xenia_lookup.lookup(w) == "stfdx" else ea_d(s, w)
    s.st(ea, 8, s.f[RT(w)])


@op("stfiwx")
def _stfiwx(s, w):
    s.st(ea_x(s, w), 4, s.f[RT(w)] & M32)


# --- branches [ISA 2.4]
def bo_ok(s, bo, bi):
    ctr_ok = True
    if not (bo & 4):
        s.ctr = (s.ctr - 1) & M64
        ctr_ok = ((s.ctr & M32) != 0) != bool(bo & 2)   # 32 bit mode tests CTR[32:63] [ISA 2.4]
    cond_ok = bool(bo & 16) or (s.crbit(bi) == ((bo >> 3) & 1))
    return ctr_ok and cond_ok


# Branch handlers return the next pc, or None for "fall through", or "ret" for a return.
@op("bx")
def _b(s, w, pc):
    li = sext(w & 0x03FFFFFC, 26)
    tgt = li if w & 2 else (pc + li) & M32
    if w & 1:
        s.lr = pc + 4
    return tgt


@op("bcx")
def _bc(s, w, pc):
    bo, bi = RT(w), RA(w)
    taken = bo_ok(s, bo, bi)
    if w & 1:
        s.lr = pc + 4
    if not taken:
        return None
    bd = sext(w & 0xFFFC, 16)
    return bd if w & 2 else (pc + bd) & M32


@op("bclrx")
def _bclr(s, w, pc):
    bo, bi = RT(w), RA(w)
    taken = bo_ok(s, bo, bi)
    tgt = s.lr & ~3
    if w & 1:
        s.lr = pc + 4
    return tgt if taken else None


@op("bcctrx")
def _bcctr(s, w, pc):
    bo, bi = RT(w), RA(w)
    cond_ok = bool(bo & 16) or (s.crbit(bi) == ((bo >> 3) & 1))
    if w & 1:
        s.lr = pc + 4
    return (s.ctr & ~3) & M32 if cond_ok else None


BRANCHES = {"bx", "bcx", "bclrx", "bcctrx"}


# ----------------------------------------------------------------------------------------------------
# Scalar floating point [ISA 4]
# ----------------------------------------------------------------------------------------------------
def FT(w): return X(w, 6, 10)
def FA(w): return X(w, 11, 15)
def FB(w): return X(w, 16, 20)
def FC(w): return X(w, 21, 25)


def fp_result(s, w, bits, single):
    s.f[FT(w)] = bits
    if RCB(w):
        raise Unsupported("FP record form (CR1 from FPSCR) not modelled")


def nan_pick(*ops):
    for b in ops:
        if D.isnan(b):
            return quiet_d(b)
    return None


def fp_arith(kind, single):
    def h(s, w):
        a, b, c = s.f[FA(w)], s.f[FB(w)], s.f[FC(w)]
        rn = s.rn
        if kind in ("add", "sub"):
            n = nan_pick(a, b)
            ops = (a, b)
        elif kind == "mul":
            n = nan_pick(a, c)
            ops = (a, c)
        elif kind == "div":
            n = nan_pick(a, b)
            ops = (a, b)
        elif kind == "sqrt":
            n = nan_pick(b)
            ops = (b,)
        else:  # fused: frA*frC +/- frB, NaN priority A, B, C [ISA 4.3.2 / 4.6.6]
            n = nan_pick(a, b, c)
            ops = (a, c, b)
        neg_res = kind in ("nmadd", "nmsub")
        if n is not None:
            # QNaNs propagate without a sign change, also for fnmadd/fnmsub [ISA fnmadd]
            r = n
            if single:
                r = frsp_bits(r, rn)
            fp_result(s, w, r, single)
            return
        r = fp_special(kind, ops, rn)
        if r is None:
            x = fp_exact(kind, ops)
            fmt = S if single else D
            zsign = 0
            if x == 0:
                zsign = fp_zero_sign(kind, ops, rn)
            bits = fmt.round(x, rn, zsign)
            if single:
                bits = s2d(bits)
            r = bits
            if neg_res:
                r ^= 1 << 63
        else:
            if neg_res and r != DNAN:
                r ^= 1 << 63
            if single and r != DNAN:
                r = frsp_bits(r, rn)
        fp_result(s, w, r, single)
    return h


def fp_special(kind, ops, rn):
    """Infinities and invalid operations; returns bits or None for the finite path."""
    inf = [D.isinf(b) for b in ops]
    zero = [D.iszero(b) for b in ops]
    sg = [D.neg(b) for b in ops]
    if kind in ("add", "sub"):
        a, b = ops
        sb = sg[1] ^ (kind == "sub")
        if inf[0] and inf[1]:
            return DNAN if sg[0] != sb else D.inf(sg[0])
        if inf[0]:
            return a
        if inf[1]:
            return D.inf(sb)
        return None
    if kind == "mul":
        if (inf[0] and zero[1]) or (inf[1] and zero[0]):
            return DNAN
        if inf[0] or inf[1]:
            return D.inf(sg[0] ^ sg[1])
        return None
    if kind == "div":
        if (inf[0] and inf[1]) or (zero[0] and zero[1]):
            return DNAN
        if inf[0]:
            return D.inf(sg[0] ^ sg[1])
        if inf[1]:
            return (1 << 63) if sg[0] ^ sg[1] else 0
        if zero[1]:
            return D.inf(sg[0] ^ sg[1])
        return None
    if kind == "sqrt":
        b = ops[0]
        if zero[0]:
            return b
        if sg[0]:
            return DNAN
        if inf[0]:
            return b
        return None
    # fused a*c + b (msub: a*c - b)
    a, c, b = ops
    sb = sg[2] ^ (kind in ("msub", "nmsub"))
    if (inf[0] and zero[1]) or (inf[1] and zero[0]):
        return DNAN
    pinf = inf[0] or inf[1]
    psign = sg[0] ^ sg[1]
    if pinf and inf[2]:
        return DNAN if psign != sb else D.inf(psign)
    if pinf:
        return D.inf(psign)
    if inf[2]:
        return D.inf(sb)
    return None


def fp_exact(kind, ops):
    v = [D.value(b) for b in ops]
    if kind == "add":
        return v[0] + v[1]
    if kind == "sub":
        return v[0] - v[1]
    if kind == "mul":
        return v[0] * v[1]
    if kind == "div":
        return v[0] / v[1]
    if kind == "sqrt":
        return SqrtExact(v[0])
    p = v[0] * v[1]
    return p + v[2] if kind in ("madd", "nmadd") else p - v[2]


class SqrtExact(Fraction):
    """sqrt as a fraction close enough that one rounding is exact: 200 extra bits and a sticky bit."""
    def __new__(cls, x):
        n, d = x.numerator, x.denominator
        k = 200
        q = math.isqrt((n * d) << (2 * k))
        val = Fraction(q, d << k)
        if val * val != x:
            val += Fraction(1, (d << (k + 2)))
        return Fraction.__new__(cls, val)


def fp_zero_sign(kind, ops, rn):
    sg = [D.neg(b) for b in ops]
    if kind == "mul":
        return sg[0] ^ sg[1]
    if kind == "div":
        return sg[0] ^ sg[1]
    if kind == "sqrt":
        return sg[0]
    if kind in ("add", "sub"):
        sa, sb = sg[0], sg[1] ^ (kind == "sub")
        if D.iszero(ops[0]) and D.iszero(ops[1]) and sa == sb:
            return sa
        return 1 if rn == 3 else 0
    # fused: sign of an exact zero sum
    a, c, b = ops
    ps = sg[0] ^ sg[1]
    sb = sg[2] ^ (kind in ("msub", "nmsub"))
    if (D.iszero(a) or D.iszero(c)) and D.iszero(b) and ps == sb:
        return ps
    return 1 if rn == 3 else 0


for _nm, _k, _s in [("faddx", "add", 0), ("faddsx", "add", 1), ("fsubx", "sub", 0), ("fsubsx", "sub", 1),
                    ("fmulx", "mul", 0), ("fmulsx", "mul", 1), ("fdivx", "div", 0), ("fdivsx", "div", 1),
                    ("fsqrtx", "sqrt", 0), ("fsqrtsx", "sqrt", 1),
                    ("fmaddx", "madd", 0), ("fmaddsx", "madd", 1), ("fmsubx", "msub", 0),
                    ("fmsubsx", "msub", 1), ("fnmaddx", "nmadd", 0), ("fnmaddsx", "nmadd", 1),
                    ("fnmsubx", "nmsub", 0), ("fnmsubsx", "nmsub", 1)]:
    H[_nm] = fp_arith(_k, _s)


@op("fmrx")
def _fmr(s, w): fp_result(s, w, s.f[FB(w)], 0)
@op("fnegx")
def _fneg(s, w): fp_result(s, w, s.f[FB(w)] ^ (1 << 63), 0)
@op("fabsx")
def _fabs(s, w): fp_result(s, w, s.f[FB(w)] & ~(1 << 63), 0)
@op("fnabsx")
def _fnabs(s, w): fp_result(s, w, s.f[FB(w)] | (1 << 63), 0)


@op("fselx")
def _fsel(s, w):
    a = s.f[FA(w)]
    ge = (not D.isnan(a)) and (D.iszero(a) or not D.neg(a))
    fp_result(s, w, s.f[FC(w)] if ge else s.f[FB(w)], 0)


@op("frspx")
def _frsp(s, w):
    fp_result(s, w, frsp_bits(s.f[FB(w)], s.rn), 1)


def _round_int(x, rn):
    """Rounds a Fraction to an integer with the FPSCR mode."""
    fl = x.numerator // x.denominator
    r = x - fl
    if r == 0:
        return fl
    if rn == 1:
        return fl if x > 0 else fl + 1
    if rn == 2:
        return fl + 1
    if rn == 3:
        return fl
    if r > Fraction(1, 2) or (r == Fraction(1, 2) and fl & 1):
        return fl + 1
    return fl


def fcti(width, rz):
    def h(s, w):
        b = s.f[FB(w)]
        lo, hi = -(1 << (width - 1)), (1 << (width - 1)) - 1
        if D.isnan(b):
            v = lo
        elif D.isinf(b):
            v = lo if D.neg(b) else hi
        else:
            v = _round_int(D.value(b), 1 if rz else s.rn)
            v = max(lo, min(hi, v))
        # [SDK] instr_fctixz.s: the 32 bit forms sign extend the word into the high half on Xenon
        fp_result(s, w, v & M64, 0)
    return h


H["fctiwx"] = fcti(32, False)
H["fctiwzx"] = fcti(32, True)
H["fctidx"] = fcti(64, False)
H["fctidzx"] = fcti(64, True)


@op("fcfidx")
def _fcfid(s, w):
    v = sext(s.f[FB(w)], 64)
    fp_result(s, w, D.round(Fraction(v), s.rn), 0)


@op("fcmpu", "fcmpo")
def _fcmp(s, w):
    a, b = s.f[FA(w)], s.f[FB(w)]
    if D.isnan(a) or D.isnan(b):
        c = 1
    else:
        va = D.value(a) if not D.isinf(a) else (-1 if D.neg(a) else 1) * Fraction(10) ** 400
        vb = D.value(b) if not D.isinf(b) else (-1 if D.neg(b) else 1) * Fraction(10) ** 400
        c = 8 if va < vb else 4 if va > vb else 2
    s.setcrf(X(w, 6, 8), c)


@op("mffsx")
def _mffs(s, w):
    # Only RN is modelled; the other bits read as the guest last wrote them [model choice]
    s.f[FT(w)] = 0xFFF8000000000000 & 0 | (s.fpscr_hi & ~3) | s.rn
    raise Unsupported("mffs: FPSCR status bits are not modelled")


@op("mtfsfx")
def _mtfsf(s, w):
    flm = X(w, 7, 14)
    v = s.f[FB(w)] & M32
    if flm & 1:
        s.rn = v & 3
    s.fpscr_hi = v
    if RCB(w):
        raise Unsupported("mtfsf.")


@op("mtfsfix")
def _mtfsfi(s, w):
    if X(w, 6, 8) == 7:
        s.rn = X(w, 16, 19) & 3


@op("frsqrtex", "fresx")
def _fest(s, w):
    raise Unsupported("estimate: hardware vectors only")


# ----------------------------------------------------------------------------------------------------
# VMX / VMX128 [PEM], Xenon forms [XEN]
# ----------------------------------------------------------------------------------------------------
def vx_d(w): return X(w, 6, 10)
def vx_a(w): return X(w, 11, 15)
def vx_b(w): return X(w, 16, 20)
def vx_c(w): return X(w, 21, 25)


def v128_d(w): return ((w >> 21) & 31) | (((w >> 2) & 3) << 5)
def v128_a(w): return ((w >> 16) & 31) | (((w >> 5) & 1) << 5) | (((w >> 10) & 1) << 6)  # VA128h bit 5, VA128H bit 10 [XEN]
def v128_b(w): return ((w >> 11) & 31) | ((w & 3) << 5)


def regs(w, name):
    """(vd, va, vb, vc) for both encodings."""
    if name.endswith("128"):
        vc = (w >> 6) & 7
        return v128_d(w), v128_a(w), v128_b(w), vc
    return vx_d(w), vx_a(w), vx_b(w), vx_c(w)


def to_bytes(v):
    return b"".join(x.to_bytes(4, "big") for x in v)


def from_bytes(b):
    return [int.from_bytes(b[i:i + 4], "big") for i in range(0, 16, 4)]


def lanes(v, size):
    b = to_bytes(v)
    return [int.from_bytes(b[i:i + size], "big") for i in range(0, 16, size)]


def unlanes(ls, size):
    return from_bytes(b"".join((x & ((1 << (8 * size)) - 1)).to_bytes(size, "big") for x in ls))


def vint(fn, size):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, c = regs(w, name)
        la, lb = lanes(s.v[a], size), lanes(s.v[b], size)
        s.v[d] = unlanes([fn(s, x, y) for x, y in zip(la, lb)], size)
    return h


def sat(s, v, lo, hi):
    if v < lo:
        s.sat = 1
        return lo
    if v > hi:
        s.sat = 1
        return hi
    return v


H["vaddubm"] = vint(lambda s, x, y: x + y, 1)
H["vadduwm"] = vint(lambda s, x, y: x + y, 4)
H["vadduws"] = vint(lambda s, x, y: sat(s, x + y, 0, M32), 4)
H["vmaxsw"] = vint(lambda s, x, y: max(sext(x, 32), sext(y, 32)), 4)
H["vavgsb"] = vint(lambda s, x, y: (sext(x, 8) + sext(y, 8) + 1) >> 1, 1)
H["vavgsh"] = vint(lambda s, x, y: (sext(x, 16) + sext(y, 16) + 1) >> 1, 2)
H["vslb"] = vint(lambda s, x, y: x << (y & 7), 1)
H["vslw"] = vint(lambda s, x, y: x << (y & 31), 4)
H["vsrw"] = vint(lambda s, x, y: x >> (y & 31), 4)
H["vsraw"] = vint(lambda s, x, y: sext(x, 32) >> (y & 31), 4)


def vlogic(fn):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, c = regs(w, name)
        s.v[d] = [fn(x, y) & M32 for x, y in zip(s.v[a], s.v[b])]
    return h


H["vand"] = H["vand128"] = vlogic(lambda x, y: x & y)
H["vandc"] = H["vandc128"] = vlogic(lambda x, y: x & ~y)
H["vor"] = H["vor128"] = vlogic(lambda x, y: x | y)
H["vxor"] = H["vxor128"] = vlogic(lambda x, y: x ^ y)
H["vnor"] = H["vnor128"] = vlogic(lambda x, y: ~(x | y))


@op("vsel", "vsel128")
def _vsel(s, w):
    name = xenia_lookup.lookup(w)
    if name == "vsel128":
        d, a, b, _ = regs(w, name)
        c = d  # [XEN] vsel128 vD, vA, vB, vD: the mask is the destination register
    else:
        d, a, b, c = regs(w, name)
    s.v[d] = [(x & ~m) | (y & m) for x, y, m in zip(s.v[a], s.v[b], s.v[c])]


@op("vperm", "vperm128")
def _vperm(s, w):
    name = xenia_lookup.lookup(w)
    d, a, b, c = regs(w, name)
    src = to_bytes(s.v[a]) + to_bytes(s.v[b])
    ctl = to_bytes(s.v[c])
    s.v[d] = from_bytes(bytes(src[x & 31] for x in ctl))


@op("vsldoi", "vsldoi128")
def _vsldoi(s, w):
    name = xenia_lookup.lookup(w)
    d, a, b, _ = regs(w, name)
    sh = X(w, 22, 25) if name == "vsldoi" else (w >> 6) & 15
    src = to_bytes(s.v[a]) + to_bytes(s.v[b])
    s.v[d] = from_bytes(src[sh:sh + 16])


def _merge(size, high):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, _ = regs(w, name)
        la, lb = lanes(s.v[a], size), lanes(s.v[b], size)
        n = len(la) // 2
        src_a, src_b = (la[:n], lb[:n]) if high else (la[n:], lb[n:])
        out = []
        for x, y in zip(src_a, src_b):
            out += [x, y]
        s.v[d] = unlanes(out, size)
    return h


H["vmrghb"], H["vmrghh"], H["vmrghw"] = _merge(1, True), _merge(2, True), _merge(4, True)
H["vmrglb"], H["vmrglh"], H["vmrglw"] = _merge(1, False), _merge(2, False), _merge(4, False)
H["vmrghw128"], H["vmrglw128"] = _merge(4, True), _merge(4, False)


def _splat(size):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        if name.endswith("128"):
            d, _, b, _ = regs(w, name)
            uimm = (w >> 16) & 31
        else:
            d, b = vx_d(w), vx_b(w)
            uimm = vx_a(w)
        ls = lanes(s.v[b], size)
        x = ls[uimm & (len(ls) - 1)]
        s.v[d] = unlanes([x] * len(ls), size)
    return h


H["vspltb"], H["vsplth"], H["vspltw"], H["vspltw128"] = _splat(1), _splat(2), _splat(4), _splat(4)


def _splti(size):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        if name.endswith("128"):
            d = v128_d(w)
            simm = sext((w >> 16) & 31, 5)
        else:
            d = vx_d(w)
            simm = sext(vx_a(w), 5)
        s.v[d] = unlanes([simm] * (16 // size), size)
    return h


H["vspltisb"], H["vspltish"], H["vspltisw"], H["vspltisw128"] = _splti(1), _splti(2), _splti(4), _splti(4)


@op("vsr", "vsl")
def _vsr(s, w):
    name = xenia_lookup.lookup(w)
    d, a, b, _ = regs(w, name)
    sh = s.v[b][3] & 7       # [PEM] the shift count of the last byte, all bytes must agree
    x = int.from_bytes(to_bytes(s.v[a]), "big")
    x = (x >> sh) if name == "vsr" else (x << sh) & ((1 << 128) - 1)
    s.v[d] = from_bytes(x.to_bytes(16, "big"))


@op("lvsl", "lvsr")
def _lvs(s, w):
    sh = ea_x(s, w) & 15
    if xenia_lookup.lookup(w) == "lvsl":
        b = bytes(range(sh, sh + 16))
    else:
        b = bytes(range(16 - sh, 32 - sh))
    s.v[RT(w)] = from_bytes(b)


def vec_mem_regs(s, w, name):
    if name.endswith("128"):
        return v128_d(w), ea_x(s, w) & M32
    return RT(w), ea_x(s, w) & M32


@op("lvx", "lvx128")
def _lvx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    ea &= ~15
    s.v[vd] = [s.ld(ea + 4 * i, 4) for i in range(4)]


@op("stvx", "stvx128")
def _stvx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    ea &= ~15
    for i in range(4):
        s.st(ea + 4 * i, 4, s.v[vd][i])


@op("stvewx", "stvewx128")
def _stvewx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    ea &= ~3
    s.st(ea, 4, s.v[vd][(ea >> 2) & 3])


@op("stvehx")
def _stvehx(s, w):
    vd, ea = vec_mem_regs(s, w, "stvehx")
    ea &= ~1
    b = to_bytes(s.v[vd])
    i = ea & 15
    s.st(ea, 2, int.from_bytes(b[i:i + 2], "big"))


@op("lvlx", "lvlx128")
def _lvlx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    eb = ea & 15
    b = bytes(s.rb(ea + i) for i in range(16 - eb)) + bytes(eb)
    s.v[vd] = from_bytes(b)


@op("lvrx", "lvrx128")
def _lvrx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    eb = ea & 15
    base = ea - eb
    b = bytes(16 - eb) + bytes(s.rb(base + i) for i in range(eb))
    s.v[vd] = from_bytes(b)


@op("stvlx", "stvlx128")
def _stvlx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    eb = ea & 15
    b = to_bytes(s.v[vd])
    for i in range(16 - eb):
        s.st(ea + i, 1, b[i])


@op("stvrx", "stvrx128")
def _stvrx(s, w):
    vd, ea = vec_mem_regs(s, w, xenia_lookup.lookup(w))
    eb = ea & 15
    b = to_bytes(s.v[vd])
    base = ea - eb
    for i in range(eb):
        s.st(base + i, 1, b[16 - eb + i])


@op("vpermwi128")
def _vpermwi(s, w):
    d, b = v128_d(w), v128_b(w)
    uimm = ((w >> 16) & 31) | (((w >> 6) & 7) << 5)
    src = s.v[b]
    s.v[d] = [src[(uimm >> 6) & 3], src[(uimm >> 4) & 3], src[(uimm >> 2) & 3], src[uimm & 3]]


@op("vrlimi128")
def _vrlimi(s, w):
    d, b = v128_d(w), v128_b(w)
    imm = (w >> 16) & 31
    z = (w >> 6) & 3
    src = s.v[b]
    rot = [src[(i + z) & 3] for i in range(4)]
    s.v[d] = [rot[i] if imm & (8 >> i) else s.v[d][i] for i in range(4)]


# VMX float lanes. NJ=1: denormal inputs and results become zero of the same sign [PEM 3.2.2].
def vf_in(s, b):
    if s.nj and S.isdenorm(b):
        return b & 0x80000000
    return b


def vf_out(s, b):
    if s.nj and S.isdenorm(b):
        return b & 0x80000000
    return b


def vf_nan(*ops):
    for b in ops:
        if S.isnan(b):
            return b | S.qbit
    return None


def vf_binop(kind):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, _ = regs(w, name)
        out = []
        for x, y in zip(s.v[a], s.v[b]):
            x, y = vf_in(s, x), vf_in(s, y)
            n = vf_nan(x, y)
            if n is not None:
                out.append(n)
                continue
            if kind == "max" or kind == "min":
                vx = S.value(x) if not S.isinf(x) else (-1 if S.neg(x) else 1) * Fraction(10) ** 50
                vy = S.value(y) if not S.isinf(y) else (-1 if S.neg(y) else 1) * Fraction(10) ** 50
                if vx == vy:  # +0/-0: max picks +0, min picks -0 [PEM vmaxfp]
                    r = (x & y) if kind == "max" else (x | y)
                else:
                    r = x if (vx > vy) == (kind == "max") else y
                out.append(r)
                continue
            dx, dy = s2d(x), s2d(y)
            ops = (dx, dy)
            k = {"add": "add", "sub": "sub", "mul": "mul"}[kind]
            r = fp_special(k, ops, 0)
            if r is not None:
                out.append(SNAN32 if r == DNAN else S.round(D.value(r), 0) if not D.isinf(r) else
                           ((r >> 32) & 0x80000000) | 0x7F800000)
                continue
            ex = fp_exact(k, ops)
            zs = fp_zero_sign(k, ops, 0) if ex == 0 else 0
            out.append(vf_out(s, S.round(ex, 0, zs)))
        s.v[d] = out
    return h


H["vaddfp"] = H["vaddfp128"] = vf_binop("add")
H["vsubfp"] = H["vsubfp128"] = vf_binop("sub")
H["vmulfp128"] = vf_binop("mul")
H["vmaxfp"] = H["vmaxfp128"] = vf_binop("max")
H["vminfp"] = H["vminfp128"] = vf_binop("min")


def vf_cmp(kind):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, _ = regs(w, name)
        out = []
        for x, y in zip(s.v[a], s.v[b]):
            x, y = vf_in(s, x), vf_in(s, y)
            if S.isnan(x) or S.isnan(y):
                out.append(0)
                continue
            vx = S.value(x) if not S.isinf(x) else (-1 if S.neg(x) else 1) * Fraction(10) ** 50
            vy = S.value(y) if not S.isinf(y) else (-1 if S.neg(y) else 1) * Fraction(10) ** 50
            t = {"eq": vx == vy, "ge": vx >= vy, "gt": vx > vy}[kind]
            out.append(M32 if t else 0)
        s.v[d] = out
        rc = (w >> 10) & 1 if not name.endswith("128") else (w >> 6) & 1
        if rc:
            allt = all(o == M32 for o in out)
            nonet = all(o == 0 for o in out)
            s.setcrf(6, (8 if allt else 0) | (2 if nonet else 0))
    return h


H["vcmpeqfp"] = H["vcmpeqfp128"] = vf_cmp("eq")
H["vcmpgefp"] = H["vcmpgefp128"] = vf_cmp("ge")
H["vcmpgtfp"] = H["vcmpgtfp128"] = vf_cmp("gt")


def vf_round(rmode):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, _, b, _ = regs(w, name)
        out = []
        for x in s.v[b]:
            x = vf_in(s, x)
            if S.isnan(x):
                out.append(x | S.qbit)
            elif S.isinf(x) or S.iszero(x):
                out.append(x)
            else:
                v = S.value(x)
                n = _round_int(v, rmode) if rmode != 0 else _round_int(v, 0)
                out.append(S.round(Fraction(n), 0, S.neg(x)))
        s.v[d] = out
    return h


H["vrfin"], H["vrfiz"], H["vrfim"], H["vrfip"] = vf_round(0), vf_round(1), vf_round(3), vf_round(2)


@op("vcfsx", "vcsxwfp128", "vcfux", "vcuxwfp128")
def _vcf(s, w):
    name = xenia_lookup.lookup(w)
    if name.endswith("128"):
        d, _, b, _ = regs(w, name)
        uimm = (w >> 16) & 31
    else:
        d, b, uimm = vx_d(w), vx_b(w), vx_a(w)
    signed_ = name in ("vcfsx", "vcsxwfp128")
    out = []
    for x in s.v[b]:
        v = sext(x, 32) if signed_ else x
        out.append(S.round(Fraction(v, 1 << uimm), 0))
    s.v[d] = out


@op("vctsxs", "vcfpsxws128", "vctuxs", "vcfpuxws128")
def _vct(s, w):
    name = xenia_lookup.lookup(w)
    if name.endswith("128"):
        d, _, b, _ = regs(w, name)
        uimm = (w >> 16) & 31
    else:
        d, b, uimm = vx_d(w), vx_b(w), vx_a(w)
    signed_ = name in ("vctsxs", "vcfpsxws128")
    lo, hi = (-(1 << 31), (1 << 31) - 1) if signed_ else (0, M32)
    out = []
    for x in s.v[b]:
        x = vf_in(s, x)
        if S.isnan(x):
            r = 0
            pass  # NaN converts to 0 without VSCR[SAT] [SDK instr_vscr_convert.s]
            out.append(0)
            continue
        if S.isinf(x):
            out.append((lo if S.neg(x) else hi) & M32)
            s.sat = 1
            continue
        v = S.value(x) * (1 << uimm)
        n = int(v)  # truncation toward zero [PEM vctsxs]
        out.append(sat(s, n, lo, hi) & M32)
    s.v[d] = out


def _unpk(size_in, signed_, high):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, _, b, _ = regs(w, name)
        ls = lanes(s.v[b], size_in)
        n = len(ls) // 2
        part = ls[:n] if high else ls[n:]
        s.v[d] = unlanes([sext(x, 8 * size_in) for x in part], 2 * size_in)
    return h


H["vupkhsb"] = H["vupkhsb128"] = _unpk(1, True, True)
H["vupklsb"] = H["vupklsb128"] = _unpk(1, True, False)
H["vupkhsh"] = H["vupkhsh128"] = _unpk(2, True, True)
H["vupklsh"] = H["vupklsh128"] = _unpk(2, True, False)


@op("vpkswss", "vpkswss128")
def _vpkswss(s, w):
    name = xenia_lookup.lookup(w)
    d, a, b, _ = regs(w, name)
    out = [sat(s, sext(x, 32), -32768, 32767) for x in s.v[a] + s.v[b]]
    s.v[d] = unlanes(out, 2)


# vupkd3d128 [XEN field layout, SDK vectors]
def half_to_float_bits(h):
    sgn = (h >> 15) << 31
    e = (h >> 10) & 31
    m = h & 0x3FF
    if e == 0:
        # Xenos half floats have no denormals: the SDK pack vectors flush them to a signed zero; we
        # assume the same for unpack (no hardware vector for it)
        return sgn
    # [XEN] Xenos half floats have no infinity or NaN: exponent 31 is a normal binade
    return sgn | ((e - 15 + 127) << 23) | (m << 13)


@op("vupkd3d128")
def _vupkd3d(s, w):
    d, b = v128_d(w), v128_b(w)
    typ = ((w >> 16) & 31) >> 2
    src = s.v[b]
    if typ == 1:   # NORMSHORT2: 3.0 + s16 * 2^-22, z = 0, w = 1.0; -32768 unpacks as QNaN [SDK]
        x, y = sext(src[3] >> 16, 16), sext(src[3], 16)
        s.v[d] = [SNAN32 if t == -32768 else (0x40400000 + t) & M32 for t in (x, y)] + [0, 0x3F800000]
    elif typ == 3:  # FLOAT16_2
        s.v[d] = [half_to_float_bits(src[3] >> 16), half_to_float_bits(src[3] & 0xFFFF), 0, 0x3F800000]
    elif typ == 0:  # D3DCOLOR ARGB -> 1.0 + c * 2^-23 in xyzw = RGBA
        c = src[3]
        a, r, g, bl = (c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255
        s.v[d] = [0x3F800000 | r, 0x3F800000 | g, 0x3F800000 | bl, 0x3F800000 | a]
    elif typ == 5:
        hs = [src[2] >> 16, src[2] & 0xFFFF, src[3] >> 16, src[3] & 0xFFFF]
        s.v[d] = [half_to_float_bits(x) for x in hs]
    else:
        raise Unsupported("vupkd3d128 type %d" % typ)


def ref_supported(word):
    name = xenia_lookup.lookup(word)
    return name in H


def step(s, word, pc):
    """Executes one instruction. Returns the next pc, or 'ret' for a return to the harness."""
    name = xenia_lookup.lookup(word)
    if name is None:
        raise Unsupported("unknown word %08X" % word)
    h = H.get(name)
    if h is None:
        raise Unsupported(name)
    if name in BRANCHES:
        return h(s, word, pc)
    h(s, word)
    return None


RETURN_SENTINEL = 0x0BADC0DE & ~3


def run(s, code, entry, max_steps=100000):
    """Runs from entry until a branch to the return sentinel in LR. code: dict address -> word."""
    s.lr = RETURN_SENTINEL
    pc = entry
    for _ in range(max_steps):
        if pc == RETURN_SENTINEL:
            return
        if pc not in code:
            raise Unsupported("pc %08X outside the test code" % pc)
        nxt = step(s, code[pc], pc)
        pc = pc + 4 if nxt is None else nxt
    raise Unsupported("step limit")


import ppcref_more  # noqa: E402,F401  (registers further forms in H)
