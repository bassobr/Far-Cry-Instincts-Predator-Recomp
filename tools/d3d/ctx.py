#!/usr/bin/env python3
"""Context around sites: ctx.py <before> <after> <address> [<address> ...]  (instructions before/after)."""
import sys

import d3dlib
import q


def main():
    im = d3dlib.Img()
    before, after = int(sys.argv[1]), int(sys.argv[2])
    for s in sys.argv[3:]:
        a = int(s, 16)
        f = im.func_of(a)
        print("== %08X in %s" % (a, "%08X" % f if f else "-"))
        q.dis(im, a - 4 * before, before + after + 1)


if __name__ == "__main__":
    main()
