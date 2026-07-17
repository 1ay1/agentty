---
title: Architecture
description: The update loop, the view function, the provider, and the subprocess model.
nav_section: Advanced
nav_order: 70
slug: architecture
---

agentty is small enough to read in an afternoon. The whole thing is a pure update loop with a single render function and a closed set of effects.

## The update loop

Everything is one pure function: `(Model, Msg) → (Model, Cmd)`. State transitions are total and inspectable. Strong ID newtypes — `ToolCallId`, `ThreadId`, `OAuthCode`, `PkceVerifier` — mean swapping two arguments is a compile error, not a debugging session.

```text
Msg  →  reducer (one std::visit over a closed event sum)  →  (Model, Cmd)
Cmd  →  runtime executes side effects  →  new Msg events
```

## The view

Rendering is a single function `Model → Element`. agentty builds widget Configs from `Model` state; the actual chrome — every glyph, layout decision, and breathing animation — is owned by [maya](https://github.com/1ay1/maya), a sister header-mostly TUI engine. The host constructs no Elements directly.

## The provider

The Anthropic provider speaks HTTP/2 + SSE directly through an in-house `nghttp2` + OpenSSL stack. OAuth (PKCE) and API key both flow through the same `auth::cmd_login` path. SSE deltas are smoothed into the screen at ⅛ buffer per tick so server batching doesn't produce chunky text.

A second transport covers every OpenAI-compatible backend — OpenAI, Groq, OpenRouter, Together, Cerebras, and local Ollama — collapsing the differences to configuration (base URL, auth header, model id). Ollama's native `/api/chat` path adds incremental salvage for weaker local models that leak tool calls as raw JSON, so they can still drive the full tool suite. The active provider is chosen per session and switchable live. See [Providers & Models](/docs/providers).

## The subprocess model

Subprocesses use `posix_spawn` + `poll(2)` with in-process `SIGTERM → SIGKILL` deadlines on POSIX, and `CreateProcessW` + a reader thread on Windows. No GNU `timeout` dependency, no `popen` quoting hazards. File writes are atomic: `write` + `fsync`/`_commit` + `rename`/`MoveFileExW`.

## The permission matrix

The permission policy is a `constexpr` matrix guarded by `static_assert`s. Each tool declares its effect set at compile time; changing a policy cell breaks the build rather than silently weakening a guarantee.

:::tip
Going deeper? The repo's `docs/RENDERING.md` walks the view pipeline turn-by-turn and `docs/UI.md` is the per-widget Config reference.
:::
