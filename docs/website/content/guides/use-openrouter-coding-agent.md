---
title: "How to use OpenRouter as your coding agent backend"
description: "Route dozens of models through one API: use OpenRouter with agentty for flexible, cost-optimized terminal coding. Step-by-step setup."
competitor: "OpenRouter coding agent"
verdict: "Point agentty at OpenRouter to access dozens of models (Claude, GPT, Llama, DeepSeek, and more) through a single API key — switch models per task without changing tools."
updated: "2026-08-19"
---

# How to use OpenRouter as your coding agent backend

[OpenRouter](https://openrouter.ai) gives you one API for many model providers. With agentty you can tap all of them from the terminal and pick the right model per task.

## 1. Install agentty

```
curl -fsSL https://agentty.org/install.sh | sh
```

## 2. Add your OpenRouter API key

Configure the OpenRouter provider with your key. You can then select any model OpenRouter offers — Claude, GPT, Llama, DeepSeek, Qwen, and more.

## 3. Pick models per task

Use Smart Mode to map a flagship model to strategic reasoning and a cheaper/faster one to mechanical edits — all through the same OpenRouter key. agentty's built-in retrieval keeps each request small so you pay for less context.

## Why OpenRouter + agentty

- **One key, many models** — experiment without juggling provider accounts.
- **Cost control** — route by price/latency per task.
- **No lock-in** — swap to Claude Pro/Max, Groq, or local Ollama anytime.

See the [providers docs](/docs/providers/) for configuration.
