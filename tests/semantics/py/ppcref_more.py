"""Second part of the reference (ppcref.py): forms the SDK vectors exercise and the Xenon D3D packs.

Rule sources as in ppcref.py ([ISA], [PEM], [XEN], [SDK]).
"""
from fractions import Fraction

import xenia_lookup
from ppcref import (H, M32, S, SNAN32, Fmt, Unsupported, from_bytes, lanes, op, regs, sat, sext,
                    to_bytes, unlanes, v128_b, v128_d, vec_mem_regs, vint, vx_b, vx_d, RT, RA, RB)

H["addic"] = H["addicx"]


@op("tw", "td")
def _trap_reg(s, w):
    to = RT(w)
    bits = 64 if xenia_lookup.lookup(w) == "td" else 32
    a, b = s.r[RA(w)], s.r[RB(w)]
    sa, sb = sext(a, bits), sext(b, bits)
    ua, ub = a & ((1 << bits) - 1), b & ((1 << bits) - 1)
    if ((to & 16) and sa < sb) or ((to & 8) and sa > sb) or ((to & 4) and sa == sb) or \
       ((to & 2) and ua < ub) or ((to & 1) and ua > ub):
        raise Unsupported("trap taken")


@op("mtvscr")
def _mtvscr(s, w):
    word = s.v[vx_b(w)][3]
    s.sat = word & 1
    s.nj = (word >> 16) & 1


@op("mfvscr")
def _mfvscr(s, w):
    s.v[vx_d(w)] = [0, 0, 0, (s.nj << 16) | s.sat]


def _satlanes(size, sa, sb, fn, lo, hi):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, _ = regs(w, name)
        la = [sext(x, 8 * size) if sa else x for x in lanes(s.v[a], size)]
        lb = [sext(x, 8 * size) if sb else x for x in lanes(s.v[b], size)]
        s.v[d] = unlanes([sat(s, fn(x, y), lo, hi) for x, y in zip(la, lb)], size)
    return h


def _rot(b):
    def f(s, x, y):
        n = y & (b - 1)
        return ((x << n) | (x >> (b - n))) if n else x
    return f


for _sz, _nm in ((1, "b"), (2, "h"), (4, "w")):
    _bits = 8 * _sz
    _slo, _shi, _uhi = -(1 << (_bits - 1)), (1 << (_bits - 1)) - 1, (1 << _bits) - 1
    H["vadds%ss" % _nm] = _satlanes(_sz, 1, 1, lambda x, y: x + y, _slo, _shi)
    H["vsubs%ss" % _nm] = _satlanes(_sz, 1, 1, lambda x, y: x - y, _slo, _shi)
    H["vaddu%ss" % _nm] = _satlanes(_sz, 0, 0, lambda x, y: x + y, 0, _uhi)
    H["vsubu%ss" % _nm] = _satlanes(_sz, 0, 0, lambda x, y: x - y, 0, _uhi)
    H["vaddu%sm" % _nm] = vint(lambda s, x, y: x + y, _sz)
    H["vsubu%sm" % _nm] = vint(lambda s, x, y: x - y, _sz)
    H["vmaxs%s" % _nm] = vint((lambda b: lambda s, x, y: max(sext(x, b), sext(y, b)))(_bits), _sz)
    H["vmins%s" % _nm] = vint((lambda b: lambda s, x, y: min(sext(x, b), sext(y, b)))(_bits), _sz)
    H["vmaxu%s" % _nm] = vint(lambda s, x, y: max(x, y), _sz)
    H["vminu%s" % _nm] = vint(lambda s, x, y: min(x, y), _sz)
    H["vavgu%s" % _nm] = vint(lambda s, x, y: (x + y + 1) >> 1, _sz)
    H["vsl%s" % _nm] = vint((lambda b: lambda s, x, y: x << (y & (b - 1)))(_bits), _sz)
    H["vsr%s" % _nm] = vint((lambda b: lambda s, x, y: x >> (y & (b - 1)))(_bits), _sz)
    H["vsra%s" % _nm] = vint((lambda b: lambda s, x, y: sext(x, b) >> (y & (b - 1)))(_bits), _sz)
    H["vrl%s" % _nm] = vint(_rot(_bits), _sz)
H["vslw128"], H["vsrw128"], H["vsraw128"], H["vrlw128"] = H["vslw"], H["vsrw"], H["vsraw"], H["vrlw"]


