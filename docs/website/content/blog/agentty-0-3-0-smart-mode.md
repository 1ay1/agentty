---
title: "agentty 0.3.0 — Smart Mode: a self-supervised orchestrator that learns your repo"
date: 2026-08-13
author: agentty
tags: [release, smart-mode, routing, retrieval]
excerpt: "0.3.0 introduces Smart Mode — a role-based execution router that pins your flagship, mid, and cheap models to Strategic / Implementation / Utility roles, delegates mechanical work to subagents, scales reasoning effort per turn, and gets measurably better at your repo across sessions. Off = a byte-for-byte no-op. Plus a one-decision RAG picker, thread forking, and fixes for a cross-process OAuth refresh loop and widened-workspace path resolution."
---

# agentty 0.3.0

The headline of this release is **[Smart Mode](/docs/smart-mode)** — a
self-supervised orchestrator that routes each turn to the right model at the
right effort, delegates the mechanical work, and gets better at *your* repo
the more you use it. It's everything-toggleable, and **off it is a
byte-for-byte no-op**. Full detail in the
[changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md).

## Smart Mode: the right model, for the right role, at the right effort

Most of a coding session isn't the hard thinking — it's grep, read,
summarise, boilerplate. Paying flagship-model prices for all of it is waste;
paying cheap-model prices for the *architecture* is a mistake. Smart Mode
(`Ctrl+K → Smart Mode`, config overlay on `Ctrl+S`) is a role-based execution
router built on the orchestrator-workers pattern — Anthropic's multi-agent
research plus the RouteLLM / cascade literature — that puts each piece of work
on the model it deserves.

You pin three models to three roles (or leave them to zero-config auto-fill
from your live catalog):

- **Strategic** — your flagship. The thinking.
- **Implementation** — mid-tier. Writing code.
- **Utility** — cheap. grep / read / summarise.

Nothing ever checks a model *name*. One pure, tested resolver
(`agentty::smart`) maps `role → (model, effort)`, so it works with any
provider and any future model. Each layer is an independent toggle:

- **Internal routing** — engine-internal utility work (like the
  auto-compaction summary) runs on the cheapest capable model, never the
  flagship.
- **Orchestration** — the main turn runs on Strategic, and a `<smart-mode>`
  directive teaches it to keep the decisions and **delegate** mechanical work
  to subagents (`task explorer` / `coder`) with a complete brief, in parallel,
  wide-then-narrow.
- **Subagent routing** — each subagent's model resolves by its role
  (explorer → Utility, reviewer → Strategic, coder/tester → Implementation).
- **Complexity-scaled effort** — a local classifier rates each turn
  Trivial / Simple / Standard / Complex and scales the Strategic model's
  reasoning effort to match (ambiguity biases *up*).
- **Cascade feedback** — the effort heuristic self-corrects within a session
  from what the orchestrator *actually did* (heavy delegation ⇒ it was harder
  than rated).

### It learns your repo

This is the part a stateless router structurally can't do. A per-workspace
prior (`.agentty/routing_memory.tsv`, Beta-smoothed) remembers whether each
*class* of turn tends to be under- or over-rated **in this repo**, so the
router improves the more you use it. The ground-truth signal comes from
**outcome feedback**: a failed build/test in the turn, or a correction on the
next turn ("no", "that's wrong", "revert"), is treated as a routing regret
that re-rates that class of turn. And successful decompositions are captured
per-workspace (`.agentty/decompositions.jsonl`) so the closest past one is
injected into the next delegation prompt as a concrete few-shot — the
orchestrator reuses what worked instead of re-deriving it.

Every orchestrated turn surfaces its routing **decision** as a first-class
🧠 Smart Mode card in the transcript — routed model · scaled effort ·
complexity · active layers — and the delegations render as ordinary `task`
cards. It's persisted to `settings.json`, single-provider, and a
single-model / Opus-only account degrades to a no-op per layer. See
[Smart Mode](/docs/smart-mode), [Roles & Routing](/docs/smart-mode-routing),
[Orchestration](/docs/smart-mode-orchestration),
[Learning](/docs/smart-mode-learning), and the
[Reference](/docs/smart-mode-reference).

## One RAG decision, not a wall of toggles

Proactive retrieval collapses to a single picker (`Ctrl+K → RAG`):

- **On** — inject retrieved context before every turn.
- **First turn only** — ground the first turn, then stay quiet.
- **Off**.

The advanced retrieval knobs remain env-tunable but are out of the UI, and
proactive `<retrieved-context>` blocks no longer leak into the composer's
↑/↓ history recall.

## Fork a thread

`Ctrl+K → Fork thread` branches the current conversation into a new thread
(recording `forked_from` provenance) and lets you choose its RAG behaviour:
RAG per turn or first-turn RAG (both keep the transcript verbatim), or RAG
off (summarised by the utility model). The original thread is saved
untouched.

## Fixes worth calling out

- **No more cross-process OAuth refresh loop.** With several agentty windows
  open, an expired OAuth token used to send them all refreshing at once —
  and because Anthropic (and ChatGPT/Codex) *rotate* refresh tokens, the
  first refresh invalidated the shared token and the losers looped. A
  cross-process advisory file lock (`flock` / `LockFileEx` on `<creds>.lock`)
  now serializes refresh across processes, with a double-checked re-read so
  the losers adopt the winner's freshly-saved token instead of refreshing
  again.
- **Widened `--workspace` launches resolve paths correctly.** Tools now
  resolve relative paths against the active *project* (`project_root()`), not
  the access *boundary* — so under `--workspace /` a `read src/foo.cpp` lands
  in the launched project instead of the nonexistent `/src/foo.cpp`, and
  `repo_map`/`diagnostics`/`test`/the @-file picker stop trying to scan the
  whole disk. Every path is still containment-checked against the boundary.
- **`grep` and `find_definition` stop crawling build/vendor/`_deps` trees.**
  Both backends now prune the same skip-list the built-in walker used, passed
  to ripgrep as excludes — a symbol grep on a tree with non-gitignored build
  dirs dropped from 4 hits (3 generated) to the 1 real source hit.
- **Streaming reveal and tool cards are smoother.** A long paragraph no
  longer dumps its whole body in one frame at a block seam (worst streaming
  frame dropped from 172 to 22 cells), and a running tool's card is never
  hidden behind the reveal-defer machine.

## Get it

```bash
curl -fsSL https://agentty.org/install.sh | sh
# or
brew tap 1ay1/tap && brew install agentty
```

See the [full changelog](https://github.com/1ay1/agentty/blob/master/CHANGELOG.md)
for every detail.
