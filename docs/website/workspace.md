---
title: Workspace Boundary
description: Why filesystem tools refuse paths outside your project, and how to opt out.
nav_section: Tools
nav_order: 30
slug: workspace
---

agentty's filesystem tools refuse any path outside the workspace root. The agent can't read or write your home directory, system files, or another project unless you explicitly allow it.

## The workspace root

By default, the directory you launch in is the root. Every `read`, `write`, `edit`, `glob`, and `list_dir` call is checked against it — a path that escapes is rejected before the tool runs.

`repo_map`'s codebase walk applies an extra rule on top: it stops at any nested repository boundary — a subdirectory carrying its own `.git`/`.hg`/`.svn`/`.jj` (including a submodule's gitlink *file*, not just a directory). A submodule or vendored checkout living inside your workspace root passes the path check but is still excluded, so the ranked skeleton never mixes in a different project's source.

```bash
cd ~/code/my-app
agentty                          # root = ~/code/my-app
# read ../other-project/secret  → refused
```

## Pointing at another workspace

Run against a different project without changing directories:

```bash
agentty --workspace ~/code/other-project
```

## Opting out

To remove the boundary entirely, set the workspace to the filesystem root:

```bash
agentty --workspace /
```

:::warn
`--workspace /` lets the agent touch any path your user can. Combined with the [Write profile](/docs/profiles), that's a lot of trust — use it deliberately.
:::

## Access boundary vs. active project

There are really two roots, and they only diverge when you widen the boundary:

- The **access boundary** is the security gate above — the outermost path the filesystem tools will touch. `--workspace` moves it; `--workspace /` removes it.
- The **active project** is the directory you actually launched agentty in. agentty never changes directory away from it.

By default these are the same folder, so the distinction is invisible. It matters the moment you widen the boundary. When you run `--workspace /` (or point it at a *parent* of your project), **relative paths still resolve against the project you launched in, not the boundary.** So:

```bash
cd ~/code/my-app
agentty --workspace /            # boundary = /, project = ~/code/my-app
# read src/main.rs   → ~/code/my-app/src/main.rs   (not /src/main.rs)
# repo_map           → maps ~/code/my-app           (not the whole disk)
# grep / find_definition / diagnostics / test / git → all scoped to the project
```

Everything that means “your project” — relative-path resolution, the `repo_map`/`grep`/`glob`/`list_dir` defaults, `find_definition`, `diagnostics` and `test` (build directory + manifest), the git tools, checkpoints, the `@`-file picker, the symbol index, and project-scoped memory — is anchored on the project. The boundary is used only to *reject* paths that escape it. An absolute path is still honored anywhere inside the boundary, so `--workspace /` remains a genuine opt-out for reaching other locations — you just have to name them explicitly.

## Boundary vs. sandbox

The workspace boundary and the [sandbox](/docs/sandboxing) are two independent layers. The boundary governs agentty's own filesystem tools; the sandbox governs what spawned shell commands can reach. Both apply at once.
