---
title: "Kimi K2 and DeepSeek in agentty — sign in with Kimi, key in DeepSeek, code from your terminal"
date: 2026-08-22
author: agentty
tags: [providers, kimi-k2, deepseek, oauth, tool-calling]
excerpt: "As of 0.4.0, Kimi and DeepSeek are first-class providers in agentty. Sign in with your Kimi plan over OAuth (no API key) and run Kimi K2, or drop in a DEEPSEEK_API_KEY and run DeepSeek V4 / reasoner — both with full streaming, native tool-calling, and live-streamed chain-of-thought, from a single static binary. Here's the exact setup."
---

# Kimi K2 and DeepSeek, first-class in agentty

If you run **Kimi** or **DeepSeek**, you now have a fast, native coding agent
built for them. As of **agentty 0.4.0**, both are **built-in providers** — not
a proxy, not a wrapper, not a plugin. **Kimi** you *sign in* to with your
existing Kimi plan over OAuth (no API key, just like "Sign in with ChatGPT").
**DeepSeek** takes a `DEEPSEEK_API_KEY`. Either way you get the whole agent —
streaming output, **native tool-calling**, file edits, shell, retrieval, and
**live-streamed reasoning** — from a **single static binary** with a
millisecond cold start. No Node, no Python, no Electron, no `npm install`.

## Sign in with Kimi (no API key)

Have a [Kimi](https://www.kimi.com) plan? Use it directly. agentty implements
Kimi Code's **OAuth device flow** (RFC 8628) against `auth.kimi.com` and talks
to Kimi's OpenAI-compatible coding API — so you run **Kimi K2** with no key to
manage:

```bash
# 1) install agentty (single static binary)
curl -fsSL https://agentty.org/install.sh | sh

# 2) sign in — pick Kimi at the menu; it prints a one-time code + URL
agentty login

# 3) run agentty on Kimi
agentty --provider kimi
```

`agentty login` shows a one-time code and opens the Kimi authorization page.
**Over SSH?** Press `c` to copy the code (OSC 52 → your local clipboard),
paste it in any browser, approve, and agentty switches the moment it's
authorized. The token is stored encrypted and refreshed automatically
mid-session — and you can hold multiple Kimi accounts and switch between them
live in-app with `^P`.

That's the whole setup: no `MOONSHOT_API_KEY`, no base-URL juggling. `kimi` is
a real provider preset, so `--provider kimi` persists between runs — after the
first launch you just type `agentty`.

## Bring a key for DeepSeek

DeepSeek is a first-class preset too. Set your key and point agentty at it:

```bash
# general coding — DeepSeek V4
export DEEPSEEK_API_KEY=sk-deepseek-YOURKEY
agentty --provider deepseek -m deepseek-v4-pro

# the thinking model — reasoning streams live as it works
agentty --provider deepseek -m deepseek-reasoner
```

agentty checks `DEEPSEEK_API_KEY` first (falling back to `OPENAI_API_KEY`, or
an explicit `-k` for the session), and knows DeepSeek's endpoint natively — you
don't set a URL. `--provider` and `-m` persist, so subsequent runs are just
`agentty`. Switch provider live with `^P` and model with `^/`; both overlays
have a search filter, so just start typing `deepseek` or `kimi` to jump to it.

Use `deepseek-v4-pro` for fast day-to-day work and `deepseek-reasoner` when you
want the model to think longer — and with agentty, **its chain-of-thought
streams live** in the same reasoning view as Claude's thinking, Grok's, and
Gemini's.

## What you get with either

Once you're on Kimi or DeepSeek, this is a **real** agent, not a chat box:

- **Native tool-calling** — agentty's full tool suite (read / edit / write
  files, ripgrep-backed search, structural search, shell, apply-patch,
  subagents) runs through standard OpenAI-style function calls. Both Kimi K2
  and DeepSeek support them, so **the entire toolchain works**.
- **Streaming** token output, and **live-streamed reasoning** for the thinking
  models — DeepSeek's `reasoning_content` surfaces through the same event as
  every other provider's, so it renders identically.
- **In-app model picker** (`^/`) populated from the provider's live catalog.
- **Cost + token accounting** per turn — and both Kimi K2 and DeepSeek are
  among the cheapest capable models you can drive.
- Everything else agentty does — [Smart Mode](/blog/agentty-0-3-0-smart-mode)
  routing, [retrieval](/blog/agentty-0-2-9-retrieval-engine), sandboxed shell,
  one-command SSH air-gap, [Zed over ACP](/blog/agentty-0-2-0-zed-acp) — works
  the same regardless of which model is behind it.

## Why agentty for Kimi / DeepSeek users

agentty ships its own **C++ OpenAI-compatible transport** — SSE streaming,
streamed `tool_calls` reassembled natively, provider differences collapsed to
configuration. So a hosted model like Kimi K2 or DeepSeek V4 is a first-class
citizen, not a compatibility shim:

- **One static binary.** No Node, no Python, no Electron. `curl … | sh` and
  you have an agent.
- **Millisecond cold start.** ~18 MB, starts instantly, so the loop that
  matters — *type → model → tool → result* — is bound by the model's latency,
  not the client's.
- **Sandboxed by default**, one-command SSH air-gap, runs inside Zed over ACP.
- **Your choice of model behind one client** — swap Kimi ⇄ DeepSeek ⇄ Claude ⇄
  GPT ⇄ Gemini ⇄ Grok live with `^P`, no restart.

## Frequently asked

**Does agentty support Kimi K2?**
Yes — first-class since 0.4.0. `agentty login` signs you in with your Kimi plan
over OAuth (no API key), then `agentty --provider kimi` runs you on Kimi K2.
Streaming and native tool-calling both work.

**Does agentty support DeepSeek?**
Yes. Set `DEEPSEEK_API_KEY` and run `agentty --provider deepseek -m
deepseek-v4-pro` (or `deepseek-reasoner` for the thinking model — its reasoning
streams live). DeepSeek's endpoint is built in; you don't configure a URL.

**Do I need an API key for Kimi?**
No. Kimi uses OAuth sign-in (`agentty login` → choose Kimi), the same way
ChatGPT and GitHub Copilot sign-in work. DeepSeek uses a `DEEPSEEK_API_KEY`.

**Do file edits and tools work, or just chat?**
Full tools. agentty sends standard OpenAI-style `tools` and reassembles
streamed `tool_calls`, so reading/editing files, shell, grep/ripgrep search,
apply-patch, and subagents all work on both Kimi K2 and DeepSeek.

**Can I switch between Kimi, DeepSeek, and Claude without restarting?**
Yes. `^P` switches provider and `^/` switches model live mid-thread; both have
a search filter — type `kimi` or `deepseek` to jump straight to it. Your
selection persists between runs.

**Does the reasoning show for deepseek-reasoner?**
Yes — DeepSeek's streamed chain-of-thought surfaces in agentty's reasoning
view, the same place Claude/Grok/Gemini thinking appears.

**How do I install agentty?**
`curl -fsSL https://agentty.org/install.sh | sh` on macOS or Linux (Intel and
Apple Silicon). Single static binary — no runtime to install.

---

Kimi K2 and DeepSeek give you frontier-class coding at a fraction of the cost.
agentty gives you a native, instant, tool-calling agent to drive them from your
terminal — sign in with Kimi, key in DeepSeek, and go:
`curl -fsSL https://agentty.org/install.sh | sh`.
