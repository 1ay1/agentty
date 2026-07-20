# Discovery / distribution submissions

Ready-to-paste text for getting agentty into the directories and lists that
drive **organic** discovery. Brand search already ranks #1 (`agentty …`); the
gap is intent search (`claude code alternative …`), which is dominated by
curated lists and listicles — so the play is to get *into* those lists.

Track status here as each is submitted / merged.

| Target | Type | Status | URL |
|---|---|---|---|
| awesome-cli-coding-agents | Awesome list (PR) | **PR open #212** | https://github.com/bradAGI/awesome-cli-coding-agents/pull/212 |
| awesome-ai-coding-tools | Awesome list (PR) | **PR open #558** | https://github.com/ai-for-developers/awesome-ai-coding-tools/pull/558 |
| awesome-cli-apps (agarrharr, 20k★) | Awesome list (PR) | **PR open #1245** | https://github.com/agarrharr/awesome-cli-apps/pull/1245 |
| awesome-tuis (rothgar, 19.8k★) | Awesome list (PR) | **PR open #774** | https://github.com/rothgar/awesome-tuis/pull/774 |
| awesome-cpp (fffaraz, 72k★) | Awesome list | **SKIPPED** — libraries-only + auto-blocks post-2023 repos | https://github.com/fffaraz/awesome-cpp |
| Terminal Trove | Directory (submit form) | TODO (web form) | https://terminaltrove.com/submit/ |
| AlternativeTo | Directory (submit form) | TODO (web form) | https://alternativeto.net/manage/suggest-software/ |
| Show HN | Post | TODO | https://news.ycombinator.com/submit |

---

## 1. awesome-* list PR entry (one-liner)

Most awesome lists want a single markdown bullet. Match the surrounding style;
this is the canonical form:

```markdown
- [agentty](https://github.com/1ay1/agentty) — Native C++26 terminal coding agent and drop-in claude-code alternative. Single static binary, sub-millisecond cold start, sandboxed by default, runs any model (Claude, GPT, Groq, OpenRouter, Ollama), and drives an air-gapped host over SSH. MIT.
```

PR title: `Add agentty (native C++ terminal coding agent)`
PR body:

> agentty is a native C++26 terminal coding agent — a drop-in claude-code
> alternative that ships as a single static binary (no Node/Python/Electron),
> cold-starts in under a millisecond, sandboxes every shell call by default,
> and runs against Claude, OpenAI, Groq, OpenRouter, Together, Cerebras, or a
> local Ollama model. Also runs inside Zed over ACP and on air-gapped hosts
> over SSH. MIT-licensed, ~82★ and growing.
>
> Repo: https://github.com/1ay1/agentty · Site: https://agentty.org

---

## 2. Terminal Trove submission

Form at https://terminaltrove.com/submit/. Fields:

- **Name:** agentty
- **Category:** AI Coding Agents / TUI
- **Homepage:** https://agentty.org
- **Repo:** https://github.com/1ay1/agentty
- **Short description:** A blazing-fast native C++26 coding agent in your terminal — a single static binary, sub-ms cold start, sandboxed by default, any model.
- **Long description:** agentty is a drop-in claude-code alternative written in C++26. One ~13.6 MB static binary with no runtime dependencies (no Node, Python, or Electron). Signs in with your Claude Pro/Max subscription, or points at OpenAI, Groq, OpenRouter, Together, Cerebras, or a local Ollama model. Every shell call is sandboxed by default (bwrap / sandbox-exec). Runs inside Zed over ACP, and can drive an agent on an air-gapped host with a single SSH command. Linux, macOS, Windows, and Termux/Android. MIT.

---

## 3. AlternativeTo

At https://alternativeto.net, add agentty as an alternative to **Claude Code**,
**Aider**, and **GitHub Copilot CLI**.

- **Name:** agentty
- **URL:** https://agentty.org
- **Tagline:** Native C++26 terminal coding agent — single static binary, any model, sandboxed by default.
- **Description:** A drop-in claude-code alternative written in C++26 that ships as a single static binary with millisecond cold start. Model-agnostic (Claude, OpenAI, Groq, OpenRouter, Together, Cerebras, local Ollama), sandboxed by default, runs inside Zed over ACP, and can drive an air-gapped host over SSH. Open source (MIT), Linux/macOS/Windows/Termux.
- **Tags:** cli, terminal, ai, coding-assistant, open-source, self-hosted
- **License:** Open Source / MIT
- **Platforms:** Linux, Mac, Windows, Self-Hosted, Android (Termux)
- **Mark it "Free" and "Open Source".**

---

## 4. Show HN (when ready for a spike)

- **Title:** `Show HN: agentty – a native C++26 claude-code alternative (single static binary)`
- **URL:** https://github.com/1ay1/agentty
- **First comment:** lead with the honest technical hook — why C++ (sub-ms
  start, no Node/GC), the sandbox-by-default design, model-agnostic, ACP-in-Zed,
  SSH air-gap. Acknowledge it's pre-1.0. Invite architecture questions —
  HN rewards the "reads like a single function" reducer/permission-matrix angle.

---

## Priority order

1. **awesome-cli-coding-agents PR** — it already lists your direct peers (Pi,
   OpenCode, Aider, Goose); highest-fit, lowest-effort, durable backlink.
2. **Terminal Trove + AlternativeTo** — intent-driven directory traffic.
3. **More awesome-list PRs** — awesome-claude, awesome-ai-coding, awesome-tui.
4. **Show HN** — one well-timed post for a spike + backlinks, once 1-3 are live.
