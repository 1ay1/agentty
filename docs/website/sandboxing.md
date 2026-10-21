---
title: Sandboxing
description: Every shell command runs inside a sandbox the kernel enforces. What it guarantees, how to configure it, and what it deliberately doesn't cover.
nav_section: Tools
nav_order: 20
slug: sandboxing
---

When the agent runs `npm install` or `cargo build`, that command executes on your machine with your permissions. Nothing about "it's an AI" changes that — a shell command is a shell command.

So agentty puts every one of them inside a sandbox, by default. Not a permission prompt you click through; a boundary the **operating system kernel** enforces, which means the command cannot cross it even if it tries.

You don't have to configure anything. This page is for when you want to.

## The one promise

**A command can't read a credential it wasn't given, and can't write outside your project folder.**

Everything below serves that one sentence.

Your `~/.ssh` keys, `~/.aws` credentials, cloud tokens, and any `.env` file in your repo are **masked** — the command opens the file and finds it empty. Masking rather than blocking is deliberate: a tool that gets "permission denied" often crashes, while a tool that reads an empty config usually carries on.

## Check what you've got

The startup banner tells you:

```
agentty: sandbox: active (claybin)
```

That line is a **real test, not a claim**. Before printing it, agentty actually tries to build a sandbox and watches whether the kernel allows it. A sandbox tool that's installed but can't run is reported as unavailable — because the worst possible failure here is confidently announcing protection you don't have.

For the full picture, open **Settings → Sandbox**. The pane shows every wall, what's enforcing it, and — crucially — anything it *can't* enforce on your machine.

:::caution
On **Windows** the pane previews through claybin's Windows compiler, so the footer and per-row walls match what commands get (filesystem `partial` via appcontainer+DACL, memory and process caps `strong` via the job object).
:::

## Pick a posture, not 28 switches

The pane has a lot of knobs. You almost certainly want the top one.

A **posture** is a preset: pick one and it fills in every other setting for you.

| Posture | Pick it when | What you give up |
|---|---|---|
| `permissive` | a build breaks and you suspect the sandbox | the strictest walls; file protection stays |
| `balanced` | **the default** — everyday work | the network is open, so a leaked secret could be sent out |
| `hardened` | running an agent you haven't reviewed | an unusual build might need something it can't do |
| `airgapped` | reading or editing code you don't trust | `git push`, `npm install` and `curl` all stop working |

Everything stays visible and editable afterwards — a posture is a starting point, not a mode you're locked into. Change one row and the pane honestly relabels itself `custom` instead of still claiming the preset.

:::tip
`airgapped` is the one worth knowing about. In every other mode, a command that can read your files can also send them somewhere. Turn the network off and that stops being true — and if you're only reading and editing code, it costs you nothing.
:::

## What the walls actually are

The sandbox isn't one mechanism. It's several, each closing a different route out. Taking Linux first, because it's the platform where every one of them exists:

**A private view of the filesystem.** The command gets its own *mount namespace* — its own idea of what the filesystem looks like. Your project is there, system libraries are there read-only, and `~/.ssh` is an empty file. Not hidden by convention: it genuinely isn't in that view.

**A list of syscalls it may make.** Every program asks the kernel to do things — open a file, start a process, send a packet. These requests are called **syscalls**, and a *seccomp filter* is a list of which ones are allowed. agentty's default profile blocks the ones used to escape a sandbox (`ptrace`, `mount`, `unshare`) while allowing everything a compiler needs. Sockets are limited to the families real tools use (Unix, IPv4, IPv6); netlink, vsock, raw and packet sockets are refused.

**File rules the kernel checks directly.** *Landlock* is a Linux feature (kernel 5.13+) that lets a program permanently drop its own file access. Once set, it applies to that process and everything it starts — so a build script spawning a shell spawning a Python script is still confined.

**Hard resource ceilings.** *cgroups* cap memory, CPU and process count. A runaway build hits a limit inside the sandbox instead of taking your laptop down with it.

**No new privileges.** The `no_new_privs` flag means a program inside the sandbox can't gain permissions by running a setuid binary — the normal escalation route is closed.

**A detached terminal session.** Without this, a command can push fake keystrokes into your terminal (the `TIOCSTI` trick) and make your shell run them after agentty exits.