def _pack(size_in, signed_in, lo, hi, modulo=False):
    def h(s, w):
        name = xenia_lookup.lookup(w)
        d, a, b, _ = regs(w, name)
        src = [sext(x, 8 * size_in) if signed_in else x
               for x in lanes(s.v[a], size_in) + lanes(s.v[b], size_in)]
        if modulo:
            out = [x & ((1 << (4 * size_in)) - 1) for x in src]
        else:
            out = [sat(s, x, lo, hi) for x in src]
        s.v[d] = unlanes(out, size_in // 2)
    return h


for _n, _args in {"vpkshss": (2, 1, -128, 127), "vpkshus": (2, 1, 0, 255), "vpkuhus": (2, 0, 0, 255),
                  "vpkswss": (4, 1, -32768, 32767), "vpkswus": (4, 1, 0, 65535),
                  "vpkuwus": (4, 0, 0, 65535)}.items():
    H[_n] = H[_n + "128"] = _pack(*_args)
H["vpkuhum"] = H["vpkuhum128"] = _pack(2, 0, 0, 0, True)
H["vpkuwum"] = H["vpkuwum128"] = _pack(4, 0, 0, 0, True)


@op("vslo", "vsro", "vslo128", "vsro128")
def _vslo(s, w):
    name = xenia_lookup.lookup(w)
    d, a, b, _ = regs(w, name)
    sh = (s.v[b][3] >> 3) & 15
    x = to_bytes(s.v[a])
    s.v[d] = from_bytes((x[sh:] + bytes(sh)) if name.startswith("vslo") else (bytes(sh) + x[:16 - sh]))


@op("lvebx", "lvehx", "lvewx", "lvewx128")
def _lve(s, w):
    name = xenia_lookup.lookup(w)
    vd, ea = vec_mem_regs(s, w, name)
    # The bytes outside the element are undefined [PEM]; the SDK vectors (instr_lvexx.s) expect the whole
    # aligned quadword, as Xenia loads it. We follow the SDK.
    ea &= ~15
    s.v[vd] = [s.ld(ea + 4 * i, 4) for i in range(4)]


# vpkd3d128 [XEN field layout and lane rules, checked against the SDK vectors]
def _f32val(b):
    if S.isinf(b):
        return (-1 if S.neg(b) else 1) * Fraction(10) ** 50
    return S.value(b)


def _clampf(b, lo, hi, s=None):
    """maxps then minps (Xenia's order): NaN packs as the lower bound. A clamp of a number sets
    VSCR[SAT], a NaN does not [SDK instr_vscr_d3d.s]."""
    if S.isnan(b):
        return lo
    v = _f32val(b)
    if v < _f32val(lo):
        if s is not None:
            s.sat = 1
        return lo
    if v > _f32val(hi):
        if s is not None:
            s.sat = 1
        return hi
    return b


HALF = Fmt(5, 10)


def float_to_half_xenos(b):
    """Xenos half float: no infinity or NaN, exponent 31 is a normal binade [XEN]."""
    sgn = (b >> 16) & 0x8000
    if S.isnan(b):
        return sgn | 0x7FFF          # [SDK] kPackD3DResults: NaN packs as the signed field maximum
    if S.isinf(b):
        return sgn | 0x7FFF
    v = abs(_f32val(b))
    if v == 0:
        return sgn
    e = v.numerator.bit_length() - v.denominator.bit_length()
    if Fraction(2) ** e > v:
        e -= 1
    if e > 16:
        return sgn | 0x7FFF
    if e >= -14:
        m = v / Fraction(2) ** e - 1
        mi = m * 1024
        n = mi.numerator // mi.denominator   # truncation [SDK: 11 of 11 discriminating lanes]
        ee = e + 15
        if n == 1024:
            n, ee = 0, ee + 1
        if ee > 31:
            return sgn | 0x7FFF
        return sgn | (ee << 10) | n
    return sgn                       # [SDK] results below the smallest normal half flush to a signed zero


@op("vpkd3d128")
def _vpkd3d(s, w):
    d, b = v128_d(w), v128_b(w)
    imm = (w >> 16) & 31
    typ, pack, shift = imm >> 2, imm & 3, (w >> 6) & 3
    x, y, z, ww = s.v[b]
    if typ == 0:     # D3DCOLOR: lanes 3.0 + n * 2^-22, n in 0..255; ARGB from xyzw = RGBA
        c = [_clampf(v, 0x40400000, 0x404000FF, s) & 0xFF for v in (x, y, z, ww)]
        packed = [0, 0, 0, (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2]]
    elif typ == 1:   # NORMSHORT2
        c = [_clampf(v, 0x403F8001, 0x40407FFF, s) & 0xFFFF for v in (x, y)]
        packed = [0, 0, 0, (c[0] << 16) | c[1]]
    elif typ == 3:   # FLOAT16_2
        packed = [0, 0, 0, (float_to_half_xenos(x) << 16) | float_to_half_xenos(y)]
    elif typ == 5:   # FLOAT16_4
        h = [float_to_half_xenos(v) for v in (x, y, z, ww)]
        packed = [0, 0, (h[0] << 16) | h[1], (h[2] << 16) | h[3]]
    else:
        raise Unsupported("vpkd3d128 type %d" % typ)
    # placement after Xenia InstrEmit_vpkd3d128: (from packed, index) per destination element
    table = {
        (1, 0): ((0, 0), (0, 1), (0, 2), (1, 3)), (1, 1): ((0, 0), (0, 1), (1, 3), (0, 3)),
        (1, 2): ((0, 0), (1, 3), (0, 2), (0, 3)), (1, 3): ((1, 3), (0, 1), (0, 2), (0, 3)),
        (2, 0): ((0, 0), (0, 1), (1, 2), (1, 3)), (2, 1): ((0, 0), (1, 2), (1, 3), (0, 3)),
        (2, 2): ((1, 2), (1, 3), (0, 2), (0, 3)), (2, 3): ((1, 3), (0, 1), (0, 2), (0, 3)),
        (3, 0): ((0, 0), (0, 1), (1, 2), (1, 3)), (3, 1): ((0, 0), (1, 2), (1, 3), (0, 3)),
        (3, 2): ((1, 2), (1, 3), (0, 2), (0, 3)), (3, 3): ((0, 0), (0, 1), (0, 2), (1, 2)),
    }
    if pack == 0:
        raise Unsupported("vpkd3d128 pack 0")
    old = list(s.v[d])
    s.v[d] = [(packed if src else old)[i] for src, i in table[(pack, shift)]]
