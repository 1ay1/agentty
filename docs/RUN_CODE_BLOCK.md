# Run Code Block (Ctrl+G)

The AI says *"run these commands"* and hands you a fenced block:

````markdown
```sh
sudo umount /dev/sdb1
sudo mkfs.vfat -F 32 -n USB /dev/sdb1
```
````

The old ritual: select the text with the mouse, hope the terminal didn't wrap
it, paste into another shell, strip the `$ ` prompts the model helpfully added,
run it, then copy the output *back* into the chat to say "it failed with X".

The new ritual: **Ctrl+G, Enter.**

You don't even have to remember the key: when a reply settles with shell
blocks in it, a toast nudges you — *▶ 2 runnable code blocks — Ctrl+G to
run*. Replies without runnable blocks stay quiet, so the nudge keeps its
meaning.

## The flow

```
Ctrl+G ──► picker (blocks from the last reply)
             │
             ├─ Enter / 1-9 ─► TUI suspends ─► block runs on the REAL terminal
             │                    · sudo password prompts work
             │                    · output streams live
             │                    · Ctrl+C kills the command, not agentty
             │                  TUI restores ─► Result card
             │                                   ├─ a   attach output to composer (chip)
             │                                   ├─ y   copy output to clipboard
             │                                   └─ Esc discard (output stays in scrollback)
             ├─ e ─► block staged into the composer for editing
             ├─ y ─► block copied to the clipboard, cleaned
             └─ Esc ─► close
```

Three beats, each one a decision point:

1. **Pick** — which block from the reply.
2. **Watch** — the run is fully interactive and live; you are at a real shell
   prompt for the duration.
3. **Decide** — after the run, choose what happens to the captured output.
   Nothing is auto-attached; the composer only ever receives output you
   explicitly asked for.

## The picker

`Ctrl+G` scans the **newest assistant reply that contains fenced blocks**
(a trailing "let me know how it goes" reply without fences doesn't mask the
one with the commands) and lists them in document order — block 1 is the
topmost on screen, so the numbers read naturally against the reply.

Each row shows the block's first line, its language tag, and its line count.
Non-shell blocks (python, js, …) are dimmed: they can be edited or copied but
not run — `sh -c` on a python script fails confusingly, so Run shows a toast
instead of executing garbage.

| Key | Action |
|-----|--------|
| `↑` `↓` | Move selection |
| `Enter` | Run the selected block |
| `1`–`9` | Run that row directly (1-based, matches the on-screen numbers) |
| `e` | Stage the cleaned block into the composer to edit first |
| `y` | Copy the cleaned block to the clipboard |
| `Esc` / `q` | Close |

The zero-navigation fast path is `Ctrl+G`, `2`, done.

Opening is gated on an idle session — mid-stream the "latest reply" is still
growing, so you get a toast asking you to wait for the turn to finish.

### What "cleaned" means

- Fences and the info string are stripped.
- When the block is a pasted-transcript shape where **every** non-empty line
  starts with `$ ` or `> `, those prompt markers are stripped too. The
  all-lines rule is deliberate: `#` starts a comment in real shell scripts,
  and mixed lines mean real script content — only the uniform transcript
  pattern is unambiguous to strip.
