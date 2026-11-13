#!/usr/bin/env python3
"""Metrics of a ReXGlue codegen run (counts in the generated code, for reports).

    python -I tools/codegen_stats.py <generated_dir>

Counts files, bytes, lines, generated functions (REX_FUNC definitions), registered addresses
(SetFunction), jump tables (switch), indirect calls, setjmp/longjmp sites, traps, unsupported
instructions and guest memory accesses. Prints no code.
"""
import os
import re
import sys

PATTERNS = {
    "DEFINE_REX_FUNC (generated functions)": re.compile(r"^DEFINE_REX_FUNC\("),
    "switch (jump tables)": re.compile(r"\bswitch\s*\("),
    "REX_CALL_INDIRECT_FUNC": re.compile(r"REX_CALL_INDIRECT_FUNC\("),
    "REX_TAIL_INDIRECT_FUNC": re.compile(r"REX_TAIL_INDIRECT_FUNC\("),
    "REX_MUSTTAIL": re.compile(r"REX_MUSTTAIL"),
    "ppc_setjmp calls": re.compile(r"\bppc_setjmp\(ctx"),
    "ppc_longjmp calls": re.compile(r"\bppc_longjmp\(ctx"),
    "__builtin_trap": re.compile(r"__builtin_trap"),
    "ppc_trap": re.compile(r"\bppc_trap\b"),
    "REX_UNIMPLEMENTED calls": re.compile(r"\bREX_UNIMPLEMENTED\((?!addr)"),
    "guest loads (REX_LOAD_*)": re.compile(r"\bREX_LOAD_[A-Z0-9]+\("),
    "guest stores (REX_STORE_*)": re.compile(r"\bREX_STORE_[A-Z0-9]+\("),
    "stack loads (REX_STACK_LOAD_*)": re.compile(r"\bREX_STACK_LOAD_[A-Z0-9]+\("),
    "stack stores (REX_STACK_STORE_*)": re.compile(r"\bREX_STACK_STORE_[A-Z0-9]+\("),
    "MMIO (REX_MM_*)": re.compile(r"\bREX_MM_[A-Z0-9_]+\("),
}


def main():
    gendir = sys.argv[1]
    counts = {k: 0 for k in PATTERNS}
    nfiles = {"cpp": 0, "h": 0, "other": 0}
    nbytes = {"cpp": 0, "h": 0, "other": 0}
    lines = 0
    setfunc = 0
    for f in sorted(os.listdir(gendir)):
        p = os.path.join(gendir, f)
        if not os.path.isfile(p):
            continue
        kind = "cpp" if f.endswith(".cpp") else "h" if f.endswith(".h") else "other"
        nfiles[kind] += 1
        nbytes[kind] += os.path.getsize(p)
        if kind == "other":
            continue
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                lines += 1
                if "SetFunction(" in line:
                    setfunc += 1
                for k, rx in PATTERNS.items():
                    counts[k] += len(rx.findall(line))
    print(f"Files: {sum(nfiles.values())} ({nfiles['cpp']} .cpp, {nfiles['h']} .h, {nfiles['other']} other)")
    print(f"Bytes: {sum(nbytes.values())} (.cpp {nbytes['cpp']}, .h {nbytes['h']}, other {nbytes['other']})")
    print(f"Lines .cpp/.h: {lines}")
    print(f"SetFunction (registered addresses): {setfunc}")
    for k, v in counts.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()
