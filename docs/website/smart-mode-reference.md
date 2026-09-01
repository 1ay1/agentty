---
title: "Smart Mode: Reference"
description: Every toggle, settings.json key, on-disk store, and the design rationale behind Smart Mode.
nav_section: User Manual
nav_order: 56
slug: smart-mode-reference
---

The complete reference for Smart Mode: the overlay, the persisted config, the on-disk learning stores, constraints, and why it's built the way it is.

## The overlay ([[Ctrl+S]])

Four rows. [[↑]]/[[↓]] move; [[Enter]] toggles the master switch or assigns a slot; [[x]] resets a slot to auto; [[Esc]] closes. The slots dim when Smart Mode is off.

| Row | What it controls | Default |
|-----|------------------|---------|
| **Smart Mode** | master switch — off is a byte-for-byte no-op | off |
| **Strategic / Implementation / Utility** | the three model slots (pinned, or auto) | auto |

That's the whole surface. Turning Smart Mode **on** enables all three of its behaviours together — internal routing (compaction and other engine calls run on the Utility model), orchestration (the main turn runs on Strategic with the delegation directive), and subagent routing (each `task` worker's model resolves by its role).

They aren't separate toggles because no one reasonably wants them apart: switching internal routing off just makes your compaction summaries more expensive for no benefit. A toggle earns a row only where a reasonable user would reasonably choose either way.

## Commands

- **[[Ctrl+K]] → Smart Mode** (or [[Ctrl+S]]) — open the config overlay.

## Persisted config

Every overlay choice is saved to your settings so the next session starts where you left off. The state lives under the `smart_mode` key in `settings.json` (`~/.config/agentty/settings.json`, or the platform equivalent).

| Key | Meaning |
|-----|---------|
| `enabled` | master switch |
| `strategic` / `implementation` / `utility` | pinned model id for each slot, or empty for auto |

Older settings files may still contain `route_internal`, `orchestrate`, `route_subagents`, `learn_routing`, `outcome_feedback`, `speculative` and `recall_plans`. Those keys are ignored — no migration is needed.

:::note
You never have to hand-edit this file — the [[Ctrl+S]] overlay writes it for you. The keys are listed here so you know what a synced/checked-in settings file is carrying.
:::

## On-disk learning stores

All learning is **local to the workspace** and lives in the project's `.agentty/` directory. Nothing is uploaded; delete the files (or run **Reset Smart Mode learning**) to start clean.

| File | Written by | Contents |
|------|-----------|----------|
| `.agentty/routing_memory.tsv` | learned routing + outcome feedback | one row per turn signature: the effort prior and its running success rate |
| `.agentty/decompositions.jsonl` | plan recall | append-only log of successful task decompositions, keyed by turn signature |

Both are plain text and safe to inspect, diff, or delete. The routing memory is a small TSV keyed by a **hierarchical turn signature** (a language-agnostic structural class plus a content feature-hash — the task's *shape*, never the prompt text); the decomposition log is one JSON object per line. Both are **periodically compacted** so they stay small no matter how long you use the repo, and both are safe to write from **two agentty processes at once** in the same repo (an advisory file lock serialises them and merges rather than clobbers).

## Advanced tuning

The overlay controls *which* layers run. Four numeric **policy** knobs — for power users who want to retune the router's aggressiveness — are exposed as environment variables (read live, clamped to a safe range, unset = the shipped default). They're documented in full under [Configuration › Smart Mode tuning](/docs/configuration#smart-mode-tuning):

| Variable | Controls |
|----------|----------|
| `AGENTTY_SMART_COMPLEX_THRESHOLD` | how readily a turn classifies as Complex (the main cost/quality dial) |
| `AGENTTY_SMART_DEEP_MARGIN` | how deep into a tier before continuous effort adds an extra step |
| `AGENTTY_SMART_PRIOR_EVIDENCE` | how much evidence before the learned prior is trusted (learn-speed vs. stability) |
| `AGENTTY_SMART_BIAS_CLAMP` | how far the session cascade can drift effort from baseline |

### Force the master switch for one session

`AGENTTY_SMART_MODE=1` forces Smart Mode **on** for that process; `=0` forces it **off**. `1`/`true`/`yes`/`on` count as on, `0`/`false`/`no`/`off` as off; unset means your saved setting governs, as before.

```bash
AGENTTY_SMART_MODE=1 agentty   # deterministic routing for a scripted run
AGENTTY_SMART_MODE=0 agentty   # force it off without touching your config
```

The pin is **session-scoped and non-destructive**: agentty never persists it, so a benchmark, CI run, or bisect can't overwrite the preference you use interactively. While a pin is active the `^S` overlay's Enabled row shows `on (env pin)` / `off (env pin)`, and toggling it in-app is a hinted no-op (unset the variable to toggle again). Ideal for reproducible experiments.

Individual classifier weights are deliberately *not* exposed. The tier **threshold** is the right control surface, not fifteen fiddly weights.

### Developer escape hatches

Each folded-in behaviour keeps a negative env override, for bisecting a routing bug without editing settings:

```bash
AGENTTY_SMART_NO_INTERNAL=1     # compaction/titles stay on the main model
AGENTTY_SMART_NO_ORCHESTRATE=1  # main turn stays on the selected model
AGENTTY_SMART_NO_SUBAGENTS=1    # workers use the tier auto-router
```

These are deliberately env-only and deliberately negative: the default is on, and an escape hatch in the UI is just a toggle with extra steps.

## Constraints

- **Off is a strict no-op.** With the master switch off, Smart Mode adds zero tokens, zero latency, and makes no routing decisions — the turn runs exactly as if the feature did not exist.
- **Roles resolve to models, never model names to behavior.** The resolver maps a *role* to `(model, effort)`. It never inspects a model id string to decide what to do, so pinning any model to any slot is always safe.
- **Effort never exceeds the turn's ceiling.** Complexity-scaled effort and cascade correction only move within the bounds the active model allows; a Utility model is never asked for more effort than it supports.
- **Learning is bounded and reversible.** Priors decay toward the default, are keyed by a turn *signature* (a structural class plus a content hash) rather than exact text, and can be wiped at any time. A cold workspace behaves identically to one with the learning layers off.

## Design rationale {#design}

Smart Mode follows the **orchestrator-workers** pattern from Anthropic's multi-agent work: a strong model owns the plan and delegates well-scoped subtasks to cheaper workers, rather than one model doing everything at one effort level. Three ideas make that practical here:

1. **Roles, not model names.** Decoupling behavior from model identity keeps every layer composable — you can pin models, swap providers, or turn a layer off without touching the others.
2. **Complexity-scaled effort + cascade.** Most turns are simple; spending flagship effort on them is waste. The classifier scales effort to the turn, and cascade correction retries at higher effort only when a cheap attempt actually falls short — the RouteLLM/cascade insight applied inside the agent loop.
3. **Outcome-grounded learning.** A stateless router can't learn, because it never sees whether its choice worked. The agent loop *does* — it sees the build fail, the test go red, the user correct the next turn. Smart Mode's learning layers close that loop: they persist what the cascade discovered and what decompositions succeeded, so the second session in a repo is smarter than the first.

See the [design note](https://github.com/1ay1/agentty/blob/master/docs/design/smart-mode.md) for the full write-up and the layer-to-file map.
