---
title: "Smart Mode: Learning"
description: The self-supervised loop that makes Smart Mode get measurably better at your specific repo — learned routing, outcome feedback, and plan recall.
nav_section: User Manual
nav_order: 55
slug: smart-mode-learning
---

This is what makes Smart Mode more than a good heuristic: it **observes its own execution** in your workspace and improves. A stateless model router can't do this — it only ever sees the query. agentty is an agent loop, so it sees what happened *after* the routing decision: whether the model delegated heavily, whether the build failed, whether you said "no, that's wrong." Those are ground-truth signals, and Smart Mode learns from them.

Everything here is **per-workspace** and **local** — the learning for one repo never leaks into another, and nothing leaves your machine.

## Learned routing

The [cascade correction](/docs/smart-mode-routing#the-cascade-correction) adjusts effort *within* a session. **Learned routing** persists that signal *across* sessions.

Each turn gets a **signature** — a hierarchical key with a coarse and a fine part. The **coarse** part is language-agnostic structure: the complexity tier plus a few cheap buckets (does it mention code? ask a question? how long is it?). The **fine** part is a compact feature-hash of the turn's salient content words (any language, order-independent). agentty keeps a Beta-smoothed tally, per signature, of whether that class of turn tends to be **under-** or **over-rated** by the classifier in *this* repo.

The two levels give you the best of both: **specificity when there's evidence, generalization when there isn't.** On a future turn, the learned prior reads the fine key first, but if that exact turn hasn't been seen enough, it **backs off** to its coarse structural class — so a brand-new turn *inherits the prior its whole class has already earned*, and a turn you've done many times gets its own sharper prior. (This is the empirical-Bayes shrinkage that production ranking systems use.)

The effect compounds. In a gnarly systems codebase, "fix the build" is almost always a complex, multi-file turn — after a few of them, Smart Mode learns to route that *shape* with more effort here, even though the same words in a notes repo stay trivial. **The router gets better at your repo the more you use it.**

- Stored at `.agentty/routing_memory.tsv` (append-only, Beta-smoothed, periodically compacted so the file stays small).
- A single event never swings the prior; confidence grows with evidence. How much evidence is [tunable](/docs/configuration#smart-mode-tuning) via `AGENTTY_SMART_PRIOR_EVIDENCE`.
- Neutral when unseen — a fresh repo behaves exactly like the plain classifier.
- Safe across concurrent agentty processes in the same repo (advisory file lock; a peer's writes are never lost).

## Outcome feedback

Learned routing is only as good as the signal feeding it. **Outcome feedback** grounds it in what actually *worked*, using signals a query-only router structurally can't observe:

- **A failed build or test in the turn** — a `bash` / `diagnostics` / `test` / `edit` tool call that failed is evidence the turn was harder than it was routed for. The router marks that signature as under-rated.
- **A correction on the very next turn** — if you reply with dissatisfaction ("no, that doesn't work", "that's wrong", "undo", "revert", "still failing"), agentty reads it as ground truth that the previous turn's route was too weak, and re-rates *that* turn's signature. It's deliberately precise: a redirection ("wrong file, look elsewhere"), an additive request ("actually, also add tests"), or praise ("actually that's perfect") is **not** a correction — so normal follow-ups don't ratchet the prior.

This is counterfactual routing evaluation from the agent's own trajectory — the router learns from real outcomes, not from how hard the prompt merely *looked*.

:::note
The correction detector is a cheap keyword scan on the opening of your next message. It's a bias signal, not a verdict — it nudges a Beta-smoothed prior that takes sustained evidence to move.
:::

## Plan recall

Routing learns *how hard* a turn is. **Plan recall** learns *how to structure* it.

When a Complex turn succeeds by delegating — say `explorer: map the auth call sites → coder: apply the change → tester: run the auth tests` — that sequence is a reusable artifact. agentty captures the decomposition (agent type + a short brief for each step) keyed by the same turn signature. On a future similar turn, it retrieves the closest past decomposition and hands it to the orchestrator as a concrete example:

> *A past turn like this in THIS repo was decomposed as: • explorer: map the auth call sites • coder: apply the change • tester: run the auth tests. Reuse this shape where it fits.*

So the lead reuses what worked instead of re-deriving the structure from scratch every time. This is retrieval-augmented orchestration — the decomposition memory grows with your repo.

- Stored at `.agentty/decompositions.jsonl` (append-only, deduped).
- Recall prefers an exact signature match, then falls back to the same complexity tier.
- Only *successful* turns (delegated real work, no tool failure) are captured.

## Seeing and resetting what it learned

The [[Ctrl+S]] overlay footer shows how much this workspace has taught the router:

```
Learned 12 routing patterns · 4 plans in this repo
```

To wipe it — starting a new kind of project in the same directory, or just a clean slate — run **[[Ctrl+K]] → *Reset Smart Mode learning***. That deletes this workspace's `routing_memory.tsv` and `decompositions.jsonl`. It only affects the current workspace.

:::tip
The stores are plain text (`.tsv` and `.jsonl`) under `.agentty/`. You can inspect them directly, and they're safe to delete by hand — Smart Mode just falls back to the neutral heuristic until it relearns.
:::

## The full loop

Putting it together, one turn:

1. **Classify** the turn (complexity tier → signature).
2. **Route** — fold in the learned prior and session cascade bias, scale the Strategic effort, recall a past decomposition.
3. **Execute** — the lead thinks and delegates; the 🧠 card shows the decision.
4. **Observe** — count delegations, watch for tool failures, capture a successful decomposition.
5. **Feed back** — a correction on the *next* turn closes the loop; the prior updates; the next similar turn is routed better.

Every step is gated behind its own toggle, and the whole thing is off unless you turn Smart Mode on.
