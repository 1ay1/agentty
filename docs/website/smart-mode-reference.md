---
title: "Smart Mode: Reference"
description: Every toggle, settings.json key, on-disk store, and the design rationale behind Smart Mode.
nav_section: User Manual
nav_order: 56
slug: smart-mode-reference
---

The complete reference for Smart Mode: the overlay, the persisted config, the on-disk learning stores, constraints, and why it's built the way it is.

## The overlay ([[Ctrl+S]])

[[↑]]/[[↓]] move; [[Enter]] or [[Space]] toggles a row or assigns a slot; [[x]] resets a slot to auto; [[Esc]] closes. A filled dot marks an active layer; layers dim when the master switch is off.

| Row | What it controls | Default |
|-----|------------------|---------|
| **Enabled** | master switch — off means every layer is inert (byte-for-byte no-op) | off |
| **Internal routing** | engine-internal utility calls (compaction summary) run on the Utility model | on |
| **Orchestration** | main turn runs on Strategic + the delegation directive | on |
| **Subagent routing** | each `task` worker's model resolved by its role | on |
| **Learned routing** | persist the per-workspace effort prior across sessions | on |
| **Outcome feedback** | build-fail / next-turn correction re-rates the turn's signature | on |
| **Speculative** | detached retrieval warm-up on Complex turns | off |
| **Plan recall** | capture and recall successful decompositions | on |
| **Strategic / Implementation / Utility** | the three model slots (pinned, or auto) | auto |

The learning layers (learned routing, outcome feedback, speculative, plan recall) additionally require **Orchestration** — they refine the orchestrated turn, which only exists when it's running. The footer shows the workspace's learning: `Learned N routing patterns · M plans in this repo`.

## Commands

- **[[Ctrl+K]] → Smart Mode** (or [[Ctrl+S]]) — open the config overlay.
- **[[Ctrl+K]] → Reset Smart Mode learning** — wipe this workspace's learned routing priors and captured decompositions.

## Persisted config

Every overlay choice is saved to your settings so the next session starts where you left off. The state lives under the `smart_mode` key in `settings.json` (`~/.config/agentty/settings.json`, or the platform equivalent).

| Key | Meaning |
|-----|---------|
| `enabled` | master switch |
| `route_internal` | internal routing layer |
| `orchestrate` | orchestration layer |
| `route_subagents` | subagent routing layer |
| `learn_routing` | learned routing layer |
| `outcome_feedback` | outcome feedback layer |
| `speculative` | speculative prewarm layer |
| `recall_plans` | plan recall layer |
| `strategic` / `implementation` / `utility` | pinned model id for each slot, or empty for auto |

:::note
You never have to hand-edit this file — the [[Ctrl+S]] overlay writes it for you. The keys are listed here so you know what a synced/checked-in settings file is carrying.
:::

## On-disk learning stores

All learning is **local to the workspace** and lives in the project's `.agentty/` directory. Nothing is uploaded; delete the files (or run **Reset Smart Mode learning**) to start clean.

| File | Written by | Contents |
|------|-----------|----------|
| `.agentty/routing_memory.tsv` | learned routing + outcome feedback | one row per turn signature: the effort prior and its running success rate |
| `.agentty/decompositions.jsonl` | plan recall | append-only log of successful task decompositions, keyed by turn signature |

Both are plain text and safe to inspect, diff, or delete. The routing memory is a small TSV keyed by a coarse turn signature (task shape, not prompt text); the decomposition log is one JSON object per line.

## Constraints

- **Off is a strict no-op.** With the master switch off, Smart Mode adds zero tokens, zero latency, and makes no routing decisions — the turn runs exactly as if the feature did not exist.
- **Roles resolve to models, never model names to behavior.** The resolver maps a *role* to `(model, effort)`. It never inspects a model id string to decide what to do, so pinning any model to any slot is always safe.
- **Effort never exceeds the turn's ceiling.** Complexity-scaled effort and cascade correction only move within the bounds the active model allows; a Utility model is never asked for more effort than it supports.
- **Learning is bounded and reversible.** Priors decay toward the default, are keyed by a coarse signature rather than exact text, and can be wiped at any time. A cold workspace behaves identically to one with the learning layers off.

## Design rationale

Smart Mode follows the **orchestrator-workers** pattern from Anthropic's multi-agent work: a strong model owns the plan and delegates well-scoped subtasks to cheaper workers, rather than one model doing everything at one effort level. Three ideas make that practical here:

1. **Roles, not model names.** Decoupling behavior from model identity keeps every layer composable — you can pin models, swap providers, or turn a layer off without touching the others.
2. **Complexity-scaled effort + cascade.** Most turns are simple; spending flagship effort on them is waste. The classifier scales effort to the turn, and cascade correction retries at higher effort only when a cheap attempt actually falls short — the RouteLLM/cascade insight applied inside the agent loop.
3. **Outcome-grounded learning.** A stateless router can't learn, because it never sees whether its choice worked. The agent loop *does* — it sees the build fail, the test go red, the user correct the next turn. Smart Mode's learning layers close that loop: they persist what the cascade discovered and what decompositions succeeded, so the second session in a repo is smarter than the first.

See the [design note](https://github.com/1ay1/agentty/blob/master/docs/design/smart-mode.md) for the full write-up and the layer-to-file map.
