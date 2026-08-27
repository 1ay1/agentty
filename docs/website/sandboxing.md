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
- **Read-only user toolchains:** tools installed outside `/usr` are bound read-only so an approved command can find them — `~/.local/bin` and `~/.local/opt` (webinstall.dev go/node/etc.), `~/.cargo/bin` + `~/.rustup`, `~/go/bin`, `~/.nvm`, `~/.pyenv` / `~/.rbenv` / `~/.asdf`, `~/.bun/bin`, `~/.deno/bin`, `~/.dotnet`, `~/.sdkman/candidates`. These are the only `$HOME` sub-paths exposed, and they're read-only — no secret dirs (`~/.ssh`, `~/.aws`, `~/.config`, `~/.local/share`, `~/.npm`) are mounted.
- **Reachable:** the network (`--share-net`) — so `git push`, `npm`, and `curl` still work.
- **Blocked (not mounted):** `$HOME` at large, `~/.ssh`, credential stores, and every other project on the machine.
- **Only an allow-list of `/etc` is exposed** — `resolv.conf`, `hosts`, CA certs, `gitconfig` and a few others are readable so networking and git identity work; the rest of `/etc` (e.g. `shadow`, keytabs, corporate config) is invisible.

Hardened with its own user / PID / IPC / UTS / cgroup namespaces (`--unshare-user --unshare-pid --unshare-ipc --unshare-uts --unshare-cgroup-try`), a detached session (`--new-session`, blocks TIOCSTI terminal injection), and `--die-with-parent`. The payload runs with no ambient capabilities and `no_new_privs` set, so a setuid binary inside the sandbox can't escalate. macOS uses `sandbox-exec` with a `(deny default)` profile: broad file reads, writes restricted to the workspace + temp dirs, network open.

## If bwrap can't build a namespace

Some hardened hosts install `bwrap` but block **unprivileged user namespaces** — Ubuntu 24.04's AppArmor `userns` restriction, or `kernel.unprivileged_userns_clone=0` / `user.max_user_namespaces=0`. There, a real sandbox fails with `bwrap: setting up uid map: Permission denied`.

agentty **probes for this at startup by actually creating a throwaway sandbox** (not just checking that `bwrap` exists). If the probe fails:

- `--sandbox auto` (the default) runs **unsandboxed** so your commands still work, and the startup line says so plainly (`running unsandboxed — user namespaces are blocked`).
- `--sandbox on` **refuses to start** and tells you why, so you never *believe* you're sandboxed when you aren't.

To enable real containment on such a host, allow unprivileged userns — e.g. add an AppArmor profile for `/usr/bin/bwrap` with `userns,`, or set `sudo sysctl -w kernel.unprivileged_userns_clone=1`.

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
