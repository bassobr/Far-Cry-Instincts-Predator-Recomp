"""Checks the HDR10 output from present dumps ([display] hdr, FCP_DUMP_PRESENT=1; also without an HDR display).

With an HDR10 swapchain every present dump <name>.png is the SDR picture the HDR10 pass got (gamma ramp, SMAA, bars;
no overlay), <name>_hdr10.raw the swapchain image as presented (R10G10B10A2, PQ / BT.2020) and <name>_hdr10.txt the
parameters (src/renderer/core/game_renderer.cpp, DumpHdr). For every dump:
  - PQ (SMPTE ST 2084) decoded to nits, BT.2020 back to BT.709 (linear)
  - outside the picture: black (code 0)
  - SDR white against the paper white, the brightest pixel against the peak, greys stay grey
  - the HDR picture brought back to SDR (inverse of the highlight stretch, / paper white, gamma 1/gamma) against the SDR
    picture: largest and mean difference in 8-bit levels, share above 1 level, PSNR
  - the HDR picture expected from the SDR one (forward, as the shader): largest difference in 10-bit PQ codes and
    Delta E ITP (BT.2124; 1 = about one just noticeable difference). Going back to 8-bit SDR amplifies the 10-bit
    rounding in dark channels of saturated colours (BT.2020 -> BT.709 subtracts large values), so the SDR difference
    there can be several levels while Delta E ITP stays below 1.
  - optional (--pfm): <name>_hdr10_linear.pfm, linear BT.709 light in scRGB units (1.0 = 80 nits), and
    <name>_hdr10_preview.png, the HDR picture mapped back to SDR for looking at it
Pure Python (no numpy); --step n checks every n-th pixel in both directions (faster on large dumps).
Usage: python tools/hdr_check.py [--step n] [--pfm] <dump folder or _hdr10.raw> [...]
Adapted from FCI tools/hdr_check.py (Far Cry Instincts Recomp, MIT, same author).
"""
import glob
import math
import os
import struct
import sys
import zlib

M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
TO_2020 = [[0.627404, 0.329283, 0.043313], [0.069097, 0.919540, 0.011362], [0.016391, 0.088013, 0.895595]]


