---
title: Sandboxing
description: Every shell command runs inside an OS-enforced sandbox — seccomp, landlock and cgroup2 on Linux. What it guarantees, how to configure it, and what it deliberately doesn't cover.
nav_section: Tools
nav_order: 20
slug: sandboxing
---

Every shell and build call runs inside a sandbox by default — not as an opt-in, not as an afterthought. An approved `bash` call still can't read your SSH keys.

You don't have to configure anything. This page is for when you want to.

## The one thing it guarantees

**A command can't read a credential it wasn't granted, and can't write outside your workspace.**

Your `~/.ssh`, `~/.aws`, `~/.gnupg`, cloud tokens and any `.env` in your repo are *masked* — the command opens the file and sees nothing, rather than being denied in a way that breaks tooling. That happens whether or not you ever open the settings pane.

## Check what you've got

The startup banner tells you, and it's a real capability probe rather than a claim:

```
agentty: sandbox: active (claybin)
```

For the full picture, open **Settings → Sandbox**. The pane shows every wall, what's enforcing it, and — crucially — what it *can't* enforce on your machine. It never claims a wall it isn't building.

## Pick a posture, not 28 switches

The pane has a lot of knobs. You almost certainly want the top one:

| Posture | Use it when | What it costs |
|---|---|---|
| `permissive` | a build breaks under the syscall filter | no filter, no resource caps — mount walls only |
| `balanced` | **the default** — ordinary work | network is open, so a leaked secret can leave |
| `hardened` | running agents you haven't read | an unusual build may need a syscall it can't make |
| `airgapped` | reading or editing code you don't trust | `git push`, `npm install` and `curl` all stop working |

Picking one writes every row below it. Everything stays visible and editable — a preset is a starting point, not a mode you're locked into. Edit one row afterwards and the pane honestly reports `custom` rather than still claiming the preset.

:::tip
`airgapped` is the interesting one. Everywhere else, a command that can read can also send; with the network off, that stops being true. It costs you nothing if you're only reading and editing code.
:::

## Backends

| Platform | Backend | Notes |
|---|---|---|
| Linux | **claybin** (default) | seccomp + landlock + cgroup2, in-process |
| Linux | bwrap (Bubblewrap) | fallback where claybin can't start |
| macOS | `sandbox-exec` | filesystem + process walls |
| Windows | — | no first-class equivalent yet |

On Linux, claybin is the default because the difference is measured rather than argued:

| Capability | claybin | bwrap |
|---|---|---|
| filesystem read/write | strong (mount ns) | strong (mount ns) |
| filesystem exec | strong (landlock) | partial (binds only) |
| network isolation | strong (netns) | none |
| syscall filter | **strong** (seccomp-bpf) | **none** |
| memory / CPU / process caps | **strong** (cgroup2) | **none** |
| `ptrace` + `kill` brokering | yes | **none** |

There's no capability where bwrap wins. It stays because the two fail on *different* hosts: bwrap dies where unprivileged user namespaces are blocked (Ubuntu 24.04's AppArmor), claybin needs landlock (kernel 5.13+). Asking for claybin where it can't run gives you bwrap — never nothing.

Override with `--sandbox-backend claybin|bwrap`, or the Backend row in the pane.

## What's reachable

- **Read-write:** the workspace directory, plus a fresh `tmpfs` at `/tmp`.
- **Read-only:** system libraries and binaries (`/usr`, `/bin`, `/lib`, `/opt` …) so builds and toolchains work.
- **Read-only user toolchains:** `~/.local/bin`, `~/.cargo/bin` + `~/.rustup`, `~/go/bin`, `~/.nvm`, `~/.pyenv` / `~/.rbenv` / `~/.asdf`, `~/.bun/bin`, `~/.deno/bin`, `~/.dotnet`, `~/.sdkman/candidates`. These are the only `$HOME` sub-paths exposed, and they're read-only.
- **Masked:** `~/.ssh`, `~/.aws`, `~/.gnupg`, `~/.kube`, `~/.docker/config.json`, `~/.config/gh`, `~/.config/gcloud`, `~/.azure`, `~/.netrc`, `~/.npmrc`, `~/.cargo/credentials`, `~/.git-credentials`, agentty's own credential store — plus any `.env`, `.npmrc`, `id_*`, `credentials.json`, `*.pem` or `*.tfvars` found in your workspace.
- **Only an allow-list of `/etc`:** `resolv.conf`, `hosts`, CA certs, `gitconfig` and a few others, so networking and git identity work. The rest of `/etc` is invisible.
- **Network:** reachable by default, so `git push`, `npm` and `curl` work. Turn it off with the `airgapped` posture.

Hardened with its own user / PID / IPC / UTS / cgroup namespaces, a detached session (blocks TIOCSTI terminal injection), and die-with-parent. The payload runs with no ambient capabilities and `no_new_privs` set, so a setuid binary inside the sandbox can't escalate.

