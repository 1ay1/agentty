#!/usr/bin/env python3
"""io_roots: only the files in io_roots.txt may mint an Io.

agentty::Io (include/agentty/util/io.hpp) is the permission to do IO. A
function that touches the world takes one, and reducers and views never get
one. That only holds if nobody else can make one, so IoAccess, the one way to
make one, is allowed only in the files listed in io_roots.txt.

Fails on an unlisted file that names IoAccess, and on a listed file that no
longer does (so the list can only shrink to what is true).
"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1])
listed = {}
for line in (root / "tests/lint/io_roots.txt").read_text().splitlines():
    line = line.split("#", 1)[0].strip()
    if line:
        listed[line] = True

pat = re.compile(r"\bIoAccess\b")
comment = re.compile(r"//.*")
found = set()
for top in ("src", "include", "tests"):
    for p in (root / top).rglob("*"):
        if p.suffix not in (".cpp", ".hpp", ".h"):
            continue
        rel = p.relative_to(root).as_posix()
        for line in p.read_text(errors="replace").splitlines():
            if pat.search(comment.sub("", line)):
                found.add(rel)
                break

bad = sorted(found - listed.keys())
stale = sorted(listed.keys() - found)
if bad:
    print("io_roots: these files mint an Io but are not IO entry points:")
    for f in bad:
        print("  " + f)
    print("Take an Io parameter instead, or (if this really is an entry point:"
          " an effect body, a tool body, main, a test) add it to"
          " tests/lint/io_roots.txt with a comment saying why.")
if stale:
    print("io_roots: listed but no longer mint an Io (drop them):")
    for f in stale:
        print("  " + f)
sys.exit(1 if bad or stale else 0)
