#!/usr/bin/env python3
"""Measurement for the HLE seam: who accesses the device structure directly outside the D3D library?

    devscan.py            summary on stdout, all sites to work/d3d/devscan_sites.tsv

Two independent methods (plus C and D, see pattern_cd):

A) Register tracking from the global device pointer. The engine keeps the D3D device pointer in the global
   DEVICE_GLOBAL (field +0x14 of the block 0x829161A0). Per function outside the library, it is tracked linearly
   which registers carry the pointer (or pointer + constant): load from the global, `mr`, `addi`. After `bl`,
   r0 and r3-r12 count as lost. Every load/store through such a register is counted with its offset, plus
   indirect calls through the setter/getter table (`lwz rT,off(rDev)` ... `mtctr rT` ... `bctrl`).
   The tracking knows no control flow; it is a count with a small error rate, not a proof.

B) Pattern count without the origin of the pointer: the dirty mask idiom
   `ld rX,off(rB)` ... `or|ori|oris` ... `std rY,off(rB)` with off in 0x10..0x30. This also finds sites
   that get the device pointer as an argument or from an object field.

Regions of the device structure (offsets): see REGIONS; the fields were mapped statically from the library
functions that read and write them (the same layout as the [device.fields] in config/predator/renderer.toml).
"""
import os
import sys
from collections import Counter, defaultdict

import d3dlib
from d3dlib import s16

DEVICE_BLOCK = 0x829161A0
DEVICE_GLOBAL = 0x829161B4

REGIONS = [
    (0, 12, "command buffer (pointer, segment start, segment limit)"),
    (12, 16, "reference count"),
    (16, 56, "dirty masks (5 x 64 bits)"),
    (56, 96, "header, unknown"),
    (96, 564, "setter table (117 pointers)"),
    (564, 1032, "getter table (117 pointers)"),
    (1032, 1152, "unknown 1032..1151"),
    (1152, 1920, "fetch constants 0..31"),
    (1920, 6016, "vertex shader constants c0..c255"),
    (6016, 10112, "pixel shader constants c0..c255"),
    (10112, 10272, "bool/loop constants"),
    (10272, 11456, "state 10272..11455 (fence, flags, declaration)"),
    (11456, 11952, "register images"),
    (11952, 12532, "render state caches, surveys"),
    (12532, 12972, "bound objects, viewport, scissor, clip planes"),
    (12972, 0x10000, "rest (tiling, display, gamma, present)"),
]

INT_LOADS = {32, 33, 34, 35, 40, 41, 42, 43}
FP_LOADS = {48, 49, 50, 51}
STORES = {36, 37, 38, 39, 44, 45, 52, 53, 54, 55}
WIDTH = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2, 48: 4, 49: 4, 50: 8, 51: 8,
         36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2, 52: 4, 53: 4, 54: 8, 55: 8, 58: 8, 62: 8}
VOL = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)


def region_of(off):
    for lo, hi, n in REGIONS:
        if lo <= off < hi:
            return n
    return "outside (%d)" % off