:::note
Each platform compiles the same policy into what it actually has. On Linux that's namespaces, Landlock, seccomp and cgroup2 at full strength. On macOS the profile is seatbelt plus rlimits: `no_new_privs` has an equivalent in that a seatbelt profile cannot be dropped by `exec`; resource ceilings become POSIX rlimits, which are weaker than cgroups (they count *per process*, not per process tree); and there is **no syscall filter** — macOS has no seccomp equivalent available to an unprivileged process — so agentty reports that capability as `none` instead of quietly implying it. On Windows the walls are an AppContainer, a restricted token, a job object and process mitigation policies: memory, CPU and process caps are genuinely as strong as cgroup2, filesystem and exec walls are `partial` because Windows enforces ACLs per object rather than per path prefix, and there is no syscall filter either (mitigation policies are a fixed menu of hardening switches, not a programmable filter).
:::

## Which backend, and why

| Platform | Engine | What it puts up | Fallback |
|---|---|---|---|
| Linux | **claybin** | namespaces, Landlock, seccomp, cgroup2 | none — one Linux engine |
| macOS | **claybin** | seatbelt from your compiled policy, plus rlimits | `sandbox-exec`, one fixed profile |
| Windows | **claybin** | AppContainer, restricted token, job object, mitigation policies | **none, by design** |

On Windows there is no weaker engine to fall back to and no `bwrap`: the probe either proves it can build a wall (an AppContainer or a job object) and claybin runs, or the startup banner says `sandbox: unavailable, running unsandboxed` and commands run unconfined — reported, never silent. Windows Sandbox (the Hyper-V VM product) was deliberately not chosen: it is a whole-OS VM with seconds of boot time, no pipe-stdout model and no per-command exit codes.

claybin is the default because the difference is measured, not argued.

**On Linux**, against the bwrap path it replaced:

| Capability | claybin | bwrap |
|---|---|---|
| file read/write walls | yes | yes |
| syscall filter | **yes** | **no** |
| memory / CPU / process limits | **yes** | **no** |
| runtime `ptrace` + `kill` decisions | **yes** | **no** |

There's no capability where bwrap wins. It used to stay as a second Linux engine on the argument that the two **fail on different machines** — bwrap can't start where *user namespaces* are blocked (Ubuntu 24.04 disables them for untrusted programs), and claybin needs Landlock, so it can't do its full job on kernels older than 5.13. That argument did not survive the code it described: both fail for the same underlying reason, and the probe now proves a real namespace rather than checking that a binary exists. So there is one Linux engine, and `--sandbox-backend bwrap` still parses, says what it did, and runs claybin.

**On macOS**, the comparison is a different shape: both engines are seatbelt, so this is not a contest about strength. claybin calls `sandbox_init` with a profile compiled from *your* policy; `sandbox-exec` is Apple's command-line wrapper around the same kernel mechanism, driven by one fixed profile string. So the question is what survives the trip:

| Capability | claybin | sandbox-exec |
|---|---|---|
| file read/write/exec walls | yes | yes, but fixed |
| network off, or per-port | **yes** | **no** |
| your **Settings → Sandbox** choices apply | **yes** | **no** |
| extra readable / masked paths | **yes** | **no** |
| per-capability guarantee report | **yes** | **no** |
| memory / CPU / process limits | **partial** (rlimits) | **no** |
| syscall filter | no — platform has none | no |

That last row is the honest one. macOS has no seccomp equivalent available to an ordinary program, so agentty reports the syscall filter as unavailable rather than implying it. The resource limits are rlimits, which count *per process* rather than per process tree — four children under a 1 GB cap can use 4 GB between them — so they report `partial`, and the process limit reports `advisory`, because it is counted per *user account*, not per sandbox.

Asking for claybin where it can't run gives you the platform's answer, not a silent nothing. On macOS the fallback works in both directions, so a Mac missing `sandbox-exec` still gets claybin, which talks to the kernel directly. On Windows there is nothing to fall back to, so the banner tells you instead.

```sh
agentty --sandbox-backend claybin   # default everywhere
agentty --sandbox-backend bwrap     # accepted for old scripts; bwrap is gone
```

## What the command can reach

**Read and write**

- your project folder
- a private, empty `/tmp` that vanishes when the command ends

**Read only**

- system libraries and binaries (`/usr`, `/bin`, `/lib`) so builds work
- your language toolchains: `~/.cargo/bin`, `~/.rustup`, `~/go/bin`, `~/.nvm`, `~/.pyenv`, `~/.rbenv`, `~/.asdf`, `~/.bun/bin`, `~/.deno/bin`, `~/.dotnet`, `~/.sdkman`, `~/.local/bin`
- a short allow-list of `/etc` — DNS config, CA certificates, your git identity. The rest of `/etc` isn't visible.

**Masked (reads as empty)**

