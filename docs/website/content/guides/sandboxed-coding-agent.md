---
title: "How to run a sandboxed AI coding agent (safe by default)"
description: "AI agents run shell and build commands — that's risky. Learn how agentty sandboxes every command by default with bubblewrap, and how to air-gap a session over SSH."
competitor: "sandboxed coding agent"
verdict: "agentty runs shell and build commands inside a Linux sandbox (bubblewrap) by default, so an agent can't touch anything you didn't intend — and you can air-gap an entire session over SSH with one command."
updated: "2026-08-19"
---

# How to run a sandboxed AI coding agent

An AI coding agent that runs shell commands is powerful — and risky. A bad command (or a prompt-injected one) can delete files or exfiltrate data. agentty is built to be **safe by default**.

## Sandbox by default

agentty runs shell and build commands inside a Linux sandbox using [bubblewrap](https://github.com/containers/bubblewrap) (`bwrap`). The agent's commands are isolated from the rest of your system unless you explicitly allow more access. You don't have to configure anything — it's on by default.

## Air-gap a whole session

For maximum safety, agentty can air-gap an entire session over SSH with a single command — the agent operates on a remote machine with no path back to your local environment.

## Why this matters

- **Prompt-injection resistance** — a malicious instruction can't run arbitrary commands against your real system.
- **Blast-radius control** — mistakes are contained.
- **Peace of mind** — let the agent work autonomously without babysitting every command.

## Try it

```
curl -fsSL https://agentty.org/install.sh | sh
```

Sandboxing is enabled by default. See the [security docs](/docs/) for details.