def scan_func(im, lo, hi, sites, tblcalls, ringw):
    ws = im.words(lo, (hi - lo) // 4)
    dev = {}      # reg -> offset relative to the device
    const = {}    # reg -> constant (lis/addi)
    tbl = {}      # reg -> table offset (loaded from the device)
    ring = set()  # registers holding the command buffer pointer (loaded from +0)
    ctr = None
    for i, w in enumerate(ws):
        a = lo + 4 * i
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF

        def kill(r):
            dev.pop(r, None)
            const.pop(r, None)
            tbl.pop(r, None)
            ring.discard(r)

        if op == 15:
            kill(rd)
            if ra == 0:
                const[rd] = (imm << 16) & 0xFFFFFFFF
            elif ra in const:
                pass
            continue
        if op == 14:
            if ra != 0 and ra in dev:
                d = dev[ra] + s16(imm)
                kill(rd)
                dev[rd] = d
            elif ra != 0 and ra in const:
                v = (const[ra] + s16(imm)) & 0xFFFFFFFF
                kill(rd)
                const[rd] = v
            elif ra != 0 and ra in ring:
                kill(rd)
                ring.add(rd)
            else:
                kill(rd)
            continue
        if op == 31:
            xo = (w >> 1) & 0x3FF
            rb = (w >> 11) & 31
            if xo == 444 and rd == rb:  # mr ra <- rd
                src = rd
                d, c, t, rg = dev.get(src), const.get(src), tbl.get(src), src in ring
                kill(ra)
                if d is not None:
                    dev[ra] = d
                if c is not None:
                    const[ra] = c
                if t is not None:
                    tbl[ra] = t
                if rg:
                    ring.add(ra)
            elif xo == 467 and ((w >> 11) & 0x3FF) == 0x120:  # mtctr
                ctr = tbl.get(rd)
            elif xo in (151, 183, 215, 247, 407, 439, 663, 695, 727, 759, 149, 181):  # indexed stores
                if ra in dev or rb in dev:
                    sites.append((a, lo, "Wx", dev.get(ra, dev.get(rb)), 0))
            elif xo in (23, 55, 87, 119, 279, 311, 535, 567, 599, 631, 21, 53):  # indexed loads
                if ra in dev or rb in dev:
                    sites.append((a, lo, "Rx", dev.get(ra, dev.get(rb)), 0))
                kill(rd)
            elif xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954, 986, 27, 539, 794, 58, 413):
                kill(ra)
            elif xo not in (0, 32, 4, 68, 86, 54, 278, 246, 1014, 470, 598, 854, 982):
                kill(rd)
            continue
        if op in INT_LOADS or op in FP_LOADS or op in STORES or op in (58, 62):
            off = s16(imm & 0xFFFC) if op in (58, 62) else s16(imm)
            is_store = op in STORES or op == 62
            if ra in dev and ra != 0:
                o = dev[ra] + off
                sites.append((a, lo, "W" if is_store else "R", o, WIDTH[op]))
                if not is_store and op in INT_LOADS:
                    kill(rd)
                    if op in (32, 33):
                        if 96 <= o < 1032:
                            tbl[rd] = o
                        elif o == 0:
                            ring.add(rd)
                    continue
            elif ra in ring and is_store:
                ringw.append((a, lo, off))
                if op == 37:
                    pass
            elif ra in const and ra != 0 and op == 32:
                v = (const[ra] + off) & 0xFFFFFFFF
                kill(rd)
                if v == DEVICE_GLOBAL:
                    dev[rd] = 0
                continue
            if not is_store and (op in INT_LOADS or op == 58):
                kill(rd)
            if op in (33, 35, 41, 43, 37, 39, 45, 49, 51, 53, 55) and ra not in ring:
                kill(ra)
            continue
        if op == 18:
            if w & 1:
                for r in VOL:
                    kill(r)
            continue
        if op == 19:
            xo = (w >> 1) & 0x3FF
            if xo == 528:
                if ctr is not None:
                    tblcalls.append((a, lo, ctr))
                if w & 1:
                    for r in VOL:
                        kill(r)
                ctr = None
            continue
        if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
            if op == 24 and rd in const:
                v = const[rd] | imm
                kill(ra)
                const[ra] = v
            else:
                kill(ra)
        elif op in (7, 8, 12, 13):
            kill(rd)


