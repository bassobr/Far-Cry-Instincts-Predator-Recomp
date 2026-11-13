#!/usr/bin/env python3
"""Checks whether an existing Ninja build folder must be reconfigured before building.

    python -I tools/needs_configure.py <build-folder>
    Exit 0: build.ninja is up to date. Exit 1: a CMake input is newer than build.ninja (or missing).
    Exit 2: check not possible (build.cmd then reconfigures to be safe).

Reason: `sources.cmake` (an output of the codegen) is a configure dependency. If build.ninja is
stale, Ninja still starts the codegen according to the old graph before it reconfigures; build steps that
the new graph would run before the codegen are then missing. A Ninja dry run does not help because
CMake's glob check (`cmake.verify_globs`) always appears as pending there.

Hence the glob marker and the codegen output `sources.cmake` are not counted; Ninja handles both
correctly by itself in the normal run once the graph is up to date.
"""
import os
import shutil
import subprocess
import sys

IGNORED_SUFFIXES = ("cmake.verify_globs", "/sources.cmake")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    bdir = sys.argv[1]
    ninja_file = os.path.join(bdir, "build.ninja")
    if not os.path.exists(ninja_file):
        return 1
    ninja = shutil.which("ninja")
    if not ninja:
        return 2
    try:
        out = subprocess.run([ninja, "-C", bdir, "-t", "query", "build.ninja"], capture_output=True,
                             text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return 2
    built = os.path.getmtime(ninja_file)
    in_inputs = False
    for line in out.splitlines():
        stripped = line.strip()
        if stripped.startswith("input:"):
            in_inputs = True
            continue
        if stripped.startswith("outputs:"):
            break
        if not in_inputs or not stripped:
            continue
        path = stripped.lstrip("|").strip()
        norm = path.replace("\\", "/")
        if norm.endswith(IGNORED_SUFFIXES):
            continue
        if not os.path.isabs(path):
            path = os.path.join(bdir, path)
        if not os.path.exists(path):
            print(f"needs_configure: missing: {norm}")
            return 1
        if os.path.getmtime(path) > built:
            print(f"needs_configure: newer than build.ninja: {norm}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
