---
title: Quick Start
description: From install to your first agent turn in under a minute.
nav_section: Getting Started
nav_order: 30
slug: quick-start
---

Install, sign in, and run your first turn in under a minute.

## 1. Launch in your project

The current working directory is the workspace root — agentty's filesystem tools won't touch anything outside it.

```bash
cd path/to/your/project
agentty
```

## 2. Sign in

First launch opens an auth modal. Pick one:

- **OAuth (Claude Pro/Max)** — opens your browser; the callback writes the token to `~/.config/agentty/credentials.json` (mode `0600`).
- **API key** — paste an `sk-ant-…` token, saved to the same file.

:::tip
OAuth against your existing Pro/Max subscription is the main path — no extra billing, same account you already pay for. See [Authentication](/docs/authentication).
:::

:::note
Prefer a different model? Launch with `--provider` — e.g. `agentty --provider openai -m gpt-4o` or `agentty --provider ollama` for a local model. See [Providers & Models](/docs/providers).
:::

## 3. Your first turn

Type a request, hit [[Enter]]. agentty streams the reply and lands tool calls inline. Mid-stream typing queues your next message and sends it when the current turn finishes. [[Esc]] cancels.

```text
▌ add a --version flag that prints the build version and exits
```

## 4. Pick a permission profile

You start in **Ask** — writes, shell calls, and network calls each prompt before running. Press [[S-Tab]] to cycle to **Write** (autonomous) or **Minimal** (prompts for everything but pure reads). Your choice persists.

## Where to go next

- [The Interface](/docs/interface) — what every part of the screen means.
- [Keybindings](/docs/keybindings) — the full keymap.
- [Providers & Models](/docs/providers) — Claude, GPT, Groq, or a local model.
- [Tools](/docs/tools) — what agentty can actually do.
- [SSH Air-gap](/docs/airgap) — run on a box with no internet.