- **Credentials:** `~/.ssh`, `~/.aws`, `~/.gnupg`, `~/.kube`, `~/.docker/config.json`, `~/.config/gh`, `~/.config/gcloud`, `~/.azure`, `~/.netrc`, `~/.npmrc`, `~/.pypirc`, `~/.cargo/credentials`, `~/.git-credentials`, `~/.password-store`, `~/.config/op`, `~/.config/rclone/rclone.conf`, `~/.terraform.d/credentials.tfrc.json`, `~/.m2/settings.xml`, `~/.gradle/gradle.properties`.
- **Other agents' sign-ins:** `~/.codex/auth.json`, `~/.claude/.credentials.json`, `~/.claude.json`, `~/.config/github-copilot`.
- **Keyrings and browser profiles:** `~/.local/share/keyrings`, `~/.config/google-chrome`, `~/.config/chromium`, `~/.config/BraveSoftware`, `~/.mozilla`. These are encrypted, but a key and the data it unlocks shouldn't leave together.
- **Shell and REPL history:** `~/.bash_history`, `~/.zsh_history`, fish history, and the Python, psql, MySQL and Node REPL histories. Pasted tokens end up there.
- **agentty's own state:** its credential store and its memory file.

A mask always wins. Nothing else the sandbox mounts, including the read-only binds below, can make a masked path readable again.

**Not there at all**

Everything else in your home directory. Your other projects, your documents, your browser profile.

## Secrets inside your own repo

Credentials aren't only in `$HOME`. They end up committed, copied into examples, and generated by tooling — so agentty also walks your project and masks files by name:

`.env` and its variants, `.npmrc`, `.netrc`, `.git-credentials`, `.pgpass`, SSH keys (`id_rsa`, `id_ed25519`, `id_ecdsa`, `id_dsa`), `credentials.json`, `service-account.json`, and anything ending in `.pem` or `.tfvars`.

The walk goes three levels deep by default (configurable) and skips `.git`, `node_modules`, `target`, `build` and `.venv` — directories that are reliably enormous and reliably not where credentials live.

:::tip
A monorepo's `services/api/.env` gets masked, not just the one at the repo root. Depth-limited rather than root-only is the whole reason the walk exists — the one at the root is rarely the only one.
:::

## Adding paths

Need a dependency outside your project to be readable? In **Settings → Sandbox**, the **Also readable** row opens a list editor with path completion — type a few characters, [[Tab]] accepts. It completes folders as well as files.

- clear a line (or press [[Ctrl+X]]) to remove an entry
- **Masked** punches a hole *inside* something you've granted — useful for "this whole folder, except that one file"
- **Allowed ports** switches Network to per-port mode for you when you add the first one

## Why changes need a restart

This is the one place in agentty where saving doesn't apply immediately, and it's deliberate.

A boundary that can change while commands are running can only usefully change in the *loosening* direction — and loosening can't be taken back. Re-tightening doesn't un-read a key that already left. It would also make the sandbox impossible to audit: two commands in one session, two different sets of walls, and no record of which got which.

So edits save as you type and take effect next launch.

The pane doesn't just promise that, it **checks** it. If the next launch won't deliver what the settings describe — the policy won't compile, the engine can't start on this machine, some capability quietly degrades — the footer tells you which, and why.

## The escape a sandbox can't see

There's one attack that no process sandbox stops, and it's the one that keeps showing up in real incidents.

The agent writes a completely ordinary file in your project — a `.vscode/tasks.json`, a git hook, a `.envrc`, a virtualenv interpreter. Nothing is violated: it's a normal write, to a normal path, inside the folder it's allowed to write.

Then **you** open the project in your editor, or run `git commit`, or activate the virtualenv. Your editor runs that file. There's no sandbox involved at that point — the command that wrote it finished long ago.

agentty refuses those writes by default, and tells the model what to do instead ("write the content somewhere inert and ask the user to wire it up"). You can relax it to warn-and-allow in the pane, but `refuse` is the default because this is the shape nearly every real agent escape has taken.

The files it guards:

- **Editor and agent config:** `.vscode`, `.idea`, `.cursor`, `.claude`, `.agentty`, `.codex`, `.gemini`, `hooks.json`
- **git:** `.git/config`, `.git/hooks`, `.git/info`
- **Shell start-up:** `.bashrc`, `.zshrc`, `.profile`, `.bash_profile`, `.zprofile`, `.zshenv`, `.xprofile`, `.xinitrc`, `.envrc`, `.config/fish`, `.config/environment.d`, virtualenv `activate`
- **Login session:** `.config/autostart`, `.config/systemd`, and window-manager config (Hyprland, Sway, i3, Plasma)
- **Executables found on `PATH`:** `.local/bin`, `node_modules/.bin`, virtualenv interpreters
- **Build scripts:** `Makefile`, `package.json`, `build.rs`, and similar

