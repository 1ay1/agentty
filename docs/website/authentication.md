---
title: Authentication
description: OAuth, API keys, and the credential override order.
nav_section: Getting Started
nav_order: 40
slug: authentication
---

agentty is **bring-your-own-model**: point it at any provider with an API key — Anthropic, OpenAI, Groq, OpenRouter, Together, Cerebras — or a fully local Ollama model that needs no key at all. It also supports signing in with existing Claude Pro/Max or ChatGPT Plus/Pro OAuth. Pick whichever fits; they all flow through the same login path.

## API key (recommended, zero ambiguity)

Paste an `sk-ant-…` (or any provider's) key into the modal, or set the matching environment variable. Pay-as-you-go, unquestionably within each provider's terms, and the same key works headless/over SSH. Saved to `~/.config/agentty/credentials.json` at mode `0600`. For a fully local, no-account setup, use [Ollama](/docs/providers) — no key, no network, nothing to authorize.

## OAuth (Claude Pro/Max)

If you'd rather use the Pro/Max plan you already pay for, agentty completes the **same OAuth flow and `CLAUDE_CODE_OAUTH_TOKEN` mechanism Claude Code uses**. On first launch the auth modal opens your browser; the callback writes the token to the same credentials file. No extra billing, same account. (It's a third-party client on subscription auth — see the [FAQ](/docs/faq) for the honest footing; if you want zero ambiguity, use an API key or Ollama above.)

## OAuth (ChatGPT Plus/Pro)

Choose **Sign in with ChatGPT** to use a ChatGPT subscription through agentty's native Codex provider. On a local terminal, the browser returns through `http://localhost:1455`. In an SSH session, agentty automatically uses OpenAI's device-code flow instead: open the displayed `auth.openai.com/codex/device` link on any device and enter the one-time code. No browser or callback port is required on the server.

Device login must be enabled in your personal ChatGPT security settings or by your workspace administrator. To force device auth in an unusual headless terminal, set `AGENTTY_CHATGPT_DEVICE_AUTH=1`; set it to `0` to retain loopback login (for example when using `ssh -L 1455:localhost:1455`).

## Override order

Highest priority first:

1. `-k <key>` / `--key <key>` — single-session, never written to disk.
2. `ANTHROPIC_API_KEY` environment variable.
3. `CLAUDE_CODE_OAUTH_TOKEN` environment variable.
4. The on-disk credentials from the modal.

## Other providers

When you run with `--provider`, agentty reads that backend's key from its environment variable (e.g. `OPENAI_API_KEY`, `GROQ_API_KEY`), falling back to `OPENAI_API_KEY`, or an explicit `-k` for the session. Ollama needs no key. A key entered in-app is saved per-provider so you only paste it once. See [Providers & Models](/docs/providers).

## Non-interactive auth (over SSH)

```bash
agentty login     # ChatGPT automatically prints a device URL + one-time code
agentty logout    # clear stored credentials
agentty status    # show which auth source will be used
```

:::warn
Credentials are stored at mode `0600` and written atomically (temp + fsync + rename). Treat the file like any other secret — anyone who can read it can act as you against the Anthropic API.
:::
