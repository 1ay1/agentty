# Slash menu

Design for typing `/` in the composer. Replaces the current behaviour,
where `/` at the start of a line opens the ^K action palette and takes
the text away from the composer.

## What is wrong now

- `/` at line start opens the action palette. Everything typed after it
  goes into the palette's filter, not the composer.
- The palette only knows the 21 built-in actions. Your commands
  (`.agentty/commands`, `.claude/commands`), skills and MCP prompts are
  not in it, so `/review x` can't be typed at all in the TUI.
- Esc, or Enter on no match, throws the typed text away. `/etc/hosts`
  as the start of a message is lost.
- Enter runs the top fuzzy match. `/undo` runs "Run code block",
  `/init` opens sign-in, `/reject` rejects all changes.
- The TUI, ACP and `agentty run` each resolve `/name` their own way and
  disagree on what exists.

## The idea

`/` is just text. It stays in the composer, with the caret, like any
other character. A small menu opens right above the composer and
filters as you type. The menu suggests; it never owns the input.

```
  ╭─────────────────────────────────────────────────────╮
  │ ▸ /review        Review a file for security   cmd   │
  │   /reject        Reject all pending changes   action│
  │   /rewind        Rewind to a checkpoint       action│
  │                                                     │
  │   <file> [focus]                                    │
  ╰─────────────────────────────────────────────────────╯
 ╭─────────────────────────────────────────────────────────╮
 │ › /re▏                                                  │
 ╰─────────────────────────────────────────────────────────╯
   ↹ complete · ↵ run · esc dismiss
```

The input never moves and never empties. What you typed is what you see.

## When it opens

- Open when the composer text is `/` followed by a name being typed:
  the caret is in the first token, the token starts at the very start
  of the text, and the token has no second `/`.
- Close as soon as that stops being true: a space after the name, a
  second `/` (`/etc/hosts`), the caret moving out of the token, or the
  text no longer starting with `/`.
- Only line one, only the first token. A `/` in the middle of prose,
  in a URL, or on a later line never opens it.
- No match: the menu closes by itself. It doesn't show an empty
  "no commands" box. The text stays and sends as a normal message.

Dismiss is sticky for the token: Esc hides the menu, and it stays
hidden while you keep editing that same token. Clearing back to an
empty `/` reopens it.

## What it lists

One list, every source, deduplicated by name with a fixed precedence:

1. Built-in actions: `/new`, `/compact`, `/model`, `/rename`, `/fork`,
   `/rewind`, `/review`, `/threads`, `/settings`, `/login`, `/quit`
2. Your commands, project then user (same shadowing as today)
3. Skills
4. MCP prompts, as `/server:prompt`

Each row: name, one-line description, and a dim kind tag on the right
(`action`, `cmd`, `skill`, `mcp`). The selected row shows its argument
hint (`argument-hint` from the command file, MCP prompt arguments)
under a divider, so you know what to type next.

Ranking: exact name, then name prefix, then fuzzy on the name, then
description hits. Prefix beats fuzzy, so `/re` lists `/review` before
`/fork-and-rename`.

## Keys while it is open

- Up/Down, ^P/^N: move the selection. They don't walk history.
- Tab: complete the selected name into the composer, add a space, and
  close the menu. The caret lands after the space, ready for arguments.
- Enter:
  - text is exactly a known name (with or without arguments): run it
  - otherwise: complete the selected name, like Tab (don't send yet)
- Esc: hide the menu. The text stays.
- Any other key edits the composer as usual, and the menu refilters.

Enter never runs a fuzzy guess. You only run what the text names.

## Enter when the menu is closed

Submit resolves the first token through one resolver:

- known built-in action: run it (`/new`, `/compact` ...); arguments
  allowed where the action takes them (`/rename my title`, `/model opus`)
- your command: expand the template with the arguments and send
- skill: load it and send the rest of the line
- MCP prompt: render it and send
- anything else: send the text unchanged (`/etc/hosts is odd` works)

An action that takes no arguments and is given some (`/new now`) shows
a short toast and keeps the text, instead of guessing.

## One resolver

A single `slash::resolve(text, Catalog)` returns
`Action | Template | Skill | McpPrompt | Plain`. The TUI submit, ACP
`session/prompt` and `agentty run` all call it. ACP's
`available_commands` and the TUI menu read the same `Catalog`, so Zed's
`/` menu and the TUI menu list the same things.

The catalog reloads when a commands or skills folder changes (the same
mtime check the loaders already do), so a new `.md` shows up without a
restart.

## ^K

^K stays the action palette. It no longer opens on `/`. Its rows show
their slash name on the right, so people learn the typed form.

## Layout

- The menu floats over the bottom of the thread, its last row right on
  top of the composer's border. It takes no row in the layout, so opening
  or closing it moves nothing: the composer, the status bar and the frame
  height stay put. It's opaque, so the thread under it doesn't show
  through its blank cells.
- Height: up to 8 rows plus the hint line. On a short terminal the hint
  goes first, then rows, down to 3.
- It reuses `maya::CompletionMenu` through `AppLayout::above_composer`,
  which floats it in a z-stack over the base. Rows, match highlighting,
  scroll hints and the argument line come from the widget.
- The hint row under the composer switches to
  `↹ complete  ↵ run  ↑↓ pick  esc hide` while the menu is open.
- A one-line `/name ...` pasted into an empty composer stays text instead
  of becoming a paste chip, so the menu and the resolver can read it.

## State

- `ComposerState::slash`: `{ index, dismissed, query }`. Whether the menu
  shows is worked out from the text and caret every update, so it can't go
  stale. Only the selection, the query it belongs to and the sticky
  dismiss are stored.
- No new panel `Kind`. Keys go to the composer as now; the composer
  key handler checks "menu visible" for Up/Down/Tab/Enter/Esc.

## Tests

Through the real key path (`on_key` → `update`), not the expander alone:

- typing `/re` keeps `/re` in the composer and shows the menu
- `/review x` Enter expands the command and sends
- `/etc/hosts` Enter sends the text unchanged
- Esc keeps the text; the menu stays hidden for that token
- `/undo` with no such command sends `/undo` as text, runs nothing
- Tab completes the name and a space
- skill and MCP prompt rows resolve the same way
- ACP and `run` give the same result as the TUI for the same text
- paste of `/review x` then Enter expands

## Out of scope

- Argument completion (files after `/review `): later, using the `@`
  picker's index.
- Showing the expanded prompt before sending.
