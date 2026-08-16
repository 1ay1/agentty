---
title: Subagents
description: Delegate a self-contained task to an autonomous subagent with its own context window — it works in isolation and returns one tight report, so the main thread never fills up with exploration.
nav_section: Tools
nav_order: 50
slug: subagents
---

A **subagent** is a fresh agent spawned to complete one self-contained task in isolation. It has its **own context window**, runs the full agent loop with its own tools, and returns a **single condensed report** — its transcript and tool output never enter the parent thread. This keeps the main conversation focused: a big investigation ("map how auth works across these 12 files") happens in the subagent's window, and only the answer comes back.

Subagents are driven by the built-in **`task` tool**, so the model spawns them on its own when a job is worth isolating — and you can ask for one explicitly ("spawn an explorer to map the render pipeline").

## Built-in agent types

Each type is a role with a tailored system prompt and a tool allowlist. `general` is the default.

| Type | Role | Access |
|------|------|--------|
| **`explorer`** | Map and explain a codebase region — trace call sites, cite exact file:line | read-only |
| **`reviewer`** | Critically review code/changes — bugs, edge cases, races, security; prioritised findings | read-only |
| **`tester`** | Reproduce, run, and diagnose — build/run tests, report root cause with the failing assertion | read + run (no prod edits) |
| **`coder`** | Implement a change end-to-end, verify it builds | read + write |
| **`general`** | Complete any delegated task with whatever tools fit (default) | full toolset |

Read-only types (`explorer`, `reviewer`) are physically prevented from writing files, running commands, or reaching the network — the sandbox rejects any tool with a write/exec/net effect, so a review can't accidentally mutate your tree.

:::tip
Fan out. Because each subagent is isolated, the model can launch several in parallel — three explorers over three subsystems — and merge their reports, without any of them polluting each other's or the parent's context.
:::

## Custom agent types

Drop a Markdown file in `.agentty/agents/` (project) or `~/.agentty/agents/` (all projects). The **body is the role's system prompt**; optional frontmatter configures its sandbox:

```markdown
---
description: Reviews SQL migrations for safety.   # shown in the catalog
tools: read grep glob list_dir                    # optional allowlist (space-separated)
read-only: true                                   # optional effect gate
---
Your role: MIGRATION REVIEWER. Check every migration for destructive
operations, missing transactions, and non-idempotent DDL. Report each
issue with the file and line and a safer rewrite.
```

| Field | Meaning |
|-------|---------|
| `description` | one-line summary for the subagents catalog and the `task` tool's type list |
| `tools` | space-separated allowlist — the agent may use only these tools (omit for all) |
| `read-only` | `true` blocks every write/exec/net tool, exactly like `explorer`/`reviewer` |
| *(body)* | the role prompt the subagent runs under |

The file name (without `.md`) is the type name you pass as `agent_type`. Search roots, project first: `.agentty/agents`, `.agents/agents`, `.claude/agents`, then the same three under your home directory. **Built-ins always win** — a `general.md` can't silently replace the built-in `general`. Files are picked up live (no restart); up to 32 custom agents, 32 KiB each.

## Managing subagents in the app

[[Ctrl+K]] → **Subagents** lists the built-in types and every custom agent it discovered, and can drop a starter template into `.agentty/agents/` to author from. Authoring is a file you edit; the panel is for browsing what's available.

## How a subagent runs

- **Isolated & autonomous.** It never sees the parent conversation and can't ask it questions — it works from the task prompt alone, makes a reasonable assumption on ambiguity (and notes it), then stops and writes a tight, evidence-cited report. The report is the *only* thing the parent receives.
- **Bounded.** Each subagent runs a capped burst — 8 K output tokens, tool results clipped, up to 24 turns — comfortably inside the base 200 K window, so a fan-out stays cheap. It never uses the 1M-context beta.
- **Prompt-cached.** The heavy shared prefix (system prompt + tool schemas + accumulated results) is cached across the subagent's turns, keyed per role + task, so a long investigation doesn't re-pay for its prefix every turn.

## Model routing

By default, **read-only roles** (`explorer`, `reviewer`) are routed to the **cheapest capable model** your active provider offers — mapping, reading, and reviewing are grunt work a small model does well — while **write-capable roles** (`coder`, `tester`, `general`) keep the parent model so their edits match its quality. The router never routes *up* and never crosses providers, so a single-model or Opus-only account sees no change.

With [Smart Mode](/docs/smart-mode) subagent routing enabled, each worker's model is resolved by its **role** through your pinned model slots instead (explorer → Utility, reviewer → Strategic, coder/tester/general → Implementation).

## Related

- [Smart Mode](/docs/smart-mode) — role-based routing that decides which model each subagent runs on.
- [Tools](/docs/tools) — the tools a subagent can be allowed.
- [Sandboxing & permissions](/docs/sandboxing) — how read-only enforcement and tool gating work.
