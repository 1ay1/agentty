---
title: Threads & Persistence
description: Where agentty stores conversations and credentials, and how to manage them.
nav_section: User Manual
nav_order: 40
slug: threads
---

Every conversation is a thread, stored as a single JSON file you can inspect, back up, or delete. Nothing is hidden in a database.

## Where threads live

Threads are written to `~/.agentty/threads/`, one JSON file per thread keyed by its id. They're plain files you can inspect, back up, or delete.

```text
~/.agentty/
├── threads/
│   ├── f24a29c6….json     # one file per conversation
│   └── 86be6534….json
├── settings.json          # provider, model, profile, favourites
└── memory.jsonl           # user-scope remembered facts
```

Threads are global — the directory is flat and a thread isn't bound to the workspace you created it in, so [[Ctrl+J]] lists every conversation regardless of which project you launched from.

## Managing threads

Press [[Ctrl+J]] to open the thread list and switch between past conversations. [[Ctrl+N]] starts a new thread. Since each thread is plain JSON, you can also `rm` one or copy it elsewhere as a backup.

## Checkpoints & rewind

Inside a git repo, every user turn pins a **worktree snapshot** before the agent starts editing. The turn's meta line carries a subtle `· ↺ checkpoint` tag so a restore point reads as an ordinary turn, not a banner. Nothing is committed to your history — the snapshot is captured out-of-band, concurrent with the request, so it costs you nothing.

Open the command palette ([[Ctrl+K]]) and pick **Rewind to checkpoint** to reach *any* earlier turn, not just the last. The picker lists every checkpointed turn (turn number + prompt preview + relative time), and each row shows a `N files · +A −D` summary of what the worktree has changed *since* that point — computed asynchronously, so opening is instant even on a big repo. [[↑↓]] / [[j]] / [[k]] move, [[Enter]] rewinds, [[Esc]] cancels.

A rewind is a destructive double restore: the worktree files *and* the transcript both return to the instant before that turn was submitted, and the original prompt is refilled into the composer so you can edit and resend. It's gated on an idle session and a real git repo (a friendly toast explains why otherwise). Checkpoints key off the project directory agentty was launched from, so they keep working even under `--workspace /`.

## Atomic writes

Thread and credential writes are atomic: agentty writes to a temp file, calls `fsync`/`_commit`, then `rename`s into place (`MoveFileExW` on Windows). A crash mid-write can't leave you with a half-written, corrupt thread.

## Credentials

Auth lives separately at `~/.config/agentty/credentials.json` (mode `0600`) — see [Authentication](/docs/authentication).
