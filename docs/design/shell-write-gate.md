# The shell write-gate hole — research notes

## The hole

The trust-handoff gate is enforced at the tool call, keyed on tool name
(`src/tool/mcp_tools_bridge.cpp`, `def.execute`):

```cpp
if (tool_name == "write" || tool_name == "edit" ||
    tool_name == "apply_patch" || tool_name == "move") { … }
```

`shell` is not in that list, and `grep handoff` across `subprocess.cpp` and
`mcp_tools_backends.cpp` returns nothing. So:

```
write  → .vscode/tasks.json   REFUSED (verified live, file absent)
shell  → cat > .vscode/tasks.json   never consulted
```

The *table* is fine. `is_host_trusted(".vscode/tasks.json")` returns YES
regardless of caller — it is a pure path-shape walk. The policy knows the file
is dangerous; the shell tool never asks.

Why the gate is at the tool boundary and not in the sandbox is correct and
worth restating (`handoff_gate.hpp`): a sandbox wall answers "may this process
write here", and the answer is legitimately *yes* — `.vscode/` is inside the
workspace. The danger is that VS Code executes it **later, outside the
sandbox**. So the check is about intent at the call.

## Why not parse the command

The obvious fix — sniff `>`, `>>`, `tee`, `dd of=` — is an arms race against a
grammar: `sh -c`, `python -c`, `printf`, heredocs, base64-then-decode, a path
in a variable. mcp-cpp's `bash_validate.hpp` already carries the scar tissue
from this: its first version *"falsely matched `sed -i` and `cat > f` with a
READ suggestion"*, and the lesson recorded there is to return the analysis and
let the caller choose, never to guess the action. Any sniffer is defence in
depth, never the mechanism.

## Landlock: measured, and it does not work the obvious way

claybin's `compile.cpp` claims a nested deny is "not expressible". That is
true, but the experiment behind it (a **zero**-rights rule, rejected `ENOMSG`)
is not the same as a **reduced**-rights rule. I tested the latter.

### Finding 1 — within one layer, rights inherit and do NOT subtract

```
layer: rw on <ws>, ro on <ws>/.git/hooks      (same ruleset)
→ write <ws>/.git/hooks/pre-commit   WRITE OK      ← carve ignored
```

The kernel doc's wording is the explanation: a layer grants access if *at
least one* of its rules on the path grants it. One layer is **union**. A
deeper, stricter rule is not a subtraction.

### Finding 2 — across layers, access INTERSECTS, so a carve is possible

> A sandboxed thread can only access a file path if **all** its enforced policy
> layers grant the access.

Verified:

```
layer 1: handles everything, rw <ws>
layer 2: handles ONLY write bits, granted to <ws>/src but NOT <ws>/.git/hooks
→ <ws>/src/a.c                      WRITE OK
→ <ws>/.git/hooks/pre-commit        READ OK
→ <ws>/.git/hooks/pre-commit        WRITE DENIED   ← a real wall
```

### Finding 3 — three constraints that make it unshippable as the mechanism

1. **Rules must target directories.** A rule on a file is `EINVAL`. So a
   single trusted *file* cannot be carved; only its directory.
2. **The walk must run before any layer is enforced.** Layer 2's carve needs
   `opendir()` down the tree, and once layer 1 is live those opendirs are
   themselves mediated. My first attempt granted 0 directories for exactly
   this reason.
3. **The fatal one: the ancestors on the path to the carve cannot be
   granted.** Granting `<ws>` inherits write into the carve and undoes it
   (Finding 1). But *not* granting `<ws>` makes files sitting directly in the
   workspace root read-only:

```
→ <ws>/ordinary.txt    WRITE DENIED    ← collateral, not acceptable
→ mkdir <ws>/later     DENIED
```

Since rules are directories-only, there is no way to restore write to the
root's loose files. Carving `.vscode` makes the workspace root read-only.
That is a worse regression than the hole it closes.

**Conclusion: landlock carving is real but cannot be the primary mechanism
here.** It is also expensive — a sibling walk at every spawn (57 directories
just from `/`).

