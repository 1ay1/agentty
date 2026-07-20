---
title: Providers & Models
description: Run agentty against Claude, OpenAI, Groq, OpenRouter, Together, Cerebras, Ollama, or any OpenAI-compatible endpoint.
nav_section: Getting Started
nav_order: 50
slug: providers
---

agentty is **bring-your-own-model**: it speaks to any OpenAI-compatible backend, plus Anthropic and local Ollama. Pick one with `--provider`, or switch live mid-thread with `^P` (provider) and `^/` (model).

## Pick a provider

Run agentty with an API key for any hosted provider, or point it at a local Ollama model that needs no key at all. Anthropic works with an `sk-ant-…` key or your [Claude Pro/Max OAuth](/docs/authentication).

```bash
agentty --provider openai -m gpt-4o        # GPT
agentty --provider groq -m llama-3.3-70b   # Groq
agentty --provider ollama -m qwen2.5-coder # local model, no key
agentty --provider openrouter              # any model via OpenRouter
agentty -m claude-opus-4-5                 # Claude (API key or Pro/Max OAuth)
```

`--provider` and `-m` are persisted between runs, so you only pass them when you want to change the backend.

Inside a thread, press `^P` to switch provider and `^/` to switch model — no restart, no re-auth. Both are also reachable from the command palette (`^K`). The next turn uses the new backend.

## Supported providers

| ID | Backend | Key |
|---|---|---|
| `anthropic` | Claude — API key or Pro/Max OAuth | `agentty login` |
| `openai` | GPT / o-series on `api.openai.com` | `OPENAI_API_KEY` |
| `groq` | Llama / Mixtral on Groq LPUs — very fast | `GROQ_API_KEY` |
| `openrouter` | Any model via `openrouter.ai` | `OPENROUTER_API_KEY` |
| `together` | Open models on `together.ai` | `TOGETHER_API_KEY` |
| `cerebras` | Wafer-scale inference — very fast | `CEREBRAS_API_KEY` |
| `ollama` | Local models at `localhost:11434` | None |
| `host:port` | Any raw OpenAI-compatible endpoint | `OPENAI_API_KEY` |

## API keys

Hosted OpenAI-compatible providers read their key from the provider-specific environment variable (e.g. `GROQ_API_KEY`), falling back to `OPENAI_API_KEY`, or an explicit `-k <key>` for the session. Ollama needs no key.

```bash
export GROQ_API_KEY=gsk_…
agentty --provider groq -m llama-3.3-70b

# or a one-off, never written to disk:
agentty --provider openai -k sk-… -m gpt-4o
```

## Local models (Ollama)

Point agentty at a model served by Ollama on `localhost:11434` — no key, no cloud, no data leaving your machine. agentty uses Ollama's native `/api/chat` protocol and salvages tool calls that weaker local models leak as raw JSON, so even smaller models can drive the full tool suite.

```bash
ollama pull qwen2.5-coder
agentty --provider ollama -m qwen2.5-coder
```

:::note
`--provider` and `-m` persist between sessions. Run `agentty --provider anthropic` to switch to Claude, or just press `^P` in-app.
:::
