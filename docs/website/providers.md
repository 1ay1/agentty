---
title: Providers & Models
description: Run agentty against Claude, ChatGPT, GitHub Copilot, Kimi, DeepSeek, Gemini, Grok, Mistral, Groq, or any OpenAI-compatible endpoint.
nav_section: Getting Started
nav_order: 50
slug: providers
---

agentty is **bring-your-own-model**: it speaks to any OpenAI-compatible backend, plus Anthropic and local Ollama. Sign in with a subscription you already pay for — Claude Pro/Max, ChatGPT Plus/Pro, GitHub Copilot, or **Kimi** — or bring an API key for **DeepSeek, Google Gemini, xAI Grok, Mistral, Groq, OpenRouter, Together, Cerebras, Fireworks**, or any custom endpoint. Pick one with `--provider`, or switch live mid-thread with `^P` (provider) and `^/` (model).

## Pick a provider

Sign in with a subscription (no API key), bring a hosted provider's API key, or point agentty at a local Ollama model that needs no key at all.

```bash
# Sign in with a subscription — no API key (see the sections below)
agentty login                              # 1) Claude  2) ChatGPT  3) Copilot  4) Kimi
agentty --provider kimi                    # Kimi K2 via your Kimi plan

# Bring an API key
agentty --provider deepseek -m deepseek-v4-pro   # DeepSeek (DEEPSEEK_API_KEY)
agentty --provider gemini -m gemini-2.5-pro      # Google Gemini (GEMINI_API_KEY)
agentty --provider xai -m grok-4.6               # xAI Grok (XAI_API_KEY)
agentty --provider groq -m llama-3.3-70b         # Groq
agentty --provider openrouter                    # any model via OpenRouter
agentty --provider ollama -m qwen2.5-coder       # local model, no key
agentty -m claude-opus-4-5                        # Claude (API key or Pro/Max OAuth)
```

`--provider` and `-m` are persisted between runs, so you only pass them when you want to change the backend.

Inside a thread, press `^P` to switch provider and `^/` to switch model — no restart, no re-auth. Both are also reachable from the command palette (`^K`). The next turn uses the new backend.

## 1M-context models

Signed in with Claude Pro/Max OAuth, the model picker offers a **"(1M context)"** row right below the base model for every Sonnet/Opus/Haiku 4+ model — e.g. `Claude Opus 4.8` followed by `Claude Opus 4.8 (1M context)`. Picking the 1M row widens the context window agentty tracks for that model from 200K to 1M tokens: the status bar's context gauge and auto-compaction both use the wider ceiling, so a long session with a large codebase can grow much further before agentty needs to summarize it. The 1M variant sends Anthropic's extended-context beta on your behalf; nothing else changes about how you use the model. A raw API key isn't offered the 1M row (the beta is account-tier gated and a lower tier would 400 on a request over 200K) — it's available on the OAuth path.

Even a 1M window eventually fills on a long session. When it does, you can [fork](/docs/fork) the thread into a fresh one that carries near-zero context (the parent transcript is read on demand) instead of compacting in place — an O(1)-token way to keep going with a clean slate.

## Supported providers

| ID | Backend | Key |
|---|---|---|
| `anthropic` | Claude — API key or Pro/Max OAuth | `agentty login` → Claude |
| `chatgpt` | Codex models — Sign in with ChatGPT (Plus/Pro) | `agentty login` → ChatGPT |
| `copilot` | GitHub Copilot models — Sign in with GitHub | `agentty login` → GitHub Copilot |
| `kimi` | Kimi K2 models — Sign in with Kimi | `agentty login` → Kimi |
| `openai` | GPT / o-series on `api.openai.com` | `OPENAI_API_KEY` |
| `deepseek` | DeepSeek V4 (chat + reasoner) on `api.deepseek.com` | `DEEPSEEK_API_KEY` |
| `gemini` | Google Gemini via the OpenAI-compat API | `GEMINI_API_KEY` |
| `xai` | xAI Grok models on `api.x.ai` | `XAI_API_KEY` |
| `mistral` | Mistral / Codestral / Magistral on `api.mistral.ai` | `MISTRAL_API_KEY` |
| `groq` | Llama / Mixtral / Kimi on Groq LPUs — very fast | `GROQ_API_KEY` |
| `cerebras` | Wafer-scale inference — very fast | `CEREBRAS_API_KEY` |
| `together` | Open models on `together.ai` | `TOGETHER_API_KEY` |
| `fireworks` | Open models on `fireworks.ai` | `FIREWORKS_API_KEY` |
| `openrouter` | Any model via `openrouter.ai` | `OPENROUTER_API_KEY` |
| `ollama` | Local models at `localhost:11434` | None |
| `host:port` | Any raw OpenAI-compatible endpoint | `OPENAI_API_KEY` |
| `https://host[:port]/path` | Any OpenAI-compatible endpoint with a custom path prefix (e.g. a gateway serving on `/api` instead of `/v1`) | `OPENAI_API_KEY` |

## API keys