Where the sandbox can, it also binds the ones that already exist read-only, so the write fails with `EROFS` instead of landing. Where it can't (a host without user namespaces), the write is still detected and reported.

## What it doesn't stop

A boundary you can't see the edge of isn't one you can trust. So, plainly:

- **Sending data out over a network you allowed.** If a command can read your files and the network is on, it can upload them. This is a real trade — blocking the network breaks `git push` and `npm install` — and it's exactly why `airgapped` exists. For one headless run, `agentty run --no-network` takes the network away from every command it spawns without changing your saved setting.
- **Ports mode is TCP only.** With Network set to `ports`, the allowed-ports list limits TCP connections. UDP and ICMP aren't filtered, so DNS-based exfiltration still works there. Under `full` the ports list is ignored entirely, and the pane says so.
- **A memory bug that maps, writes, then flips a page executable.** W^X refuses a mapping that is writable and executable at once. Writing a page and then making it executable still works, because JITs need it.
- **Secrets deeper than the scan depth,** or inside a skipped folder like `node_modules`.
- **Credential files nobody would recognise.** A secret in `config.local.yaml` doesn't look like a secret.
- **Kernel bugs.** The command shares your kernel. A flaw in the kernel itself defeats any sandbox of this kind — which is what the pane means when it reports kernel isolation as `none`.
- **A host that cannot build a wall.** On Linux, a kernel without Landlock (older than 5.13) leaves no engine; on Windows, a host where the AppContainer probe fails (policy software, token shape) leaves none either. `--sandbox on` fails loudly rather than quietly doing nothing, and `--sandbox auto` prints what you actually got.

## The model knows it's sandboxed

A model that doesn't know it's confined **misdiagnoses every wall it hits**. A permission error becomes "your corporate proxy is blocking this"; a failed connection becomes "check your firewall". It then either sends you chasing a problem that doesn't exist, or retries a command the kernel will never allow.

agentty tells it twice: once in the system prompt — what's *allowed*, not just what's blocked, so it knows where to work on the first try — and again on the output of any command that got denied, which is where the model is actually looking when it decides what to do next.

## Locking down a single run

`agentty run` takes two flags for running on code you don't trust, like a pull request in CI:

- `--read-only` gives the model only tools that read. Write, shell and network tools aren't offered, and if the model asks for one anyway the call is refused.
- `--no-network` runs every command with no network at all (an empty network namespace, so not even DNS works) and refuses network tools. It needs the claybin sandbox; with the sandbox off or unavailable the run refuses to start instead of pretending.

```sh
agentty run --read-only --no-network "review this branch for anything that leaks secrets"
```

Both apply to that run only. Your saved sandbox setting is untouched.

## Modes

| Mode | Behaviour |
|---|---|
| `auto` (default) | Use a sandbox if one is available; otherwise run without and say so. |
| `on` | Require one. Refuse to start if the machine can't provide it. |
| `off` | No sandbox, including the write gate above. "Off" means off. |

```sh
agentty --sandbox on
```

If your machine blocks the feature agentty needs, `--sandbox auto` runs unsandboxed and prints why; `--sandbox on` refuses to start. On Linux, to enable it on a host that blocks unprivileged user namespaces, allow them — an AppArmor profile for `/usr/bin/bwrap` containing `userns,`, or `sudo sysctl -w kernel.unprivileged_userns_clone=1`. (claybin's default path doesn't need user namespaces at all, which is why it's tried first.) On macOS there's nothing to enable: seatbelt is always present.

:::warn
Running with `--workspace /` makes your entire filesystem the project folder, so there's nothing left to contain — agentty reports the sandbox as degraded. Keep the workspace scoped to the project you're working on.
:::

:::warn Don't use your home directory as the workspace
The workspace is the one place commands may write. Start agentty in `~` and every dotfile becomes writable: shell start-up files, `~/.local/bin`, autostart entries, other tools' config. The masks and the read-only binds above still hold for the files they cover, but anything they don't name is fair game, and the trust-handoff check hits its size cap on a whole home directory. Settings → Sandbox warns when this is the case. Start agentty in a project folder instead.
:::

## See it work

```bash
# inside the sandbox
$ cmake --build build -j     # works — project + system libraries reachable
$ cat ~/.ssh/id_rsa          # empty — the key never reaches the command
$ cat .env                   # empty — masked even inside your own project
```

:::warn
A sandbox shrinks the blast radius. It is not a substitute for reading what the agent proposes. With the network on, an approved command can still send your code somewhere.
:::

Technical detail, the full threat model, and how to verify any of this on your own machine: [`docs/SANDBOX.md`](https://github.com/agentty-org/agentty/blob/master/docs/SANDBOX.md).
