---
title: "Smart Mode: Roles & Routing"
description: The three model roles, zero-config auto-fill, complexity-scaled effort, and the resolver that never checks a model name.
nav_section: User Manual
nav_order: 53
slug: smart-mode-routing
---

The heart of Smart Mode is a single idea: **every unit of LLM work has a role, and the role — not a hard-coded model name — decides which model runs it.** This page covers the three roles, how they're filled, and how reasoning effort scales to each turn.

## The three roles

| Role | Answers | Runs |
|------|---------|------|
| **Strategic** | *What should we build?* — architecture, API design, debugging hard bugs, review, decomposition | the main turn (and reviewer subagents) |
| **Implementation** | *Build what Strategic decided* — write/edit code, fix compiles, write tests | coder / tester subagents |
| **Utility** | *Gather info / mechanical work* — grep, read, summarise, commit messages, compaction | explorer subagents + internal engine calls |

Nothing in the engine ever writes `if (model == "opus")`. A single pure resolver maps `role → (model, effort)`; every call site just asks it. That's what makes Smart Mode provider-agnostic and what makes "off" a perfect no-op — with the master switch off, every role resolves to your single active model.

## Assigning models

Open [[Ctrl+S]]. Rows *Strategic*, *Implementation*, *Utility* each show the model that would run right now, tagged **· pinned** (you chose it) or **· auto** (Smart Mode filled it).

- **[[Enter]]** on a role row opens the model picker in *assign* mode — pick a model to pin that role.
- **[[x]]** on a pinned role resets it to **auto**.

### Zero-config auto-fill

Leave a role empty and Smart Mode fills it from your provider's live catalog using the same capability tiers agentty already uses:

- **Strategic** → your current (flagship) model, at your effort.
- **Implementation** → the strongest **mid-tier** model, at one effort step down.
- **Utility** → the **cheapest capable** model, with reasoning effort **off**.

So enabling Smart Mode with nothing pinned is safe and immediately useful. On a **single-model** or **Opus-only** account there's nothing cheaper to route to, so every role stays on your model — no change, no regression.

:::note
All three slots must be on the **same provider** (see the [reference](/docs/smart-mode-reference#constraints)). The picker only offers models from your active provider.
:::

## Complexity-scaled effort

Reasoning effort has to be decided *before* the request is sent, so Smart Mode classifies each turn upfront — no extra model call, no latency — into one of four tiers:

| Tier | Looks like | Effort |
|------|-----------|--------|
| **Trivial** | "yes", "commit it", "run it" | none |
| **Simple** | a short, single-clause fix | one step **down** from your baseline |
| **Standard** | the everyday working turn | your baseline (unchanged) |
| **Complex** | "why does this deadlock?", "refactor the auth module", a long or multi-part ask | one step **up** (or **two** when the turn is *deeply* complex) |

### How the classifier works

It's not a keyword lookup — it's a small **additive feature score**. Three orthogonal signal families each contribute weight, and the sum is thresholded into a tier:

- **Structural** (carries most of the weight, and is *language-agnostic*): enumerated asks, clause and conjunction density, code-token density, question shape, and length. A request's complexity lives mostly in its **structure**, not its verbs — so this works whether you write in English or not.
- **Lexical**: weighted design/debug/architecture keyword sets across the major languages. Keywords **add evidence** (capped) rather than overriding — one stray "design" in *"add a design token"* no longer forces the top tier.
- **Morphological**: token-shape variety (prose vs. identifiers vs. paths).

Because it's a score and not a switch, Smart Mode also knows *how far* into a tier a turn sits. **Continuous effort scaling** uses that: a turn that is barely Complex gets the normal one-step bump, but a turn that is *overwhelmingly* complex (long, multi-part, code-heavy, design vocabulary) gets an extra step — reaching the ceiling immediately instead of waiting for the session cascade to drift there.

The classifier is deliberately **conservative**: *Standard* is the fallback, and ambiguity biases **upward** — under-thinking a hard turn costs far more than a little wasted budget on an easy one. Effort is always clamped to what the chosen model actually supports (a model with no reasoning control resolves to *none*, honestly, rather than requesting an effort it will reject).

The tier boundary and the deep-band aggressiveness are [tunable](/docs/configuration#smart-mode-tuning) via `AGENTTY_SMART_COMPLEX_THRESHOLD` and `AGENTTY_SMART_DEEP_MARGIN` if you want the router more or less eager to escalate.

Classification is also **context-aware**: a short follow-up to a hard turn (*"now do the same for the other module"*) would score as Simple on its own text, but it inherits weight from the Complex turn it continues rather than collapsing to a one-liner — while a plain acknowledgement (*"thanks"*) is always taken at face value.

:::tip
You don't set the tier — it's inferred from your prompt. Ask a design or "why" question, or spell out a multi-step task, and the Strategic model automatically gets more room to think.
:::

### The cascade correction

The upfront classifier is a good first guess, not an oracle. Smart Mode layers a **cascade** on top: as a turn plays out, agentty watches what the orchestrator actually did and adjusts a running effort bias for the rest of the session. If a turn the heuristic called "Simple" ends up spawning several parallel workers, it was really complex — so the bias nudges up and the next turns think harder. If a "Complex"-rated turn delegated nothing and answered directly, the bias relaxes. The bias decays toward neutral each turn, so one anomaly never sticks.

This is the routing research's actual recommendation — *cascade beats one-shot routing* — and it's free here because the agent loop already sees the outcome. Persisting that correction across sessions is [Learned Routing](/docs/smart-mode-learning).

## Where each role is used

- **Main turn** → Strategic (when orchestration is on).
- **Subagents** (`task`) → by agent type: explorer → Utility, reviewer → Strategic, coder / tester / general → Implementation.
- **Internal engine calls** (auto-compaction summary today) → Utility.

Each funnels through the same resolver, so "which model is cheaper / stronger" lives in exactly one place.
