"""Evaluation of the semantic test.

  python compare.py <gen_dir> <rex_results.bin> [--xr <xr_results.bin>] --known <known.toml>
                    --report <report.txt> [--json <summary.json>] [--leaf <leaf_dir>]

Per case: ReXGlue against the expectation (hardware vector or reference), XenonRecomp against the
expectation, ReXGlue against XenonRecomp. Exit 1 when ReXGlue fails a case whose form is not listed in
known.toml (or fails more often than listed), 0 otherwise.
"""
import json
import os
import struct
import sys
import time
import tomllib

KIND = ["gpr", "fpr", "vr", "cr", "xer", "ctr", "lr", "vscr", "msr", "rn"]
FULL64 = [0xFFFFFFFF, 0xFFFFFFFF, 0, 0]
FULL32 = [0xFFFFFFFF, 0, 0, 0]
FULLV = [0xFFFFFFFF] * 4


def default_mask(kind):
    if kind in (0, 1, 5, 6):
        return FULL64
    if kind == 2:
        return FULLV
    return FULL32


def read_results(path, meta, magic):
    data = open(path, "rb").read()
    assert data[:4] == magic, path
    n = struct.unpack_from("<I", data, 4)[0]
    assert n == len(meta), "%s: %d cases, meta %d" % (path, n, len(meta))
    off = 8
    out = []
    for m in meta:
        outs, mem_out = m[5], m[6]
        status = struct.unpack_from("<I", data, off)[0]
        off += 4
        vals = []
        for _ in outs:
            vals.append(list(struct.unpack_from("<4I", data, off)))
            off += 16
        mems = []
        for a, ln in mem_out:
            mems.append(data[off:off + ln])
            off += ln + ((-ln) & 3)
        out.append((status, vals, mems))
    return out


def isnan64(hi, lo):
    return (hi & 0x7FF00000) == 0x7FF00000 and ((hi & 0xFFFFF) or lo)


def isnan32(x):
    return (x & 0x7F800000) == 0x7F800000 and (x & 0x7FFFFF)


def nan_fold(kind, w):
    """Maps every NaN to one value (FPR: double, VR: per single lane)."""
    if kind == 1 and isnan64(w[0], w[1]):
        return [0x7FF80000, 0, 0, 0]
    if kind == 2:
        return [0x7FC00000 if isnan32(x) else x for x in w]
    return w


def diff(m, a, b, skip_kinds=(), nan_equal=False):
    """List of differences between two result tuples (status, vals, mems) under the case masks."""
    sa, va, ma = a
    sb, vb, mb = b
    if sa != sb:
        return ["status %d != %d" % (sa, sb)]
    res = []
    masks = {int(k): v for k, v in m[4].items()}
    for i, (kind, idx) in enumerate(m[5]):
        if kind in skip_kinds:
            continue
        mask = masks.get(i, default_mask(kind))
        ai, bi = (nan_fold(kind, va[i]), nan_fold(kind, vb[i])) if nan_equal else (va[i], vb[i])
        x = [p & q for p, q in zip(ai, mask)]
        y = [p & q for p, q in zip(bi, mask)]
        if x != y:
            res.append("%s%s %s/%s" % (KIND[kind], "" if kind > 2 else idx, fmt(kind, va[i]), fmt(kind, vb[i])))
    for i, (x, y) in enumerate(zip(ma, mb)):
        if x != y:
            first = next(j for j in range(len(x)) if x[j] != y[j])
            addr = m[6][i][0] + first
            res.append("mem@%08X %s/%s" % (addr, x[first:first + 8].hex(), y[first:first + 8].hex()))
    return res


def fmt(kind, w):
    if kind in (0, 1, 5, 6):
        return "%08X%08X" % (w[0], w[1])
    if kind == 2:
        return "[%s]" % ",".join("%08X" % x for x in w)
    return "%08X" % w[0]


def rel_errors(m, got, want):
    """Relative errors of finite float results (VR lanes as single, FPR as double)."""
    errs = []
    nan_mismatch = 0
    for i, (kind, idx) in enumerate(m[5]):
        if kind == 2:
            pairs = [(struct.unpack('<f', struct.pack('<I', g))[0], struct.unpack('<f', struct.pack('<I', w))[0])
                     for g, w in zip(got[1][i], want[1][i])]
        elif kind == 1:
            gb = (got[1][i][0] << 32) | got[1][i][1]
            wb = (want[1][i][0] << 32) | want[1][i][1]
            pairs = [(struct.unpack('<d', struct.pack('<Q', gb))[0], struct.unpack('<d', struct.pack('<Q', wb))[0])]
        else:
            continue
        for g, w in pairs:
            if g != g or w != w:
                nan_mismatch += (g != g) != (w != w)
                continue
            if w in (float('inf'), float('-inf')) or g in (float('inf'), float('-inf')):
                if g != w:
                    errs.append(float('inf'))
                continue
            if w != 0:
                errs.append(abs(g - w) / abs(w))
            elif g != 0:
                errs.append(float('inf'))
    return errs, nan_mismatch


