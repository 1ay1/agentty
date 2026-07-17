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

## Boundary vs. sandbox

The workspace boundary and the [sandbox](/docs/sandboxing) are two independent layers. The boundary governs agentty's own filesystem tools; the sandbox governs what spawned shell commands can reach. Both apply at once.
