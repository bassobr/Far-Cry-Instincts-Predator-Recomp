"""Runs the semantic test runners and the evaluation (target predator_semtest).

  python run.py --gen <gen_dir> --out <dir> --known <known.toml> --runners <rex.exe> [<xr.exe>]
                [--stamp <file>]
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402


def main():
    args = sys.argv[1:]

    def opt(name):
        return args[args.index(name) + 1] if name in args else None

    gen, out, known, stamp = opt("--gen"), opt("--out"), opt("--known"), opt("--stamp")
    runners = []
    i = args.index("--runners") + 1
    while i < len(args) and not args[i].startswith("--"):
        runners.append(args[i])
        i += 1
    meta = json.load(open(os.path.join(gen, "meta.json")))
    image = meta.get("image")
    t0 = time.time()
    results = []
    for exe in runners:
        base = os.path.basename(exe)
        tag = ("xr" if "fcp_semtest_xr" in base else "exact" if "_exact" in base else
               "fprules" if "_fprules" in base else "rex")
        res = os.path.join(out, "%s_results.bin" % tag)
        cmd = [exe, os.path.join(gen, "cases.bin"), res] + ([image] if image else [])
        t = time.time()
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("run.py: %s exited with %d" % (exe, r.returncode))
            return 2
        print("run.py: %s %.2f s" % (tag, time.time() - t))
        results.append((tag, res))
    cargs = [gen, dict(results)["rex"], "--known", known, "--report", os.path.join(out, "report.txt"),
             "--json", os.path.join(out, "summary.json")]
    if "xr" in dict(results):
        cargs += ["--xr", dict(results)["xr"]]
    if "exact" in dict(results):
        cargs += ["--exact", dict(results)["exact"]]
    if "fprules" in dict(results):
        cargs += ["--fprules", dict(results)["fprules"]]
    sys.argv = ["compare.py"] + cargs
    rc = compare.main()
    print("predator_semtest: %s in %.1f s (report: %s)" % ("FAILED" if rc else "passed", time.time() - t0,
                                                           os.path.join(out, "report.txt")))
    if stamp:
        if rc == 0:
            open(stamp, "w").write("ok\n")
        elif os.path.exists(stamp):
            os.remove(stamp)
    return rc


if __name__ == "__main__":
    sys.exit(main())
