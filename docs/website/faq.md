---
title: FAQ
description: Frequently asked questions about agentty — the C++26 claude-code alternative: auth, providers, platforms, sandboxing, air-gap, storage, and licensing.
nav_section: Help
nav_order: 20
slug: faq
---

Quick answers to the questions people ask most.

## Does it work with my Claude Pro/Max subscription?

Yes — OAuth against your existing Pro/Max plan is the main path. No extra billing; same account you already pay for. You can also use an `ANTHROPIC_API_KEY`, or a different [provider](/docs/providers) entirely.

## Will using my Claude subscription with agentty get my account banned?

The honest answer: agentty is a third-party client, so it carries the same footing as any third-party client that speaks to Anthropic's API. What agentty actually does is deliberately unexotic — it completes the **same OAuth flow** and uses the **same `CLAUDE_CODE_OAUTH_TOKEN`** mechanism Claude Code itself uses, sends ordinary Messages-API requests over HTTPS, and adds nothing that spoofs, scrapes, or circumvents rate limits. Credentials live only in `~/.config/agentty/credentials.json` (mode `0600`); nothing is sent anywhere except Anthropic.

We're not affiliated with Anthropic and can't speak for their enforcement, and their terms can change. If you want **zero ambiguity**, use an `ANTHROPIC_API_KEY` (pay-as-you-go, unquestionably in-bounds) or point agentty at [another provider](/docs/providers) — OpenAI, Groq, OpenRouter, or a local Ollama model — with `--provider`. See [Authentication](/docs/authentication).

## Is it really a drop-in for claude-code?

It targets the same workflow — a coding agent in your terminal with the same Claude auth — as a single native binary. Claude is the default, but agentty also runs against OpenAI, Groq, OpenRouter, Together, Cerebras, and local Ollama models. See the full [agentty vs Claude Code](/docs/vs-claude-code) comparison and [Providers & Models](/docs/providers).

## Can I use models other than Claude?

Yes. Pass `--provider` to point agentty at OpenAI, Groq, OpenRouter, Together, Cerebras, a local Ollama model, or any raw `host:port`. It persists like `-m`, and you can switch backends live in-app with `^P`. See [Providers & Models](/docs/providers).

## Do I need Node or Python?

No. agentty is a single static C++26 binary. No Node runtime, no `npm install`, no Python, no Electron.

## What platforms are supported?

Linux, macOS, and Windows — all built and tested daily. Prebuilt binaries ship for Linux (x86_64, aarch64) and Windows (x86_64); macOS builds from source in seconds.

## How is it sandboxed?

Every shell/build call runs in `bwrap` (Linux) or `sandbox-exec` (macOS). The workspace is read-write, system libs read-only, and `~/.ssh` / `/etc` / other projects are blocked. Windows runs unsandboxed for now.

## Can I run it on a machine with no internet?

Yes. `agentty airgap user@host` relays traffic from your laptop over SOCKS5-over-SSH, with TLS pinned end-to-end. See the [air-gap guide](/docs/airgap).

## Can I use it inside my editor?

Yes. `agentty acp` runs agentty as an [Agent Client Protocol](/docs/acp) agent inside Zed — streaming responses, inline diffs, native permission prompts, and session reload. Any ACP client works; it's the same engine as the TUI, just over JSON-RPC on stdio.

## Where are my conversations stored?

As plain JSON, one file per thread, under `~/.agentty/threads/`. Safe to inspect, back up, or delete.

## Is it stable / production ready?

It's pre-1.0 and moving fast, but the core loop, tools, streaming, auth, and persistence all work and get daily smoke testing on Linux. Treat it as a capable beta.

## What license is it under?

MIT. Use it, fork it, ship it. See the [license page](/license).

## How do I fix or improve these docs?

Every page here is a Markdown file in the agentty repo under `docs/website/`. Click **Edit this page** at the bottom of any doc, or open a PR against that file — the site rebuilds itself from the Markdown automatically, so a merged change goes live on its own.
