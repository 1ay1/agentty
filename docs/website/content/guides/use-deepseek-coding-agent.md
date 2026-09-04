---
title: "How to use DeepSeek as a coding agent with agentty"
description: "Run DeepSeek (V4 and reasoner) as your terminal coding agent with agentty — set DEEPSEEK_API_KEY, point one flag at it, and get native tool-calling, live-streamed reasoning, and file edits from a single static binary."
competitor: "DeepSeek coding agent"
verdict: "Drop in a DEEPSEEK_API_KEY and drive DeepSeek V4 (or the reasoner, with live chain-of-thought) as a full tool-calling agent — native, instant, cheap, with no Node or Python runtime."
updated: "2026-08-22"
---

# How to use DeepSeek as a coding agent with agentty

[DeepSeek](https://www.deepseek.com) is one of the cheapest frontier-class
coding models available. **agentty** makes it a first-class terminal coding
agent: set your API key, point one flag at it, and get native tool-calling,
file edits, shell, retrieval, and **live-streamed reasoning** — from a single
static binary with a millisecond cold start.

## 1. Install agentty

```
curl -fsSL https://agentty.org/install.sh | sh
```

One static binary — no Node, no Python, no Electron.

## 2. Add your DeepSeek API key

```
export DEEPSEEK_API_KEY=sk-deepseek-YOURKEY
```

agentty knows DeepSeek's endpoint natively — you don't configure a URL. It
reads `DEEPSEEK_API_KEY` (falling back to `OPENAI_API_KEY`, or an explicit `-k`
for the session).

## 3. Code with DeepSeek

```
# general coding — DeepSeek V4
agentty --provider deepseek -m deepseek-v4-pro

# the thinking model — reasoning streams live as it works
agentty --provider deepseek -m deepseek-reasoner
```

`--provider` and `-m` persist. Switch provider live with `^P` and model with
`^/` — both have a search filter, so type `deepseek` to jump to it. With
`deepseek-reasoner`, the model's chain-of-thought streams live in agentty's
reasoning view, the same place Claude/Grok/Gemini thinking appears.

## Why DeepSeek + agentty

- **Very cheap, very capable** — frontier coding at a fraction of the cost.
- **Native tool-calling** — read/edit files, ripgrep search, shell, apply-patch,
  and subagents all work through standard OpenAI-style function calls.
- **Live reasoning** for `deepseek-reasoner`, streamed as it thinks.
- **No lock-in** — switch to Kimi, Claude, GPT, or Gemini live, no restart.

See the [providers docs](/docs/providers/) for details, or the deep-dive:
[Kimi K2 and DeepSeek in agentty](/blog/kimi-k2-deepseek-terminal-coding-agent).
