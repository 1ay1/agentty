---
title: Smart Mode
description: A self-supervised orchestrator that routes each turn to the right model and effort — and gets better at your repo the more you use it.
nav_section: User Manual
nav_order: 52
slug: smart-mode
---

Smart Mode turns agentty from "one model does everything" into a small **team of models with a lead that delegates** — and a router that learns your codebase. It sends the hard thinking to your flagship, the grunt work to a cheaper model, scales reasoning effort to how hard each turn actually is, and remembers what worked in *this* repo so the next similar turn is routed better.

It's **off by default** and, when off, a **byte-for-byte no-op** — nothing about your turns changes until you enable it.

:::tip
Open the config overlay with [[Ctrl+S]] (or [[Ctrl+K]] → *Smart Mode*). Toggle the master switch, then leave everything else on the defaults — Smart Mode auto-fills sensible roles from your live model list.
:::

## The one-minute version

You pin three models to three **roles**:

| Role | Does | Typical model |
|------|------|---------------|
| **Strategic** | the thinking — architecture, decisions, review, decomposition | your flagship (e.g. Opus) |
| **Implementation** | writing code, fixing compiles, tests | a mid model (e.g. Sonnet) |
| **Utility** | grep / read / summarise / compaction | the cheapest capable model (e.g. Haiku) |

The planner thinks in *roles*, never model names. At dispatch time each unit of work resolves to the model that fits it. Turn Smart Mode off and every role collapses back to your single active model — exactly today's behaviour.

Leave the roles empty and Smart Mode **auto-fills** them from your provider's catalog: Strategic = your current model, Implementation = the strongest mid-tier model, Utility = the cheapest capable one.

## What it does, layer by layer

Smart Mode is one master switch plus a stack of independently-selectable layers. Each is a toggle in the [[Ctrl+S]] overlay; each is a pure win in isolation, so you pick exactly what you want.

- **Internal routing** — engine-internal utility calls (like the auto-compaction summary) run on the cheap Utility model instead of your flagship.
- **Orchestration** — the *main turn* runs on Strategic, and it's told to keep the decisions and **delegate** mechanical work to subagents.
- **Subagent routing** — each `task` subagent's model is chosen by its role (an explorer runs cheap; a reviewer runs strong).
- **Complexity-scaled effort** — a fast classifier rates every turn and scales the Strategic model's reasoning budget up for hard turns, down for trivial ones.
- **Cascade feedback** — the effort estimate self-corrects *within a session* from what the model actually did.
- **Learned routing** — a per-workspace memory remembers whether each kind of turn was under- or over-rated in *this* repo, so routing improves across sessions.
- **Outcome feedback** — a failed build or a "no, that's wrong" on the next turn teaches the router that class of turn needs more.
- **Speculative** — on hard turns, retrieval warms up while the lead thinks.
- **Plan recall** — successful decompositions are remembered and offered back as a template next time.

Deep dives:

- **[Roles & Routing](/docs/smart-mode-routing)** — the three roles, auto-fill, effort scaling, and how the resolver never checks a model name.
- **[Orchestration](/docs/smart-mode-orchestration)** — the orchestrator-workers pattern, the delegation directive, and the 🧠 routing card.
- **[Learning](/docs/smart-mode-learning)** — the self-supervised loop that makes Smart Mode get better at your repo.
- **[Reference](/docs/smart-mode-reference)** — every toggle, every `settings.json` key, the on-disk stores, and the design rationale.

## Why this shape

Smart Mode is built on the **orchestrator-workers** pattern (the one Anthropic's multi-agent research system uses and that shipped coding agents converge on), not a planner emitting a static step graph. The lead model stays in agentty's normal reactive loop and delegates *using a tool it already has* — so there's no second agent framework, no brittle plan to fall out of sync, and every layer degrades cleanly to a no-op. See the [design rationale](/docs/smart-mode-reference#design).

:::note
Smart Mode is **single-provider**: all three role slots must belong to the same provider (auth and prompt-caching don't transfer across providers). A single-model or one-tier account degrades to "everything on your model" — no change, no regression.
:::

## Cost, honestly

Routing grunt work to a cheaper model is where the savings come from — frontier and efficient models in the same family differ ~5× on price. The industry rule of thumb (RouteLLM and the cascade literature) is that good routing keeps ~95% of frontier quality while sending the majority of work to cheaper models. Smart Mode's orchestration layer costs *more* tokens on a hard turn (the lead spawns workers), so it's gated on task complexity and fully under your control — turn the layer off and the main turn just runs on your model.
