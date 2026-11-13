"""Encoding forms the game uses, from the code the codegen actually translated.

Walks the generated C++ (generated/default/*.cpp): every instruction the codegen emits carries a comment
"// <mnemonic> <operands>" in address order, labels "loc_X:" resynchronise the address. Each comment is
matched with the word at that address as our codegen's decoder (fcp_semtool decode) sees it, so the
histogram is exactly the translated set (no inline jump tables, no padding).

A form is the decoder mnemonic (it carries Rc "." and OE "o", e.g. "addo."). Within a form, variants are
the operand texts with register numbers and, outside IMM_FORMS, immediates normalised.

  python forms.py <generated/default> <default.bin> <fcp_semtool.exe> <out.json> [--tested <tsv>...]

Writes the histogram with up to MAX_VARIANTS representative words per form and the register class
spread (VMX128 registers >= 32/64/96). The words are single instruction encodings (no game code).
"""
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xenia_lookup  # noqa: E402

IMAGE_BASE = 0x82000000
GAME_CODE_END = 0x82620000  # below: game code, above: libraries (static analysis of default.xex)
MAX_VARIANTS = 48
# Forms whose immediates change the semantics beyond what random register inputs cover.
IMM_FORMS = re.compile(
    r"^(rlw|rld|rot|clr|ext[lr]|ins|sradi|srawi|sldi|srdi|slwi|srwi|vupkd3d128|vpkd3d128|vrlimi128|"
    r"vpermwi128|vsldoi|vsplt|vspltis|mtcrf|mtfsf|mtfsfi|mtfsb|mfspr|mtspr|mftb|tw|td|vcfsx|vcfux|vctsxs|"
    r"vctuxs|vcsxwfp|vcuxwfp|vcfpsxws|vcfpuxws|dcb|lvsl|lvsr|sync|mcrf|crf)")
REG = re.compile(r"\b(r|f|v|cr)(\d+)\b")
NUM = re.compile(r"(?<![\w.])-?(0x[0-9a-fA-F]+|\d+)\b")


def decode_table(binpath, tool, base, size):
    data = open(binpath, "rb").read()
    off = base - IMAGE_BASE
    with tempfile.TemporaryDirectory() as tmp:
        words = os.path.join(tmp, "code.bin")
        out = os.path.join(tmp, "code.tsv")
        open(words, "wb").write(data[off:off + size])
        subprocess.run([tool, "decode", words, "%08X" % base, out], check=True)
        table = {}
        for line in open(out, encoding="latin-1"):
            a, w, i, name, ops = line.rstrip("\n").split("\t", 4)
            table[int(a, 16)] = (int(w, 16), int(i), name, ops)
    return table


def code_range(gen_dir):
    pch = open(os.path.join(gen_dir, "predator_pch.h"), encoding="utf-8").read()
    base = int(re.search(r"REX_CODE_BASE (0x[0-9A-F]+)", pch).group(1), 16)
    size = int(re.search(r"REX_CODE_SIZE (0x[0-9A-F]+)", pch).group(1), 16)
    return base, size


def translated(gen_dir, table):
    names = {}
    for line in open(os.path.join(gen_dir, "predator_register.cpp"), encoding="utf-8"):
        m = re.search(r"SetFunction\(0x([0-9A-F]+), (\w+)\)", line)
        if m:
            names[m.group(2)] = int(m.group(1), 16)
    fn_re = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
    loc_re = re.compile(r"^loc_([0-9A-F]+):")
    cmt_re = re.compile(r"^\t// (\S+)(?: (.*))?$")
    seen = {}
    mismatched = 0
    for fname in sorted(os.listdir(gen_dir)):
        if not fname.endswith(".cpp") or not fname.startswith("predator_recomp"):
            continue
        pc = None
        for line in open(os.path.join(gen_dir, fname), encoding="utf-8"):
            m = fn_re.match(line)
            if m:
                n = m.group(1)
                pc = int(n[4:], 16) if n.startswith("sub_") else names.get(n)
                continue
            m = loc_re.match(line)
            if m:
                pc = int(m.group(1), 16)
                continue
            m = cmt_re.match(line)
            if m and pc is not None:
                ent = table.get(pc)
                if ent and ent[2] == m.group(1):
                    seen[pc] = ent
                    pc += 4
                else:
                    mismatched += 1
    return seen, mismatched


def variant_key(name, ops):
    t = REG.sub(lambda m: m.group(1).upper(), ops)
    if not IMM_FORMS.match(name):
        t = NUM.sub("N", t)
    return t


def reg_class(ops):
    """Highest register number class used (VMX128 v32..v127 sit in extra encoding bits)."""
    hi = 0
    for kind, num in REG.findall(ops):
        if kind == "v":
            hi = max(hi, int(num) // 32)
    return hi


def main():
    gen_dir, binpath, tool, out = sys.argv[1:5]
    tested_files = sys.argv[sys.argv.index("--tested") + 1:] if "--tested" in sys.argv else []
    base, size = code_range(gen_dir)
    table = decode_table(binpath, tool, base, size)
    seen, mismatched = translated(gen_dir, table)
    forms = {}
    for pc, (word, iid, name, ops) in seen.items():
        f = forms.setdefault(name, {"id": iid, "count": 0, "game": 0, "lib": 0, "variants": {},
                                    "regclass": {}})
        f["count"] += 1
        xn = xenia_lookup.lookup(word) or "-"
        f.setdefault("xenia", {})
        f["xenia"][xn] = f["xenia"].get(xn, 0) + 1
        f["game" if pc < GAME_CODE_END else "lib"] += 1
        key = variant_key(name, ops)
        v = f["variants"].setdefault(key, {"count": 0, "word": word, "addr": pc, "ops": ops})
        v["count"] += 1
        rc = reg_class(ops)
        f["regclass"].setdefault(str(rc), {"word": word, "ops": ops, "count": 0})["count"] += 1
    for f in forms.values():
        vs = sorted(f["variants"].items(), key=lambda kv: -kv[1]["count"])
        f["n_variants"] = len(vs)
        f["variants"] = [dict(key=k, **v) for k, v in vs[:MAX_VARIANTS]]
    tested = set()
    for t in tested_files:
        for line in open(t, encoding="latin-1"):
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 4 and parts[3] != "-":
                tested.add(parts[3])
    result = {"code_base": base, "code_size": size, "instructions": len(seen),
              "comment_mismatches": mismatched, "forms": forms, "sdk_tested": sorted(tested)}
    json.dump(result, open(out, "w"), indent=1, sort_keys=True)
    print("forms.py: %d translated instructions, %d forms, %d comment mismatches"
          % (len(seen), len(forms), mismatched))


if __name__ == "__main__":
    main()