## The mechanism that fits: a read-only bind

agentty already masks secrets with **mounts**, not landlock, and
`handoff_gate.hpp` records why: landlock has no negative rule, so the way to
make a path unwritable is to make it *not be that file*. The same trick gives
read-only exactly:

```
bind_ro(<ws>/.vscode/tasks.json, <ws>/.vscode/tasks.json)
```

claybin already supports `bind_ro`. Properties, against the landlock attempt:

- **Exact.** Targets the file, not its directory. No sibling enumeration, no
  ancestor problem, no collateral on the workspace root.
- **Cheap.** One mount op per existing trusted path, no tree walk.
- **Reads unaffected.** The guest sees the real contents and gets `EROFS` on
  write, which is a truthful error rather than a confusing `EACCES`.
- **No grammar.** `cat >`, `python -c`, `dd`, a heredoc — all of them hit the
  same wall, because the wall is the filesystem.

### What it costs

Mount namespaces. On a host that denies unprivileged userns — the exact host
`33e38a5` was written for — there is no mount namespace, so this wall is
absent. It must degrade honestly rather than silently.

## Therefore: two layers, and be honest about which is active

1. **Prevention** (`bind_ro`) where mount namespaces exist. A real wall.
2. **Detection** everywhere, including hosts with no namespaces: snapshot
   `(mtime, size)` of existing trusted-shape paths before a shell call, compare
   after, and raise a handoff event on any change.

Detection is not a consolation prize. It cannot be bypassed by any grammar
trick, has zero false positives, needs no parsing, and it is the only layer
that works on the locked-down hosts claybin exists to serve. It converts a
silent bypass into a loud one, which is what the handoff feed is already built
to surface.

Prevention is the stronger guarantee where available; detection is the one
that is always true. Shipping only prevention would be a wall that quietly
isn't there on a hardened laptop.

## What shipped

Detection only, for now. `snapshot_trusted()` / `review_trusted()` in
`handoff_gate.{hpp,cpp}`, wrapped around `provider->execute` in
`mcp_tools_bridge.cpp` for `shell` / `process_start` / `test` /
`diagnostics`.

Verified live, which is the only way this was ever going to be convincing:

```
$ agentty run 'echo CHANGED >> .vscode/tasks.json'
W handoff: modified a host-trusted path outside the gate:
  /tmp/dettest/.vscode/tasks.json
  (an editor task -- VS Code can run this when the folder opens)
```

The handoff names the COMMAND, not the tool. "shell" tells a user nothing;
this is the one place the opaque string earns its keep — explaining after the
fact, never deciding.

### Two things the implementation got wrong first

**Created files were invisible.** The first version only diffed paths present
at snapshot time, so overwriting a hook was caught and *creating* one was not —
the easier and more dangerous move, and the only one available in a fresh repo
with no hooks. Found by testing it rather than reasoning about it. Fixed with a
second pass that re-scans the trusted-shaped directories the walk recorded.
`handoff_shell_test.cpp` fails on exactly three checks if that pass is removed.

**411 of 1824 paths were signal.** `.git` is a Component shape rule, so every
blob under `.git/objects/` read as host-trusted: 1605 inert files watched to
catch writes to three. Git executes hooks and honours config; it does not run
its own object database. Pruned to `.git/hooks`, `.git/info` and files directly
in `.git` — and pruned at the walk, not after, since descending only to discard
is the cost without the benefit. Confirmed `.git/hooks/pre-commit` is still
caught afterwards, so this removed noise and not signal.

What remains is honest: 162 `CMakeLists.txt` + 37 `Makefile` on this repo, all
of which genuinely run on the next build. Walk cost measured at ~14 ms over
2168 directories.

### Still open

The `bind_ro` prevention half. It is the better wall where mount namespaces
exist, and it needs a claybin change plus a decision about what to do when
`git config` legitimately wants to write. Detection landing first is the right
order: it is the layer that works on every host, so shipping it does not depend
on that decision.
