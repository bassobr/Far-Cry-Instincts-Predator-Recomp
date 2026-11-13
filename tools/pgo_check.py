"""Checks whether the PGO profile still matches the generated game code.

  python -I tools/pgo_check.py <build>/game_code_build.txt --profile <file.profdata> [--march x86-64-v3]
      [--physical-offset 0|1] [--threshold 2.0] [--sample N] [--jobs N] [--stamp <file>] [--fail]

Runs only clang's front end (-emit-llvm, LLVM passes disabled) on the generated sources with the same flags as the
game code build (src/fcp/game_code_build.cpp, BaseFlags/CompileCommand) and the profile, and reads the IR: a
function that got profile data carries "!prof" (function_entry_count) on its definition. Counted are the guest
functions (section "rexguest"); one without "!prof" is either new or changed since training (clang ignores
profile data whose structure hash differs) and is optimized as if there were no profile. A stale profile is never
wrong, only slower. If the share of guest functions without profile data is above --threshold percent, it prints
a warning (with --fail: exit code 1). Retrain with tools\\pgo.ps1 after codegen, title configuration or compiler
changes. Header functions of the SDK (not in section rexguest) are only reported.
--sample N checks every N-th source file (default 1 = all).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

RE_MISMATCH = re.compile(r"of (\d+) functions?, (\d+) (?:has|have) mismatched data")


def read_settings(path):
    s = {"include": [], "isystem": []}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            if isinstance(s.get(k), list):
                s[k].append(v)
            else:
                s[k] = v
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("settings")
    ap.add_argument("--profile", required=True)
    ap.add_argument("--march", default="x86-64-v3")
    ap.add_argument("--physical-offset", default="0")
    ap.add_argument("--threshold", type=float, default=2.0)
    ap.add_argument("--sample", type=int, default=1)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--stamp", default="")
    ap.add_argument("--fail", action="store_true")
    a = ap.parse_args()

    s = read_settings(a.settings)
    gen = s.get("gen", "")
    if not gen or not os.path.isdir(gen):
        print(f"pgo_check: generated code missing ({gen!r}), nothing checked")
        return 0
    if not os.path.isfile(a.profile):
        print(f"pgo_check: profile missing: {a.profile}")
        return 1 if a.fail else 0
    srcs = sorted(os.path.join(gen, n) for n in os.listdir(gen) if n.endswith("_init.cpp") or "_recomp." in n)
    srcs = srcs[:: max(a.sample, 1)]
    work = s.get("work", gen)
    base = [
        s["clang"], "--driver-mode=g++", "--target=x86_64-pc-windows-msvc", "-fms-compatibility-version=19.44",
        "-std=c++23", "-O3", "-DNDEBUG", f"-march={a.march}", "-ffp-contract=off", "-msse4.1", "-fasync-exceptions",
        "-g0", "-nostdlibinc", "-DREX_GUEST_FREESTANDING", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
        f"-DREX_PHYSICAL_OFFSET_IN_CODE={a.physical_offset}", f"-fprofile-instr-use={a.profile}",
        "-Wno-profile-instr-unprofiled", "-Wno-profile-instr-missing", "-Wprofile-instr-out-of-date",
        "-emit-llvm", "-S", "-Xclang", "-disable-llvm-passes", "-o", "-",
    ]
    for inc in s["isystem"]:
        base += ["-isystem", inc]
    base += [f"-I{gen}", f"-I{work}"] + [f"-I{inc}" for inc in s["include"]]

    def run(src):
        # stream the IR (about 10 MB per file), keep only the counts of the definitions
        p = subprocess.Popen(base + [src], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        guest = guest_prof = other = other_prof = 0
        for raw in p.stdout:
            if not raw.startswith(b"define "):
                continue
            prof = b" !prof " in raw
            if b'section "rexguest"' in raw:
                guest += 1
                guest_prof += prof
            else:
                other += 1
                other_prof += prof
        err = p.stderr.read().decode("utf-8", "replace")
        rc = p.wait()
        mism = sum(int(m.group(2)) for m in RE_MISMATCH.finditer(err))
        return src, rc, err, guest, guest_prof, other, other_prof, mism

    guest = guest_prof = other = other_prof = mism = 0
    errors = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(a.jobs, 1)) as ex:
        for src, rc, err, g, gp, o, op, mm in ex.map(run, srcs):
            if rc != 0:
                errors.append((src, (err.strip().splitlines() or [""])[-1]))
                continue
            guest += g; guest_prof += gp; other += o; other_prof += op; mism += mm
    if errors:
        for src, last in errors[:5]:
            print(f"pgo_check: front end failed for {os.path.basename(src)}: {last}")
        print(f"pgo_check: {len(errors)} of {len(srcs)} files failed, result incomplete")
        return 1 if a.fail else 0
    stale = guest - guest_prof
    share = 100.0 * stale / guest if guest else 100.0
    msg = (f"pgo_check: {len(srcs)} files, {guest} guest functions, {stale} without profile data "
           f"({share:.2f} %, threshold {a.threshold:g} %; {mism} of them with a changed structure hash); "
           f"header functions {other} ({other_prof} with data)")
    print(msg)
    bad = share > a.threshold
    if bad:
        print("pgo_check: WARNING: the PGO profile is out of date for the generated code. It is never wrong, "
              "but those functions are optimized without a profile. Retrain: tools\\pgo.ps1 "
              "(docs/en/BUILDING.md, PGO profile).")
    if a.stamp:
        with open(a.stamp, "w", encoding="utf-8") as f:
            f.write(msg + "\n")
    return 1 if (bad and a.fail) else 0


if __name__ == "__main__":
    sys.exit(main())
