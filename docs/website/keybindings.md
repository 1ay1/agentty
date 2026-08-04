---
title: Keybindings
description: The complete agentty keymap.
nav_section: User Manual
nav_order: 20
slug: keybindings
---

Everything you can do without leaving the home row.

## Global

| Key | Action |
|---|---|
| [[Enter]] | Send the current message |
| [[Esc]] | Cancel the current turn / reject a permission / close a modal (does NOT quit) |
| [[S-Tab]] | Cycle permission profile (Write → Ask → Minimal) |
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
| [[Ctrl+O]] | Browse the full text of a frozen retrieved-context card |
| [[Ctrl+U]] | Expand / collapse the newest retrieved-context card (empty composer) |
| [[Ctrl+L]] | Redraw the screen |
| [[Ctrl+C]] | Quit (the only quit key) |

## Composer

The composer is a full readline-style editor. Keys are grouped below by what
they do.

### Submit & compose

| Key | Action |
|---|---|
| [[Enter]] | Send the message (or, while streaming, queue it) |
| [[Shift+Enter]] | Insert a newline |
| [[Alt+Enter]] | Insert a newline (fallback for terminals that don't deliver Shift+Enter) |
| [[Ctrl+E]] | Toggle the expanded (taller) composer |
| [[/]] | Open the command palette — when line-leading (start of buffer or after a newline) |
| [[@]] | Open the file-mention picker — at a word boundary; inserts an `@file` chip |
| [[#]] | Open the symbol picker — at a word boundary; inserts a `#symbol` chip |

### Cursor motion

| Key | Action |
|---|---|
| [[←]] / [[→]] | Move one character (steps over an attachment chip as a single unit) |
| [[Ctrl+←]] / [[Ctrl+→]] | Move one word (a run of punctuation counts as one unit) |
| [[Home]] / [[End]] | Jump to start / end of the buffer |

[[Ctrl+←]] / [[Ctrl+→]] switch to word-motion only when the composer has text;
on an **empty** composer with an idle session they cycle threads instead (same
as [[Alt+←/→]]).

### Editing

| Key | Action |
|---|---|
| [[Backspace]] | Delete the character (or whole chip) before the cursor |
| [[Ctrl+W]] | Delete the word before the cursor (readline *unix-word-rubout*) |
| [[Alt+D]] | Delete the word after the cursor (readline *kill-word*) |
| [[Ctrl+U]] | Kill to beginning of line |
| [[Alt+K]] | Kill to end of line |
| [[Ctrl+Z]] | Undo — rewinds word-by-word, not keystroke-by-keystroke |
| [[Ctrl+Y]] / [[Ctrl+Shift+Z]] | Redo |
| [[Ctrl+V]] / [[Alt+V]] | Paste an image from the clipboard as an attachment chip |

Undo coalesces a run of typing into one step (broken on whitespace and on any
non-typing edit), so one [[Ctrl+Z]] after a paste reaches the pre-paste state.
[[Ctrl+V]] is intercepted by some terminals (Windows Terminal binds it to its
own paste); [[Alt+V]] is the fallback that every terminal passes through.

> Kill-to-end is [[Alt+K]], not the readline-standard [[Ctrl+K]] — [[Ctrl+K]] is
> reserved app-wide for the command palette. It pairs with [[Ctrl+U]]
> (kill-to-start) the same way [[Alt+D]] (delete word forward) pairs with
> [[Ctrl+W]] (delete word back). [[Ctrl+U]] also doubles as “expand/collapse the
> newest retrieved-context card” **when the composer is empty** and such a card
> is on screen; with text in the box it always means kill-to-line-start.

### History & queue

| Key | Action |
|---|---|
| [[↑]] | On an empty composer: recall queued messages (if any), else walk back through your previous messages |
| [[↓]] | Walk forward through history toward the live draft |
| [[Alt+↑]] | Edit queued messages one at a time (loads the most recently queued first) |
| [[Alt+↓]] | Step back out of the per-item queue editor toward the live draft |
| [[Alt+Backspace]] | Drop the most recently queued message (empty composer, nothing peeked) |

## Queue behavior

Typing while a turn streams **queues** the message rather than interrupting.
There are two ways to work with the queue:

- **[[↑]] on an empty composer** pulls *every* queued message back into the
  buffer at once (joined by newlines, cursor at the seam) — destructive on the
  queue, so re-submit to re-queue.
- **[[Alt+↑]] / [[Alt+↓]]** step through the queue **one message at a time** to
  fix a single entry; [[Enter]] re-queues the edited message in place. This
  works even mid-edit, so you don't have to clear the composer first.

The composer placeholder hints `press ↑ to edit queued — type to queue
another…` when relevant.

## Palette-only actions

Some actions have no dedicated key — reach them from the command palette
([[Ctrl+K]]). Notably **Rewind to checkpoint** opens a diff-preview picker over
every checkpointed turn (git repo + idle session); see
[Checkpoints & rewind](/docs/threads#checkpoints). **Compaction depth** cycles
how full the context window gets before agentty auto-compacts in the background
(75/90/95%) — see [Providers & Models](/docs/providers#1m-context-models).
