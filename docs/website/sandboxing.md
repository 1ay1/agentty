---
title: Sandboxing
description: How agentty isolates shell and build calls with bwrap and sandbox-exec.
nav_section: Tools
nav_order: 20
slug: sandboxing
---

Every shell and build call runs inside a sandbox by default — not as an opt-in, not as an afterthought. An approved `bash` call still can't read your SSH keys.

## How it works

- **Linux:** commands run inside `bwrap` (Bubblewrap).
- **macOS:** commands run inside `sandbox-exec`.
- **Windows:** runs unsandboxed — no first-class equivalent yet.

## What's reachable

Inside the Linux (bwrap) sandbox:

- **Read-write:** the workspace directory, plus a fresh `tmpfs` mounted at `/tmp`.
- **Read-only:** system libraries and binaries (`/usr`, `/bin`, `/lib`, `/opt` …) so builds and toolchains work.
- **Reachable:** the network (`--share-net`) — so `git push`, `npm`, and `curl` still work.
- **Blocked (not mounted):** `$HOME`, `~/.ssh`, and every other project on the machine.
- **Only an allow-list of `/etc` is exposed** — `resolv.conf`, `hosts`, CA certs, `gitconfig` and a few others are readable so networking and git identity work; the rest of `/etc` (e.g. `shadow`, keytabs, corporate config) is invisible.

Hardened with `--unshare-pid`, `--new-session`, and `--die-with-parent`. macOS uses `sandbox-exec` with a `(deny default)` profile: broad file reads, writes restricted to the workspace + temp dirs, network open.

:::tip
The practical upshot: even if you approve a shell command in the autonomous [Write profile](/docs/profiles), it can't `cat ~/.ssh/id_rsa` or tamper with other projects on the machine.
:::

## Modes

Control the sandbox with `--sandbox`:

| Mode | Behaviour |
|---|---|
| `auto` (default) | Use the OS sandbox backend if present; otherwise run unsandboxed with a warning. |
| `on` | Require a backend — exit rather than run `bash`/`diagnostics` unsandboxed. |
| `off` | Disable the sandbox entirely. |

:::warn
Running with `--workspace /` makes the whole filesystem writable, so the sandbox reports as *degraded* — there's no directory left to contain. Keep the workspace scoped to your project to preserve containment.
:::

## Concrete example

An approved build command sees the workspace and system libs, but secrets stay out of reach:

```bash
# inside the sandbox
$ cmake --build build -j     # works — workspace + system libs reachable
$ cat ~/.ssh/id_rsa          # blocked — home dir not mounted writable/readable
```

:::warn
Sandboxing reduces blast radius; it is not a substitute for review. Treat network access inside the sandbox as real — a command can still exfiltrate workspace contents if you approve it.
:::