def describe_inputs(m):
    parts = []
    for kind, idx, w in m[9]:
        parts.append("%s%s=%s" % (KIND[kind], "" if kind > 2 else idx, fmt(kind, w)))
    return " ".join(parts) + " rn=%d nj=%d" % (m[8] & 3, (m[8] >> 2) & 1)


def main():
    args = sys.argv[1:]
    gen_dir, rex_path = args[0], args[1]

    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default

    t0 = time.time()
    meta_all = json.load(open(os.path.join(gen_dir, "meta.json")))
    meta = meta_all["cases"]
    exp = read_results(os.path.join(gen_dir, "expected.bin"), meta, b"FCPE")
    rex = read_results(rex_path, meta, b"FCPR")
    xr = read_results(opt("--xr"), meta, b"FCPR") if opt("--xr") and os.path.exists(opt("--xr")) else None
    exact = read_results(opt("--exact"), meta, b"FCPR") if opt("--exact") and os.path.exists(opt("--exact")) else None
    # FCP: variant with [fp_rules] on (tests/semantics/fp_rules.toml)
    fpr = read_results(opt("--fprules"), meta, b"FCPR") if opt("--fprules") and os.path.exists(opt("--fprules")) else None
    known = {}
    if opt("--known") and os.path.exists(opt("--known")):
        known = {k["form"]: k for k in tomllib.load(open(opt("--known"), "rb")).get("known", [])}

    forms = {}
    for i, m in enumerate(meta):
        form, origin = m[0], m[1]
        f = forms.setdefault(form, {"cases": 0, "hw": 0, "ref": 0, "none": 0, "rex_fail": 0, "xr_fail": 0,
                                    "rex_xr": 0, "exact_fail": 0, "fpr_fail": 0, "fpr_regress": 0,
                                    "fpr_examples": [], "examples": [], "xr_examples": [], "diff_examples": []})
        f["cases"] += 1
        f[origin] += 1
        has_exp = exp[i][0] != 0xFFFFFFFF
        if has_exp:
            dr = diff(m, rex[i], exp[i])
            if dr and diff(m, rex[i], exp[i], nan_equal=True):
                f["rex_fail_nonnan"] = f.get("rex_fail_nonnan", 0) + 1
            if dr:
                f["rex_fail"] += 1
                if origin == "hw":
                    errs, nanm = rel_errors(m, rex[i], exp[i])
                    finite = [e for e in errs if e != float("inf")]
                    f["max_rel"] = max([f.get("max_rel", 0.0)] + finite)
                    f["inf_mismatch"] = f.get("inf_mismatch", 0) + sum(1 for e in errs if e == float("inf"))
                    f["nan_mismatch"] = f.get("nan_mismatch", 0) + nanm
                if len(f["examples"]) < 3:
                    f["examples"].append("%s | %s | got/want %s" % (origin, describe_inputs(m), "; ".join(dr)))
            if exact is not None and diff(m, exact[i], exp[i]):
                f["exact_fail"] += 1
            if fpr is not None:
                dp = diff(m, fpr[i], exp[i])
                if dp:
                    f["fpr_fail"] += 1
                    if not dr:
                        f["fpr_regress"] += 1   # [fp_rules] breaks a case the host emission gets right
                    if len(f["fpr_examples"]) < 2:
                        f["fpr_examples"].append("%s | %s | got/want %s" % (origin, describe_inputs(m), "; ".join(dp)))
            if xr is not None:
                dx = diff(m, xr[i], exp[i], (7,))
                if dx:
                    f["xr_fail"] += 1
                    if len(f["xr_examples"]) < 2:
                        f["xr_examples"].append("%s | %s | got/want %s" % (origin, describe_inputs(m), "; ".join(dx)))
        if xr is not None:
            dd = diff(m, rex[i], xr[i], (7,))  # XenonRecomp has no VSCR
            if dd:
                f["rex_xr"] += 1
                if len(f["diff_examples"]) < 2:
                    f["diff_examples"].append("%s | rex/xr %s" % (describe_inputs(m), "; ".join(dd)))

    total = len(meta)
    with_exp = sum(1 for e in exp if e[0] != 0xFFFFFFFF)
    rex_fail = sum(f["rex_fail"] for f in forms.values())
    unexpected = []
    for name, f in sorted(forms.items()):
        if f["rex_fail"] == 0:
            continue
        k = known.get(name)
        if k is None or f["rex_fail"] > int(k.get("max_failures", 1 << 30)):
            unexpected.append(name)
    fixed = [n for n in known if forms.get(n, {}).get("rex_fail", 0) == 0]
    regress = [n for n, f in sorted(forms.items()) if f["fpr_regress"]]

    lines = []
    lines.append("predator_semtest: %d cases (%d with expectation: hw %d, ref %d), %d forms, %d functions"
                 % (total, with_exp, sum(f["hw"] for f in forms.values()), sum(f["ref"] for f in forms.values()),
                    len(forms), meta_all["functions"]))
    lines.append("ReXGlue: %d failures in %d forms (%d forms not in known.toml); with every NaN counted as"
                 " equal: %d failures in %d forms"
                 % (rex_fail, sum(1 for f in forms.values() if f["rex_fail"]), len(unexpected),
                    sum(f.get("rex_fail_nonnan", 0) for f in forms.values()),
                    sum(1 for f in forms.values() if f.get("rex_fail_nonnan", 0))))
    if xr is not None:
        lines.append("XenonRecomp: %d failures in %d forms; ReXGlue != XenonRecomp in %d cases (%d forms)"
                     % (sum(f["xr_fail"] for f in forms.values()), sum(1 for f in forms.values() if f["xr_fail"]),
                        sum(f["rex_xr"] for f in forms.values()), sum(1 for f in forms.values() if f["rex_xr"])))
    else:
        lines.append("XenonRecomp: not run (FCP_XENONRECOMP_DIR not set)")
    lines.append("")
    if exact is not None:
        lines.append("ReXGlue with [vector_math] exact: %d failures in %d forms (informative)"
                     % (sum(f["exact_fail"] for f in forms.values()),
                        sum(1 for f in forms.values() if f["exact_fail"])))
    if fpr is not None:
        lines.append("ReXGlue with [fp_rules] (fp_rules.toml): %d failures in %d forms, %d cases worse than"
                     " without (%d forms)"
                     % (sum(f["fpr_fail"] for f in forms.values()), sum(1 for f in forms.values() if f["fpr_fail"]),
                        sum(f["fpr_regress"] for f in forms.values()), len(regress)))
    lines.append("%-28s %6s %5s %5s %5s %7s %6s %6s %6s %6s %6s  %s" % ("form", "cases", "hw", "ref", "none",
                                                                    "rexfail", "nonNaN", "xrfail", "rex!=xr",
                                                                    "exact", "fprule", "status"))
    for name, f in sorted(forms.items(), key=lambda kv: (-kv[1]["rex_fail"], -kv[1]["rex_xr"], kv[0])):
        st = "NEW" if name in unexpected else ("known" if name in known and f["rex_fail"] else "")
        lines.append("%-28s %6d %5d %5d %5d %7d %6d %6d %6d %6d %6d  %s" % (
            name, f["cases"], f["hw"], f["ref"], f["none"], f["rex_fail"], f.get("rex_fail_nonnan", 0),
            f["xr_fail"], f["rex_xr"], f["exact_fail"], f["fpr_fail"], st))
    lines.append("")
    lines.append("ReXGlue against hardware vectors: largest relative error of finite results, inf/NaN mismatches")
    for name, f in sorted(forms.items()):
        if "max_rel" in f:
            lines.append("  %-24s max rel %.3g  inf %d  NaN %d" % (name, f["max_rel"], f.get("inf_mismatch", 0),
                                                                 f.get("nan_mismatch", 0)))
    lines.append("")
    for name, f in sorted(forms.items()):
        if f["examples"] or f["xr_examples"] or f["diff_examples"] or f["fpr_examples"]:
            lines.append("== %s" % name)
            for e in f["examples"]:
                lines.append("  rex: " + e)
            for e in f["xr_examples"]:
                lines.append("  xr:  " + e)
            for e in f["diff_examples"]:
                lines.append("  dif: " + e)
            for e in f["fpr_examples"]:
                lines.append("  fpr: " + e)
    if fixed:
        lines.append("")
        lines.append("known.toml lists forms without failures now (fixed? remove them): " + ", ".join(fixed))
    lines.append("")
    lines.append("skipped forms: " + "; ".join("%s (%s)" % kv for kv in sorted(meta_all["skipped"].items())))
    text = "\n".join(lines) + "\n"
    if opt("--report"):
        open(opt("--report"), "w", encoding="utf-8").write(text)
    if opt("--json"):
        json.dump({"total": total, "with_expectation": with_exp, "forms": forms, "unexpected": unexpected,
                   "fixed": fixed, "fprules_regress": regress}, open(opt("--json"), "w"), indent=1)
    head = lines[:3] + ([l for l in lines if l.startswith("ReXGlue with [fp_rules]")] if fpr is not None else [])
    print("\n".join(head))
    if unexpected:
        print("FAILED forms: " + ", ".join(unexpected))
        print("details: " + (opt("--report") or "-"))
    if regress:
        print("FAILED: [fp_rules] variant worse than the host emission in: " + ", ".join(regress))
    print("compare.py: %.1f s" % (time.time() - t0))
    return 1 if unexpected or regress else 0


if __name__ == "__main__":
    sys.exit(main())