def pattern_b(im, lo, hi, out):
    """ld/or/std on small offsets (dirty mask idiom)."""
    ws = im.words(lo, (hi - lo) // 4)
    pend = {}  # reg -> (base, off, addr)
    for i, w in enumerate(ws):
        a = lo + 4 * i
        op = w >> 26
        rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 58 and (w & 3) == 0:
            off = s16(imm & 0xFFFC)
            if 0 <= off <= 0x30 and off % 8 == 0:
                pend[rd] = (ra, off, a)
            else:
                pend.pop(rd, None)
        elif op == 62 and (w & 3) == 0:
            off = s16(imm & 0xFFFC)
            for r, (b, o, la) in list(pend.items()):
                if b == ra and o == off and a - la <= 64:
                    out.append((la, lo, off))
                    del pend[r]
                    break
        elif op == 18 and (w & 1):
            pend.clear()
        elif op in (24, 25) or (op == 31 and ((w >> 1) & 0x3FF) in (444, 412)):
            # or/ori/oris: the result register inherits the source
            src = rd
            src2 = (w >> 11) & 31
            for s in (src, src2) if op == 31 else (src,):
                if s in pend:
                    pend[ra] = pend[s]
                    break


CONST_DISP = {0x784: "VS", 0x788: "VS", 0x78C: "VS", 0x1784: "PS", 0x1788: "PS", 0x178C: "PS"}


def pattern_cd(im, lo, hi, outc, outd):
    """C: stfs with displacement 0x784/0x788/0x78C (device+1920+16*reg, y/z/w) or 0x1784.. (pixel): the inline
    expanded Set*ShaderConstantF for one vector.
    D: segment limit check of the command buffer: lwz rA,0(rB) / lwz rC,8(rB) / cmplw rA,rC within 4 instructions."""
    ws = im.words(lo, (hi - lo) // 4)
    hit = {}
    for i, w in enumerate(ws):
        op = w >> 26
        if op == 52:
            k = CONST_DISP.get(w & 0xFFFF)
            if k:
                hit.setdefault((k, (w >> 16) & 31, i // 8), set()).add(w & 0xFFFF)
        if op == 32 and (w & 0xFFFF) == 0:
            rb, r0 = (w >> 16) & 31, (w >> 21) & 31
            r8 = None
            for j in range(max(0, i - 3), min(len(ws), i + 4)):
                w2 = ws[j]
                if (w2 >> 26) == 32 and (w2 & 0xFFFF) == 8 and ((w2 >> 16) & 31) == rb and j != i:
                    r8 = (w2 >> 21) & 31
            if r8 is not None:
                for j in range(i + 1, min(len(ws), i + 5)):
                    w2 = ws[j]
                    if (w2 >> 26) == 31 and ((w2 >> 1) & 0x3FF) == 32 and {(w2 >> 16) & 31, (w2 >> 11) & 31} == {r0, r8}:
                        # plus the write idiom `stwu rX,4(rP)` in the next 24 instructions (rP = r0, or r11/r3
                        # after the segment switch)
                        for m in range(j + 1, min(len(ws), j + 25)):
                            w3 = ws[m]
                            if (w3 >> 26) == 37 and (w3 & 0xFFFF) == 4:
                                outd.append((lo + 4 * i, lo))
                                break
                        break
    for (k, r, blk), s in hit.items():
        if len(s) >= 3:
            outc.append((lo + 32 * blk, lo, k))


def main():
    im = d3dlib.Img()
    sites, tblcalls, ringw, pat = [], [], [], []
    patc, patd = [], []
    nfun = 0
    for f, e in im.funcs:
        if d3dlib.D3D_LO <= f < d3dlib.D3D_HI:
            continue
        if not (d3dlib.TEXT_LO <= f < d3dlib.TEXT_HI):
            continue
        nfun += 1
        scan_func(im, f, e, sites, tblcalls, ringw)
        pattern_b(im, f, e, pat)
        pattern_cd(im, f, e, patc, patd)

    print("C) inline Set*ShaderConstantF (stfs to +0x784/8/C or +0x1784/8/C, per register block): %d sites in %d "
          "functions" % (len(patc), len({f for _, f, _ in patc})))
    for f in sorted({f for _, f, _ in patc}):
        ks = [k for _, g, k in patc if g == f]
        print("  %08X: %d x VS, %d x PS%s" % (f, ks.count("VS"), ks.count("PS"),
                                            "  (D3DX)" if 0x8264A980 <= f < d3dlib.D3D_LO else ""))
    print("D) segment limit check of the command buffer (lwz 0 / lwz 8 / cmplw) outside the library: %d sites "
          "in %d functions: %s" % (len(patd), len({f for _, f in patd}),
                                   " ".join("%08X" % a for a, _ in patd[:12])))
    print()

    p =os.path.join(d3dlib.OUT, "devscan_sites.tsv")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write("site\tfunction\tkind\toffset\twidth\tregion\n")
        for a, f, k, o, wd in sites:
            fh.write("%08X\t%08X\t%s\t%d\t%d\t%s\n" % (a, f, k, o if o is not None else -1, wd, region_of(o or 0)))
        for a, f, o in tblcalls:
            fh.write("%08X\t%08X\tCALL\t%d\t4\t%s\n" % (a, f, o, region_of(o)))
        for a, f, o in ringw:
            fh.write("%08X\t%08X\tRING\t%d\t4\twrite through the command buffer pointer\n" % (a, f, o))
        for a, f, o in pat:
            fh.write("%08X\t%08X\tPEND\t%d\t8\tpattern B\n" % (a, f, o))

    print("Functions outside the D3D library examined: %d" % nfun)
    print()
    print("A) Accesses through the global device pointer [%08X]" % DEVICE_GLOBAL)
    by = defaultdict(lambda: [0, 0, set()])
    for a, f, k, o, wd in sites:
        r = "indexed (offset computed at run time)" if k.endswith("x") else region_of(o or 0)
        by[r][0 if k.startswith("R") else 1] += 1
        by[r][2].add(f)
    print("%-58s %7s %7s %6s" % ("Region", "Reads", "Writes", "Funcs"))
    for lo, hi, n in REGIONS:
        if n in by:
            print("%-58s %7d %7d %6d" % (n, by[n][0], by[n][1], len(by[n][2])))
    for n in by:
        if n.startswith("outside") or n.startswith("indexed"):
            print("%-58s %7d %7d %6d" % (n, by[n][0], by[n][1], len(by[n][2])))
    wfun = {f for a, f, k, o, wd in sites if k.startswith("W")}
    print("Write sites in total: %d in %d functions; read sites: %d" % (
        sum(1 for s in sites if s[2].startswith("W")), len(wfun), sum(1 for s in sites if s[2].startswith("R"))))
    print()
    print("Indirect calls through the tables in the device: %d sites in %d functions" % (
        len(tblcalls), len({f for _, f, _ in tblcalls})))
    c = Counter(o for _, _, o in tblcalls)
    print("  most frequent table offsets: " + ", ".join("+%d: %d" % (o, n) for o, n in c.most_common(40)))
    print()
    print("Writes through a command buffer pointer loaded from device+0: %d sites in %d functions" % (
        len(ringw), len({f for _, f, _ in ringw})))
    for a, f, o in ringw[:20]:
        print("  %08X in %08X (+%d)" % (a, f, o))
    print()
    print("B) Pattern ld/or/std on +0x10..+0x30 (without origin): %d sites in %d functions" % (
        len(pat), len({f for _, f, _ in pat})))
    c = Counter(o for _, _, o in pat)
    print("  by offset: " + ", ".join("+0x%X: %d" % (o, n) for o, n in sorted(c.items())))
    afun = {f for a, f, k, o, wd in sites}
    pfun = {f for _, f, _ in pat}
    print("  functions only in B (device pointer not from the global): %d" % len(pfun - afun))
    print("  examples: " + " ".join("%08X" % f for f in sorted(pfun - afun)[:16]))
    # most frequent write offsets
    c = Counter((o, wd) for a, f, k, o, wd in sites if k == "W")
    print()
    print("Most frequent write offsets (A): " + ", ".join("+%d/%d: %d" % (o, wd, n) for (o, wd), n in c.most_common(40)))
    c = Counter(f for a, f, k, o, wd in sites if k == "W")
    print("Functions with the most write sites: " + ", ".join("%08X: %d" % (f, n) for f, n in c.most_common(16)))
    print("-> %s" % p)


if __name__ == "__main__":
    main()
