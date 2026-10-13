---
title: "An open-source alternative to OpenAI Codex CLI (2026)"
description: "agentty is a native, open-source terminal coding agent and an alternative to the OpenAI Codex CLI — one static binary, multi-provider (not just OpenAI), sandboxed, MIT-licensed."
competitor: "Codex CLI"
verdict: "The Codex CLI ties you to OpenAI. agentty runs against OpenAI too — plus Claude, Groq, OpenRouter, Cerebras, and local Ollama — from a single native binary with a sandbox by default."
updated: "2026-08-19"
---

# An open-source alternative to the OpenAI Codex CLI

If you like the Codex CLI's terminal workflow but want to **use more than one provider** and an **open-source** codebase, agentty is the alternative to try.

## Why look for a Codex CLI alternative

- You don't want to be **locked to a single provider**.
- You want an **open-source** agent you can read and self-host.
- You want a **native binary** with millisecond startup instead of a Node runtime.

## How agentty compares

- **Provider-agnostic** — OpenAI, Claude Pro/Max, Groq, OpenRouter, Cerebras, or local Ollama.
- **Single static binary** — 16.7 MB, ~3 ms cold start.
- **Sandboxed by default**; one-command SSH air-gap.
- **Retrieval built in** so only relevant code is sent to the model.
- **Open source (MIT)**, MCP-capable, runs in Zed over ACP.

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
