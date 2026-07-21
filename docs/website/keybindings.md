---
title: Keybindings
description: The complete agentty keymap.
nav_section: User Manual
nav_order: 20
slug: keybindings
---

Everything you can do without leaving the home row.

| Key | Action |
|---|---|
| [[Enter]] | Send the current message |
| [[Alt+Enter]] | Insert a newline in the composer |
| [[Ctrl+E]] | Expand the composer |
| [[Esc]] | Cancel the current turn / reject a permission / close a modal (does NOT quit) |
| [[S-Tab]] | Cycle permission profile (Write → Ask → Minimal) |
| [[↑]] | On empty composer: recall queued messages for editing |
| [[Ctrl+K]] | Command palette |
| [[Ctrl+J]] | Thread list (opens at the current thread) |
| [[Ctrl+N]] | New thread |
| [[Alt+←/→]] | Quick-cycle to the adjacent thread (← newer, → older) |
| [[Ctrl+←/→]] | Quick-cycle threads (empty composer, idle session) |
| [[Ctrl+T]] | Todo / plan view |
| [[Ctrl+/]] | Model picker |
| [[Ctrl+P]] | Provider picker (switch LLM backend live) |
| [[Ctrl+G]] | Run a code block from the newest reply on your real terminal |
| [[Ctrl+R]] | Review pending diffs |
| [[Ctrl+L]] | Redraw the screen |
| [[Ctrl+C]] | Quit (the only quit key) |

## Queue behavior

Typing while a turn streams queues the message rather than interrupting. Press [[↑]] on an empty composer to pull every queued message back into the buffer (joined by newlines) with the cursor at the seam — destructive on the queue, so re-submit to re-queue. The composer placeholder hints `press ↑ to edit queued — type to queue another…` when relevant.

## Palette-only actions

Some actions have no dedicated key — reach them from the command palette ([[Ctrl+K]]). Notably **Rewind to checkpoint** opens a diff-preview picker over every checkpointed turn (git repo + idle session); see [Checkpoints & rewind](/docs/threads#checkpoints).
