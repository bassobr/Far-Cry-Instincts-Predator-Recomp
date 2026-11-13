"""Create the VS/PS pair list for the pipeline measurement programs.

Usage: python -I pairs.py <run>/dxil <output.tsv>

Only a runtime trace knows the real pairing. Here pairs are formed per effect and technique (name without
"vs_N"/"ps_N"): VS i with PS (i mod count), so that every VS and every PS occurs at least once. Shaders without
a counterpart get a fixed partner. All shaders have the same interpolator signature, every pair links.

Columns: group, vs, ps. The group (0/1) splits the effects into two halves without shared shaders, so that a
cold run per half with a different thread count is possible (the driver cache cannot be cleared).
"""
import collections
import os
import re
import sys

FALLBACK_VS = "2d_mainvs_0"
FALLBACK_PS = "alphawrite_mainps_0"


def main():
    src, out = sys.argv[1], sys.argv[2]
    names = sorted(os.path.splitext(f)[0] for f in os.listdir(src))
    eff = collections.defaultdict(lambda: {"vs": [], "ps": []})
    for n in names:
        m = re.match(r"^(.*?)(vs|ps)_(\d+)$", n)
        if not m:
            print("unknown name pattern:", n)
            continue
        eff[m.group(1)][m.group(2)].append((int(m.group(3)), n))
    groups = [0, 0]
    rows = []
    for key in sorted(eff, key=lambda k: -(len(eff[k]["vs"]) + len(eff[k]["ps"]))):
        vs = [n for _, n in sorted(eff[key]["vs"])]
        ps = [n for _, n in sorted(eff[key]["ps"])]
        g = 0 if groups[0] <= groups[1] else 1
        if not vs:
            pairs = [(FALLBACK_VS, p) for p in ps]
        elif not ps:
            pairs = [(v, FALLBACK_PS) for v in vs]
        else:
            pairs = [(vs[i % len(vs)], ps[i % len(ps)]) for i in range(max(len(vs), len(ps)))]
        groups[g] += len(pairs)
        rows += [(g, v, p) for v, p in pairs]
    with open(out, "w", newline="\n") as f:
        for g, v, p in rows:
            f.write(f"{g}\t{v}\t{p}\n")
    print(f"{len(rows)} pairs ({groups[0]} in group 0, {groups[1]} in group 1), {len(eff)} effect techniques, "
          f"{len(names)} shaders")


if __name__ == "__main__":
    main()
