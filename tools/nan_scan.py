"""Static scan: where could the game see NaN bit patterns (sign, payload, default NaN)?

Reads the decoder listing of fcp_semtool (address, word, id, mnemonic, operands) over the code range and
counts the patterns that make NaN bits observable (x86 and PPC differ in NaN sign and payload rules):

  1. fcmpu crX,fA,fA            self compare = isnan test (bit independent)
  2. FP store -> integer load of the same slot in the same function (bit reinterpretation), with the
     first consumer of the loaded GPR classified (sign test, magnitude mask, exponent test, integer
     compare, hash/arith, copy, ...); stores of fctiw/fctid results are listed apart (NaN -> fixed int)
  3. stvx/stvx128 -> lwz of the slot (vector float bits read as integers)
  4. immediates: lis 0x7FC0/0xFFC0/0x7FF8/0xFFF8/0x7F80/0xFF80/0x7FFF/0x8000
  5. data words in .rdata/.data: 0x7FC00000, 0xFFC00000, 0x7FF8000000000000, 0xFFF8000000000000, other NaNs
  6. VMX integer compares and vsel/vand/vxor whose input came from VMX float arithmetic

  python nan_scan.py <code.tsv> <default.bin> <default.json> [--list N] [--flows <out.tsv>]

code.tsv: fcp_semtool decode <words.bin> 820B0000 <code.tsv> over 0x820B0000..0x82880100. Prints counts
and up to N example addresses per class. Only addresses and counts are printed (no game code).
--flows writes one line per float-bits-as-int read: load address, producer of the stored FPR (looked through
fmr/fabs/fneg), its mnemonic and the consumer class, for the cross reference with the fp_rules sites that
FCP_FP_STATS=1 logs at run time.
"""
import collections
import json
import re
import struct
import sys

BASE = 0x82000000
GAME_CODE_END = 0x82620000  # below: game code, above: libraries (from the static analysis of default.xex)

FP_STORES = {"stfs": 4, "stfsu": 4, "stfd": 8, "stfdu": 8, "stfsx": 4, "stfdx": 8, "stfiwx": 4}
INT_LOADS = {"lwz": 4, "lwzu": 4, "ld": 8, "lwzx": 4, "ldx": 8, "lha": 2, "lhz": 2, "lbz": 1, "lwa": 4}
CONV = {"fctiw", "fctiwz", "fctid", "fctidz"}
MOVES = {"fmr", "fabs", "fneg", "fnabs"}
# FP arithmetic that can create a NaN or pick between NaN operands (the forms whose NaN bits follow x86)
ARITH = {"fadd", "fadds", "fsub", "fsubs", "fmul", "fmuls", "fdiv", "fdivs", "fmadd", "fmadds", "fmsub",
         "fmsubs", "fnmadd", "fnmadds", "fnmsub", "fnmsubs", "fsqrt", "fsqrts", "fres", "frsqrte", "frsp"}
VSTORES = {"stvx", "stvx128", "stvewx", "stvewx128"}
FLOWS = []  # (integer load, producer of the stored FPR, producer mnemonic, consumer class)
MEM = re.compile(r"^(-?\d+)\((r\d+)\)$")
REG = re.compile(r"\b([rfv]\d+|cr\d)\b")


def load_listing(path):
    rows = []
    for line in open(path, encoding="latin-1"):
        a, w, _i, name, ops = line.rstrip("\n").split("\t", 4)
        rows.append((int(a, 16), int(w, 16), name, ops.split(",") if ops and ops != "-" else []))
    return rows


def functions(meta):
    out = []
    for e in meta["pdata_entries"]:
        start, length = e[0], e[1]  # length in bytes
        out.append((start, start + length))
    out.sort()
    return out


def writes(name, ops):
    """Register written by an instruction (first operand for most forms), very coarse."""
    if not ops:
        return None
    if name.startswith(("st", "cmp", "fcmp", "b", "tw", "td", "dcb", "mt")) and not name.startswith("stwu"):
        if name in ("stwu", "stdu", "stfsu", "stfdu", "stwux"):
            m = MEM.match(ops[1]) if len(ops) > 1 else None
            return m.group(2) if m else None
        return None
    return ops[0]


def reads(ops):
    regs = set()
    for o in ops[1:]:
        regs.update(REG.findall(o))
    return regs


