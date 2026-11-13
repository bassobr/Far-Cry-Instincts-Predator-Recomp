"""Compares the machine code of two sets of object files function by function.

Purpose: for predator_code.dll (built on the player's first start) the generated game code is compiled freestanding
(rex/ppc/guest_prelude.h with REX_GUEST_FREESTANDING, headers from rexglue/include/rex/guest_std, -nostdlibinc). This
script checks that this yields the same machine code as the hosted build (MSVC STL and UCRT headers).

  python -I tools/compare_objects.py <llvm-objdump> <folder_a> <folder_b> [--suffix-a .cpp.obj] [--suffix-b .obj]

Pairs: same file name without extension. Addresses and jump targets are removed before comparing.
Output: per file the functions with different code, functions only in A or only in B; the totals at the end.
Exit code 1 if a function that occurs in both has different code.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

RE_SYM = re.compile(r"^<(.+)>:$")
RE_ADDR = re.compile(r"0x[0-9a-f]+|<[^>]*>")


def functions(objdump: str, obj: Path) -> dict[str, str]:
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", "--no-leading-addr", str(obj)],
                         capture_output=True, text=True, check=True).stdout
    funcs: dict[str, list[str]] = {}
    current = None
    for line in out.splitlines():
        m = RE_SYM.match(line.strip())
        if m:
            current = m.group(1)
            funcs.setdefault(current, [])
            continue
        if current is not None and line.strip() and not line.startswith("Disassembly of section"):
            funcs[current].append(RE_ADDR.sub("", line).strip())
    return {k: "\n".join(v) for k, v in funcs.items()}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("objdump")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--suffix-a", default=".cpp.obj")
    ap.add_argument("--suffix-b", default=".obj")
    ap.add_argument("--glob", default="*")
    args = ap.parse_args()
    a_dir, b_dir = Path(args.a), Path(args.b)
    totals = {"files": 0, "same": 0, "differ": 0, "only_a": 0, "only_b": 0}
    only_a_names: dict[str, int] = {}
    only_b_names: dict[str, int] = {}
    for b_obj in sorted(b_dir.glob(args.glob + args.suffix_b)):
        stem = b_obj.name[: -len(args.suffix_b)]
        a_obj = a_dir / (stem + args.suffix_a)
        if not a_obj.exists():
            continue
        fa, fb = functions(args.objdump, a_obj), functions(args.objdump, b_obj)
        totals["files"] += 1
        for name in fa.keys() & fb.keys():
            if fa[name] == fb[name]:
                totals["same"] += 1
            else:
                totals["differ"] += 1
                print(f"{stem}: different code in {name}")
        for name in fa.keys() - fb.keys():
            totals["only_a"] += 1
            only_a_names[name] = only_a_names.get(name, 0) + 1
        for name in fb.keys() - fa.keys():
            totals["only_b"] += 1
            only_b_names[name] = only_b_names.get(name, 0) + 1
    print("only in A:", ", ".join(f"{n} ({c})" for n, c in sorted(only_a_names.items())) or "-")
    print("only in B:", ", ".join(f"{n} ({c})" for n, c in sorted(only_b_names.items())) or "-")
    print(f"files {totals['files']}, functions same {totals['same']}, different {totals['differ']}, "
          f"only in A {totals['only_a']}, only in B {totals['only_b']}")
    return 1 if totals["differ"] else 0


if __name__ == "__main__":
    sys.exit(main())
