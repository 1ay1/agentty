---
title: "How to run a sandboxed AI coding agent (safe by default)"
description: "AI agents run shell and build commands — that's risky. Learn how agentty sandboxes every command by default on Linux and macOS, and how to air-gap a session over SSH."
competitor: "sandboxed coding agent"
verdict: "agentty runs shell and build commands inside an OS-level sandbox by default on both Linux and macOS, so an agent can't touch anything you didn't intend — and you can air-gap an entire session over SSH with one command."
updated: "2026-08-19"
---

# How to run a sandboxed AI coding agent

An AI coding agent that runs shell commands is powerful — and risky. A bad command (or a prompt-injected one) can delete files or exfiltrate data. agentty is built to be **safe by default**.

## Sandbox by default

agentty runs shell and build commands inside an OS-level sandbox, on **Linux and macOS**, using its own policy compiler ([claybin](https://github.com/1ay1/claybin)). The agent's commands are isolated from the rest of your system unless you explicitly allow more access. You don't have to configure anything — it's on by default.

The same policy compiles to whatever the kernel underneath can actually enforce:

- **Linux** — mount + pid + network namespaces, a landlock ruleset, a seccomp filter, and cgroup v2 resource caps.
- **macOS** — a seatbelt profile (the mechanism Chrome and every App Store app use) plus POSIX rlimits.

agentty reports *per capability* which walls the host actually built, rather than claiming "sandbox: active" and leaving you to guess. Where a platform cannot enforce something — macOS has no equivalent of the Linux syscall filter — it says so instead of pretending. [bubblewrap](https://github.com/containers/bubblewrap) (`bwrap`) and Apple's `sandbox-exec` remain as per-platform fallbacks.

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