def classify_consumer(name, ops, reg):
    """First instruction that reads reg after the integer load."""
    s = ",".join(ops)
    if name in ("rlwinm", "rlwinm.") and len(ops) == 5:
        sh, mb, me = int(ops[2]), int(ops[3]), int(ops[4])
        if mb == me == 31 and sh == 1 or mb == me == 0 and sh == 0:
            return "sign bit"
        if mb == 1 and me == 31 and sh == 0:
            return "magnitude (sign cleared)"
        if (sh, mb, me) in ((9, 24, 31), (0, 1, 8), (1, 24, 31)) or (sh == 9 and me == 31):
            return "exponent"
        return "rlwinm other"
    if name in ("srwi", "srwi.") and ops[-1] == "31":
        return "sign bit"
    if name in ("srawi", "srawi.") and ops[-1] == "31":
        return "sign mask"
    if name in ("clrlwi", "clrlwi.") and ops[-1] == "1":
        return "magnitude (sign cleared)"
    if name in ("extrwi", "extrwi."):
        return "exponent"
    if name in ("cmpwi", "cmpdi") and ops[-1] == "0":
        return "signed compare with 0"
    if name in ("cmpwi", "cmpdi", "cmplwi", "cmpldi"):
        return "compare with immediate"
    if name in ("cmpw", "cmpd", "cmplw", "cmpld"):
        return "integer compare"
    if name in ("stw", "std", "stwx", "stdx", "sth", "stb", "mr"):
        return "copy"
    if name in ("xor", "add", "mullw", "subf", "rotlwi", "xori", "addi"):
        return "hash/arith"
    if name in ("and", "andc", "andi.", "andis.", "or", "ori", "oris", "xoris"):
        return "mask"
    if name in ("rldicl", "rldicr", "srdi", "sldi", "clrldi"):
        return "64-bit field"
    return "other:" + name


def scan(rows, meta, nlist):
    funcs = functions(meta)
    starts = [f[0] for f in funcs]
    import bisect

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and funcs[i][0] <= addr < funcs[i][1]:
            return funcs[i]
        return None

    idx = {a: k for k, (a, _w, _n, _o) in enumerate(rows)}
    res = collections.OrderedDict()

    def add(cls, addr):
        lst = res.setdefault(cls, [])
        lst.append(addr)

    self_cmp = 0
    for k, (a, w, name, ops) in enumerate(rows):
        f = func_of(a)
        if f is None:
            continue
        lib = a >= GAME_CODE_END
        tag = " [lib]" if lib else ""
        if name in ("fcmpu", "fcmpo") and len(ops) == 3 and ops[1] == ops[2]:
            add("1 fcmpu fA,fA (isnan)" + tag, a)
        if name == "lis" and len(ops) == 2:
            v = int(ops[1], 0) & 0xFFFF
            names = {0x7FC0: "0x7FC0", 0xFFC0: "0xFFC0", 0x7FF8: "0x7FF8", 0xFFF8: "0xFFF8",
                     0x7F80: "0x7F80 (inf/exp)", 0xFF80: "0xFF80", 0x7FFF: "0x7FFF", 0x7FF0: "0x7FF0"}
            if v in names:
                add("4 lis " + names[v] + tag, a)
        if name in FP_STORES:
            # FP store: find the producer of the stored FPR and later integer loads of the slot
            fs = ops[0]
            prod = None
            prod_addr = None
            j = k - 1
            while j > max(k - 32, -1):
                pa, _pw, pn, po = rows[j]
                if pa < f[0]:
                    break
                if po and po[0] == fs and not pn.startswith(("st", "fcmp")):
                    prod = pn
                    prod_addr = pa
                    if pn in MOVES and len(po) > 1:
                        fs = po[1]  # look through fmr/fabs/fneg/fnabs
                        j -= 1
                        continue
                    break
                j -= 1
            if name.endswith("x"):
                if len(ops) < 3:
                    continue
                key = ("x", ops[1], ops[2])
                d0, size = 0, FP_STORES[name]
            else:
                m = MEM.match(ops[1]) if len(ops) > 1 else None
                if not m:
                    continue
                key = ("d", m.group(2))
                d0, size = int(m.group(1)), FP_STORES[name]
            for j in range(k + 1, min(k + 24, len(rows))):
                ja, _jw, jn, jo = rows[j]
                if ja >= f[1]:
                    break
                if jn in INT_LOADS and len(jo) >= 2:
                    hit = False
                    if key[0] == "x" and jn.endswith("x") and len(jo) >= 3:
                        hit = (jo[1], jo[2]) in ((key[1], key[2]), (key[2], key[1]))
                        off = 0
                    elif key[0] == "d" and not jn.endswith("x"):
                        m2 = MEM.match(jo[1])
                        if m2 and m2.group(2) == key[1]:
                            off = int(m2.group(1)) - d0
                            hit = 0 <= off < size
                    if hit:
                        conv = prod in CONV or name == "stfiwx"
                        rt = jo[0]
                        cons = "no reader"
                        for q in range(j + 1, min(j + 12, len(rows))):
                            qa, _qw, qn, qo = rows[q]
                            if qa >= f[1]:
                                break
                            if rt in reads(qo) or (qn.startswith(("st", "cmp")) and qo and qo[0] == rt):
                                cons = classify_consumer(qn, qo, rt)
                                break
                            if writes(qn, qo) == rt:
                                cons = "overwritten"
                                break
                        if conv:
                            add("2b FP->int conversion result read back" + tag, ja)
                        else:
                            what = "%s->%s+%d" % ("stfd" if size == 8 else "stfs", jn, off)
                            src = "arith" if prod in ARITH else ("load/move" if prod else "unknown")
                            add("2 float bits read as int: " + cons + tag, ja)
                            add("2p %s <- %s%s" % (cons, src, tag), ja)
                            if prod_addr is not None:
                                FLOWS.append((ja, prod_addr, prod, cons))
                            add("2x pair " + what + tag, ja)
                        break
                # a store to the same slot ends the window
                if jn.startswith("st") and key[0] == "d" and len(jo) >= 2:
                    m2 = MEM.match(jo[1])
                    if m2 and m2.group(2) == key[1] and int(m2.group(1)) == d0:
                        break
                if writes(jn, jo) == key[1]:
                    break
        if name in VSTORES and len(ops) >= 3:
            key = (ops[1], ops[2])
            for j in range(k + 1, min(k + 24, len(rows))):
                ja, _jw, jn, jo = rows[j]
                if ja >= f[1]:
                    break
                if jn in ("lwz", "lwzx", "lhz", "lbz") and len(jo) >= 2:
                    # stvx uses rA,rB; the integer reload usually uses d(rB) after addi; approximate: same base
                    m2 = MEM.match(jo[1])
                    base = m2.group(2) if m2 else (jo[1] if len(jo) > 1 else None)
                    if base in key:
                        add("3 vector bits read as int" + tag, ja)
                        break
        if name.startswith("v") and name.rstrip(".").rstrip("128") in (
                "vcmpequw", "vcmpgtsw", "vcmpgtuw", "vmaxsw", "vminsw", "vsel", "vand", "vxor", "vandc",
                "vor", "vsraw", "vsrw", "vslw") and len(ops) >= 3:
            srcs = set(o for o in ops[1:] if o.startswith("v"))
            for j in range(k - 1, max(k - 10, -1), -1):
                pa, _pw, pn, po = rows[j]
                if pa < f[0]:
                    break
                if po and po[0] in srcs and pn.startswith("v") and "fp" in pn and not pn.startswith(
                        ("vcmp", "vctsxs", "vctuxs", "vcfsx", "vcfux", "vcsxwfp", "vcuxwfp", "vcfpsxws")):
                    add("6 %s on VMX float result" % name.rstrip(".") + tag, a)
                    break
    return res


