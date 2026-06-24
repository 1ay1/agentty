# agentty

[![Release](https://img.shields.io/github/v/release/1ay1/agentty?display_name=tag&color=blue)](https://github.com/1ay1/agentty/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/1ay1/agentty/total?color=brightgreen)](https://github.com/1ay1/agentty/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C)](https://en.cppreference.com/w/cpp/26)

**A coding agent in your terminal that starts in under a millisecond.** One 9 MB static binary — no Node, no Python, no Electron, no `npm install`. Sandboxed by default. Runs against Claude, GPT, Groq, OpenRouter, or your local Ollama. SSH-airgaps in one command.

<p align="center">
  <img src="https://raw.githubusercontent.com/1ay1/agentty/master/agentty.gif" alt="agentty streaming a turn with a tool call landing inline" width="800" />
</p>

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

## Why agentty

- **It's instant.** C++26, statically linked, `posix_spawn` everywhere. Cold-start `--help` is **< 1 ms** vs ~150 ms for the Node CLI. No JIT warmup, no `require()` graph, no GC pauses ticking while SSE bytes stream in.
- **It's one file.** A 9 MB binary you `curl | chmod +x`. No runtime to install, no version drift between machines.
- **It's many models.** Claude (OAuth Pro/Max **or** API key), OpenAI, Groq, OpenRouter, Together, Cerebras, local **Ollama**, or any OpenAI-compatible `host:port`. Switch live with `^P`.
- **It's sandboxed.** Every shell/build call runs inside `bwrap` (Linux) / `sandbox-exec` (macOS). An approved `bash` call still can't `cat ~/.ssh/id_rsa`. File tools refuse paths outside your workspace.
- **It air-gaps.** `agentty airgap user@host` runs the agent on a box with no internet; your laptop relays the bytes over SOCKS5-over-SSH with TLS pinned end-to-end.
- **It plugs in.** Speaks [ACP](https://agentclientprotocol.com) (drive it from inside Zed), serves its tools over [MCP](https://modelcontextprotocol.io), and implements the open [Agent Skills](https://agentskills.io) standard.

### Speed, measured

Same Arch box, same shell, same day:

|                      | agentty (C++26)    | claude-code (Node)      |
|----------------------|--------------------|-------------------------|
| Cold-start `--help`  | **< 1 ms**         | ~150 ms                 |
| `--version`          | **< 1 ms**         | ~60 ms                  |
| Binary on disk       | **9 MB**           | 222 MB (+ Node runtime) |
| Install              | `curl \| chmod +x` | `npm i -g` + Node       |
| GC pauses mid-stream | None               | V8 GC                   |

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
```

Detects OS/arch, downloads the right binary, verifies SHA256, installs to `/usr/local/bin` or `~/.local/bin`. **Re-run the same line to update.** Linux + macOS, x86_64 + aarch64. Flags: `--prefix ~/somewhere`, `--version v0.2.0`.

<details>
<summary><b>Package managers &amp; raw binaries</b></summary>

```bash
# Debian / Ubuntu
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty_0.2.0_amd64.deb
sudo dpkg -i agentty_0.2.0_amd64.deb            # arm64 variant also published

# Fedora / RHEL / openSUSE
sudo rpm -Uvh https://github.com/1ay1/agentty/releases/latest/download/agentty-0.2.0-1.x86_64.rpm

# Arch
yay -S agentty-bin                              # paru/pikaur work too

# macOS
brew tap 1ay1/tap && brew install agentty

# Windows
scoop bucket add 1ay1 https://github.com/1ay1/scoop-bucket && scoop install agentty

# Fully-static binary, drop and run (Alpine + musl, zero shared deps)
curl -fsSL https://github.com/1ay1/agentty/releases/latest/download/agentty-linux-x86_64 -o agentty && chmod +x agentty
```

Verify against [`SHA256SUMS`](https://github.com/1ay1/agentty/releases/latest) on the release page. Each manager's normal upgrade command (`dpkg -i`, `rpm -U`, `yay -Syu`, `brew upgrade`, `scoop update`) tracks new releases.

</details>

<details>
<summary><b>From source</b></summary>

```bash
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty && cmake -B build && cmake --build build -j
./build/agentty
```

GCC 14+ / Clang 18+ / MSVC 14.40+, CMake 3.28+. `-DAGENTTY_STANDALONE=ON` statically links OpenSSL + nghttp2 + libstdc++; add `-DAGENTTY_FULLY_STATIC=ON` with a musl toolchain for a 100% static binary.

</details>

## Quick start

```bash
cd path/to/your/project   # cwd is the workspace root
agentty
```

First launch opens an auth modal: **OAuth** (uses your existing Claude Pro/Max subscription — no extra billing) or paste an `sk-ant-…` **API key**. Either is saved to `~/.config/agentty/credentials.json` (`0600`).

Then type and hit `Enter`. Typing mid-stream queues your next message; `Esc` cancels. You start in the **Ask** profile — writes, shell, and network each prompt before running. `S-Tab` cycles to **Write** (autonomous) or **Minimal** (prompts for everything but pure reads). Threads live one-JSON-file-each at `~/.agentty/threads/`; `^J` lists them.

```bash
agentty --provider ollama -m qwen2.5-coder    # run a local model, no key needed
agentty --provider openai -m gpt-4o           # or GPT, Groq, OpenRouter, …
agentty --workspace ~/code/other              # different workspace, no cd
agentty status                                # which auth/provider is active
agentty login / logout                        # non-interactive auth (handy over SSH)
agentty airgap user@host                      # run on an internet-less box
agentty acp                                   # run as an ACP agent for Zed
agentty mcp-serve                             # serve agentty's tools over MCP
```

### Keys

```
Enter      send                   ^K     command palette
Alt+Enter  newline                ^J     thread list
Ctrl+E     expand composer        ^T     todo / plan
Esc        cancel / reject        ^P     model / provider picker
S-Tab      cycle profile          ^N     new thread
                                  ^C     quit
```

## Providers

agentty is multi-provider. Anthropic is the default (OAuth or `ANTHROPIC_API_KEY`); everything else speaks the OpenAI-compatible wire:

| Spec         | Backend                              | Key                                   |
|--------------|--------------------------------------|---------------------------------------|
| `anthropic`  | Claude                               | OAuth (Pro/Max) or `ANTHROPIC_API_KEY` |
| `openai`     | GPT                                  | `OPENAI_API_KEY`                      |
| `groq`       | Llama/Mixtral on Groq LPUs           | `GROQ_API_KEY`                        |
| `openrouter` | any model via openrouter.ai          | `OPENROUTER_API_KEY`                  |
| `together`   | open models on together.ai           | `TOGETHER_API_KEY`                    |
| `cerebras`   | wafer-scale inference                | `CEREBRAS_API_KEY`                    |
| `ollama`     | **local** models at `localhost:11434`| none                                  |
| `host:port`  | any OpenAI-compatible endpoint       | `-k` / `OPENAI_API_KEY`               |

`--provider X` persists like `-m`, and restores the model you last used on that provider. Switch live in-app with `^P`. Weak local models get extra care: agentty inlines a JSON tool-call protocol, grammar-constrains the output, repairs malformed tool args, and never lets a reply render blank.

## Tools

Each tool gets a purpose-built widget: diffs render as diffs, search groups by file with line numbers, bash shows exit codes, todos become checklists.

`read` · `write` · `edit` · `bash` · `grep` · `glob` · `list_dir` · `find_definition` · `web_fetch` · `web_search` · `search_docs` · `todo` · `diagnostics` · `git_status` · `git_diff` · `git_log` · `git_commit` · `remember` · `forget` · `wipe_memory` · `task` · `skill`

## Skills — teach it your codebase once

agentty implements the open [Agent Skills](https://agentskills.io) standard (the same `SKILL.md` format Claude Code, Codex, and Cursor use) with full three-tier progressive disclosure:

1. **Catalog** — each skill's name + one-line description rides the system prompt (~50 tokens). Twenty skills cost a paragraph, not a context window.
2. **Instructions** — the full `SKILL.md` body loads only when the task matches (or you `/skill-name …`). Re-loading in the same thread returns a one-line "already active" sentinel.
3. **Resources** — bundled `scripts/`, `references/`, `assets/` are *listed* at activation; the model `read`s the exact file it needs. Skill dirs are read-allowlisted — and only read; the write boundary never widens.

Drop a `SKILL.md` directory anywhere below and it's live next turn — no restart, no config:

```
<project>/.agentty/skills/   <project>/.agents/skills/   <project>/.claude/skills/
~/.agentty/skills/           ~/.agents/skills/           ~/.claude/skills/
```

`.agents/` interop means skills from any compliant client are visible to agentty; `.claude/` means your existing Claude Code skills work unchanged. `agentty skills` lists every discovered skill with spec-lint diagnostics (exit 1 on warnings — drop it into CI).

Why it matters: on a codebase or internal DSL no model has seen, agent accuracy starts below 20% — with curated skills it reaches ~85% ([research](https://arxiv.org/abs/2410.03981)). Write it once, commit it, and every teammate's agent knows your conventions from turn one. Pair with `CLAUDE.md` (always-on context) and `remember` (the model's scratchpad) for the full memory hierarchy.

## Air-gapped hosts

Run agentty on a box that can't reach the internet. Your laptop relays the bytes; TLS pins on the real upstreams, so the network in between can't MITM you.

```bash
agentty airgap --setup user@host   # first time: also copies your credentials
agentty airgap user@host           # every time after
```

`ssh -R 1080` exposes a SOCKS5 proxy on the remote; the remote agentty routes every TCP destination (chat, OAuth refresh, `web_fetch`, `web_search`) through it back over SSH. One env var, no per-host enumeration. Requires OpenSSH ≥ 7.6 on both ends.

> **Trust model.** Airgap protects the *network* between laptop and remote, not the remote itself — `--setup` copies `credentials.json` to it. Use it on hosts you'd already trust with the same secret.

> Behind a TLS-terminating corporate proxy (Zscaler/Bluecoat/mitmproxy)? SOCKS keeps TLS end-to-end, so install the proxy's CA into the system trust store and agentty picks it up. Last resort: `AGENTTY_INSECURE=1` skips peer verification.

## Inside Zed (ACP)

agentty speaks the [Agent Client Protocol](https://agentclientprotocol.com) — the same protocol Zed uses for Claude Code and Gemini. Point Zed at `agentty acp` and your terminal agent becomes a first-class agent panel: streaming responses, inline diffs for every edit, native permission prompts, follow-along file highlighting, and the full session lifecycle (new / load / resume / list / close / delete). Sessions are shared with the TUI's thread store, so a conversation started in Zed shows up in `^J` and vice versa.

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

Open the agent panel (`cmd-?` / `ctrl-?`), pick **agentty**, prompt. Auth is whatever `agentty login` set up. The `--profile` flag tunes which tools prompt (`ask` / `minimal` / `write`), live-switchable from Zed's mode picker. `--workspace` and `--sandbox` apply here too. It's the *same* engine as the TUI — same provider, tools, and permission policy, just driven over JSON-RPC on stdio.

<details>
<summary><b>agentty-in-Zed on an air-gapped remote</b> — full walkthrough</summary>

You can run agentty inside Zed against a server with **zero internet**. The laptop (has internet + your key, runs Zed + ssh) relays for the remote (air-gapped, runs `agentty acp`).

**Prereqs:** `agentty` on both machines, passwordless SSH laptop→remote (`ssh-copy-id` if it prompts), and `agentty status` shows you logged in on the laptop.

```bash
# 1. Copy credentials to the remote (once):
agentty airgap --setup user@remote
# 2. Print the Zed config block (this only PRINTS, starts nothing):
agentty airgap user@remote --acp -m claude-haiku-4-5 --profile ask
```

Paste the printed `"agent_servers"` block into Zed's `settings.json`, save, then pick **agentty (airgap)** from the agent panel. Zed spawns one `ssh` process that is the tunnel, the agent, and the JSON-RPC transport at once — close the panel and both ssh + the remote agent die. No background daemon.

Under the hood Zed runs `ssh -R 1080 user@remote 'AGENTTY_SOCKS_PROXY=localhost:1080 exec agentty acp …'`: ACP flows over ssh stdio, outbound connections tunnel back through the SOCKS proxy and are dialed by your laptop, TLS negotiates end-to-end with the real upstream.

- **Greyed-out / "failed to start"** → check Zed's log for the ssh line; usual causes are a password prompt (`ssh-copy-id`) or `agentty` not on the remote PATH (pass `--remote-agentty /full/path`, re-print).
- **Non-default ssh port / key / jump host** → `export AGENTTY_AIRGAP_SSH="-p 2222 -i ~/.ssh/work -J bastion"` before printing the config; the flags get embedded.

> Confirm it works from the CLI first: `agentty airgap user@remote` (no `--acp`) runs the remote agent's TUI in your local terminal. Same tunnel, different transport.

</details>

## How it works

A pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. The reducer is one `std::visit` over a closed event sum; the permission matrix is a `constexpr` with `static_assert`s — change a policy cell and the *build* breaks, not a test nobody runs. Strong ID newtypes (`ToolCallId`, `ThreadId`, `OAuthCode`) make swapping arguments a compile error.

The view is a single `Model -> Element` function; rendering is delegated to [maya](https://github.com/1ay1/maya), a sister header-mostly TUI engine that owns every glyph, layout decision, and breathing animation. Subprocess management is `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines (no GNU `timeout`, no `popen` quoting hazards); file writes are atomic (`write` + `fsync` + `rename`).

Deep dive: [`docs/RENDERING.md`](docs/RENDERING.md) walks the view pipeline turn-by-turn; [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) the rest.

## How it compares

|                    | agentty                                   | claude-code           | aider                 |
|--------------------|-------------------------------------------|-----------------------|-----------------------|
| Runtime            | C++26 — single static binary              | TypeScript / Node     | Python                |
| Footprint          | ~9 MB, no runtime                         | npm + Node            | pip + Python          |
| Cold start         | **< 1 ms**                                | ~150 ms               | ~300 ms               |
| Models             | Claude · GPT · Groq · OpenRouter · Ollama | Claude                | many                  |
| Sandbox by default | Yes (bwrap / sandbox-exec)                | No                    | No                    |
| Air-gapped mode    | Yes (`agentty airgap`)                    | No                    | No                    |
| ACP / MCP / Skills | Yes / Yes / Yes                           | Skills                | No                    |

agentty is the pick when you want a single-binary, dependency-free agent that's fast, sandboxed, model-agnostic, and works behind an SSH airgap.

## Status

Linux, macOS, and Windows — all three built and tested daily. Prebuilt binaries for Linux (x86_64, aarch64) and Windows; macOS builds from source in seconds.

File bugs with `$TERM`, your terminal emulator, and a screenshot. Code-path bugs welcome — paste the block and `git rev-parse HEAD`.

## License

MIT — see [LICENSE](LICENSE).
