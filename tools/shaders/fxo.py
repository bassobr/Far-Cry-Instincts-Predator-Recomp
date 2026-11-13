#!/usr/bin/env python3
"""fxo.py: read and analyse Predator's Xbox 360 shader containers 0x102A0E00/01 (XDK 2.0.2417).

Commands:
  fxo.py extract <disc-folder> <output> [--names names.txt]
      All shader containers from shaders.fat and common.fat (DAT2) to <output>/fxo/ (unique by SHA-1,
      name of the first occurrence), the HLSL/assembler sources from common.fat to <output>/src/ and a
      list <output>/manifest.tsv. Extract only into work/ (derived game data).
  fxo.py info <file.fxo>           Print a container broken down (header, register block, CTAB, binding table, CF).
  fxo.py scan <fxo-folder> [--json out.json] [--csv out.csv]
      Metrics over all containers: vertex inputs and layouts, vfetch fields, texture kinds, bool jumps,
      predicates, kill, depth output, multiple render targets, constants.

Container layout (measured on all 1,965 unique Predator containers, field meanings after the
container adapter of King Kong Recompiled, native-renderer/shaders/src/container.cpp, MIT):
  0x00 u32 magic 0x102A0E00 (pixel) / 0x102A0E01 (vertex), big-endian like everything here
  0x04 u32 virtualSize  header + tables; the microcode starts there
  0x08 u32 physicalSize microcode (multiple of 12)
  0x0C u32 offset of the register block (0 = none): {u64 mask, u64, u32 list length, u32, list
           {u16 offset into the device's register image (+1152), u16 count, count x u32}}.
           768..8959 float constants (16 bytes per register; VS 0..255, PS 256..511), 8960..8991 bool dwords,
           8992..9119 loop constants. These are the shader's literal constants (HLSL `def`).
  0x10 u32 offset of CTAB (u32 length, then D3DXSHADER_CONSTANTTABLE, offsets relative to the CTAB start)
  0x14 u32 offset of the binding table: word 0 = SQ_PROGRAM_CNTL bits of this stage;
           VS: word 6 = gap, word 7 = number of vfetch bindings (from word 10 + word 6, each
           {address:12, usage:4, usageIndex:4, class:4}), word 8 = number of words after them, of which the first
           VS_EXPORT_COUNT+1 (word 0 bits 20-23) are interpolators {usageIndex:4, usage:4, register:4, mask:4};
           PS: word 1 bits 8-15 count, word 6 mask of the interpolators, descriptors from word 8,
           word 0 bit 18 = reads VPOS (param_gen).
  The header is 6 words (0x18 bytes) long. It is followed, in this order, by the register block (if present,
  then always at 0x18 and starting with 0, 1|3, 0, 0), CTAB, binding table, microcode.
Microcode bit fields after Xenia `src/xenia/gpu/ucode.h` (BSD-3-Clause), re-implemented here in Python.

Difference to the later container 0x102A11xx that XenosRecomp (hedge-dev, `XenosRecomp/shader.h`) reads:
9 header words; 0x0C fieldC, 0x10 CTAB, 0x14 definition table {4 u32, u32 size, float4/int4/bool definitions,
float4 values in the physical part}, 0x18 shader description {physicalOffset, size, ..., VS: vertexElementCount +
elements + interpolators; PS: outputs + interpolators}, 0x1C/0x20 = 0.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from xfat import Archive, load_names  # noqa: E402

MAGIC_PS, MAGIC_VS = 0x102A0E00, 0x102A0E01
USAGE = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
         "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]
REGSET = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}
PCLASS = {0: "scalar", 1: "vector", 2: "matrix_rows", 3: "matrix_columns", 4: "object", 5: "struct"}
PTYPE = {0: "void", 1: "bool", 2: "int", 3: "float", 4: "string", 5: "texture", 6: "texture1d", 7: "texture2d",
         8: "texture3d", 9: "texturecube", 10: "sampler", 11: "sampler1d", 12: "sampler2d", 13: "sampler3d",
         14: "samplercube"}
CF_NAMES = {0: "NOP", 1: "EXEC", 2: "EXEC_END", 3: "COND_EXEC", 4: "COND_EXEC_END", 5: "COND_PRED_EXEC",
            6: "COND_PRED_EXEC_END", 7: "LOOP_START", 8: "LOOP_END", 9: "COND_CALL", 10: "RETURN",
            11: "COND_JMP", 12: "ALLOC", 13: "COND_EXEC_PRED_CLEAN", 14: "COND_EXEC_PRED_CLEAN_END",
            15: "MARK_VS_FETCH_DONE"}
EXEC_OPS = {1, 2, 3, 4, 5, 6, 13, 14}
VFMT = {0: "0(template)", 6: "8_8_8_8", 7: "2_10_10_10", 16: "10_11_11", 17: "11_11_10", 25: "16_16",
        26: "16_16_16_16", 31: "16_16_FLOAT", 32: "16_16_16_16_FLOAT", 33: "32", 34: "32_32", 35: "32_32_32_32",
        36: "32_FLOAT", 37: "32_32_FLOAT", 38: "32_32_32_32_FLOAT", 57: "32_32_32_FLOAT"}
FETCH_OPS = {0: "vfetch", 1: "tfetch", 16: "getBCF", 17: "getCompTexLOD", 18: "getGradients", 19: "getWeights",
             24: "setTexLOD", 25: "setGradientsH", 26: "setGradientsV"}
TEXDIM = {0: "1D", 1: "2D", 2: "3D", 3: "Cube"}
VEC_OPS = ["add", "mul", "max", "min", "seq", "sgt", "sge", "sne", "frc", "trunc", "floor", "mad", "cndeq", "cndge",
           "cndgt", "dp4", "dp3", "dp2add", "cube", "max4", "setp_eq_push", "setp_ne_push", "setp_gt_push",
           "setp_ge_push", "kill_eq", "kill_gt", "kill_ge", "kill_ne", "dst", "maxa"]
SCA_OPS = ["adds", "adds_prev", "muls", "muls_prev", "muls_prev2", "maxs", "mins", "seqs", "sgts", "sges", "snes",
           "frcs", "truncs", "floors", "exp", "logc", "log", "rcpc", "rcpf", "rcp", "rsqc", "rsqf", "rsq", "maxas",
           "maxasf", "subs", "subs_prev", "setp_eq", "setp_ne", "setp_gt", "setp_ge", "setp_inv", "setp_pop",
           "setp_clr", "setp_rstr", "kills_eq", "kills_gt", "kills_ge", "kills_ne", "kills_one", "sqrt", "UNK41",
           "mulsc0", "mulsc1", "addsc0", "addsc1", "subsc0", "subsc1", "sin", "cos", "retain_prev"]


def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def u16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def cstr(b, o, end):
    if o >= end:
        return None
    e = b.find(b"\0", o, end)
    if e < 0:
        return None
    return b[o:e].decode("latin-1")


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


class ParseError(Exception):
    pass


def parse(b):
    """Breaks down a container. Returns a dict, raises ParseError on inconsistencies."""
    if len(b) < 0x28:
        raise ParseError("too short")
    magic, vsz, psz, blk, ctab, bind = struct.unpack_from(">6I", b, 0)
    if magic & 0xFFFFFF00 != 0x102A0E00:
        raise ParseError("Magic %08X" % magic)
    if vsz + psz != len(b) or psz % 12 or psz == 0:
        raise ParseError("sizes %d+%d != %d" % (vsz, psz, len(b)))
    r = {"kind": "VS" if magic & 1 else "PS", "virtualSize": vsz, "physicalSize": psz,
         "order": ("blk@%X " % blk if blk else "") + ("ctab@%X" % ctab if ctab else "noctab") + (" bind>ctab" if bind > ctab else " bind<ctab"), "offsets": {"block": blk, "ctab": ctab, "binding": bind}}
    # register block (literal constants)
    lits, bools, loops, writes = [], [], [], []
    if blk:
        n = u32(b, blk + 16)
        at, end = blk + 24, blk + 24 + n
        if end > vsz:
            raise ParseError("register block too long")
        while at + 4 <= end:
            off, cnt = u16(b, at), u16(b, at + 2)
            at += 4
            if cnt == 0:
                break
            vals = list(struct.unpack_from(">%dI" % cnt, b, at))
            writes.append((off, cnt))
            if 768 <= off < 8960:
                reg = (off - 768) // 16
                if r["kind"] == "PS":
                    reg -= 256
                for k in range(cnt // 4):
                    lits.append((reg + k, vals[4 * k: 4 * k + 4]))
            elif 8960 <= off < 8992:
                for k in range(cnt):
                    bools.append(((off - 8960) // 4 + k, vals[k]))
            elif 8992 <= off < 9120:
                for k in range(cnt):
                    loops.append(((off - 8992) // 4 + k, vals[k]))
            else:
                raise ParseError("register write outside the constants (%d)" % off)
            at += 4 * cnt
        rest = [u16(b, at + 2)] if at + 4 <= end else []
        r["maskedWrites"] = bool(rest and rest[0])
    r["registerWrites"] = writes
    r["floatLiterals"] = lits
    r["boolLiterals"] = bools
    r["loopLiterals"] = loops
    # CTAB
    consts = []
    r["creator"] = r["target"] = ""
    if ctab:
        size = u32(b, ctab)
        c0 = ctab + 4
        cend = c0 + size
        _sz, creator, version, n, info, flags, target = struct.unpack_from(">7I", b, c0)
        r["creator"] = (cstr(b, c0 + creator, cend) or "") if creator else ""
        r["target"] = (cstr(b, c0 + target, cend) or "") if target and target < size else ""
        r["ctabVersion"] = version
        for i in range(n):
            nm, rs, ri, rc, _res, ti, dv = struct.unpack_from(">IHHHHII", b, c0 + info + 20 * i)
            cls, typ, rows, cols, elems, members = struct.unpack_from(">6H", b, c0 + ti)
            consts.append({"name": cstr(b, c0 + nm, cend) or "?", "set": REGSET.get(rs, rs), "reg": ri, "count": rc,
                           "class": PCLASS.get(cls, cls), "type": PTYPE.get(typ, typ), "rows": rows, "cols": cols,
                           "elements": elems})
    r["constants"] = consts
    # binding table
    t = list(struct.unpack_from(">%dI" % ((vsz - bind) // 4), b, bind))
    r["programControl"] = t[0]
    r["bindingWords"] = len(t)
    fetches, interps = [], []
    ninstr = psz // 12
    if r["kind"] == "VS":
        fs = 10 + t[6]
        nf, nw = t[7], t[8]
        if fs + nf + nw > len(t):
            raise ParseError("VS binding table too short")
        ni = min(nw, ((t[0] >> 20) & 0xF) + 1)
        for i in range(nf):
            e = t[fs + i]
            f = {"address": e & 0xFFF, "usage": USAGE[(e >> 12) & 0xF] if (e >> 12) & 0xF < 14 else "?%d" % ((e >> 12) & 0xF),
                 "usageIndex": (e >> 16) & 0xF, "class": (e >> 20) & 0xF}
            if f["address"] >= ninstr:
                raise ParseError("vfetch address outside the microcode")
            fetches.append(f)
        for i in range(ni):
            e = t[fs + nf + i]
            interps.append({"usage": USAGE[(e >> 4) & 0xF], "usageIndex": e & 0xF, "reg": (e >> 8) & 0xF,
                            "mask": (e >> 12) & 0xF})
        r["extraWords"] = nw - ni
    else:
        ni = max((t[1] >> 8) & 0xFF, bin(t[6] & 0xFFFF).count("1"))
        for i in range(ni):
            e = t[8 + i]
            interps.append({"usage": USAGE[(e >> 4) & 0xF], "usageIndex": e & 0xF, "reg": (e >> 8) & 0xF,
                            "mask": (e >> 12) & 0xF})
        r["readsVPos"] = bool((t[0] >> 18) & 1)
        r["psCountField"] = (t[1] >> 8) & 0xFF
        r["psMask"] = t[6] & 0xFFFF
    r["fetchBindings"] = fetches
    r["interpolators"] = interps
    r["ucode"] = b[vsz:]
    r["ucodeSha1"] = hashlib.sha1(b[vsz:]).hexdigest()
    r["microcode"] = analyze_ucode(b[vsz:])
    return r


def analyze_ucode(uc):
    dw = struct.unpack(">%dI" % (len(uc) // 4), uc)
    n = len(dw) // 3
    cf = []
    t = 0
    while t < n:
        w0, w1, w2 = dw[3 * t: 3 * t + 3]
        cf += [w0 | ((w1 & 0xFFFF) << 32), (w1 >> 16) | (w2 << 16)]
        t += 1
        addrs = [x & 0xFFF for x in cf if (x >> 44) & 0xF in EXEC_OPS and (x >> 12) & 7]
        if addrs and t >= min(addrs):
            break
    m = {"cf": collections.Counter(), "boolExec": 0, "boolJmp": 0, "predExec": 0, "predJmp": 0, "uncondJmp": 0,
         "bools": set(), "loops": 0, "calls": 0, "memexport": 0, "predInstr": 0, "setp": 0, "kill": collections.Counter(),
         "exports": collections.Counter(), "fetch": collections.Counter(), "vfetch": [], "tfetch": [],
         "aluVec": collections.Counter(), "aluSca": collections.Counter(), "instructions": 0, "relConst": 0,
         "relTemp": 0}
    seen = set()
    for x in cf:
        op = (x >> 44) & 0xF
        m["cf"][CF_NAMES[op]] += 1
        if op in (3, 4, 13, 14):
            m["boolExec"] += 1
            m["bools"].add((x >> 34) & 0xFF)
        elif op in (5, 6):
            m["predExec"] += 1
        elif op == 11:
            if (x >> 13) & 1:
                m["uncondJmp"] += 1
            elif (x >> 14) & 1:
                m["predJmp"] += 1
            else:
                m["boolJmp"] += 1
                m["bools"].add((x >> 34) & 0xFF)
        elif op in (7, 8):
            m["loops"] += 1
        elif op in (9, 10):
            m["calls"] += 1
        elif op == 12 and (x >> 41) & 3 == 3:
            m["memexport"] += 1
        if op not in EXEC_OPS:
            continue
        addr, cnt, seq = x & 0xFFF, (x >> 12) & 7, (x >> 16) & 0xFFF
        for i in range(cnt):
            k = addr + i
            if k in seen or 3 * k + 2 >= len(dw):
                continue
            seen.add(k)
            m["instructions"] += 1
            w0, w1, w2 = dw[3 * k: 3 * k + 3]
            if (seq >> (2 * i)) & 1:  # Fetch
                fop = w0 & 0x1F
                m["fetch"][FETCH_OPS.get(fop, "op%d" % fop)] += 1
                if fop == 0:
                    if (w1 >> 31) & 1:
                        m["predInstr"] += 1
                    m["vfetch"].append({"address": k, "mini": (w1 >> 30) & 1,
                                        "const": ((w0 >> 20) & 0x1F) * 3 + ((w0 >> 25) & 3),
                                        "format": (w1 >> 16) & 0x3F, "stride": w2 & 0xFF,
                                        "offset": sext((w2 >> 8) & 0x7FFFFF, 23), "signed": (w1 >> 12) & 1,
                                        "integer": (w1 >> 13) & 1, "expAdjust": sext((w1 >> 24) & 0x3F, 6),
                                        "dst": (w0 >> 12) & 0x3F, "dstSwizzle": w1 & 0xFFF})
                else:
                    if (w1 >> 31) & 1:
                        m["predInstr"] += 1
                    m["tfetch"].append({"op": FETCH_OPS.get(fop, fop), "const": (w0 >> 20) & 0x1F,
                                        "dim": TEXDIM[(w2 >> 14) & 3], "denorm": (w0 >> 25) & 1})
            else:  # ALU
                vop = (w2 >> 24) & 0x1F
                sop = (w0 >> 26) & 0x3F
                vmask, smask = (w0 >> 16) & 0xF, (w0 >> 20) & 0xF
                export = (w0 >> 15) & 1
                if (w1 >> 28) & 1:
                    m["predInstr"] += 1
                vname = VEC_OPS[vop] if vop < len(VEC_OPS) else "v%d" % vop
                sname = SCA_OPS[sop] if sop < len(SCA_OPS) else "s%d" % sop
                m["aluVec"][vname] += 1
                m["aluSca"][sname] += 1
                if vname.startswith("setp") or sname.startswith("setp"):
                    m["setp"] += 1
                if vname.startswith("kill"):
                    m["kill"][vname] += 1
                if sname.startswith("kills"):
                    m["kill"][sname] += 1
                if (w1 >> 29) & 1 or (w1 >> 30) & 1 or (w1 >> 31) & 1:
                    m["relConst"] += 1
                if (w0 >> 6) & 1 or (w0 >> 14) & 1:
                    m["relTemp"] += 1
                if export and (vmask or smask):
                    m["exports"][w0 & 0x3F] += 1
    m["bools"] = sorted(m["bools"])
    return m


def cmd_extract(a):
    names = load_names(a.names)
    if a.shader_names:  # shader_names.tsv takes precedence: names.txt contains chance hits like "-kzv"
        for line in open(a.shader_names, encoding="latin-1"):
            p = line.rstrip("\n").split("\t")
            if len(p) >= 4:
                names[int(p[0], 16)] = p[3].lower()
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "fxo"), exist_ok=True)
    os.makedirs(os.path.join(out, "src"), exist_ok=True)
    seen = {}
    rows = []
    srcs = 0
    for arcname in ("shaders.fat", "common.fat"):
        arc = Archive(os.path.join(a.disc, arcname))
        for e in arc.entries:
            nm = names.get(e.hash)
            b = arc.data(e)
            if len(b) >= 4 and u32(b, 0) in (MAGIC_PS, MAGIC_VS):
                sha = hashlib.sha1(b).hexdigest()
                base = (nm.split("\\")[-1] if nm else "%08X.fxo" % e.hash)
                if sha not in seen:
                    fn = base if arcname == "shaders.fat" else "common__" + base
                    seen[sha] = fn
                    with open(os.path.join(out, "fxo", fn), "wb") as f:
                        f.write(b)
                rows.append((arcname, "%08X" % e.hash, nm or "", "VS" if u32(b, 0) & 1 else "PS", sha, seen[sha]))
            elif arcname == "common.fat" and nm and re.search(r"\.(fx|vsh|psh|h|hlsl|fxh|inc|asm|vsa)$", nm):
                dst = os.path.join(out, "src", nm.replace("\\", os.sep))
                if not os.path.abspath(dst).startswith(os.path.join(out, "src") + os.sep):
                    raise ValueError(nm)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(b)
                srcs += 1
    with open(os.path.join(out, "manifest.tsv"), "w", encoding="utf-8") as f:
        f.write("archive\thash\tname\ttype\tsha1\tfile\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    kinds = collections.Counter(r[3] for r in rows)
    print("Containers: %d entries (%s), unique %d; sources: %d" % (len(rows), dict(kinds), len(seen), srcs))
    return 0


def cmd_info(a):
    b = open(a.file, "rb").read()
    r = parse(b)
    m = r.pop("microcode")
    r.pop("ucode")
    m["vfetch"] = m["vfetch"][:16]
    print(json.dumps(r, indent=1, default=str))
    print(json.dumps(m, indent=1, default=lambda o: dict(o) if isinstance(o, collections.Counter) else str(o)))
    return 0


def cmd_scan(a):
    files = sorted(f for f in os.listdir(a.dir) if f.endswith(".fxo"))
    agg = collections.defaultdict(collections.Counter)
    per = []
    errors = collections.Counter()
    layouts = collections.Counter()
    layouts_full = collections.Counter()
    vs_out_sigs = collections.Counter()
    effect_vs, effect_ps = {}, {}
    for fn in files:
        b = open(os.path.join(a.dir, fn), "rb").read()
        try:
            r = parse(b)
        except ParseError as ex:
            errors[str(ex)] += 1
            continue
        m = r["microcode"]
        k = r["kind"]
        agg[k]["shader"] += 1
        sems = {"%s%d" % (it["usage"], it["usageIndex"]) for it in r["interpolators"]}
        for s in sems:
            agg[k + "_interp_sem"][s] += 1
        mt = re.match(r"^(?:common__)?(.+?)_[a-z0-9]+(?:vs|ps)_\d+\.fxo$", fn)
        eff = mt.group(1) if mt else fn
        (effect_vs if k == "VS" else effect_ps).setdefault(eff, []).append((fn, sems))
        row = {"file": fn, "kind": k, "instr": m["instructions"], "boolCtl": int(m["boolExec"] + m["boolJmp"] > 0),
               "boolJmp": int(m["boolJmp"] > 0), "boolExec": int(m["boolExec"] > 0),
               "pred": int(m["predExec"] + m["predJmp"] + m["predInstr"] > 0), "setp": int(m["setp"] > 0),
               "kill": int(sum(m["kill"].values()) > 0), "loops": m["loops"], "calls": m["calls"],
               "memexport": m["memexport"], "relConst": int(m["relConst"] > 0), "relTemp": int(m["relTemp"] > 0)}
        for key in ("boolCtl", "boolJmp", "boolExec", "pred", "setp", "kill", "relConst", "relTemp"):
            agg[k][key] += row[key]
        agg[k]["loops"] += m["loops"] > 0
        agg[k]["calls"] += m["calls"] > 0
        agg[k]["memexport"] += m["memexport"] > 0
        agg[k]["literals"] += len(r["floatLiterals"]) > 0
        agg[k]["boolLiterals"] += len(r["boolLiterals"]) > 0
        agg[k]["loopLiterals"] += len(r["loopLiterals"]) > 0
        agg[k]["ctab_creator_empty"] += r["creator"] == ""
        for c in r["constants"]:
            agg[k + "_constset"][c["set"]] += 1
            if c["set"] == "sampler":
                agg[k + "_samplertype"][c["type"]] += 1
        for op, nn in m["fetch"].items():
            agg[k + "_fetch"][op] += nn
        dims = set()
        for tf in m["tfetch"]:
            if tf["op"] == "tfetch":
                agg[k + "_texdim"][tf["dim"]] += 1
                dims.add(tf["dim"])
        for d in dims:
            agg[k + "_shader_texdim"][d] += 1
        row["texdims"] = ",".join(sorted(dims))
        for kk_, nn in m["kill"].items():
            agg[k + "_killops"][kk_] += nn
        if k == "PS":
            ex = m["exports"]
            colors = sorted(e for e in ex if e < 4)
            depth = 61 in ex
            row["mrt"] = int(len(colors) > 1)
            row["depth"] = int(depth)
            agg[k]["mrt"] += row["mrt"]
            agg[k]["depth"] += row["depth"]
            agg[k]["vpos"] += r["readsVPos"]
            agg[k + "_colors"][",".join(map(str, colors))] += 1
            agg[k + "_interp_count"][len(r["interpolators"])] += 1
            agg[k]["interp_count_zero_field"] += r["psCountField"] == 0 and r["psMask"] != 0
        else:
            sig = tuple(sorted((f["usage"], f["usageIndex"]) for f in r["fetchBindings"]))
            layouts[sig] += 1
            vf = {v["address"]: v for v in m["vfetch"]}
            full = []
            for f in r["fetchBindings"]:
                v = vf.get(f["address"])
                full.append((f["usage"], f["usageIndex"], v["const"] if v else -1, v["offset"] if v else -1,
                             v["stride"] if v else -1, v["format"] if v else -1, v["mini"] if v else -1))
            layouts_full[tuple(full)] += 1
            for v in m["vfetch"]:
                agg["VS_vfetch_format"][VFMT.get(v["format"], v["format"])] += 1
                agg["VS_vfetch_mini"][v["mini"]] += 1
                agg["VS_vfetch_const"][v["const"]] += 1
                agg["VS_vfetch_stride"][v["stride"]] += 1
                agg["VS_vfetch_offset"][v["offset"]] += 1
            bound = {f["address"] for f in r["fetchBindings"]}
            agg["VS"]["vfetch_unbound"] += sum(1 for v in m["vfetch"] if v["address"] not in bound)
            agg["VS"]["vfetch_total"] += len(m["vfetch"])
            agg["VS_inputs"][len(r["fetchBindings"])] += 1
            for f in r["fetchBindings"]:
                agg["VS_usage"]["%s%d" % (f["usage"], f["usageIndex"])] += 1
                agg["VS_fetchclass"][f["class"]] += 1
            vs_out_sigs[tuple(sorted((i["usage"], i["usageIndex"]) for i in r["interpolators"]))] += 1
            # check: the interpolator registers of the binding table = the export registers 0-15 of the microcode
            written = {e for e in m["exports"] if e < 16}
            declared = {i["reg"] for i in r["interpolators"]}
            agg["VS"]["exports_match_interpolators"] += written == declared
            agg["VS"]["exports_written_not_declared"] += bool(written - declared)
            agg["VS"]["declared_not_written"] += bool(declared - written)
            agg["VS"]["writes_position"] += 62 in m["exports"]
            agg["VS"]["writes_psize_misc"] += 63 in m["exports"]
            agg["VS"]["extraWords"] += r.get("extraWords", 0) > 0
            row["inputs"] = "+".join("%s%d" % s for s in sig)
        agg["order"][r["order"].replace("ctab@%X" % r["offsets"]["ctab"], "ctab@" + ("18" if r["offsets"]["ctab"] == 0x18 else "after-block"))] += 1
        per.append(row)
    # linkage: every PS input must be writable by a VS of the same effect
    uncovered = []
    for eff, plist in effect_ps.items():
        vs_union = set()
        for _fn, s in effect_vs.get(eff, []):
            vs_union |= s
        for fn, s in plist:
            if s - vs_union:
                uncovered.append((fn, sorted(s - vs_union)))
    res_link = {"psChecked": sum(len(v) for v in effect_ps.values()), "psUncovered": len(uncovered),
                "examples": uncovered[:10], "effectsWithoutVS": sorted(set(effect_ps) - set(effect_vs))}
    res = {"files": len(files), "parseErrors": dict(errors), "linkage": res_link,
           "agg": {k: dict(v) for k, v in agg.items()},
           "vsInputLayouts": len(layouts), "vsInputLayoutsWithFetchFields": len(layouts_full),
           "vsOutputSignatures": len(vs_out_sigs),
           "topLayouts": [(";".join("%s%d" % s for s in k), n) for k, n in layouts.most_common(15)]}
    print(json.dumps(res, indent=1, default=str))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(res, f, indent=1, default=str)
    if a.csv:
        keys = sorted({k for r in per for k in r})
        with open(a.csv, "w") as f:
            f.write(";".join(keys) + "\n")
            for r in per:
                f.write(";".join(str(r.get(k, "")) for k in keys) + "\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("extract")
    p.add_argument("disc")
    p.add_argument("out")
    p.add_argument("--names")
    p.add_argument("--shader-names", help="shader_names.tsv (hash, type, source, name)")
    p = sub.add_parser("info")
    p.add_argument("file")
    p = sub.add_parser("scan")
    p.add_argument("dir")
    p.add_argument("--json")
    p.add_argument("--csv")
    p = sub.add_parser("carve")
    p.add_argument("image", help="decrypted image, offset 0 = 0x82000000")
    p.add_argument("out")
    a = ap.parse_args()
    return {"extract": cmd_extract, "info": cmd_info, "scan": cmd_scan, "carve": cmd_carve}[a.cmd](a)


def cmd_carve(a):
    """Find containers 0x102A0E00/01 in the EXE image (the D3D library's own shaders) and store them one by one."""
    img = open(a.image, "rb").read()
    os.makedirs(a.out, exist_ok=True)
    n = 0
    for m in re.finditer(rb"\x10\x2a\x0e[\x00\x01]", img):
        o = m.start()
        if o % 4 or o + 0x18 > len(img):
            continue
        vsz, psz = struct.unpack_from(">II", img, o + 4)
        if not (0x18 <= vsz < 0x10000 and 0 < psz < 0x10000 and psz % 12 == 0):
            continue
        b = img[o:o + vsz + psz]
        try:
            r = parse(b)
        except (ParseError, struct.error, IndexError) as ex:
            print("0x%08X: not a valid container (%s)" % (0x82000000 + o, ex))
            continue
        fn = "exe_%08X_%s.fxo" % (0x82000000 + o, r["kind"].lower())
        with open(os.path.join(a.out, fn), "wb") as f:
            f.write(b)
        print("0x%08X %s %d+%d bytes, target %s, %d instructions, sha1 %s" % (
            0x82000000 + o, r["kind"], vsz, psz, r["target"] or "-", r["microcode"]["instructions"], r["ucodeSha1"][:12]))
        n += 1
    print("%d containers" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
