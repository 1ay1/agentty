#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Does every packaging recipe know about every submodule?

packaging/arch/agentty-git/PKGBUILD builds agentty from source on the AUR,
and it cannot use `git submodule update --recursive`: makepkg forbids network
access in build(), so every submodule has to be fetched as its own source=()
entry and rewired to that local clone in prepare(). That list is maintained
BY HAND, which means it does not track .gitmodules.

So adding a submodule silently breaks the AUR source package. That happened:
claybin became required, nothing added it to the PKGBUILD, and because
AgenttySubmodules.cmake raises FATAL_ERROR on an empty third_party/claybin,
the package stopped CONFIGURING. Not a link error at the end -- it never got
that far. No workflow referenced the file, so it stayed broken silently.

CI also builds the package for real (the `AUR agentty-git configures` job),
which is the authoritative check. This exists because that job needs an Arch
container and ~1 minute, while this is a text comparison that runs in the
normal suite and names the fix precisely. The expensive gate proves it
builds; this one tells you WHAT to add the moment you add a submodule.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GITMODULES = ROOT / ".gitmodules"
PKGBUILD = ROOT / "packaging" / "arch" / "agentty-git" / "PKGBUILD"


def submodules() -> dict[str, str]:
    """name -> path, straight from .gitmodules."""
    text = GITMODULES.read_text(encoding="utf-8")
    out: dict[str, str] = {}
    name = None
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r'\[submodule "(.+)"\]', line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"path\s*=\s*(.+)", line)
        if m and name:
            out[name] = m.group(1).strip()
            name = None
    return out


def main() -> int:
    if not GITMODULES.exists() or not PKGBUILD.exists():
        print(f"skip: missing {GITMODULES.name} or PKGBUILD")
        return 0

    pkg = PKGBUILD.read_text(encoding="utf-8")
    subs = submodules()
    if not subs:
        print("skip: no submodules parsed")
        return 0

    failures: list[str] = []

    for name, path in sorted(subs.items()):
        # Two things must be true, and they are different failure modes:
        #
        #   1. the repo is FETCHED -- a source=() entry, or makepkg has
        #      nothing to wire from (and cannot reach the network later)
        #   2. the submodule is WIRED -- `git config submodule.<name>.url`,
        #      or `git submodule update` tries the real remote and fails
        #
        # A recipe with (1) and not (2) looks right and still breaks, so they
        # are checked separately and reported separately.
        fetched = f"/{name}.git" in pkg
        wired = f"submodule.{name}.url" in pkg

        if not fetched:
            failures.append(
                f"{name}: no source=() entry\n"
                f"    add:  'git+https://github.com/1ay1/{name}.git'\n"
                f"    and a matching 'SKIP' to sha256sums"
            )
        if not wired:
            failures.append(
                f"{name}: not rewired to its local clone in prepare()\n"
                f'    add:  git config submodule.{name}.url "$srcdir/{name}"'
            )

    if failures:
        print("packaging/arch/agentty-git/PKGBUILD is missing submodules.\n")
        print("Every submodule needs BOTH a source=() entry and a local-url")
        print("rewire, because makepkg builds with no network access.\n")
        for f in failures:
            print(f"  - {f}")
        print(
            "\nThis is why agentty-git stopped building when claybin landed:\n"
            "an empty third_party/<sub> makes CMake FATAL_ERROR at configure."
        )
        return 1

    names = ", ".join(sorted(subs))
    print(f"ok: agentty-git PKGBUILD covers every submodule ({names})")

    # The nested one is easy to forget for a different reason: it is not in
    # agentty's .gitmodules at all. maya carries jaal, and agentty links
    # maya::app, which IS maya + jaal -- so a recipe that wires maya but not
    # jaal fetches a maya that cannot build.
    if "jaal" not in pkg:
        print("\nerror: maya's nested jaal submodule is not wired", file=sys.stderr)
        print(
            "    maya::app is maya + jaal, so a maya without jaal builds nothing",
            file=sys.stderr,
        )
        return 1
    print("ok: nested jaal is wired too")
    return 0


if __name__ == "__main__":
    sys.exit(main())
