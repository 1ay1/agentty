---
title: The Interface
description: A tour of the agentty TUI — composer, transcript, status bar, and tool widgets.
nav_section: User Manual
nav_order: 10
slug: interface
---

agentty renders inline at the bottom of your terminal. It never takes over the screen — your scrollback stays intact, the status bar overlays on top.

## The transcript

Conversation history flows in your terminal's normal scrollback. Your turns, the assistant's replies, and every tool call render in order. Queued messages appear as preview rows above the composer — visually identical to real user turns.

Inside a git repo, a user turn that pinned a worktree snapshot carries a subtle `· ↺ checkpoint` tag in its meta line — a restore point you can [rewind to](/docs/threads#checkpoints) from the command palette.

## The composer

The input box at the bottom. Type and press [[Enter]] to send. [[Alt+Enter]] inserts a newline. [[Ctrl+E]] expands the composer for longer prompts. A `❚ N queued` chip shows how many messages are waiting.

Type while a turn is streaming and your message **queues** — it lands automatically when the current turn finishes. On an empty composer, press [[↑]] to recall every queued message back into the buffer for editing.

The composer is project-aware: type [[@]] to mention a file, [[#]] to jump to a symbol, and [[/]] (on an empty composer) to open the command palette — the same fuzzy list as [[Ctrl+K]], with entries like *Compact context*, *Switch provider*, and *New thread*. Drop an image file's path (or paste with [[Ctrl+V]]) to attach a PNG, JPEG, GIF, or WebP inline.

## Run code blocks (Ctrl+G)

When a reply hands you a fenced block of shell commands, don't copy-paste it. [[Ctrl+G]] lists the runnable blocks from the newest reply; [[Enter]] (or a digit) runs one **interactively on your real terminal** — the TUI suspends, so `sudo` password prompts work, output streams live, and [[Ctrl+C]] kills the command (not agentty). When it exits, a result card lets you attach the captured output back to the composer as a collapsed chip ([[a]]), copy it ([[y]]), or discard ([[Esc]]) — so “it failed with X” reaches the model without you re-typing anything. A transient toast surfaces the affordance while the commands are still on screen. It runs the right shell per block on every OS: `sh`/`bash` blocks through `/bin/sh` on Linux/macOS, `powershell`/`cmd` blocks natively on Windows.

## Threads & quick-cycle

Every conversation is a saved thread. [[Ctrl+J]] opens the thread list *at the current thread* (marked with a bold `●`), and [[Ctrl+N]] starts a new one. To flip to the adjacent thread without opening the picker, use [[Alt+←/→]] (or [[Ctrl+←/→]] on an empty composer) — [[←]] newer, [[→]] older — with a “thread k/N · title” toast on every hop. Quick-cycle only fires while the session is idle, so a live stream can never be yanked out from under you.

## The status bar

A single row at the bottom edge shows the active profile, provider, and model, plus the current phase. When something needs your attention — a transient retry, an error — it swaps in a banner-style notification (`▎⚠ <text>` for errors, `▎ <text>` for info) and reverts to the keybindings strip when the toast expires. Switch provider with [[Ctrl+P]] and model with [[Ctrl+/]] without leaving the thread.

## Tool widgets

Each tool gets a purpose-built widget — agentty doesn't just print raw JSON:

- **Diffs render as diffs** — additions and deletions, color-coded.
- **Search results group by file**, with line numbers.
- **bash shows exit codes** and streamed output.
- **todos become checklists** you can watch tick off.

## Smooth streaming

SSE deltas drip into the screen at ⅛ buffer per tick (clamped 32–256 chars), so server-side batching doesn't translate into chunky on-screen text. Where the terminal supports it, frames are wrapped in DEC 2026 begin/end-sync to avoid tearing.
