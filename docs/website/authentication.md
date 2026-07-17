---
title: Authentication
description: OAuth, API keys, and the credential override order.
nav_section: Getting Started
nav_order: 40
slug: authentication
---

agentty authenticates with your Claude Pro/Max OAuth subscription or an Anthropic API key. Both flow through the same login path. Using a [different provider](/docs/providers)? See the per-provider key note below.

## OAuth (Claude Pro/Max)

The main path. On first launch the auth modal opens your browser; the callback writes the token to `~/.config/agentty/credentials.json` at mode `0600`. agentty picks the right header on relaunch automatically — no extra billing, the same account you already pay for.

## API key

Paste an `sk-ant-…` token into the modal. Saved to the same credentials file.

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
agentty login     # complete auth without entering a thread
agentty logout    # clear stored credentials
agentty status    # show which auth source will be used
```

:::warn
Credentials are stored at mode `0600` and written atomically (temp + fsync + rename). Treat the file like any other secret — anyone who can read it can act as you against the Anthropic API.
:::
