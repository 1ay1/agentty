#!/usr/bin/env python3
"""disk_writes: agentty writes shared files one way (include/agentty/io/shared_file.hpp).

  lock     no file lock outside the section: CrossProcessFileLock, flock(),
           LockFileEx, fcntl F_SETLK*, or native_file_lock::acquire
  rename   no rename(2)/MoveFileEx/fs::rename publishing a temp, which is how
           a fixed ".tmp" name gets shared by two writers
  truncate no truncating or appending ofstream/fopen: a reader can see the
           file half-written, and two appenders can splice lines
  setvbuf  no setvbuf(..., _IOFBF/_IOLBF, 0): the C standard allows size 0
           with a NULL buffer, MSVC's UCRT treats it as an invalid argument
           and fast-fails the process (0xC0000409, #115)

Each rule has a short allowlist below: the primitive itself, and the few
places that move whole files or directories around (migrations, `config
move`) rather than write them. Comments and strings are ignored.

Usage: disk_writes.py ROOT
"""

import os
import re
import sys

EXTS = (".cpp", ".hpp", ".h")

RULES = {
    "lock": (
        r"\bCrossProcessFileLock\b|[^\w.>:]flock\s*\(|\bLockFileEx\b|\bF_(OFD_)?SETLKW?\b"
        r"|native_file_lock::(try_)?acquire\b|\bacquire_within\s*<",
        {
            "src/io/shared_file.cpp",     # the section
            "src/util/storage_lock.cpp",  # the session-long "agentty is running" lock
        },
    ),
    "rename": (
        r"\bfs::rename\s*\(|std::filesystem::rename\s*\(|[^\w.>]::rename\s*\(|[^\w.>:]rename\s*\("
        r"|\bMoveFileExW?\s*\(|\bReplaceFileW?\s*\(",
        {
            "src/io/shared_file.cpp",           # write_atomic / create_exclusive
            "src/tool/util/disk_files.cpp",     # the edit tool's writer (keeps symlinks, modes)
            "include/agentty/tool/util/disk_files.hpp",
            "src/config/inventory.cpp",         # `config move`: moves whole dirs, store idle
            "src/rag/adapter.cpp",              # one-time move of old index files into cache/
            "src/util/user_root.cpp",           # one-time legacy layout migration
        },
    ),
    "truncate": (
        r"std::ofstream\b|std::fstream\b|\bios(_base)?::(trunc|app)\b|[^\w]fopen\s*\([^)]*\"[wa]"
        r"|_wfopen\s*\([^)]*L\"[wa]|\bO_TRUNC\b",
        {
            "src/io/shared_file.cpp",
            "src/tool/util/disk_files.cpp",
            "src/util/logx.cpp",                # the log: one process, O_APPEND
        },
    ),
    "setvbuf": (
        r"\bsetvbuf\s*\([^;]*_IO[FL]BF\s*,\s*0\s*\)",
        set(),
    ),
}


def strip(code):
    """Blank comments and string literals, keeping line structure."""
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if code.startswith("//", i):
            j = code.find("\n", i)
            i = n if j < 0 else j
        elif code.startswith("/*", i):
            j = code.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * code.count("\n", i, j))
            i = j
        elif c == '"' or c == "'":
            # Blank the literal but keep any newlines (raw strings span lines).
            q, j = c, i + 1
            while j < n and code[j] != q:
                j += 2 if code[j] == "\\" else 1
            out.append(q + "\n" * code.count("\n", i, j) + q)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    compiled = {k: re.compile(p) for k, (p, _) in RULES.items()}
    hits = []
    for sub in ("src", "include"):
        for dp, _, files in os.walk(os.path.join(root, sub)):
            for f in sorted(files):
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                raw = open(path, encoding="utf-8", errors="replace").read()
                # Keep the quotes so the fopen mode check can see "w"/"a".
                code = strip(raw)
                modes = raw.split("\n")
                for no, line in enumerate(code.split("\n"), 1):
                    for rule, rx in compiled.items():
                        if rel in RULES[rule][1]:
                            continue
                        probe = " " + line
                        if rule == "truncate" and "fopen" in line:
                            probe = " " + modes[no - 1]   # needs the mode string
                        if rx.search(probe):
                            hits.append(f"{rel}:{no}: [{rule}] {modes[no - 1].strip()}")
    print("disk_writes: shared files go through persistence::with_shared_file / write_atomic")
    if hits:
        print(f"\n{len(hits)} violation(s). Use include/agentty/io/shared_file.hpp"
              " ([setvbuf]: pass BUFSIZ, not 0):")
        for h in hits:
            print("  " + h)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
