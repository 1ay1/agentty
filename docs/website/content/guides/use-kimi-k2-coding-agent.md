---
title: "How to use Kimi K2 as a coding agent with agentty"
description: "Run Kimi K2 as your terminal coding agent with agentty — sign in with your Kimi plan over OAuth (no API key), then get native tool-calling, file edits, and streaming from a single static binary."
competitor: "Kimi K2 coding agent"
verdict: "Sign in with your Kimi plan (no API key) and drive Kimi K2 as a full tool-calling agent — native, instant, and sandboxed, with no Node or Python runtime."
updated: "2026-08-22"
---

# How to use Kimi K2 as a coding agent with agentty

[Kimi K2](https://www.kimi.com) is a strong, low-cost coding model. **agentty**
makes it a first-class terminal coding agent — you sign in with your existing
Kimi plan over OAuth (**no API key**) and get the whole agent: native
tool-calling, file edits, shell, retrieval, and streaming, from a single static
binary with a millisecond cold start.

## 1. Install agentty

```
curl -fsSL https://agentty.org/install.sh | sh
```

One static binary — no Node, no Python, no Electron.

## 2. Sign in with Kimi

```
agentty login
```

Pick **Kimi** at the menu. agentty runs Kimi Code's OAuth device flow: it prints
a one-time code and a URL. Approve in your browser and it switches the moment
you're authorized. On SSH? Press `c` to copy the code to your local clipboard,
paste it in any browser, and approve. The token is stored encrypted and
refreshed automatically.

## 3. Code with Kimi K2

```
agentty --provider kimi
```

`--provider kimi` persists, so after the first run you just type `agentty`.
Switch provider live with `^P` and model with `^/` — both have a search filter,
so type `kimi` to jump straight to it.

## Why Kimi K2 + agentty

- **No API key** — use the Kimi plan you already pay for, via OAuth sign-in.
- **Native tool-calling** — read/edit files, ripgrep search, shell, apply-patch,
  and subagents all run through standard OpenAI-style function calls.
- **Streaming** output and per-turn cost/token accounting.
- **No lock-in** — switch to DeepSeek, Claude, GPT, or Gemini live, no restart.

See the [providers docs](/docs/providers/) for details, or the deep-dive:
[Kimi K2 and DeepSeek in agentty](/blog/kimi-k2-deepseek-terminal-coding-agent).
