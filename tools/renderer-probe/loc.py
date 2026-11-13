"""Count lines and files per directory (size survey of third-party source trees).

Usage: python -I loc.py <root> [depth] [--files]
Counts text sources only; .git is skipped.
"""
import os
import sys

EXT = {".cpp", ".h", ".hpp", ".c", ".cc", ".inl", ".inc", ".hlsl", ".hlsli", ".glsl", ".py", ".md", ".patch",
       ".cmake", ".txt", ".toml", ".json", ".ps1", ".sh", ".bat", ".cs"}


def count(path):
    try:
        with open(path, "rb") as f:
            return f.read().count(b"\n")
    except OSError:
        return 0


def main():
    root = sys.argv[1]
    depth = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 2
    show_files = "--files" in sys.argv
    agg = {}
    files = []
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d != ".git"]
        rel = os.path.relpath(dp, root).replace("\\", "/")
        parts = [] if rel == "." else rel.split("/")
        key = "/".join(parts[:depth]) or "."
        for f in fn:
            ext = os.path.splitext(f)[1].lower()
            if ext not in EXT:
                continue
            n = count(os.path.join(dp, f))
            a = agg.setdefault(key, [0, 0])
            a[0] += 1
            a[1] += n
            files.append((n, (rel + "/" + f) if rel != "." else f))
    for k in sorted(agg):
        print(f"{agg[k][1]:>9} lines {agg[k][0]:>5} files  {k}")
    print(f"{sum(a[1] for a in agg.values()):>9} lines {sum(a[0] for a in agg.values()):>5} files  total")
    if show_files:
        for n, p in sorted(files, reverse=True)[:80]:
            print(f"{n:>8}  {p}")


if __name__ == "__main__":
    main()
