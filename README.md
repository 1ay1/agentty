<h1 align="center">agentty</h1>

<p align="center">
  <b>AI pair programming in your terminal</b><br>
  One static binary. Sub-millisecond startup. Any model.
</p>

<p align="center">
  <a href="https://github.com/1ay1/agentty/releases/latest"><img src="https://img.shields.io/github/v/release/1ay1/agentty?style=flat-square&color=blue" alt="Release" /></a>
  <a href="https://github.com/1ay1/agentty/stargazers"><img src="https://img.shields.io/github/stars/1ay1/agentty?style=flat-square&color=f1c40f&labelColor=555555" alt="Stars" /></a>
  <a href="https://github.com/1ay1/agentty/releases"><img src="https://img.shields.io/github/downloads/1ay1/agentty/total?style=flat-square&color=brightgreen" alt="Downloads" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="License" /></a>
</p>

<p align="center">
  <img src="https://raw.githubusercontent.com/1ay1/agentty/master/agentty.gif" alt="agentty demo" width="800" />
</p>

## Getting Started

```bash
curl -fsSL https://raw.githubusercontent.com/1ay1/agentty/master/install.sh | sh
cd your-project
agentty
```

First launch opens auth — OAuth (uses your Claude Pro/Max subscription) or paste an API key. Then type and hit Enter.

## Features

<table>
<tr>
<td width="50%">

### ⚡ Instant startup
Cold start under 1ms. No Node, no Python, no npm install. Just a static binary.

### 🔌 Any model
Claude, GPT, Groq, OpenRouter, Ollama, or any OpenAI-compatible endpoint. Switch live with `^P`.

### 🛡️ Sandboxed by default
Every shell call runs inside bwrap (Linux) / sandbox-exec (macOS). File tools refuse paths outside your workspace.

</td>
<td width="50%">

### 🌐 Air-gapped mode
Run on a box with no internet. Your laptop relays the bytes over SSH with TLS pinned end-to-end.

### 🔧 Full tool suite
read · write · edit · bash · grep · glob · git · web · search_docs · task — each with a purpose-built widget.

### 🧠 Learns your codebase
Agent Skills + remember/forget memory. Teach it once, every session knows your conventions.

</td>
</tr>
</table>

## Providers

```bash
agentty                                    # Claude (default)
agentty --provider openai -m gpt-4o        # GPT
agentty --provider groq -m llama-3.3-70b   # Groq
agentty --provider ollama -m qwen2.5-coder # Local model
agentty --provider openrouter              # Any model via OpenRouter
```

`--provider` persists. Switch live in-app with `^P`.

## Keys

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `Enter` | Send | `^K` | Command palette |
| `Esc` | Cancel / reject | `^J` | Thread list |
| `S-Tab` | Cycle profile | `^P` | Model picker |
| `Alt+Enter` | Newline | `^N` | New thread |

## More

<details>
<summary><b>Installation options</b></summary>

```bash
# Debian / Ubuntu
curl -fsSLO https://github.com/1ay1/agentty/releases/latest/download/agentty_amd64.deb
sudo dpkg -i agentty_amd64.deb

# Arch
yay -S agentty-bin

# macOS
brew tap 1ay1/tap && brew install agentty

# Windows
scoop bucket add 1ay1 https://github.com/1ay1/scoop-bucket && scoop install agentty

# From source
git clone --recursive git@github.com:1ay1/agentty.git
cd agentty && cmake -B build && cmake --build build -j
```

</details>

<details>
<summary><b>Air-gapped hosts</b></summary>

```bash
agentty airgap --setup user@host   # first time: copies credentials
agentty airgap user@host           # every time after
```

Your laptop relays via SOCKS5-over-SSH. TLS pins on real upstreams — the network in between can't MITM you.

</details>

<details>
<summary><b>Inside Zed (ACP)</b></summary>

agentty speaks the [Agent Client Protocol](https://agentclientprotocol.com) — the same protocol Zed uses for Claude Code. Add to Zed's settings:

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

</details>

<details>
<summary><b>Agent Skills</b></summary>

Drop a `SKILL.md` anywhere under `.agentty/skills/` or `~/.agentty/skills/` — it's live next turn. Compatible with Claude Code's `.claude/skills/` format.

On codebases with internal DSLs or tribal conventions, agent accuracy jumps from ~20% to ~85% with curated skills ([research](https://arxiv.org/abs/2410.03981)).

</details>

<details>
<summary><b>Architecture</b></summary>

Pure-functional update loop: `(Model, Msg) -> (Model, Cmd)`. View is `Model -> Element`, rendered by [maya](https://github.com/1ay1/maya). Process management via `posix_spawn` + `poll(2)`. File writes are atomic (`write` + `fsync` + `rename`).

Deep dive: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) · [`docs/RENDERING.md`](docs/RENDERING.md)

</details>

## License

MIT — see [LICENSE](LICENSE).