Hosted OpenAI-compatible providers read their key from the provider-specific environment variable (e.g. `GROQ_API_KEY`), falling back to `OPENAI_API_KEY`, or an explicit `-k <key>` for the session. Ollama needs no key.

```bash
export GROQ_API_KEY=gsk_…
agentty --provider groq -m llama-3.3-70b

# or a one-off, never written to disk:
agentty --provider openai -k sk-… -m gpt-4o
```

### Custom path prefix

Some gateways or self-hosted servers don't serve on the standard `/v1` path. Specify a full URL and agentty will use its path as a prefix, appending `/chat/completions` and `/models`:

```bash
agentty --provider https://chat.example.org/api -k sk-… -m GLM-5.2
# chats at  https://chat.example.org/api/chat/completions
# models at https://chat.example.org/api/models
```

A bare `host:port` (no `http://`/`https://`) keeps the default `/v1` prefix.

## Sign in with GitHub Copilot

If you have a GitHub Copilot subscription (Individual, Business, or Enterprise), you can use its models — GPT-4o, o-series, Claude, Gemini, and more — through your existing Copilot plan, **no API key required**.

```bash
agentty login          # choose "Sign in with GitHub Copilot"
agentty --provider copilot
```

Sign-in uses GitHub's **device flow**: agentty shows a one-time code and opens `github.com/login/device` (works over SSH too — just enter the code in any browser). The available model list is fetched live from your account's entitlements, so you see exactly the models your plan offers. agentty stores a durable GitHub token (encrypted, at `~/.config/agentty/copilot_credentials.json`) and transparently exchanges it for the short-lived Copilot session token, refreshing mid-session so long agent runs never drop. `agentty status` shows your plan, entitlement, and the active inference host; `agentty logout` → GitHub Copilot signs out.

:::note
Copilot routes to the right host automatically (Individual / Business / Enterprise each use a different endpoint) — there's nothing to configure. On the free Copilot tier, agentty surfaces a clear "chat quota exhausted" message rather than a raw error.
:::

## Sign in with Kimi

If you have a [Kimi](https://www.kimi.com) plan, sign in with it and run agentty on Kimi's K2 models — **no API key required**.

```bash
agentty login          # choose "Sign in with Kimi"
agentty --provider kimi
```

Sign-in uses Kimi's **OAuth device flow** (RFC 8628), the same in-terminal experience as Claude, ChatGPT, and Copilot: agentty shows a one-time code and opens the Kimi authorization page in your browser. It works over SSH — press [[c]] in the modal to copy the code (sent via OSC 52, so it lands on your local clipboard even through a remote session), then paste it in any browser. agentty polls in the background and switches the moment you approve.

The token is stored encrypted at `~/.config/agentty/kimi_credentials.json` and refreshed automatically mid-session, so long agent runs never drop. The picker row reflects real sign-in state (`⚠ sign in with Kimi` / `✓ signed in`), and pressing [[Enter]] on the active Kimi row opens the **multi-account manager** (switch / add / remove Kimi accounts) — hold several Kimi accounts and switch entirely in-app. `agentty status` shows the active account; `agentty logout` → Kimi signs out.

:::tip
Kimi's inference runs on its OpenAI-compatible endpoint, so tool-calling, streaming, and reasoning all work exactly as they do on every other provider — nothing Kimi-specific to configure. Prefer a raw platform API key instead of the subscription? Use any OpenAI-compatible host: `agentty --provider https://api.moonshot.ai -k <key> -m kimi-k2-0905-preview`.
:::

## DeepSeek

[DeepSeek](https://platform.deepseek.com) is a first-class built-in provider. It uses a **static API key** (DeepSeek doesn't offer developer OAuth), so set `DEEPSEEK_API_KEY` — or pass `-k`, or paste it into the in-app prompt once.

```bash
export DEEPSEEK_API_KEY=sk-…
agentty --provider deepseek -m deepseek-v4-pro      # strong general/coding model
agentty --provider deepseek -m deepseek-reasoner    # reasoning model (thinking)
```

DeepSeek's endpoints live at the API root (no `/v1` prefix) — agentty handles that automatically. Both the chat and reasoner families are recognized as native tool-callers, and **reasoning streams live**: on a reasoning model agentty renders DeepSeek's chain-of-thought exactly like Claude's thinking, and the effort control (cycle it in the model picker) maps straight to the wire. Selecting DeepSeek in `^P` before you've set a key still shows its models (a small bundled list) so you can pick one immediately; the live catalog replaces it once your key is set.

## Local models (Ollama)

Point agentty at a model served by Ollama on `localhost:11434` — no key, no cloud, no data leaving your machine. agentty uses Ollama's native `/api/chat` protocol and salvages tool calls that weaker local models leak as raw JSON, so even smaller models can drive the full tool suite.

```bash
ollama pull qwen2.5-coder
agentty --provider ollama -m qwen2.5-coder
```

:::note
`--provider` and `-m` persist between sessions. Run `agentty --provider anthropic` to switch to Claude, or just press `^P` in-app.
:::
