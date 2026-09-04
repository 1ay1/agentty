---
title: "agentty 0.2.0 — run your terminal agent inside Zed"
date: 2026-06-06
author: agentty
tags: [release, acp, zed]
excerpt: "0.2.0 teaches agentty to speak the Agent Client Protocol, so the same engine that drives the TUI now drives a first-class agent panel inside Zed — streaming text, inline diffs, native permission prompts, and session reload."
---

# agentty 0.2.0 — run your terminal agent inside Zed

agentty has always been one thing: a fast, single-binary Claude agent that lives
in your terminal. **0.2.0** keeps that, and adds a second front door — the
[Agent Client Protocol](https://agentclientprotocol.com), the same protocol Zed
uses to drive Claude Code and Gemini. Point Zed at `agentty acp` and your
terminal agent becomes a first-class agent panel inside the editor.

The important part: it's the **same engine**. Same provider, same tool registry,
same wire-message shaping, same permission policy — just driven over newline-
delimited JSON-RPC on stdio instead of a TUI. There is no second codebase to
keep honest.

## Set it up

Drop this into Zed's `settings.json`:

```json
{
  "agent_servers": {
    "agentty": {
      "command": "agentty",
      "args": ["acp"]
    }
  }
}
```

Open the agent panel, pick **agentty**, and prompt. Auth is whatever
`agentty login` already wrote to `~/.config/agentty/credentials.json` — nothing
extra to configure.

## What you get

- **Streaming text** renders token-by-token in Zed's panel.
- **Tool cards** for every `read` / `edit` / `bash` / `grep`, with live status.
- **Inline diffs** — `write` and `edit` emit ACP `diff` content, so Zed renders
  the change in place.
- **Follow-along** — tool calls carry the file path as an ACP `location`, so Zed
  opens and highlights the file the agent is touching.
- **Native permission prompts** before any side-effecting tool runs. The
  `--profile {ask|minimal|write}` flag tunes exactly which tools prompt.
- **Session persistence + reload** — sessions are written to the same on-disk
  thread store the TUI uses, so Zed can `session/load` and replay a full
  conversation. Threads started in Zed show up in the standalone TUI's picker,
  and vice versa.

## One-command Zed over an air-gap

If the box you want to run on has no direct internet, let agentty assemble the
config for you:

```
agentty airgap <host> --acp [flags…]
```

It prints a ready-to-paste Zed `agent_servers` entry whose `command` is `ssh`
itself — one ssh process is the reverse SOCKS5 tunnel, the remote agent, and the
JSON-RPC transport, all at once. Zed owns its lifecycle, so there's nothing to
babysit.

## The rest of 0.2.0

Beyond ACP, this release hardens the transport layer: per-error-class retry caps
(rate-limits keep the full budget, mid-stream drops get only two attempts), a
real `x-stainless-retry-count` header, and a fix for the stale-pooled-connection
churn that produced most of the "transient — retrying…" banners after long
sessions.

Full notes are on the [release page](https://github.com/1ay1/agentty/releases).
Install or update with the same one line:

```
curl -fsSL https://agentty.org/install.sh | sh
```
