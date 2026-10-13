---
title: "How to use Claude from the terminal (with your Pro/Max plan)"
description: "Use Claude as a coding agent from your terminal with agentty — sign in with your existing Claude Pro/Max subscription, no extra API cost. Install in one line."
competitor: "Claude terminal"
verdict: "agentty signs in with your existing Claude Pro/Max plan and turns Claude into a full terminal coding agent — read/edit files, run commands, and retrieve from your repo, all from the command line."
updated: "2026-08-19"
---

# How to use Claude from the terminal

Want Claude as a real coding agent in your terminal — reading files, editing code, running commands — using your existing Claude Pro/Max plan? That's exactly what agentty does.

## 1. Install agentty

```
curl -fsSL https://agentty.org/install.sh | sh
```

A single static binary — no Node, no Python.

## 2. Sign in with Claude

agentty uses OAuth (PKCE) to sign in with your existing Claude Pro/Max subscription — no separate API billing required to use Claude.

## 3. Start coding

Run `agentty` in your project and type what you want:

```
❯ refactor the auth handler to use the new token cache
```

agentty reads the relevant files (via built-in retrieval), proposes edits, and can run shell/build commands — sandboxed by default.

## Why agentty for Claude in the terminal

- **Your existing plan** — sign in with Claude Pro/Max, no extra cost.
- **Fast & native** — millisecond startup, single binary.
- **Safe** — commands run in a sandbox; air-gap over SSH in one command.
- **Not locked in** — switch to OpenAI, Groq, or local Ollama anytime.

Read more in the [providers docs](/docs/providers/).
