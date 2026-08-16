---
title: Forking a Thread
description: Fork branches a conversation into a fresh thread that carries near-zero context — the parent transcript is written to disk and read on demand, so forking costs O(1) tokens no matter how large the parent grew.
nav_section: User Manual
nav_order: 45
slug: fork
---

Long agent sessions fill the context window. The usual escapes are *compaction* (summarize the old turns, losing detail) or *a new thread* (lose everything). agentty adds a third, better one.

**Fork branches the current conversation into a _fresh_ thread that carries almost no context.** The parent's full transcript is written to disk; the fork holds only a small pointer to it. The model **reads that transcript on demand** — pulling just the slice it needs, only when it needs it — instead of paying for the whole history up front.

The result: **forking costs O(1) tokens no matter how big the parent got.** It turns "fork" from *"branch with the same context"* into *"reclaim the context window and keep going."*

:::tip
Fork when a productive thread has run out of window but you still want continuity. The fork becomes your new main line; if the model needs a detail from the old conversation, it `read`s or `grep`s the transcript on its own.
:::

## Forking

Open the command palette with [[Ctrl+K]] and choose **Fork thread**. A small picker opens with one decision — how proactive [retrieval (RAG)](/docs/retrieval) should behave in the new thread, since a fork always starts fresh:

| Choice | The fork's retrieval behaviour |
|--------|--------------------------------|
| **RAG per turn** | retrieve context before every turn |
| **First-turn RAG** | retrieve once, up front, then stay quiet |
| **RAG off** | no proactive injection (search tools still work) |

[[↑↓]] / [[j]] [[k]] to choose, [[Enter]] to fork, [[Esc]] to cancel. The fork opens instantly with a **⑃ Forked** card at the top, and a toast confirms the new thread is live with fresh context.

You can't fork an empty thread, or while the agent is mid-turn — a friendly toast explains why.

## What the fork carries

Exactly one message: the **fork note**. It renders as the quiet **⑃ Forked** card (so the fresh thread is never a blank screen) and tells the model, in plain text, that it's a fork and where the parent transcript lives:

```text
This conversation is a fork of an earlier one. Its full transcript is
saved at:
  ~/.agentty/threads/<parentId>.transcript.md
Read it with the `read` tool (or grep it) ONLY if you need earlier
context — don't read it pre-emptively.
```

The model receives this in full, so it always knows the transcript exists. It just doesn't pay for the transcript's contents unless it chooses to open the file.

## The transcript file

When you fork, agentty exports a clean, human-readable Markdown snapshot of the parent to `~/.agentty/threads/<parentId>.transcript.md`, right next to the thread files. It's `## user` / `## assistant` headers plus the text, with tool calls collapsed to one line each. **Tool output is not written** — the heaviest bytes of a long agentic thread (big file reads, build logs) are dropped, keeping the file small and greppable.

The export is **bounded**, so even a maxed-out, million-token parent produces a useful artifact instead of a giant file:

- **Total cap (512 KB), recency-biased.** If the whole thread doesn't fit, the transcript keeps the **most recent** turns — the ones a fork is most likely to need — and elides the oldest with a clear marker. The header still reports the true total message count.
- **Per-message cap (16 KB), head + tail.** One giant pasted block can't dominate; it's clipped at both ends (where the signal is) with a `… bytes elided …` marker.

Writes are atomic (temp file → `fsync` → `rename`), so a crash mid-fork never leaves a truncated transcript.

:::note
The transcript being large is fine — it lives on **disk**, not in the context window. The fork carries only the pointer. A 500 KB transcript that the model greps a 2 KB slice out of is exactly the point.
:::

## Fork vs. compaction

Both manage a too-full context window, and they compose cleanly — you can fork a compacted thread, or compact a fork.

| | [Compaction](/docs/providers#1m-context-models) | Fork |
|---|---|---|
| Thread | **same** conversation | **new** thread (provenance recorded) |
| Old context | lossy summary, kept in-window | verbatim on disk, read on demand |
| Up-front token cost | pays to summarize the prefix | ~zero (just a pointer) |
| Detail preserved | compressed to prose | **full** — the transcript is verbatim |
| Purpose | keep *one* long thread going | *branch* and reclaim the window |

Compaction keeps a single thread alive by summarizing its own history; fork starts a clean thread and leaves the history on disk. Use compaction to keep going in place; use fork to draw a line and start fresh without losing the ability to look back.

## Provenance

The original thread is **saved untouched** — forking is non-destructive. The new thread records which thread it was forked from, so the history stays traceable. Both threads are ordinary JSON files under `~/.agentty/threads/` (see [Threads & Persistence](/docs/threads)); you can inspect, back up, or delete either independently.
