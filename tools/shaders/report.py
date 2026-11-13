#!/usr/bin/env python3
"""report.py: summarise metrics from an fcshaders run (results.tsv, interface.jsonl) and the containers.

Usage: report.py <run-folder> <fxo-folder>
Prints Markdown tables (sizes, failures, interface, vertex layouts, usage/class of the vfetch bindings).
"""
import collections
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fxo import VFMT, parse  # noqa: E402


def main():
    run, fxodir = sys.argv[1], sys.argv[2]
    rows = []
    with open(os.path.join(run, "results.tsv"), encoding="utf-8") as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            rows.append(dict(zip(head, line.rstrip("\n").split("\t"))))
    iface = [json.loads(l) for l in open(os.path.join(run, "interface.jsonl"), encoding="utf-8")]
    print("## Result per type\n\n| Type | Containers | translated+DXIL+SPIR-V | Failed | DXIL total | DXIL mean | SPIR-V total | SPIR-V mean |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|")
    for k in ("VS", "PS", ""):
        rs = [r for r in rows if not k or r["kind"] == k]
        ok = [r for r in rs if r["stage"] == "ok"]
        dx = sum(int(r["dxil"]) for r in ok)
        sp = sum(int(r["spirv"]) for r in ok)
        print("| %s | %d | %d | %d | %.1f MB | %.1f KB | %.1f MB | %.1f KB |" % (
            k or "total", len(rs), len(ok), len(rs) - len(ok), dx / 1e6, dx / max(1, len(ok)) / 1e3, sp / 1e6,
            sp / max(1, len(ok)) / 1e3))
    disc = [r for r in rows if not r["name"].startswith("common__")]
    print("\nshaders.dat only (without the 7 differing copies from common.dat): %d of %d ok" % (
        sum(r["stage"] == "ok" for r in disc), len(disc)))
    print("\n## Failures\n")
    for r in rows:
        if r["stage"] != "ok":
            print("- `%s` (%s, step %s): %s" % (r["name"], r["kind"], r["stage"], r["error"]))
    # interface
    print("\n## Interface (from interface.jsonl)\n")
    c = collections.Counter()
    sets = collections.Counter()
    maxreg = collections.Counter()
    for s in iface:
        k = s["kind"]
        c[k + " shaders"] += 1
        c[k + " with literal constants"] += bool(s["floatLiterals"])
        c[k + " with dynamic register addressing"] += s["dynamicRegisters"]
        c[k + " with general control flow (state machine)"] += s["generalControlFlow"]
        c[k + " reads VPOS"] += s["readsVPos"]
        for con in s["constants"]:
            sets[(k, con["set"])] += 1
            end = con["reg"] + con["count"]
            maxreg[(k, con["set"])] = max(maxreg[(k, con["set"])], end)
        c[k + " sampler bindings (total)"] += len(s["samplers"])
        c[k + " max. samplers per shader"] = max(c[k + " max. samplers per shader"], len(s["samplers"]))
        c[k + " max. temp registers"] = max(c[k + " max. temp registers"], s["tempRegisters"])
        c[k + " max. vertex inputs"] = max(c[k + " max. vertex inputs"], len(s["vertexInputs"]))
    for k, v in sorted(c.items()):
        print("- %s: %d" % (k, v))
    print("\n| Type | Register set | CTAB entries | highest register + 1 |\n|---|---|---:|---:|")
    for (k, st), n in sorted(sets.items()):
        print("| %s | %s | %d | %d |" % (k, st, n, maxreg[(k, st)]))
    lay = collections.Counter(tuple(s["vertexInputs"]) for s in iface if s["kind"] == "VS")
    lay_set = collections.Counter(tuple(sorted(s["vertexInputs"])) for s in iface if s["kind"] == "VS")
    print("\nVertex inputs: %d distinct sequences, %d distinct sets (usage+index)" % (len(lay), len(lay_set)))
    # vfetch bindings: usage against the class field, components from the destination swizzle
    cls = collections.Counter()
    fmt = collections.Counter()
    for fn in sorted(os.listdir(fxodir)):
        if not fn.endswith(".fxo"):
            continue
        r = parse(open(os.path.join(fxodir, fn), "rb").read())
        if r["kind"] != "VS":
            continue
        vf = {v["address"]: v for v in r["microcode"]["vfetch"]}
        for b in r["fetchBindings"]:
            v = vf[b["address"]]
            cls[(b["usage"], b["class"])] += 1
            if v["format"] or v["const"] != 95:
                fmt[(fn, b["usage"], b["usageIndex"], VFMT.get(v["format"], v["format"]), v["const"], v["stride"],
                     v["offset"])] += 1
    print("\n| Usage | Class field | Bindings |\n|---|---:|---:|")
    for (u, k), n in sorted(cls.items()):
        print("| %s | %d | %d |" % (u, k, n))
    print("\nvfetch with format != 0 or fetch constant != 95:")
    for k, n in sorted(fmt.items()):
        print("- %s: %s%d format %s, constant %d, stride %d, offset %d (%dx)" % (k + (n,)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
