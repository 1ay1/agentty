---
title: "An open-source OpenCode alternative (2026)"
description: "agentty is a native C++26 terminal coding agent and an OpenCode alternative — a single static binary with millisecond startup, multi-provider support, and a sandbox by default."
competitor: "OpenCode"
verdict: "Both are open-source terminal coding agents. agentty's edge is a single native C++26 binary (no runtime), millisecond cold start, built-in retrieval, and a sandbox by default."
updated: "2026-08-19"
---

# An open-source OpenCode alternative

OpenCode and agentty share a philosophy: open-source, terminal-first AI coding. Here's where agentty differs.

## How agentty compares

- **Native C++26 single binary** — 16.7 MB, ~3 ms cold start, zero runtime dependencies.
- **Multi-provider** — Claude Pro/Max, OpenAI, Groq, OpenRouter, Cerebras, local Ollama.
- **Sandboxed by default**; SSH air-gap in one command.
- **Built-in RAG** — hybrid BM25 + dense retrieval so only relevant code reaches the model.
- **Runs in Zed** over ACP; speaks MCP.
- **MIT-licensed.**

## Which to choose

Both are strong open-source picks. Choose agentty if raw speed, a dependency-free binary, and built-in retrieval matter most to you.

## Install in one line

```
curl -fsSL https://agentty.org/install.sh | sh
```