- Both ``` ``` ``` and `~~~` fences are recognised, up to 3 spaces of leading
  indent (the CommonMark limit — blocks nested under list items still count),
  CRLF-tolerant, and an unterminated fence at end-of-reply is still salvaged
  (the stream may have been cut mid-block).

Blocks with a bare fence (no language) count as shell — that's what models
emit for command sequences most of the time.

## The run — a real terminal, not a sandbox

This is the part that makes `sudo` work. Running the block does **not** go
through the bash tool's sandboxed, non-interactive runner. Instead:

1. maya **suspends the TUI**: raw mode off, alternate keyboard protocols off,
   mouse reporting off, the writer's non-blocking mode off. You're back on a
   cooked tty, exactly as if agentty had exited.
2. The block runs via `/bin/sh -c` with **stdin inherited from the real
   terminal** — password reads, line editing, and anything that opens
   `/dev/tty` (sudo does) behave exactly as in your shell.
3. stdout + stderr flow through a **tee pipe**: every byte is written straight
   to your screen *live* and simultaneously appended to a capture buffer.
4. A `$ command` banner is echoed first and an
   `[exit N — returning to agentty]` banner last, so the transcript reads
   like a shell session.
5. On exit the TUI restores. The run's output stays in your terminal's
   native scrollback **above** the restored UI, like shell history.

Signal semantics are classic `system()`: while the child runs, agentty
ignores `SIGINT`/`SIGQUIT` and the child gets the default dispositions —
**Ctrl+C stops the command and returns you to agentty**, it never kills
agentty itself.

The capture is capped at **2 MB**. A runaway command still streams everything
to your screen; only the buffer stops growing, and the capture ends with
`[capture truncated at 2 MB — full output was shown on screen]`.

The exit code is the child's `WEXITSTATUS`, or `128 + signal` if it died to a
signal (the shell convention — Ctrl+C shows as `exit 130`).

> **Windows** runs blocks natively through the right interpreter: a
> ```` ```powershell ````/`pwsh`/`ps1` block goes to `powershell` (via
> `-EncodedCommand`, so quoting and multi-line scripts survive intact), a
> `cmd`/`bat`/`batch` block and bare fences go to `cmd.exe`. There's no
> fork/tty, so it runs through the same captured (non-interactive) runner
> the bash tool uses — output is captured and shown in the Result card
> rather than streamed live. sudo isn't a Windows concept anyway.
>
> Which interpreter runs which language is decided by one classifier,
> `shell_for_language()` — shared by the Run gate, the runnable-block
> nudge, and the runner, so a block that shows as runnable always is. A
> language the platform has no interpreter for (e.g. `powershell` on
> Linux) offers edit/copy instead.

## The Result card

When the run exits, agentty shows a summary card — green accent on exit 0,
red on failure or timeout:

```
┌ Run Result ───────────────────────────────┐
│ $ sudo mkfs.vfat -F 32 -n USB /dev/sdb1   │
│   exit 0 · 14 lines · 1 KB                │
│ ──────────────────────────────────────────│
│   mkfs.fat 4.2 (2021-01-31)               │
│   …full capture, scrollable…              │
│                                           │
│  a attach to composer   y copy   Esc discard │
└───────────────────────────────────────────┘
```

| Key | Action |
|-----|--------|
| `a` | Attach the capture to the composer as an **Output chip** |
| `y` | Copy the capture to the clipboard |
| `↑` `↓` / `PgUp` `PgDn` | Scroll the capture |
| `Esc` / `Enter` / `q` / `d` | Discard |

`Enter` deliberately **discards** — the default action is the safe one.
Attaching is always the explicit `a`. Discarding loses nothing you saw: the
full output is still in native scrollback above the TUI.

## The Output chip

Attaching uses the **same machinery as a big paste**: the capture becomes an
`Attachment::Kind::Output` and the composer shows one collapsed pill —

```
[ Output: sudo mkfs.vfat -F 32 … · 1240 lines · 48 KB ]
```

— however huge the log is. You can type your annotation around it ("this
failed at step 2, what now?") and the full body only materialises on the wire
when you actually submit, expanded as:

````markdown
I ran:
```sh
sudo mkfs.vfat -F 32 -n USB /dev/sdb1
```
output:
```
mkfs.fat 4.2 (2021-01-31)
…
```
````

So the model sees exactly what happened, in context, without you re-typing a
thing — and without a 48 KB wall of text ever cluttering your composer.

## Also reachable via

- **Command palette** (`Ctrl+K`) → *Run code block*.

## Implementation map

| Piece | Where |
|-------|-------|
| Extraction + picker state (pure) | `include/agentty/runtime/code_block_picker.hpp` |
| Reducer + tty runner | `src/runtime/app/update/codeblock.cpp` |
| Key dispatch | `src/runtime/app/subscribe.cpp` (`on_code_block_picker`, `on_code_block_result`) |
| Picker + Result card views | `src/runtime/view/pickers.cpp` |
| Output chip kind + wire expansion | `include/agentty/runtime/composer_attachment.hpp`, `src/runtime/composer_attachment.cpp` |
| TUI suspend primitive | maya: `Runtime::suspend` (`app.cpp`), `Terminal::suspend` (`terminal.hpp`), `Cmd::suspend` (`cmd.hpp`) |

The state machine is the usual agentty sum type:
`Closed | Open{blocks, index} | Result{command, output, exit_code, timed_out}`.
The run itself is a `Cmd::suspend` — maya executes it synchronously on the UI
thread between a full TUI teardown and restore, then re-anchors the inline
frame below the child's output so the transcript scrolls away naturally.
