---
title: Introduction
description: What agentty is, who it's for, and what makes it different.
nav_section: Getting Started
nav_order: 10
slug: ""
---

agentty is a native C++26 terminal coding agent — a drop-in alternative to `claude-code` that ships as a single {{sizeMB}} static binary.

It signs in with your existing **Claude Pro/Max OAuth** subscription (or an `ANTHROPIC_API_KEY`) — or points at **OpenAI, Groq, OpenRouter, Together, Cerebras**, or a local **Ollama** model. It runs every shell call in a sandbox by default, and can drive an agent on an air-gapped host through a single SSH command. No Node, no Python, no Electron, no `npm install`.

## Who it's for

- You want a **single-binary coding agent** with zero runtime dependencies.
- You care about **cold-start speed** and a TUI that never pauses for GC.
- You want **your choice of model** — Claude, GPT, or a local Ollama model — behind one client.
- You need to run an agent on an **air-gapped host** through an SSH tunnel.
- You want shell calls **sandboxed by default**, not as an afterthought.

:::tip
Already convinced? Jump straight to [Installation](/docs/installation) or the [Quick Start](/docs/quick-start).
:::

## Design principles

- **Native speed.** C++26, statically linked, `posix_spawn` everywhere. Spawns in microseconds, no GC pauses mid-stream.
- **One static binary.** {{sizeMB}}. `curl | chmod +x | run`. No version drift between machines.
- **Sandbox by default.** Every shell/build runs inside `bwrap` (Linux) / `sandbox-exec` (macOS). `~/.ssh`, `/etc`, other projects stay read-only.
- **One-command SSH air-gap.** Relay bytes over SOCKS5-over-SSH; TLS pins end-to-end on the real upstreams.
- **Reads like a single function.** The reducer is one `std::visit` over a closed event sum; the permission matrix is a `constexpr` with `static_assert`s — change a policy cell and the build breaks.

## How it works, in one paragraph

agentty is a pure-functional update loop: `(Model, Msg) → (Model, Cmd)`. The view is a single function `Model → Element`, rendered by [maya](https://github.com/1ay1/maya), a sister TUI engine. The Anthropic provider speaks HTTP/2 + SSE directly through an in-house `nghttp2` + OpenSSL stack. Subprocesses use `posix_spawn` + `poll(2)` with in-process kill deadlines. See [Architecture](/docs/architecture) for the full tour.

## Project status

Works on Linux, macOS, and Windows — all three actively tested and built daily. Prebuilt release binaries ship for Linux (x86_64, aarch64) and Windows (x86_64); macOS builds from source in seconds. Pre-1.0 and moving fast.
