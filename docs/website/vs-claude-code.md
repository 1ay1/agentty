---
title: agentty vs Claude Code
description: An honest comparison of agentty and Claude Code — the native C++26, single-binary, model-agnostic claude-code alternative. Startup speed, cost, sandboxing, air-gap, editor integration, and platform support side by side.
nav_section: Getting Started
nav_order: 15
slug: vs-claude-code
---

**Short version:** agentty is a drop-in [claude-code](https://github.com/anthropics/claude-code) alternative that targets the *same workflow* — a coding agent in your terminal, signed in with your existing Claude Pro/Max subscription — but ships as a single native **C++26** binary instead of a Node.js app, starts in **under a millisecond**, sandboxes every shell call by default, and runs against **any model** (Claude, GPT, Groq, OpenRouter, Together, Cerebras, or a local Ollama model) rather than Claude only.

If you already like Claude Code's workflow but want it faster, dependency-free, model-agnostic, and sandboxed, agentty is built for you.

## At a glance

| | agentty | Claude Code |
|---|---|---|
| **Runtime** | Single static C++26 binary | Node.js app (`npm install -g`) |
| **Cold start** | < 1 ms | ~hundreds of ms (Node boot) |
| **Install size** | {{sizeMB}}, one file | Node runtime + `node_modules` |
| **Dependencies** | None (no Node, Python, Electron, npm) | Node.js ≥ 18 |
| **Models** | Claude, OpenAI, Groq, OpenRouter, Together, Cerebras, local Ollama, any OpenAI-compatible host | Claude only |
| **Auth** | Claude Pro/Max OAuth *or* API key *or* any provider | Claude Pro/Max OAuth or API key |
| **Shell sandbox** | On by default (`bwrap` / `sandbox-exec`) | Permission prompts, no OS sandbox |
| **Editor integration** | Runs inside Zed over [ACP](/docs/acp) | Terminal + IDE extensions |
| **Air-gapped hosts** | One-command [SSH air-gap](/docs/airgap) | — |
| **Platforms** | Linux, macOS, Windows (x86_64 + aarch64), Termux/Android | macOS, Linux, Windows (WSL) |
| **License** | MIT (open source) | Proprietary |
| **MCP tools** | Yes | Yes |

## Where agentty is different

### It's a native binary, not a Node app

Claude Code is distributed as an npm package and boots a Node.js runtime on every invocation. agentty is a single statically-linked C++26 executable ({{sizeMB}}) — `curl | sh` and you have one file with **no runtime dependencies**. Cold start is under a millisecond, and the TUI never pauses for garbage collection mid-stream. No version drift between machines, no `npm install`, no `node_modules`.

### Any model, not just Claude

Claude is agentty's default and OAuth works with your existing Pro/Max plan — but you can point it at **OpenAI, Groq, OpenRouter, Together, Cerebras, or a local Ollama model** with `--provider`, and switch backends live in-app with `^P`. See [Providers & Models](/docs/providers). If you want Claude Code's ergonomics with a local, zero-API-cost model, that's a one-flag change.

### Sandboxed by default, not as an afterthought

Every shell and build command agentty runs is wrapped in an OS-native sandbox — `bwrap` on Linux, `sandbox-exec` on macOS. Your workspace is read-write, system libraries are read-only, and `~/.ssh`, `/etc`, and other projects are blocked. It's the default, not an opt-in. See [Sandboxing](/docs/sandboxing).

### Runs on air-gapped hosts

`agentty airgap user@host` relays traffic from your laptop to a machine with no direct internet, over SOCKS5-over-SSH with TLS pinned end-to-end. One command. See the [air-gap guide](/docs/airgap).

### Lives inside your editor over ACP

`agentty acp` runs the exact same engine as the TUI as an [Agent Client Protocol](/docs/acp) agent inside **Zed** — streaming responses, inline diffs, native permission prompts, session reload — over JSON-RPC on stdio. Any ACP client works.

## Where Claude Code is still ahead

Being honest: Claude Code is a mature, first-party Anthropic product with a large team behind it. It has broader IDE-extension coverage, a longer track record, and first-day support for new Anthropic features. agentty is pre-1.0 and moving fast — the core loop, tools, streaming, auth, and persistence all work and get daily testing, but treat it as a capable beta. If you want the officially-supported Anthropic experience above all else, use Claude Code.

## When to choose agentty

Choose agentty if you want:

- A **single-binary coding agent** with zero runtime dependencies.
- **Sub-millisecond startup** and a TUI that never GC-pauses.
- **Your choice of model** — Claude, GPT, or a local Ollama model — behind one client.
- Shell calls **sandboxed by default**.
- To drive an agent on an **air-gapped host** over SSH.
- An **open-source (MIT)** tool you can read, fork, and ship.

## Try it in one line

```bash
curl -fsSL https://agentty.org/install.sh | sh
cd your-project
agentty
```

First launch opens auth — sign in with your Claude Pro/Max subscription or paste an API key, and you're in. See the [Quick Start](/docs/quick-start) or the full [Installation guide](/docs/installation).
