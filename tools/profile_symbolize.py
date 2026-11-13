"""Names host addresses in the FCP_PROFILE log (sampling profile of the main thread).

  python -I tools/profile_symbolize.py <predator.log> <bin folder> [filter regex]

Replaces `module+0xOFF` (predator.exe and rex*.dll with a PDB in the bin folder) by `module+0xOFF<function>` using
llvm-symbolizer from <toolchain>\\llvm\\bin and `ntdll.dll+0xOFF` by the nearest export. Prints only the lines
with "profile:" (optionally also filtered by the regex). For analysis only, nothing is written.
Toolchain: FCP_TOOLCHAIN_DIR, else <FCP_SHARED_ROOT>\\toolchain, else (in a Git worktree) the toolchain of the
main checkout, else <repo>\\toolchain.
"""
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def toolchain_dir():
    if os.environ.get("FCP_TOOLCHAIN_DIR"):
        return Path(os.environ["FCP_TOOLCHAIN_DIR"])
    if os.environ.get("FCP_SHARED_ROOT"):
        return Path(os.environ["FCP_SHARED_ROOT"]) / "toolchain"
    try:
        common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        if common:
            return Path(common).resolve().parent / "toolchain"
    except (OSError, subprocess.CalledProcessError):
        pass
    return ROOT / "toolchain"


SYMBOLIZER = toolchain_dir() / "llvm" / "bin" / "llvm-symbolizer.exe"
RE_ADDR = re.compile(r"([A-Za-z0-9_.]+\.(?:exe|dll))\+0x([0-9a-f]+)")


def pe_info(path):
    d = Path(path).read_bytes()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    return d, pe, image_base


def exports(path):
    d, pe, _ = pe_info(path)
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    exp_rva = struct.unpack_from("<I", d, pe + 24 + 112)[0]
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsz + 40 * i
        secs.append((struct.unpack_from("<I", d, s + 12)[0], struct.unpack_from("<I", d, s + 8)[0],
                     struct.unpack_from("<I", d, s + 20)[0]))

    def off(r):
        for va, vs, raw in secs:
            if va <= r < va + max(vs, 1):
                return r - va + raw
        raise ValueError(hex(r))

    e = off(exp_rva)
    nfun, nnames, af, an, ao = struct.unpack_from("<IIIII", d, e + 20)
    funcs = [struct.unpack_from("<I", d, off(af) + 4 * i)[0] for i in range(nfun)]
    names = {}
    for i in range(nnames):
        nr = struct.unpack_from("<I", d, off(an) + 4 * i)[0]
        o = struct.unpack_from("<H", d, off(ao) + 2 * i)[0]
        no = off(nr)
        names[o] = d[no:d.index(b"\0", no)].decode()
    return sorted((f, names.get(i, "#%d" % i)) for i, f in enumerate(funcs) if f)


def main():
    log, bindir = Path(sys.argv[1]), Path(sys.argv[2])
    flt = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
    lines = [l.rstrip("\n") for l in log.read_text(encoding="utf-8", errors="replace").splitlines()
             if "profile:" in l and (not flt or flt.search(l))]
    wanted = {}
    for l in lines:
        for m, o in RE_ADDR.findall(l):
            wanted.setdefault(m.lower(), set()).add(int(o, 16))
    names = {}
    for mod, offs in wanted.items():
        if mod == "ntdll.dll":
            ex = exports(r"C:\Windows\System32\ntdll.dll")
            for o in offs:
                best = max((x for x in ex if x[0] <= o), default=None)
                names[(mod, o)] = f"{best[1]}+{o - best[0]:#x}" if best else "?"
            continue
        p = bindir / mod
        if not p.exists() or not p.with_suffix(".pdb").exists():
            continue
        _, _, base = pe_info(p)
        offs = sorted(offs)
        q = "\n".join(hex(base + o) for o in offs) + "\n"
        out = subprocess.run([str(SYMBOLIZER), f"--obj={p}", "--functions=short", "--demangle", "--no-inlines"],
                             input=q, capture_output=True, text=True).stdout.split("\n\n")
        for o, block in zip(offs, out):
            fn = block.strip().split("\n")[0] if block.strip() else "?"
            names[(mod, o)] = fn
    for l in lines:
        print(RE_ADDR.sub(lambda m: f"{m.group(0)}<{names.get((m.group(1).lower(), int(m.group(2), 16)), '')}>", l))


if __name__ == "__main__":
    main()
