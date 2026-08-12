---
title: "Smart Mode: Orchestration"
description: The orchestrator-workers pattern — how the Strategic model delegates mechanical work to cheaper subagents, and the routing card that shows it.
nav_section: User Manual
nav_order: 54
slug: smart-mode-orchestration
---

Orchestration is the layer that turns Smart Mode from "cheaper compaction" into an actual **lead agent that delegates**. When it's on, the main turn runs on your **Strategic** model and it's taught to keep the decisions for itself and hand mechanical work to cheaper **subagents**.

## The pattern

This is the **orchestrator-workers** pattern — the same one Anthropic's multi-agent research system uses, and the one shipped coding agents converge on. Crucially, it is *not* a planner that emits a static list of steps to execute. The Strategic model stays in agentty's normal reactive loop and delegates **using the `task` tool it already has**, deciding moment-to-moment what to keep and what to offload. There's no second agent framework and no brittle plan to fall out of sync with reality.

```
        You ── prompt ──▶  Strategic (the lead: thinks, decides, delegates)
                               │
             ┌─────────────────┼─────────────────┐
             ▼                 ▼                 ▼
        task(explorer)    task(coder)       task(tester)
         Utility model    Impl model         Impl model
        (map the code)   (make the edit)   (run the tests)
             │                 │                 │
             └──── one condensed report each ────┘
                               ▼
                     Strategic synthesises → replies to you
```

## The delegation directive

When orchestration is active, agentty appends a `<smart-mode>` instruction to the system prompt for the main turn. It encodes the hard-won lessons from production multi-agent systems:

- **Think first.** Plan the decomposition before acting.
- **Delegate with a complete brief.** Each subagent gets an objective, the output format you need back, which tools/paths are in scope, and clear boundaries. Vague briefs make workers duplicate effort or miss the point.
- **Scale the number of workers to complexity.** A trivial turn spawns none; a complex one runs several in parallel.
- **Pass a structured hand-off, not a transcript.** The lead sends a crisp brief (objective + constraints + relevant `file:line` refs), and each subagent returns *one condensed report* — not its whole reasoning trace.
- **Start wide, then narrow.** Broad exploration first, then drill into specifics.

The worker-budget line is keyed to the [classified complexity](/docs/smart-mode-routing#complexity-scaled-effort): a *Trivial* turn is told to just do it directly; a *Complex* turn is told to plan, then run several explorers in parallel and synthesise.

:::note
The directive is guidance, not a hard rule — the lead decides. In practice a strong Strategic model delegates readily once told the taxonomy; a weaker one may keep more work itself, which is still correct, just less parallel.
:::

## Subagent routing

With **Subagent routing** on, each `task` worker's model is resolved by its role rather than a blanket "read-only → cheap" rule:

| Agent type | Role | Runs on |
|-----------|------|---------|
| `explorer` | Utility | cheapest capable |
| `reviewer` | Strategic | your flagship |
| `coder` | Implementation | mid model |
| `tester` | Implementation | mid model |
| `general` | Implementation | mid model |

So exploration and search stay cheap, code edits match the parent's quality, and a critical review still gets your best model. With the layer off, agentty falls back to its existing tier auto-router (read-only roles → cheapest capable) — no regression.

## The routing card

Every orchestrated turn shows its **routing decision** as a first-class card in the transcript, right before the reply:

```
🧠 Smart Mode                                    14:32
   → claude-opus-4-5  · effort high  · complex
   ● orchestrate   ● subagents
```

It tells you, at a glance: the model the turn was routed to, the reasoning effort it was scaled to, the classified complexity, and which layers are active. The **card is the decision**; the actual delegations render as ordinary `task` [tool cards](/docs/tools) below it — so you can watch each worker run and read its report. The card is display-only: it never goes to the model and is never saved to the thread.

:::tip
Watching the 🧠 card + the `task` cards together is the whole story of a turn: *what* agentty decided, then *how* it executed. If a turn feels over- or under-powered, the card tells you why — and the [learning loop](/docs/smart-mode-learning) is already adjusting for next time.
:::

## Speculative execution

On **Complex** turns with the *Speculative* layer on, agentty fires a detached, local retrieval warm-up the moment the turn launches — so the workspace index is hot and the grounding pre-fetched by the time the lead delegates its first explorer. It overlaps real work with the lead's thinking, and because retrieval is local it costs zero tokens. It's opt-in (off by default) since it does a little speculative work that a turn might not need.
