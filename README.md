# agentty

[![Release](https://img.shields.io/github/v/release/1ay1/agentty?display_name=tag&color=blue)](https://github.com/1ay1/agentty/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/1ay1/agentty/total?color=brightgreen)](https://github.com/1ay1/agentty/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C)](https://en.cppreference.com/w/cpp/26)

**Blazing-fast Claude in your terminal. 8.8 MB binary, sub-millisecond cold start, sandboxed by default, SSH-airgap in one command.**

A drop-in alternative to `claude-code` written in C++26 — no Node, no Python, no Electron, no `npm install`. Signs in with your **Claude Pro/Max OAuth** (or `ANTHROPIC_API_KEY`).

<p align="center">
  <img src="https://raw.githubusercontent.com/1ay1/agentty/master/agentty.gif" alt="agentty streaming a turn with a tool call landing inline" width="800" />
</p>

## Speed

Measured on the same Arch box, same shell, same day:

|                       | agentty (C++26)        | claude-code (Node) |
|-----------------------|------------------------|--------------------|
| Cold-start `--help`   | **< 1 ms**             | ~150 ms            |
| `--version`           | **< 1 ms**             | ~60 ms             |
| Binary on disk        | **8.8 MB**             | 222 MB (+ Node runtime) |
| Install               | `curl \| chmod +x`     | `npm i -g` + Node  |
| GC pauses mid-stream  | None                   | V8 GC              |

No JIT warmup, no `require()` graph to walk, no GC ticking while bytes stream in from the API. The TUI redraw loop is a `poll(2)` over the model stream + your input fd — every keystroke and every SSE chunk lands on the next frame.

## Why

Four things the official client doesn't try to do:

1. **Native speed.** C++26, statically linked, `posix_spawn` everywhere. Spawns in microseconds, no GC pauses mid-stream, no warmup. See the table above.
2. **One static binary.** 8.8 MB. `curl | chmod +x | run`. No Node runtime, no `npm install`, no version drift between machines.
3. **Sandbox by default.** Every shell and build call runs inside `bwrap` (Linux) / `sandbox-exec` (macOS). Workspace + system libs + network reachable; `~/.ssh`, `/etc`, other projects read-only. An approved `bash` call still can't `cat ~/.ssh/id_rsa`.
4. **One-command SSH airgap.** `agentty airgap user@host` runs the agent on a box with no direct internet — your laptop relays the bytes over SOCKS5-over-SSH. TLS pins on the real upstreams end-to-end.

Plus:

- **Open-standard skills.** Full [Agent Skills](https://agentskills.io) implementation — three-tier progressive disclosure, `.agents/` + `.claude/` interop so your existing Claude Code skills work unchanged. Teach it your internal codebase or DSL once; every session starts pre-loaded. See [Skills](#skills--teach-it-your-codebase-once).
- **Workspace boundary.** Filesystem tools refuse paths outside the launch directory (`--workspace /` opts out).
- **Inline render.** Lives at the bottom of your terminal, preserves scrollback, doesn't take over the screen.
- **Reads like a single function.** The reducer is one `std::visit` over a closed event sum; the permission matrix is a `constexpr` with `static_assert`s — change a policy cell and the build breaks, not a test nobody runs.

## Install

One line. Same line to update.

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

Detects your OS and arch, downloads the right binary from the latest release, verifies SHA256, installs to `/usr/local/bin` (if root) or `~/.local/bin`. **Re-running the same command updates** to the newest release. No `apt`, no `brew`, no version drift.

Flags: `--prefix ~/somewhere`, `--version v0.2.0`. Works on Linux + macOS, x86_64 + aarch64.

<details>
<summary><b>Distro-native packages</b> — if you'd rather your package manager track it</summary>

### Debian / Ubuntu

```bash
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty_0.2.0_amd64.deb
sudo dpkg -i agentty_0.2.0_amd64.deb       # or agentty_0.2.0_arm64.deb
```

Update: `dpkg -i` the new release's `.deb`.

### Fedora / RHEL / openSUSE

```bash
sudo rpm -Uvh https://github.com/1ay1/agentty/releases/latest/download/agentty-0.2.0-1.x86_64.rpm
```

`-U` is upgrade; works for the first install too. Same command future-proofs the update path.

### Arch Linux

```bash
yay -S agentty-bin                          # or paru, pikaur, etc.
yay -Syu agentty-bin                        # update
```

Or install the release-page `.pkg.tar.zst` with `sudo pacman -U`.

### macOS (Homebrew)

```bash
brew tap 1ay1/tap
brew install agentty
brew upgrade agentty                        # update
```

Linux Homebrew gets the prebuilt static binary; macOS builds from source (~1 min).

### Windows (Scoop)

```powershell
scoop bucket add 1ay1 https://github.com/1ay1/scoop-bucket
scoop install agentty
scoop update agentty                        # update
```

Direct `.exe`: `curl -L https://github.com/1ay1/agentty/releases/latest/download/agentty-windows-x86_64.exe -o agentty.exe`.

### Raw static binaries

Fully-static, no shared-library dependencies. Drop and run, replace on update:

```bash
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-linux-x86_64 -o agentty && chmod +x agentty
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-linux-aarch64 -o agentty && chmod +x agentty
```

Verify with [`SHA256SUMS`](https://github.com/1ay1/agentty/releases/latest) on the release page.

### From source

```bash
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty && cmake -B build && cmake --build build -j
./build/agentty
```

Requires GCC 14+ / Clang 18+ / MSVC 14.40+ and CMake 3.28+.

</details>

<details>
<summary><b>Cut your own release</b> — maintainer workflow, one script</summary>

Every published artifact is built locally, no CI:

```bash
scripts/bump.sh 0.2.0        # bumps CMakeLists, builds everything, tags, uploads via gh
```

Under the hood: rewrites `project(agentty VERSION ...)`, commits the bump, builds deb/rpm/pkg.tar.zst/tarball/binaries/homebrew/scoop/AUR manifests via `scripts/release.sh`, tags `vX.Y.Z`, pushes, and `gh release create`s with every artifact attached. Single source of truth: `CMakeLists.txt`.

Or step-by-step:

```bash
scripts/release.sh                  # build every artifact into dist/, no upload
scripts/release.sh --tag v0.2.0     # build + tag + upload via gh
```

</details>

## Quick start

```bash
cd path/to/your/project   # cwd is the workspace root
agentty
```

First launch opens an auth modal:

- **OAuth (Claude Pro/Max).** Opens your browser; the callback writes the token to `~/.config/agentty/credentials.json` (`0600`). agentty picks the right header on relaunch automatically.
- **API key.** Paste an `sk-ant-…` token. Saved to the same file.

OAuth against your existing Pro/Max subscription is the main path — no extra billing, same account you already pay for.

Override order, highest priority first:

1. `-k <key>` / `--key <key>` — single-session, never written to disk.
2. `ANTHROPIC_API_KEY` env var.
3. `CLAUDE_CODE_OAUTH_TOKEN` env var.
4. The on-disk creds from the modal.

Then you're in a thread. Type, hit `Enter`. Mid-stream typing queues your next message and lands it when the current turn finishes. `Esc` cancels.

You start in the `Ask` profile — writes, shell calls, and network calls each prompt before running. `S-Tab` cycles to `Write` (autonomous) or `Minimal` (prompts for everything but pure reads). Choice persists.

Threads live at `~/.agentty/threads/<workspace-hash>/`, one JSON file each — safe to inspect, back up, delete. `^J` opens the thread list.

```bash
agentty --workspace ~/code/other-project   # run against a different workspace without cd
agentty status                              # which auth source will be used
agentty login / logout                      # non-interactive auth, useful over SSH
agentty airgap user@host                    # see below
agentty acp                                 # run as an ACP agent for Zed (see below)
```

## Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^T     todo / plan
Esc        cancel / reject        ^/     model picker
S-Tab      cycle profile          ^N     new thread
                                  ^C     quit
```

## Tools

Each tool gets a purpose-built widget: diffs render as diffs, search results group by file with line numbers, bash shows exit codes, todos become checklists.

`read`, `write`, `edit`, `bash`, `grep`, `glob`, `list_dir`, `find_definition`, `web_fetch`, `web_search`, `todo`, `diagnostics`, `git_status`, `git_diff`, `git_log`, `git_commit`, `remember`, `forget`, `wipe_memory`, `task`, `skill`.

## Skills — teach it your codebase once

agentty implements the open [Agent Skills](https://agentskills.io) standard —
the same `SKILL.md` format Claude Code, Codex, and Cursor use — with full
three-tier progressive disclosure:

1. **Catalog** — every skill's name + one-line description rides the system
   prompt (~50 tokens each). Twenty installed skills cost a paragraph, not a
   context window.
2. **Instructions** — the full `SKILL.md` body loads only when the model
   decides the task matches (or you ask for it). Loading twice in one thread
   returns a one-line "already active" sentinel instead of re-paying the body.
3. **Resources** — bundled `scripts/`, `references/`, `assets/` are *listed*
   at activation but never eagerly read; the model `read`s the exact file the
   instructions call for. Skill directories are read-allowlisted — and *only*
   read: the write/edit boundary never widens.

Drop a directory with a `SKILL.md` in any of these and it's live on the next
turn — no restart, no config (mtime-keyed rescan):

```
<project>/.agentty/skills/   <project>/.agents/skills/   <project>/.claude/skills/
~/.agentty/skills/           ~/.agents/skills/           ~/.claude/skills/
```

The `.agents/` convention means skills installed by any compliant client are
visible to agentty and vice versa; `.claude/` compatibility means your
existing Claude Code skills work unchanged. Project skills shadow user skills
with the same name. Frontmatter supports the full spec surface —
`compatibility`, `allowed-tools`, `license`, `metadata`, and
`disable-model-invocation` (hidden from the model, loadable on request) —
including YAML block scalars (`description: >-`). Parsing is lenient per the
spec's interop guidance: unquoted colons, name/dir mismatches, and missing
frontmatter all still load.

Activate explicitly with a slash command — `/skill-name fix the parser` — and
the harness splices the instructions in without waiting for the model to
decide. Validate your skills the way `skills-ref validate` would, with no
extra tooling:

```bash
agentty skills      # list every discovered skill + spec-lint diagnostics
                    # exit 1 on warnings — drop it straight into CI
```

Why this matters: on a codebase or internal DSL no model has ever seen,
agent accuracy starts below 20% — with curated skills (grammar sheets, golden
examples, your team's ticket procedure) it reaches ~85%
([research](https://arxiv.org/abs/2410.03981)). Write the skill once, commit
it to the repo, and every teammate's agent knows your conventions from turn
one. Pair with `CLAUDE.md` (always-on context) and `remember` (the model's
own scratchpad) for the complete memory hierarchy.

## Air-gapped hosts (SSH tunnel)

Run agentty on a box that can't reach the internet directly. Your laptop relays the bytes; TLS pins on the real upstreams, so the network between laptop and remote can't MITM you.

One command, from the laptop that *does* have internet:

```bash
agentty airgap --setup user@airgapped-host    # first time: also copies your credentials
agentty airgap user@airgapped-host            # every time after
```

How: `ssh -R 1080` exposes a SOCKS5 proxy on the remote at `localhost:1080`; connections to it tunnel back over SSH and are dialed by your laptop. The remote agentty gets `AGENTTY_SOCKS_PROXY=localhost:1080` and routes every TCP destination through it — chat, OAuth refresh, `web_fetch`, `web_search`. One env var, no per-host enumeration.

> **Trust model.** Airgap doesn't trust the network between laptop and remote, but does trust the *remote* with your tokens — `--setup` copies `credentials.json` over at `chmod 600`. A compromised remote can exfiltrate your Anthropic credentials independent of the tunnel. Use it on hosts you'd already trust with the same secret.

Bare-metal version, if you'd rather not use the wrapper:

```bash
ssh -t -R 1080 user@airgapped-host \
    'AGENTTY_SOCKS_PROXY=localhost:1080 agentty'
```

Requires OpenSSH ≥ 7.6 on both ends (October 2017 — every distro has it). `AGENTTY_AIRGAP_SSH` injects extra `ssh` flags; `--remote-agentty PATH` if it isn't on the remote PATH.

### Behind a TLS-terminating corporate proxy

SOCKS keeps TLS end-to-end, so cert verification works untouched. Different story if you route through a forward proxy that re-encrypts with its own cert (Zscaler, Bluecoat, mitmproxy). Install the proxy's CA into the system trust store (`update-ca-certificates` on Debian, `update-ca-trust` on Fedora) — agentty picks up system roots at startup. As a last resort, `AGENTTY_INSECURE=1` skips peer verification entirely; don't ship that to anyone you care about.

## Use agentty inside Zed (ACP)

agentty speaks the [Agent Client Protocol](https://agentclientprotocol.com) — the
same protocol Zed uses to drive Claude Code and Gemini. Point Zed at the
`agentty acp` subcommand and your terminal agent becomes a first-class agent
panel inside the editor: streaming responses, inline diffs for every edit, and
native permission prompts before any file write or shell command.

Add this to Zed's `settings.json` (`zed: open settings`):

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp"]
    }
  }
}
```

Then open the agent panel (`cmd-?` / `ctrl-?`), pick **agentty** from the agent
list, and prompt. Auth is whatever `agentty login` already set up — the ACP
process reads the same `~/.config/agentty/credentials.json`, so there's nothing
extra to configure. Set the model per-subprocess in the `args` (it does *not*
clobber your TUI's saved model):

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp", "-m", "claude-haiku-4-5", "--profile", "ask"]
    }
  }
}
```

`--profile` picks how eagerly Zed prompts you before a tool runs:

- **`ask`** (default) — prompt for `write` / `edit` / `bash` / network; read-only
  inspection (`read` / `grep` / `glob` / `list_dir`) runs without a dialog so the
  loop stays fluid.
- **`minimal`** — prompt for *everything* that touches the outside world, reads
  included. Tightest leash.
- **`write`** — same write/exec/net prompts as `ask`, but never prompts for
  reads.

What works over ACP:

- **Streaming text** — the model's reply renders token-by-token in Zed's panel.
- **Tool calls** — every `read` / `edit` / `bash` / `grep` / … shows up as a
  Zed tool card with the right icon, the raw arguments, and live status
  (pending → running → done/failed).
- **Inline diffs** — `write` and `edit` emit ACP `diff` content, so Zed renders
  the file change inline and lets you review it in place.
- **Follow-along** — read/edit/write/grep tool calls carry the file path as an
  ACP `location`, so Zed can open and highlight the file the agent is touching
  in real time.
- **Permission prompts** — side-effecting tools (`bash`, `write`, `edit`,
  network) trigger Zed's native allow/reject dialog before they run; the
  `--profile` flag (above) tunes exactly which tools prompt.
- **Session modes** — the three permission tiers (`Ask` / `Write` / `Minimal`)
  surface as ACP session modes, so you can switch them live from Zed's mode
  picker (`session/set_mode`) without restarting the agent; the starting tier
  is the `--profile` flag.
- **Cancellation** — stop a turn from Zed and the in-flight stream tears down.
- **Full session lifecycle** — agentty advertises and implements the complete
  ACP v1 session surface: `session/new`, `session/load`, `session/resume`,
  `session/list`, `session/close`, `session/delete`, plus `logout`. Zed can
  enumerate past sessions, reopen any of them, and prune them — all backed by
  the on-disk thread store.
- **Session persistence + reload** — every session is written to agentty's
  on-disk thread store after each turn (the *same* format the TUI uses), so it
  survives a subprocess restart. Zed can call `session/load` to resume a past
  conversation: agentty replays the full transcript (user + assistant messages
  and tool cards) as `session/update` notifications, then hands back control.
  Sessions started in Zed also show up in the standalone TUI's thread picker,
  and vice versa.
- **Workspace sandbox** — file tools stay inside the session's `cwd` (the
  folder you opened in Zed); `bash` is wrapped in bwrap/sandbox-exec exactly
  like the standalone TUI.

The ACP agent is the *same* engine as the TUI — same provider, same tools, same
wire-message shaping, same permission policy — just driven over JSON-RPC on
stdio instead of a terminal. Any other ACP client (not just Zed) works the same
way.

The same `--workspace` and `--sandbox` switches the TUI accepts apply in ACP
mode (they run before the agent starts), so you can loosen or disable both from
the Zed `args`:

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp", "--workspace", "/", "--sandbox", "off"]
    }
  }
}
```

- **`--workspace DIR`** — file tools refuse paths outside `DIR` (default: the
  folder you opened in Zed). Widen it to any directory, or pass `--workspace /`
  to **turn the filesystem gate off** entirely (unrestricted access).
- **`--sandbox off`** — stop wrapping `bash` / `diagnostics` in bwrap /
  sandbox-exec. Use when you already have external isolation (a container, VM,
  or you simply trust the loop). `--sandbox on` does the opposite: refuse to
  start if no OS sandbox backend is available.

With `--workspace /` the startup line honestly reports `sandbox: degraded` —
binding the whole filesystem means there's no containment left to enforce.

### agentty-in-Zed on an air-gapped remote

Yes — you can run agentty inside Zed against a server with **zero internet access**. Your laptop relays every byte. Nothing runs on the remote besides the agent + your shell session.

#### Roles (read once)

- **Laptop** — has internet, has your Anthropic OAuth/API key, runs Zed, runs `ssh`. The relay.
- **Remote** — the air-gapped box. No internet. Runs `agentty acp` (spawned by Zed via ssh).

Everything below is done **on the laptop**.

#### Prerequisites

1. `agentty` installed on **both** machines (`which agentty` on each prints a path).
2. Passwordless SSH from laptop to remote works (`ssh user@remote echo ok` returns `ok` without prompting). If it prompts, run `ssh-copy-id user@remote` first.
3. You're logged in on the laptop (`agentty status` shows OAuth or API key). If not: `agentty login`.
4. Zed installed on the laptop.

#### One-time setup (two commands, ~5 seconds total)

From the laptop:

```bash
# 1. Copy your Anthropic credentials to the remote (once):
agentty airgap --setup user@remote

# 2. Print the Zed config block for this remote:
agentty airgap user@remote --acp -m claude-haiku-4-5 --profile ask
```

The second command **prints to stderr and exits** — it does not start anything. You'll see something like:

```
agentty airgap --acp: add this to Zed's settings.json
  → /home/you/.config/zed/settings.json:

  "agent_servers": {
    "agentty (airgap)": {
      "command": "ssh",
      "args": ["-T", "-R", "1080", "-o", "ExitOnForwardFailure=yes", ...
              "user@remote",
              "AGENTTY_SOCKS_PROXY=localhost:1080 exec agentty acp -m claude-haiku-4-5 --profile ask"]
    }
  }
```

#### Wire it into Zed (one paste)

1. Open Zed's settings: `cmd-,` (macOS) or `ctrl-,` (Linux) → the settings file opens.
2. Paste the printed `"agent_servers"` block into the JSON. If you already have an `agent_servers` object, merge the `"agentty (airgap)"` key into it.
3. Save.

#### Use it

1. Open the agent panel in Zed: `cmd-?` / `ctrl-?`.
2. From the agent picker, pick **agentty (airgap)**.
3. Prompt.

That's it. Zed spawns `ssh` directly — a single process is the tunnel, the agent, and the JSON-RPC transport. Zed owns its lifecycle: close the agent panel, the ssh + remote `agentty acp` both die. No `ssh -N` running in the background, no wrapper script, no daemon.

#### What's happening under the hood

- Zed runs `ssh -R 1080 user@remote 'AGENTTY_SOCKS_PROXY=localhost:1080 exec agentty acp ...'` directly.
- `-R 1080` exposes a SOCKS5 proxy on the remote's `localhost:1080`. The remote `agentty` is told to route every outbound connection (chat, OAuth refresh, `web_fetch`, `web_search`) through it. Those connections tunnel back over SSH and are dialed by your laptop.
- ACP JSON-RPC flows over ssh's stdio. No extra port, no extra process.
- TLS still negotiates end-to-end with the real upstream (api.anthropic.com etc.). The laptop sees encrypted bytes only — it can't MITM.

#### Trust model (read before `--setup`)

`--setup` copies your laptop's `~/.config/agentty/credentials.json` to the remote (chmod 600). That file contains your OAuth refresh token (or API key). A compromised remote can exfiltrate it independent of the tunnel. **agentty airgap protects the network between laptop and remote, not the remote itself** — treat the remote as a credential-bearing peer, not a sandboxed proxy.

#### Troubleshooting

- **Zed shows the agent greyed out / "failed to start"** — check `~/.local/share/zed/logs/Zed.log` (Linux) or `~/Library/Logs/Zed/Zed.log` (macOS) for the ssh spawn line. Common causes: `ssh` not on Zed's PATH (rare), the remote prompts for a password (run `ssh-copy-id` first), or `agentty` isn't on the remote's PATH (pass `--remote-agentty /full/path/to/agentty` to the `airgap` command, then re-print the config).
- **"connection refused" or "could not resolve host" mid-turn** — the SOCKS tunnel dropped. Usually a flaky network on the laptop. Close the agent panel in Zed and reopen — Zed respawns ssh.
- **Slow first response** — first TLS handshake to api.anthropic.com tunnels through ssh, adds ~100 ms. Subsequent turns reuse the connection.
- **`agentty: command not found` in the ssh spawn log** — agentty isn't on the remote shell's non-interactive PATH (`ssh user@remote 'echo $PATH'` shows what Zed sees). Either install it system-wide on the remote, or re-print the config with `--remote-agentty /full/path/to/agentty`.
- **The remote needs a non-default ssh port / key / jump host** — export `AGENTTY_AIRGAP_SSH="-p 2222 -i ~/.ssh/work -J bastion"` before running `agentty airgap ... --acp`. Those flags get embedded into the printed config.

> Want to drive it from the command line first to confirm it works before touching Zed? `agentty airgap user@remote` (no `--acp`) launches the agentty TUI running on the remote, in your local terminal. If that works, the ACP version will too — same tunnel, different transport.

> Run `agentty acp` by hand and it'll sit waiting for newline-delimited
> JSON-RPC on stdin (diagnostics go to stderr, stdout is the protocol channel).
> `scripts/acp_smoke.py` is a tiny reference client that drives a full
> initialize → prompt → tool → permission round-trip;
> `scripts/acp_methods_test.py` exercises the rest of the v1 method surface
> (modes, list/resume/close/delete, logout) offline.

## How it compares

|                     | agentty                               | claude-code                       | aider                               |
|---------------------|---------------------------------------|-----------------------------------|-------------------------------------|
| Language / runtime  | C++26 — single static binary          | TypeScript / Node                 | Python                              |
| Footprint           | ~9 MB                                 | npm + Node runtime                | pip + Python runtime                |
| Air-gapped mode     | Yes (`agentty airgap`, SOCKS5/SSH)    | No                                | No                                  |
| Auth                | OAuth (Pro/Max) + `ANTHROPIC_API_KEY` | OAuth + `ANTHROPIC_API_KEY`       | per-provider env vars               |
| Models              | Claude (Anthropic)                    | Claude (Anthropic)                | many (OpenAI / Anthropic / local …) |

Want a multi-model agent across providers? aider. Want Anthropic's first-party experience with their support behind it? claude-code. agentty is the niche pick when you specifically want a single-binary Claude client with no runtime dependency, or you need an air-gapped host through an SSH tunnel.

## How it works

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. Strong ID newtypes (`ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier`) — swapping arguments is a compile error, not a debugging session.

View is a single function `Model -> Element`. Rendering is delegated to [maya](https://github.com/1ay1/maya), a sister header-mostly TUI engine — agentty builds widget Configs from `Model` state, maya owns every chrome glyph, layout decision, and breathing animation. The host constructs no Elements.

Subprocess uses `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX, `CreateProcessW` + a reader thread on Windows — no GNU `timeout` dependency, no `popen` quoting hazards. File writes are atomic (`write` + `fsync`/`_commit` + `rename`/`MoveFileExW`).

Deep dive: [`docs/RENDERING.md`](docs/RENDERING.md) walks the view pipeline turn-by-turn; [`docs/UI.md`](docs/UI.md) is the per-widget Config reference.

## Standalone build

```bash
cmake -B build -DAGENTTY_STANDALONE=ON
```

Statically links OpenSSL + nghttp2 + libstdc++ + libgcc when their `.a` archives are installed. libc stays dynamic on Linux/macOS (fully-static glibc breaks `getaddrinfo` and the NSS resolver). Pass `-DAGENTTY_FULLY_STATIC=ON` with a musl toolchain for a 100% static binary. Windows: implies `/MT` and pulls third-party libs from the `x64-windows-static` vcpkg triplet.

Accurate one-liner: **statically linked except libc and (usually) OpenSSL.** For a 100%-static drop-in, the [latest release](https://github.com/1ay1/agentty/releases/latest) ships pre-built `agentty-linux-x86_64` and `agentty-linux-aarch64` with zero shared-library dependencies (Alpine + musl + GCC 14.2).

## Status

Works on Linux, macOS, and Windows — all three actively tested and built daily. Prebuilt release binaries for Linux (x86_64, aarch64) and Windows (x86_64); macOS builds from source in seconds.

File bugs with `$TERM`, your terminal emulator name, and a screenshot. Code-path bugs welcome too — paste the relevant block and `git rev-parse HEAD`.

## License

MIT — see [LICENSE](LICENSE).
