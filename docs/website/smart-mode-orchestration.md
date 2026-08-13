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

When orchestration is active, agentty appends a `<smart-mode>` instruction to the system prompt for the main turn. Rather than a list of aspirations, it encodes the **decision rules** for the documented failure modes of orchestrator-workers systems:

- **Context economy is the *why*.** The lead is told its own context is the expensive resource — delegation exists to spend a *cheaper* worker's context on mechanical breadth, protecting the lead's for decisions and synthesis.
- **Delegate only when it pays.** The #1 failure mode is over-delegation: spawning a worker for a one-step lookup that costs more than doing it directly. The rule is explicit — delegate breadth and depth, never a single quick tool call.
- **Brief each worker completely.** Objective, exact output format, in-scope tools/paths, and clear boundaries (what *not* to touch) — a crisp brief (objective + constraints + `file:line` refs), not the lead's reasoning transcript. Vague briefs make workers duplicate effort; that waste is on the lead.
- **Sequence vs parallel.** Genuinely independent work runs in parallel (tasks launched together); dependent work (explore → then edit what you found) is sequenced so the lead never guesses at inputs it hasn't gathered.
- **Own the answer.** A worker's report is *evidence, not truth* — the lead verifies claims that matter (spot-check a cited `file:line`, re-run a test) before building on them, and never relays a worker report verbatim as its conclusion.

The worker-budget line is keyed to the [classified complexity](/docs/smart-mode-routing#complexity-scaled-effort): a *Trivial* turn is told the delegation overhead exceeds the work; a *Complex* turn is told to plan, then run several workers in parallel for the independent sub-tasks and synthesise them into one verified answer.

The **worker** side interlocks: each subagent is told to *decide, don't stall* (make a reasonable assumption on a minor gap and note it, rather than aborting the whole delegation) and to return a **tight** report — one-line outcome, only the details the parent needs to act with `file:line` evidence, and any assumptions — because the parent pays context for every line it sends back.

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
   → claude-opus-4-5  · effort high  · complex   medium → complex · learned +1
   ● orchestrate   ● subagents
```

It tells you, at a glance: the model the turn was routed to, the reasoning effort it was scaled to, the classified complexity, and which layers are active. The trailing note is the **effort provenance** — it makes the adaptive decision legible: it reads *base effort → complexity tier*, then any correction that moved it and where that came from (`learned` = this repo's persisted prior, `session` = the in-session cascade). So when a turn thinks harder than your baseline, the card shows you exactly why. The **card is the decision**; the actual delegations render as ordinary `task` [tool cards](/docs/tools) below it — so you can watch each worker run and read its report. The card is display-only: it never goes to the model and is never saved to the thread.

:::tip
Watching the 🧠 card + the `task` cards together is the whole story of a turn: *what* agentty decided, then *how* it executed. If a turn feels over- or under-powered, the card tells you why — and the [learning loop](/docs/smart-mode-learning) is already adjusting for next time.
:::

## Speculative execution

On **Complex** turns with the *Speculative* layer on, agentty fires a detached, local retrieval warm-up the moment the turn launches — so the workspace index is hot and the grounding pre-fetched by the time the lead delegates its first explorer. It overlaps real work with the lead's thinking, and because retrieval is local it costs zero tokens. It's opt-in (off by default) since it does a little speculative work that a turn might not need.