## Secrets in your repo

Masking isn't limited to `$HOME`. agentty walks your workspace and masks credential files by name — `.env` and its variants, `.npmrc`, `.netrc`, `.git-credentials`, `id_rsa` / `id_ed25519` / `id_ecdsa`, `credentials.json`, `*.pem`, `*.tfvars` and more.

The walk is depth-limited (default 3, configurable) because it runs on every command, and it skips `.git`, `node_modules`, `target`, `build` and `.venv` — reliably enormous, and not where credentials live.

:::tip
A monorepo's `services/api/.env` is masked, not just the one at the root. That was the whole reason the walk exists.
:::

## Adding paths

Need a dependency outside your workspace readable? **Also readable** → [[Enter]] opens a list editor with path completion — type a few characters, [[Tab]] accepts. It completes directories as well as files.

- clear a line (or [[Ctrl+X]]) to remove an entry
- **Masked** carves a hole *inside* whatever you've granted
- **Allowed ports** switches Network to per-port for you when you add one

## Changes apply on restart

This is the one place in agentty where saving doesn't take effect immediately, and it's deliberate.

A boundary that can move mid-session only ever moves usefully in the *loosening* direction — and loosening can't be undone, because re-tightening doesn't un-read a key that already left. So edits persist as you type and apply on the next launch.

The pane doesn't just promise this, it checks it: if the next launch *won't* deliver what the rows describe — the policy won't compile, the engine can't start here, a capability silently degrades — the footer says so and why.

## Modes

| Mode | Behaviour |
|---|---|
| `auto` (default) | Use the OS backend if available; otherwise run unsandboxed with a warning. |
| `on` | Require a backend — exit rather than run `bash` / `diagnostics` unsandboxed. |
| `off` | Disable the sandbox entirely, including the trust-handoff gate below. |

```sh
agentty --sandbox on
agentty --sandbox-backend claybin
```

Availability is a **capability probe**, not a `which`: agentty forks and attempts the real namespace setup. A binary on `$PATH` that can't actually start a sandbox is not an available backend — treating it as one is how "sandbox: active" comes to mean nothing.

If the probe fails, `--sandbox auto` runs unsandboxed and says so plainly; `--sandbox on` refuses to start and tells you why. To enable containment on a host that blocks unprivileged userns, allow it — e.g. an AppArmor profile for `/usr/bin/bwrap` with `userns,`, or `sudo sysctl -w kernel.unprivileged_userns_clone=1`.

:::warn
Running with `--workspace /` makes the whole filesystem writable, so the sandbox reports as *degraded* — there's no directory left to contain. Keep the workspace scoped to your project.
:::

## The escape a sandbox can't see

There's one attack no process sandbox stops: the agent writes a perfectly ordinary file in your workspace — a `.vscode/tasks.json`, a git hook, a venv interpreter, a `.envrc` — and something *outside* the sandbox runs it later. By the time your editor executes that file, there's no sandbox involved at all.

agentty refuses those writes by default and tells the model what to do instead. You can set it to warn-and-allow in the pane, but the default is `refuse` because this is the shape nearly every real agent escape has taken.

## What it doesn't stop

Stated plainly, because a boundary you can't see the edge of isn't one you can trust:

- **Exfiltration over a network you allowed.** If a command can read and the network is on, it can send. That's why the default read set is narrow, and why `airgapped` exists.
- **Secrets deeper than the scan depth**, or inside a skipped directory like `node_modules`.
- **Credential names nobody knows.** A secret in `config.local.yaml` doesn't look like one.
- **Kernel bugs.** The command shares your kernel — that's what the pane means by `host.kernel_isolation: none`.
- **Windows.** No backend yet; `--sandbox on` fails loudly rather than pretending.

## The model knows it's sandboxed

A model that doesn't know it's confined misdiagnoses every wall it hits — "your corporate proxy is blocking this", "check your firewall" — and either sends you chasing a phantom or retries a call the kernel will never allow.

agentty tells it twice: once in the system prompt (what's allowed, not just what's blocked), and again on any denied command's output, where the model is actually looking when it decides what to do next.

## Concrete example

```bash
# inside the sandbox
$ cmake --build build -j     # works — workspace + system libs reachable
$ cat ~/.ssh/id_rsa          # masked — the key never reaches the command
$ cat .env                   # masked — even inside your own workspace
```

:::warn
Sandboxing reduces blast radius; it is not a substitute for review. With the network on, a command can still exfiltrate workspace contents if you approve it.
:::

Full technical detail, including the threat model and how to verify any of this on your own machine: [`docs/SANDBOX.md`](https://github.com/1ay1/agentty/blob/master/docs/SANDBOX.md).