def inv3(m):
    a, b, c = m[0]
    d, e, f = m[1]
    g, h, i = m[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
            [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
            [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]


TO_709 = inv3(TO_2020)
LUM = (0.2126, 0.7152, 0.0722)


def mul(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def pq_to_nits(e):
    p = e ** (1 / M2)
    return 10000 * (max(p - C1, 0) / (C2 - C3 * p)) ** (1 / M1)


def nits_to_pq(n):
    y = min(max(n / 10000, 0), 1) ** M1
    return ((C1 + C2 * y) / (1 + C3 * y)) ** M2


# ICtCp (BT.2100) from linear BT.2020 nits; Delta E ITP (BT.2124): 1 is about one just noticeable difference
TO_LMS = [[1688 / 4096, 2146 / 4096, 262 / 4096], [683 / 4096, 2951 / 4096, 462 / 4096], [99 / 4096, 309 / 4096, 3688 / 4096]]
TO_ICTCP = [[0.5, 0.5, 0], [6610 / 4096, -13613 / 4096, 7003 / 4096], [17933 / 4096, -17390 / 4096, -543 / 4096]]


def ictcp(nits2020):
    return mul(TO_ICTCP, tuple(nits_to_pq(v) for v in mul(TO_LMS, tuple(max(v, 0) for v in nits2020))))


def delta_e_itp(a2020, b2020):
    a, b = ictcp(a2020), ictcp(b2020)
    return 720 * math.sqrt((a[0] - b[0]) ** 2 + (0.5 * (a[1] - b[1])) ** 2 + (a[2] - b[2]) ** 2)


def read_raw(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"FCPHDR10", path
    w, h, px, py, pw, ph = struct.unpack("<6I", data[8:32])
    return w, h, (px, py, pw, ph), struct.unpack_from("<%dI" % (w * h), data, 32)


def read_png(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype in (2, 6) and interlace == 0, "8-bit RGB/RGBA PNG only"
            bpp = 3 if ctype == 2 else 4
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                line[i] = (line[i] + a) & 255
            elif ft == 2:
                line[i] = (line[i] + b) & 255
            elif ft == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, bytes(out)


def write_png(path, w, h, rgb):
    rows = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def read_params(path):
    p = {}
    for line in open(path):
        parts = line.split()
        if len(parts) == 2:
            try:
                p[parts[0]] = float(parts[1])
            except ValueError:
                p[parts[0]] = parts[1]
    return p


def stretch(lin, gain, knee):
    if gain <= 1:
        return lin
    l = sum(LUM[i] * lin[i] for i in range(3))
    t = min(max((l - knee) / (1 - knee), 0), 1)
    k = (l + (gain - 1) * t * t) / max(l, 1e-6)
    return tuple(v * k for v in lin)


def unstretch_lum(l_out, gain, knee):
    if gain <= 1 or l_out <= 1e-9:
        return l_out
    lo, hi = 0.0, 1.0
    for _ in range(40):
        mid = (lo + hi) / 2
        t = min(max((mid - knee) / (1 - knee), 0), 1)
        if mid + (gain - 1) * t * t < l_out:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def check(raw_path, step, pfm):
    stem = raw_path[:-len("_hdr10.raw")]
    params = read_params(stem + "_hdr10.txt")
    w, h, (px, py, pw, ph), words = read_raw(raw_path)
    sw, shh, bpp, sdr = read_png(stem + ".png")
    assert (sw, shh) == (w, h), "SDR picture %dx%d, HDR10 image %dx%d" % (sw, shh, w, h)
    paper, peak, gain, knee = params["paper_white"], params["peak"], params["gain"], params["knee"]
    gamma = params.get("gamma", 2.2)
    r = {"paper": paper, "peak": peak, "gain": gain, "gamma": gamma}
    outside = 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            if not (px <= x < px + pw and py <= y < py + ph):
                word = words[y * w + x]
                outside = max(outside, word & 1023, (word >> 10) & 1023, (word >> 20) & 1023)
    r["outside_max_code"] = outside
    max_code = max_nits = 0.0
    white_n, white_sum, grey_bad = 0, 0.0, 0
    diff_max, diff_sum, diff_over1, n = 0.0, 0.0, 0, 0
    de_max, de_sum, de_over1 = 0.0, 0.0, 0
    se = 0.0
    fwd_max = 0
    lin_cache = {}
    pfm_rows = [] if pfm else None
    preview = bytearray(w * h * 3) if pfm else None
    for y in range(0, h, step) if not pfm else range(h):
        row = []
        for x in range(0, w, step) if not pfm else range(w):
            word = words[y * w + x]
            code = (word & 1023, (word >> 10) & 1023, (word >> 20) & 1023)
            if code not in lin_cache:
                nits2020 = tuple(pq_to_nits(c / 1023) for c in code)
                lin_cache[code] = mul(TO_709, nits2020)
            lin709 = lin_cache[code]
            if pfm:
                row.append(lin709)
                # back to SDR for the preview: inverse stretch, / paper, gamma
                lin = [max(v / paper, 0) for v in lin709]
                lo = sum(LUM[i] * lin[i] for i in range(3))
                k = unstretch_lum(lo, gain, knee) / lo if lo > 1e-12 else 1
                for i in range(3):
                    preview[(y * w + x) * 3 + i] = int(round(min(max(lin[i] * k, 0), 1) ** (1 / gamma) * 255))
                if (x % step) or (y % step):
                    continue
            if not (px <= x < px + pw and py <= y < py + ph):
                continue
            o = (y * w + x) * bpp
            s = (sdr[o], sdr[o + 1], sdr[o + 2])
            max_code = max(max_code, *code)
            max_nits = max(max_nits, *lin709)
            if s[0] == s[1] == s[2] and not (code[0] == code[1] == code[2]):
                grey_bad += 1
            if s == (255, 255, 255):
                white_n += 1
                white_sum += sum(LUM[i] * lin709[i] for i in range(3))
            lin = [max(v / paper, 0) for v in lin709]
            lo = sum(LUM[i] * lin[i] for i in range(3))
            k = unstretch_lum(lo, gain, knee) / lo if lo > 1e-12 else 1
            for i in range(3):
                back = min(max(lin[i] * k, 0), 1) ** (1 / gamma) * 255
                d = abs(back - s[i])
                diff_max = max(diff_max, d)
                diff_sum += d
                se += d * d
            diff_over1 += any(abs(min(max(lin[i] * k, 0), 1) ** (1 / gamma) * 255 - s[i]) > 1 for i in range(3))
            # forward: the expected PQ codes from the SDR picture
            want = mul(TO_2020, stretch(tuple((v / 255) ** gamma for v in s), gain, knee))
            want_nits = tuple(min(want[i] * paper, peak) for i in range(3))
            for i in range(3):
                fwd_max = max(fwd_max, abs(round(nits_to_pq(want_nits[i]) * 1023) - code[i]))
            de = delta_e_itp(tuple(pq_to_nits(c / 1023) for c in code), want_nits)
            de_max, de_sum, de_over1 = max(de_max, de), de_sum + de, de_over1 + (de > 1)
            n += 1
        if pfm:
            pfm_rows.append(row)
    r.update(max_code=int(max_code), max_nits=max_nits, white_px=white_n,
             white_nits=white_sum / white_n if white_n else None, grey_not_grey=grey_bad,
             sdr_max_diff=diff_max, de_max=de_max, de_mean=de_sum / max(n, 1), de_over1=100.0 * de_over1 / max(n, 1), sdr_mean_diff=diff_sum / max(n * 3, 1), sdr_over_1=100.0 * diff_over1 / max(n, 1),
             psnr=99.0 if se == 0 else 10 * math.log10(255 ** 2 / (se / max(n * 3, 1))), fwd_max_code=fwd_max, pixels=n)
    if pfm:
        with open(stem + "_hdr10_linear.pfm", "wb") as f:
            f.write(b"PF\n%d %d\n-1.0\n" % (w, h))
            for row in reversed(pfm_rows):  # PFM: bottom row first
                f.write(struct.pack("<%df" % (3 * w), *[v / 80.0 for px_ in row for v in px_]))
        write_png(stem + "_hdr10_preview.png", w, h, bytes(preview))
    return r


def main():
    args = sys.argv[1:]
    step, pfm = 1, False
    if "--pfm" in args:
        args.remove("--pfm")
        pfm = True
    if "--step" in args:
        i = args.index("--step")
        step = max(1, int(args[i + 1]))
        del args[i:i + 2]
    raws = []
    for a in args:
        raws += [a] if a.endswith("_hdr10.raw") else sorted(glob.glob(os.path.join(a, "*_hdr10.raw")))
    if not raws:
        print("no *_hdr10.raw dumps")
        return 1
    for raw in raws:
        r = check(raw, step, pfm)
        print("%s: paper %.0f peak %.0f gain %.2f gamma %.2f | outside max code %d | max code %d, max %.1f nits | SDR white "
              "%d px -> %s nits | greys not grey %d | back to SDR: max diff %.2f, mean %.3f, >1 level %.3f %%, PSNR %.1f dB | "
              "forward: max %d PQ codes, dE ITP mean %.3f max %.2f >1 %.3f %% | %d px"
              % (os.path.basename(raw), r["paper"], r["peak"], r["gain"], r["gamma"], r["outside_max_code"], r["max_code"],
                 r["max_nits"], r["white_px"], "%.1f" % r["white_nits"] if r["white_nits"] is not None else "-",
                 r["grey_not_grey"], r["sdr_max_diff"], r["sdr_mean_diff"], r["sdr_over_1"], r["psnr"], r["fwd_max_code"],
                 r["de_mean"], r["de_max"], r["de_over1"], r["pixels"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