def scan_data(data, meta):
    res = collections.OrderedDict()
    for s in meta["pe"]["sections"]:
        if s["code"] or s["name"] not in (".rdata", ".data"):
            continue
        lo, hi = s["rva"], s["rva"] + min(s["virtual_size"], s["raw_size"])
        blob = data[lo:hi]
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from(">I", blob, off)[0]
            if (v & 0x7F800000) == 0x7F800000 and (v & 0x7FFFFF):
                if v in (0x7FC00000, 0xFFC00000, 0x7FFFFFFF, 0xFFFFFFFF):
                    res.setdefault("%s word 0x%08X" % (s["name"], v), []).append(BASE + lo + off)
                elif v == 0x7FF80000 or v == 0xFFF80000:
                    nxt = struct.unpack_from(">I", blob, off + 4)[0] if off + 8 <= len(blob) else 1
                    if nxt == 0 and off % 8 == 0:
                        res.setdefault("%s double 0x%08X00000000" % (s["name"], v), []).append(BASE + lo + off)
                    else:
                        res.setdefault("%s word 0x%08X (single NaN)" % (s["name"], v), []).append(BASE + lo + off)
                else:
                    res.setdefault("%s other NaN-pattern word" % s["name"], []).append(BASE + lo + off)
    return res


def main():
    nlist = 6
    if "--list" in sys.argv:
        nlist = int(sys.argv[sys.argv.index("--list") + 1])
    rows = load_listing(sys.argv[1])
    data = open(sys.argv[2], "rb").read()
    meta = json.load(open(sys.argv[3]))
    if "--flows" in sys.argv:
        scan(rows, meta, nlist)
        with open(sys.argv[sys.argv.index("--flows") + 1], "w") as f:
            for ja, pa, pn, cons in FLOWS:
                f.write("%08X\t%08X\t%s\t%s\n" % (ja, pa, pn, cons))
        del FLOWS[:]
    for title, res in (("code", scan(rows, meta, nlist)), ("data", scan_data(data, meta))):
        print("== %s ==" % title)
        for cls in sorted(res):
            lst = res[cls]
            ex = " ".join("%08X" % a for a in lst[:nlist])
            print("%6d  %s   e.g. %s" % (len(lst), cls, ex))


if __name__ == "__main__":
    main()
