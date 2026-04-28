# Conversation rendering pipeline

How a `Thread` becomes terminal cells, function by function. Read this alongside [`UI.md`](UI.md), which catalogs the underlying maya primitives.

This describes the pipeline at commit `7ef8062`. Everything documented here lives in [`src/runtime/view/thread.cpp`](../src/runtime/view/thread.cpp) unless otherwise noted.

---

## 0. What the thread panel actually looks like

The thread panel is the upper region of moha's screen — everything above the composer + status bar. It's a vertical stack of turns, each one a "speaker rail + content block." Annotated layout, top to bottom:

```
┌──────────────────────────────────── terminal viewport ─────────────────────────────────────┐
│                                                                                            │
│ ┃ ❯ You                                                          12:34  ·  turn 1          │ ← user turn header
│ ┃                                                                                          │   (bold rail in cyan)
│ ┃ refactor the login flow to use the new auth provider                                     │
│                                                                                            │
│ ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ─── ── │ ← inter_turn_divider
│                                                                                            │
│ ┃ ✦ Opus 4.7                                          12:34  ·  4.2s  ·  turn 1            │ ← assistant turn header
│ ┃                                                                                          │   (bold rail in accent
│ ┃ I'll start by exploring the current auth structure.                                      │   color, ✦ glyph)
│ ┃                                                                                          │
│ ┃ ╭─ ACTIONS  ·  3/3  ·  1.8s ──────────────────────────────────────────────╮              │ ← Round-bordered
│ ┃ │ I N S P E C T  2  ·  M U T A T E  1                                     │              │   Actions panel
│ ┃ │                                                                         │              │   (assistant_timeline)
│ ┃ │ ╭─ ✓ Read         src/auth/login.ts  ·  87 lines              42ms      │              │
│ ┃ │ │  import { Session } from './session';                                  │              │
│ ┃ │ │  ··· 80 hidden ···                                                    │              │
│ ┃ │ │  export default login;                                                │              │
│ ┃ │ │                                                                       │              │
│ ┃ │ ├─ ✓ Grep         provider  in  src/auth  ·  12 matches       190ms     │              │
│ ┃ │ │  src/auth/login.ts:14:  const provider = …                            │              │
│ ┃ │ │  ··· 9 hidden ···                                                     │              │
│ ┃ │ │  src/auth/session.ts:88:  return provider.refresh()                   │              │
│ ┃ │ │                                                                       │              │
│ ┃ │ ╰─ ✓ Edit         src/auth/login.ts  ·  2 edits  ·  (+5 -2)   1.6s      │              │
│ ┃ │    edit 1/2  ·  −1 / +3                                                 │              │
│ ┃ │    - const provider = legacyAuth();                                     │              │
│ ┃ │    + const provider = await NewAuth.create({                            │              │
│ ┃ │    +   tenant: env.AUTH_TENANT,                                         │              │
│ ┃ │    + });                                                                 │              │
│ ┃ │                                                                          │              │
│ ┃ │ ✓ DONE   3 actions   1.8s                                                │              │
│ ┃ ╰────────────────────────────────────────────────────────────────────────╯              │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
                                                                              ↓ composer + status_bar below
```

Pieces, by function:

| Glyph / region | Element | Function |
|---|---|---|
| `┃` left bar (full turn height) | `with_turn_rail` | Speaker brand color: cyan for user, model brand (Opus = magenta, Sonnet = blue, Haiku = green) for assistant. |
| `❯` / `✦` | `turn_header` glyph | User uses `❯`, assistant uses `✦`. |
| `Opus 4.7`, `You` | `turn_header` label | Speaker label, bold + colored. |
| `12:34 · 4.2s · turn 1` | `turn_header` meta | Timestamp · elapsed · absolute turn number. Right-pinned. |
| `─── ─── …` rule | `inter_turn_divider` | Thin dim rule between consecutive turns. Width-aware ComponentElement. |
| Markdown body | `cached_markdown_for` | Either finalized `maya::markdown(text)` or live `maya::StreamingMarkdown`. |
| `╭ ╮ ╰ ╯ ─ │` outer card | `assistant_timeline` outer chrome | Round border, title in `btext`, color tracks turn state (rail color while in-flight, dim grey when settled). |
| `ACTIONS · 3/3 · 1.8s` | `assistant_timeline` title | Small-caps "ACTIONS", done/total, then either active tool name (in flight) or total duration (settled). |
| `I N S P E C T  2  · …` | stats row | Small-caps category badges with counts. Only when total > 1 events. |
| `╭─ ├─ ╰─ ──` | tree glyph per event | First / middle / last / singleton. Drawn in the per-event category color. |
| `⠋ ⠙ ⠹ … / ✓ ✗ ⊘ / ○` | `rich_status_icon` | Braille spinner for active, `✓ ✗ ⊘` for terminal, `○` for pending-no-icon (unused at runtime). |
| `Bash`, `Read`, `Grep`, `Edit` | `tool_display_name` | TitleCase per tool. Color = category (inspect=blue, mutate=magenta, execute=green, plan=yellow, vcs=cyan). Bold while active, dim when settled, danger/warn for failed/rejected. |
| `npm test  ·  exit 0` | `tool_timeline_detail` | Per-tool one-line summary; settled tools fold in stats (`· N lines`, `(+X -Y)`, `· exit N`, etc.). Italic + muted. |
| `42ms / 1.6s / 1m20s` | `format_duration` | Right-pinned, color-coded: green <250ms, dim <2s, warn <15s, danger above. |
| `│` body stripe (under each event) | body_rule | Light `│` indented 3 cols. Color = event_connector_color (status-driven), brighter while active. |
| Body content rows | `compact_tool_body` | Per-tool preview: head+tail elision for read/bash/etc., per-side diff coloring for edit/git_diff, ✓/◍/○ glyph list for todo. |
| Short `│` between events | inter-event connector | One-line connector colored by the *next* event's status. |
| `✓ DONE   3 actions   1.8s` | footer | Only when all events are terminal. Verb glyph + small-caps verb in success/danger/warn (depending on outcome) + count + total elapsed. |

### A turn that's still in flight

```
┃ ✦ Opus 4.7                                          12:34  ·  …  ·  turn 2

┃ Investigating the auth layer. I'll start by reading the relev

┃ ╭─ ACTIONS  ·  1/2  ·  Bash ──────────────────────────────────────────────╮
┃ │ ╭─ ✓ Read         src/auth/login.ts  ·  87 lines              42ms     │
┃ │ │  …                                                                    │
┃ │ │                                                                       │
┃ │ ╰─ ⠋ Bash         npm test                                              │  ← spinner ticks (cyan)
┃ │    PASS test/login.test.ts                                              │
┃ │    PASS test/session.test.ts                                            │  ← live progress_text
┃ │    Test Suites: 2 passed, 5 total                                       │     under │ stripe
┃ ╰────────────────────────────────────────────────────────────────────────╯  ← border still rail-color
                                                                              (no footer until settled)
```

Differences from the settled view:

- The streaming markdown body grows by line each text-delta — `StreamingMarkdown::set_content(streaming_text)` is called every frame.
- The active event's tree glyph and name render bold-bright; settled events above stay dim.
- The status icon is a braille spinner rotating with `m.s.spinner.frame_index()`.
- The detail line shows live state: `running…` / `queued…` / `approved…` if no per-tool detail is available yet.
- `format_duration` is omitted on the right for non-terminal events.
- The Actions panel border uses the speaker rail color (not muted) until every event settles.
- The footer is suppressed until `done == total`.
- `tool_card_cache` is populated only for terminal-state tool cards — but at this commit nothing calls `render_tool_call`, so the cache is dormant; the timeline renders live each frame from `compact_tool_body`.

### Empty thread

When `m.d.current.messages` is empty, `thread_panel` short-circuits to the welcome screen (the `m o h a` wordmark + tagline + version + model/profile chips + key hints, all centered). See [`thread.cpp:1351+`](../src/runtime/view/thread.cpp#L1351).

### Below the thread panel

Outside the thread panel itself, the screen continues with:

```
…
┃ ✦ Opus 4.7 …
┃ ╰────────────╯  (Actions panel)

╭─ ⠋ — type to queue… ──────────────────────────────────╮       ← composer (Round border)
│ ❯ ▎                                                    │           border + prompt color
│                                                        │           switches with phase
│                                                        │
│ ↵ send  ·  ⇧↵ / ⌥↵ newline  ·  ^E expand    ▎ Write   │       ← hint row (responsive)
╰────────────────────────────────────────────────────────╯
▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔  ← phase accent strip (top)
 ▌ ⠋ Streaming     · 0:08  ·  ✦ Opus 4.7  ·  CTX  ███▆░░░░░░  18%   ← activity row
                                                                       (left: phase chip
 …status banner if any…                                                 right: model + ctx)
 ^K palette  ·  ^J threads  ·  ^T todo  ·  ^N new  ·  ^C quit       ← shortcut row
▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  ← phase accent strip (bottom)
```

The `composer`, `changes_strip`, and `status_bar` are siblings of `thread_panel` inside `view.cpp`'s top-level `v(...)` stack. They're rendered by `composer.cpp`, `changes.cpp`, and `statusbar.cpp` respectively — covered in [`UI.md`](UI.md).

---

## 1. Entry chain at a glance

```
view(m)                                       [view.cpp:17]
  └─ thread_panel(m)                          [thread.cpp:1289]
       ├─ for each visible message i …
       │    ├─ inter_turn_divider()           [thread.cpp:162]   (between turns)
       │    └─ render_message(msg, i, …)      [thread.cpp:1201]
       │         ├─ if User:
       │         │     turn_header / blank / user_message_body
       │         └─ if Assistant:
       │               turn_header
       │               cached_markdown_for(msg, …)              ← if has body text
       │               assistant_timeline(msg, frame, color)    ← if any tool_calls
       │               render_inline_permission(...)            ← if a tool needs approval
       │               error banner                             ← if msg.error
       └─ trailing in-flight indicator (optional)
```

Once `render_message` returns its element, `thread_panel` wraps the whole turn in [`with_turn_rail`](../src/runtime/view/thread.cpp#L147) (the left vertical bar in the speaker's color) and pushes it onto the `rows` stack.

---

## 2. `thread_panel` — message loop and virtualization

[`thread.cpp:1289-1340`](../src/runtime/view/thread.cpp#L1289).

Three responsibilities:

1. **Pick the visible window.** `m.ui.thread_view_start` is the index of the oldest message we're rendering. Anything older has already been committed to the terminal's native scrollback via `Cmd::commit_scrollback`. The turn counter is computed from the start of the thread (not the window) so "Turn 42" doesn't reset when older turns scroll off.
2. **Walk the visible slice.** For each message:
   - Push an `inter_turn_divider()` (a thin dim `─` rule) **between** turns.
   - Push `render_message(msg, idx, turn, m)`.
   - Bump the turn counter on assistant messages.
3. **Bottom in-flight indicator.** When `m.s.active()` and the last message is an in-progress assistant turn *that doesn't already have a Timeline showing* (i.e. it's still streaming text but hasn't kicked off any tool yet), append a small `▎ <spinner> <verb>…` row. The check at [`thread.cpp:1318-1322`](../src/runtime/view/thread.cpp#L1318) suppresses it when the Timeline is up — the timeline's own spinner already says "still working."

The empty-thread case ([`thread.cpp:1351+`](../src/runtime/view/thread.cpp#L1351)) renders the `m o h a` wordmark + chips + key hints.

Final wrap: `v(rows) | padding(0, 1) | grow(1.0f)`.

---

## 3. `render_message` — per-turn assembly

[`thread.cpp:1201-1287`](../src/runtime/view/thread.cpp#L1201).

### Common scaffolding

For every message, `render_message`:

1. Computes a `rail_color` (the speaker's brand color via `speaker_style_for`).
2. Builds a `body` vector of elements.
3. Wraps `v(body)` in `with_turn_rail(rail_color)` — the left-only `Bold` border that runs the full height of the turn.
4. Appends a trailing blank line for breathing room before the next turn.

### User branch

```cpp
if (msg.checkpoint_id) rows.push_back(render_checkpoint_divider());
body.push_back(turn_header(Role::User, turn_num, msg, m, std::nullopt));
body.push_back(text(""));                       // blank line
body.push_back(user_message_body(msg.text));    // plain text, fg color
```

`render_checkpoint_divider` ([`permission.cpp:43-53`](../src/runtime/view/permission.cpp#L43)) is `─── [↺ Restore checkpoint] ───`, only rendered when the message carries a checkpoint id. `user_message_body` is a one-liner — `text(body, fg_of(fg))` — no markdown for user input.

### Assistant branch

The interesting case. In order:

1. **Elapsed time computation.** Walk back to the most recent user message and diff timestamps. Used by `turn_header` to render the right-side `· 12.4s` chip.
2. **Header row.**
   ```cpp
   body.push_back(turn_header(Role::Assistant, turn_num, msg, m, assistant_elapsed));
   body.push_back(text(""));
   ```
   `turn_header` ([`thread.cpp:102-138`](../src/runtime/view/thread.cpp#L102)) is `<glyph> <speaker_label>  ___spacer___  <hh:mm · 12.4s · turn N>`.
3. **Body text.** When there is any text content (finalized OR streaming), pull in markdown:
   ```cpp
   bool has_body = !msg.text.empty() || !msg.streaming_text.empty();
   if (has_body) {
       body.push_back(cached_markdown_for(msg, m.d.current.id, msg_idx));
       if (!msg.tool_calls.empty()) body.push_back(text(""));
   }
   ```
4. **Actions panel.** When the message has any tool calls (regardless of count or status), render the timeline:
   ```cpp
   if (!msg.tool_calls.empty()) {
       int frame = m.s.spinner.frame_index();
       body.push_back(assistant_timeline(msg, frame, rail_color));
       // Inline permission prompt for any tool currently awaiting approval
       for (const auto& tc : msg.tool_calls) {
           if (m.d.pending_permission && m.d.pending_permission->id == tc.id) {
               body.push_back(text(""));
               body.push_back(render_inline_permission(*m.d.pending_permission, tc));
           }
       }
   }
   ```
   `render_inline_permission` ([`permission.cpp:15`](../src/runtime/view/permission.cpp#L15)) builds a `maya::Permission` widget with the tool's args summary as the description.
5. **Error banner.** If `msg.error` is set (stream-level failure preserved alongside the partial body), append a single dim italic row prefixed with `⚠`. Kept distinct from the body text so the partial response and the failure reason render separately.

---

## 4. `cached_markdown_for` — markdown body caching

[`thread.cpp:34-48`](../src/runtime/view/thread.cpp#L34).

Two paths, picked by whether the message is finalized:

| State | Path | Cache layer |
|---|---|---|
| `msg.text.empty()` (still streaming) | Reuse a `maya::StreamingMarkdown`; call `set_content(msg.streaming_text)` each frame; `build()`. | `MessageMdCache::streaming` (`shared_ptr<StreamingMarkdown>`). |
| `msg.text` non-empty (finalized) | Build `maya::markdown(msg.text)` once, cache as `shared_ptr<Element>`, return the same pointer forever. | `MessageMdCache::finalized` (`shared_ptr<Element>`). |

`StreamingMarkdown`'s internal block-boundary cache makes each delta `O(new_chars)` rather than re-parsing the whole transcript. The cache lookup is keyed on `(thread_id, msg_idx)` ([`cache.hpp:29-30`](../include/moha/runtime/view/cache.hpp#L29)). Streaming → finalized switchover happens automatically: once `finalize_turn` moves `streaming_text` into `text`, the next render takes the finalized branch and drops the streaming object.

---

## 5. `assistant_timeline` — anatomy of the Actions panel

[`thread.cpp:930-1199`](../src/runtime/view/thread.cpp#L930). The single biggest rendering function in the codebase.

The output is one bordered card. From top to bottom:

```
╭─ ACTIONS · 3/5 · Bash ─────────────────────────────────────────╮
│                                                                 │
│  INSPECT 2  ·  EXECUTE 1  ·  MUTATE 2                           │  ← stats header (total>1)
│                                                                 │
│  ╭─ ⠋ Bash      npm test            1.2s                        │
│  │   PASS test/foo.test.ts                                      │  ← compact_tool_body
│  │   ✓ all 5 tests passed                                       │
│  │                                                              │
│  │                                                              │  ← inter-event connector
│  ├─ ✓ Read      src/foo.ts  · 42 lines           38ms           │
│  │   import { bar } from './bar';                               │
│  │   ··· 30 hidden ···                                          │
│  │   export default foo;                                        │
│  ╰─ ✓ Edit      src/foo.ts  · (+3 -1)            210ms          │
│                                                                 │
│  ✓ DONE   3 actions   1.4s                                      │  ← footer (all settled)
│                                                                 │
╰────────────────────────────────────────────────────────────────╯
```

### 5a. Pre-pass: counts and flags

Walk `msg.tool_calls` once to compute:
- `total`, `done`, `running_idx`, `total_elapsed`
- `cat_counts` — vector<(category, count)> in stable insertion order

Categories come from `tool_category_label(name)`:
- `mutate` for `edit` / `write`
- `execute` for `bash`
- `plan` for `todo`
- `vcs` for anything starting with `git_`
- `inspect` for everything else (read, grep, glob, list_dir, find_definition, diagnostics, web_*)

### 5b. Stats header — only when `total > 1`

Skipped on single-tool turns (would just be `INSPECT 1` — pointless).

When rendered, it's small-caps category badges separated by mid-dots, each colored to match its category (`accent` / `success` / `warn` / `highlight` / `info`). Plus a trailing blank row.

### 5c. Per-event header row

Built per-tool. The layout:

```
╭─ ⠋ Bash      npm test                                  1.2s
^^ ^ ^^^^^^^^^ ^^^^^^^^^                                 ^^^^
│  │ │         │                                         │
│  │ │         tool_timeline_detail(tc)                  duration
│  │ tool_display_name (TitleCase)                       (only when terminal)
│  rich_status_icon(tc, frame)
tree_glyph(idx)
```

| Piece | Source | Notes |
|---|---|---|
| Tree glyph | [`thread.cpp:993-999`](../src/runtime/view/thread.cpp#L993) | `──` for singletons, `╭─` first, `├─` middle, `╰─` last. Drawn in the per-tool category color. |
| Status icon | `rich_status_icon(tc, spinner_frame)` ([`thread.cpp:504`](../src/runtime/view/thread.cpp#L504)) | Braille spinner (10-frame) for Running/Approved (cyan, bold) and Pending (yellow, bold). Static glyphs for terminal: `✓` green / `✗` red / `⊘` yellow. |
| Tool name | `tool_display_name(name)` ([`thread.cpp:462`](../src/runtime/view/thread.cpp#L462)) | Lowercase moha names → display names: `read`→`Read`, `find_definition`→`Definition`, `git_status`→`Git Status`, etc. |
| Name color | Category color ([`tool_category_color`](../src/runtime/view/thread.cpp#L906)). Bold for active, dim for settled. Failed/rejected override to danger/warn. |
| Detail | `tool_timeline_detail(tc)` ([`thread.cpp:196-449`](../src/runtime/view/thread.cpp#L196)) | Per-tool one-line summary (see 5d below). |
| Detail style | Italic + muted; dim when settled. |
| Elapsed | `format_duration(secs)` ([`thread.cpp:484`](../src/runtime/view/thread.cpp#L484)), color-coded by `duration_color(secs)`: green <250 ms, dim <2 s, warn <15 s, danger above. Only emitted when terminal. |

The whole row is `h(text(tree), text(" "), icon, text("  "), text(name), text("  "), text(detail))` followed by `spacer()` + (optional elapsed). Outer wrap: `| grow(1.0f)` so the row fills the panel width.

### 5d. `tool_timeline_detail` — the per-tool summary

[`thread.cpp:196-449`](../src/runtime/view/thread.cpp#L196). One big switch over `tc.name.value`. Each branch returns a single-line string with two halves: a "what" prefix (path / command / pattern / url / message) and, when the tool is settled, a post-completion stats suffix.

| Tool | "what" | Settled stats appended |
|---|---|---|
| `read` | path (cwd-relative or `~/`-relative) `+ @<offset>` if set | `· N lines` |
| `write` | path | `(+X -Y)` extracted from output |
| `edit` | path | `· N edits` (from args) and `(+X -Y)` (from output) |
| `bash` / `diagnostics` | first line of `command` | `· exit N` if non-zero |
| `grep` | `pattern  in  path` | `· N matches` |
| `glob` | pattern | `· N hits` |
| `list_dir` | path or `.` | `· N entries` |
| `find_definition` | symbol | `· N files` |
| `web_fetch` | url | `· <HTTP status>` |
| `web_search` | query | `· N results` |
| `git_commit` | first line of message | `· <short hash>` |
| `git_status` | branch (parsed from porcelain) | `· M S? U?` or `· clean` |
| `git_diff` / `git_log` | path or `.` | — |
| `todo` | — | `done/total` `· N in progress` |
| anything else | `display_description` arg | — |

All of this is plain string assembly. The only maya call is implicit: the result becomes a `text(...)` node back at the header row.

### 5e. Per-event body — `compact_tool_body`

[`thread.cpp:604-870`](../src/runtime/view/thread.cpp#L604). Tool-specific switch returning a stack of lines that go *under* the event header, indented behind a dim `│` stripe.

The shared helper at the top of the function is `preview_block`:

```cpp
auto preview_block = [&](const std::string& body, Style line_style) -> Element {
    constexpr int kHead = 4;
    constexpr int kTail = 3;
    auto p = head_tail_lines(body, kHead, kTail);
    std::vector<Element> rows;
    for (int i = 0; i < (int)p.lines.size(); ++i) {
        if (p.elided > 0 && i == kHead)
            rows.push_back(text("·  ·  ·  N hidden  ·  ·  ·", fg_dim(muted)));
        rows.push_back(text(p.lines[i], line_style));
    }
    return v(std::move(rows)).build();
};
```

Smart head+tail (4 + 3) elision with a dim middle marker, like `git diff` smart context.

Per-tool dispatch:

| Tool | Body |
|---|---|
| `edit` | Custom diff renderer ([`thread.cpp:618-717`](../src/runtime/view/thread.cpp#L618)): per-hunk header `edit i/N · −k / +m`, then `- old` lines (red) and `+ new` lines (green), each side capped at 6 head + 2 tail with elision. Up to 4 hunks shown; surplus collapses to `… N more edits`. |
| `write` | `preview_block(content, fg_dim(fg))` of streaming/written content. |
| `bash` / `diagnostics` (terminal) | `preview_block(strip_bash_output_fence(output), fg_dim(fg))`. |
| `bash` (running) | `preview_block(progress_text, fg_dim(fg))` — live stdout snapshot. |
| `read`, `list_dir`, `grep`, `glob`, `find_definition`, `web_fetch`, `web_search`, `git_status`, `git_log`, `git_commit` (all terminal) | `preview_block(output, fg_dim(fg))`. |
| `git_diff` (terminal) | Per-line styled head+tail: `+`-lines green, `-`-lines red, `@@`/`diff`/`+++`/`---` dim, context plain. Same elision shape as preview_block. |
| `todo` | Up to 8 items, each `<glyph> <body>` where glyph is `✓`/`◍`/`○` for completed/in_progress/pending. Surplus → `… N more`. |
| any tool, when failed with non-empty output | `preview_block(output, fg_of(danger))`. |
| anything else / non-settled | `text("")` (empty). |

Critically: **none of the maya per-tool widgets** (`ReadTool`, `WriteTool`, `EditTool`, `BashTool`, `SearchResult`, `FetchTool`, `GitStatusWidget`, `GitGraph`, `TodoListTool`, `ToolCall`) **are touched here.** The body is built from `dsl::text` / `dsl::v` / `dsl::h` and `Style`. See section 9 for the dead-code path that *would* call those widgets.

### 5f. Body cascade — flattening into rows under a `│` stripe

After `compact_tool_body` returns, the assistant_timeline does this ([`thread.cpp:1077-1112`](../src/runtime/view/thread.cpp#L1077)):

```cpp
auto body_rule = h(
    text("   ", {}),                                // tree+space alignment (3 cols)
    text("│  ", stripe_style)                       // dim │ stripe in connector color
).build();

Element body_el = compact_tool_body(tc);
if (auto* bx = maya::as_box(body_el)) {
    for (const auto& child : bx->children)
        rows.push_back((h(body_rule, child) | grow(1.0f)).build());
} else if (auto* t = maya::as_text(body_el)) {
    if (!t->content.empty())
        rows.push_back((h(body_rule, body_el) | grow(1.0f)).build());
}
```

The downcasts (`as_box`, `as_text`) flatten the body into individual rows so each line of `preview_block` (or each diff line) gets its own `│` stripe — instead of the stripe sitting beside the body as one tall element. Each row is a sibling, exactly 1 cell tall, which is what maya's inline overwrite mode prefers.

Stripe color comes from `event_connector_color(tc)`: danger if failed, warn if rejected, info if running/approved, dim grey otherwise. Active events get the stripe brighter; settled stay dim.

### 5g. Inter-event connector

A single short row between adjacent events ([`thread.cpp:1120-1126`](../src/runtime/view/thread.cpp#L1120)):

```cpp
if (!is_last) {
    rows.push_back(h(
        text("   ", {}),
        text("│", Style{}.with_fg(next_cc).with_dim())
    ).build());
}
```

Color is derived from the *next* event's status, not this one's — so the lane visually flows into the upcoming event.

### 5h. Footer — only when `done == total > 0`

[`thread.cpp:1135-1169`](../src/runtime/view/thread.cpp#L1135). One row:

```
✓ DONE   3 actions   1.4s
```

If anything failed, the verb flips to `N FAILED` (red, ✗). If anything was rejected, `N REJECTED` (warn, ⊘). The verb is small-caps; the count and elapsed are dim.

### 5i. Outer chrome

[`thread.cpp:1175-1198`](../src/runtime/view/thread.cpp#L1175):

```cpp
std::string title = " ACTIONS  ·  3/5";
if (running_idx >= 0)         title += "  ·  " + tool_display_name(...);
else if (done == total > 0)   title += "  ·  " + format_duration(total_elapsed);

return (v(rows)
    | border(BorderStyle::Round)
    | bcolor(all_done ? muted : rail_color)
    | btext(title, BorderTextPos::Top, BorderTextAlign::Start)
    | padding(0, 1, 0, 1)
).build();
```

Border color is the speaker's rail color while in flight; mutes to grey once everything settled (the panel recedes when done).

---

## 6. Rendering by tool count

Same code path always runs as long as `tool_calls` is non-empty. Two cosmetic carve-outs:

| Count | Stats header | Tree glyphs |
|---|---|---|
| 0 tools | (Timeline not rendered) | — |
| 1 tool | Skipped (the row that would say `INSPECT 1`) | `──` (singleton) |
| 2+ tools | Rendered | `╭─` first / `├─` middle / `╰─` last |

Everything else — bordered chrome, header row, body cascade, footer — renders identically.

---

## 7. Caching layers

[`include/moha/runtime/view/cache.hpp`](../include/moha/runtime/view/cache.hpp), implementations in [`src/runtime/view/cache.cpp`](../src/runtime/view/cache.cpp). All caches are `thread_local` — UI runs single-threaded.

| Cache | Key | Holds | Invalidation |
|---|---|---|---|
| `tool_card_cache(ToolCallId)` | tool call id | `shared_ptr<Element>` + `uint64_t key` | `compute_render_key()` (FNV over `output().size()`, `status.index()`, `expanded`). Only populated for terminal-state tools. |
| `message_md_cache(ThreadId, msg_idx)` | thread+index | `shared_ptr<Element>` (finalized) and `shared_ptr<StreamingMarkdown>` (streaming) | None — entries persist for the thread's lifetime; streaming object reused across deltas, finalized object built once. |

The tool_card_cache is **only used by `render_tool_call` in `tool_card.cpp`**, which itself isn't called from anywhere at this commit (see section 9). So in practice the active rendering pipeline only exercises `message_md_cache`.

The whole-turn `Element` is **not** cached at this commit — `render_message` rebuilds the turn shell every frame. Later commits added a `TurnElementCache` for inline-mode scrollback stability.

---

## 8. Outer chrome around every turn

Two functions wrap the per-turn output:

### `with_turn_rail(content, color)` — [thread.cpp:147](../src/runtime/view/thread.cpp#L147)

```cpp
maya::detail::box()
    .direction(FlexDirection::Row)
    .border(BorderStyle::Bold, color)
    .border_sides({.top=false, .right=false, .bottom=false, .left=true})
    .padding(0, 0, 0, 2)
    .grow(1.0f)
  (std::move(content));
```

The bold left bar in the speaker's color, running the entire height of the turn. Padding shifts the content right so it doesn't crash into the rail. Cyan `❯` for user, brand-colored `✦` for assistant (Opus magenta, Sonnet blue, Haiku green, default cyan).

### `inter_turn_divider()` — [thread.cpp:162](../src/runtime/view/thread.cpp#L162)

A `ComponentElement` whose render closure receives the available width and produces a thin dim `─` rule indented by 3 cells. Rendered between consecutive turns by `thread_panel`.

### `turn_header(role, turn, msg, m, elapsed)` — [thread.cpp:102](../src/runtime/view/thread.cpp#L102)

`<glyph> <speaker> ___spacer___ <hh:mm · 12.4s · turn N>`. The trailing meta tail uses generous `· · ·` mid-dot separators. The whole row is `| grow(1.0f)` so the right edge stays pinned.

---

## 9. The dead path: `render_tool_call`

[`tool_card.cpp:601`](../src/runtime/view/tool_card.cpp#L601) defines:

```cpp
Element render_tool_call(const ToolUse& tc) {
    if (!tc.is_terminal())
        return render_tool_call_uncached(tc);
    auto& slot = tool_card_cache(tc.id);
    auto key = tc.compute_render_key();
    if (slot.element && slot.key == key) return *slot.element;
    auto built = render_tool_call_uncached(tc);
    slot.element = std::make_shared<Element>(built);
    slot.key     = key;
    return built;
}
```

`render_tool_call_uncached` ([`tool_card.cpp:175-592`](../src/runtime/view/tool_card.cpp#L175)) is a 400-line dispatch over `tc.name`, building rich maya widgets per tool:

- `read`, `list_dir` → `maya::ReadTool`
- `write` → `maya::WriteTool`
- `edit` → `maya::EditTool` (with `set_edits()` for the canonical multi-hunk shape)
- `bash`, `diagnostics` → `maya::BashTool`
- `grep`, `find_definition` → `maya::SearchResult` (with markdown-format parsing)
- `glob` → `maya::SearchResult`
- `web_fetch`, `web_search` → `maya::FetchTool`
- `git_status` → `maya::GitStatusWidget` (parsed from porcelain v2)
- `git_log` → `maya::GitGraph` + `maya::GitCommit` per row
- `git_diff` → `maya::DiffView`
- `git_commit` → `maya::GitCommitTool`
- `todo` → `maya::TodoListTool`
- fallback → `maya::ToolCall` chrome with raw text content

**`render_tool_call` is declared in `thread.hpp` and defined in `tool_card.cpp`, but no call site invokes it at this commit.** The conversation pipeline goes `render_message` → `assistant_timeline` → `compact_tool_body`, never crossing into `render_tool_call`. The big widget catalog is dormant.

That means `tool_card_cache`, `compute_render_key`, and the entire 614-line `tool_card.cpp` are compiled but unused. They're either remnants of an older design where each tool got its own bordered card, or scaffolding for a future surface (a sidebar / palette / detailed-view modal) that would call them. Either way, the inline timeline does not touch them.

---

## 10. Live maya widgets in the conversation surface

To be precise about what *is* maya-widget-driven during conversation rendering:

| Widget | Where | Role |
|---|---|---|
| `maya::markdown(text)` | `cached_markdown_for` finalized branch | Assistant message body once finalized. |
| `maya::StreamingMarkdown` | `cached_markdown_for` streaming branch | Live markdown body during streaming. |
| `maya::Permission` | `render_inline_permission` | Inline tool-permission card under the timeline when a tool needs approval. |

That's the full list for the conversation pipeline. Everything else in the timeline (per-tool cards, headers, dividers, footer, body previews) is built directly from `dsl::text` / `dsl::h` / `dsl::v` + the runtime pipes documented in [`UI.md`](UI.md).

The status bar and other chrome use additional widgets (`ModelBadge`, `compact_token_stream`, `FileChanges`, `DiffView`, `PlanView`, `Spinner`) — covered in [`UI.md`](UI.md) section 7.

---

## 11. Summary diagram — single assistant turn with two tools

```
thread_panel
  inter_turn_divider                                       ← dim ─── rule
  render_message                                           ← assistant turn
    with_turn_rail (Bold left border, brand color)
      v(
        turn_header                                        ← ✦ Opus  · · · 12:34 · 4.2s · turn 12
        ""                                                 ← blank
        cached_markdown_for (StreamingMarkdown.build())   ← growing markdown body
        ""                                                 ← blank (because tools follow)
        assistant_timeline (Round border, ACTIONS title)
          ├─ INSPECT 1 · MUTATE 1                          ← stats header (total=2)
          ├─ ""
          ├─ ╭─ ⠋ Read    src/foo.ts                      ← header row
          ├─ │  import…                                    ← body line
          ├─ │  ··· 30 hidden ···                          ← body elision
          ├─ │  export default foo;                        ← body line
          ├─ │                                              ← inter-event connector (next color)
          ├─ ╰─ ⠋ Edit    src/foo.ts · 2 edits             ← header row
          ├─ │  edit 1/2  ·  −1 / +3                       ← per-hunk tag
          ├─ │  - old line                                 ← red
          ├─ │  + new line a                               ← green
          ├─ │  + new line b
          ├─ │  + new line c
          └─ (no footer — still in flight)
        ""
        render_inline_permission (Permission widget)       ← only if a tool awaits approval
      )
  ""                                                       ← bottom breathing
```

---

## File index

| File | Function | What it does |
|---|---|---|
| [`thread.cpp:1289`](../src/runtime/view/thread.cpp#L1289) | `thread_panel` | Top-level: virtualization + message loop + bottom indicator |
| [`thread.cpp:1201`](../src/runtime/view/thread.cpp#L1201) | `render_message` | Per-turn dispatch (User vs Assistant) |
| [`thread.cpp:34`](../src/runtime/view/thread.cpp#L34) | `cached_markdown_for` | Markdown caching for assistant body |
| [`thread.cpp:102`](../src/runtime/view/thread.cpp#L102) | `turn_header` | Speaker badge + timestamp + elapsed + turn N |
| [`thread.cpp:147`](../src/runtime/view/thread.cpp#L147) | `with_turn_rail` | Left brand-color rail wrapping a turn |
| [`thread.cpp:162`](../src/runtime/view/thread.cpp#L162) | `inter_turn_divider` | Width-aware dim `─` rule |
| [`thread.cpp:184`](../src/runtime/view/thread.cpp#L184) | `user_message_body` | Plain text body for user turns |
| [`thread.cpp:196`](../src/runtime/view/thread.cpp#L196) | `tool_timeline_detail` | Per-tool one-line summary string |
| [`thread.cpp:462`](../src/runtime/view/thread.cpp#L462) | `tool_display_name` | Lowercase moha name → TitleCase display |
| [`thread.cpp:484`](../src/runtime/view/thread.cpp#L484) | `format_duration` | ms / s / m+s formatter |
| [`thread.cpp:504`](../src/runtime/view/thread.cpp#L504) | `rich_status_icon` | Spinner / `✓` / `✗` / `⊘` per status |
| [`thread.cpp:537`](../src/runtime/view/thread.cpp#L537) | `split_lines_view` | Non-owning line splitter |
| [`thread.cpp:581`](../src/runtime/view/thread.cpp#L581) | `head_tail_lines` | Smart head+tail elision helper |
| [`thread.cpp:604`](../src/runtime/view/thread.cpp#L604) | `compact_tool_body` | Per-tool compact body for the timeline |
| [`thread.cpp:875`](../src/runtime/view/thread.cpp#L875) | `duration_color` | Green/dim/warn/danger by elapsed |
| [`thread.cpp:886`](../src/runtime/view/thread.cpp#L886) | `event_connector_color` | Stripe color from tool status |
| [`thread.cpp:906`](../src/runtime/view/thread.cpp#L906) | `tool_category_color` | inspect/mutate/execute/plan/vcs → palette |
| [`thread.cpp:916`](../src/runtime/view/thread.cpp#L916) | `tool_category_label` | Same buckets as strings |
| [`thread.cpp:930`](../src/runtime/view/thread.cpp#L930) | `assistant_timeline` | The Actions panel |
| [`tool_card.cpp:601`](../src/runtime/view/tool_card.cpp#L601) | `render_tool_call` | **Dead** — wraps the per-tool widgets, no call sites |
| [`permission.cpp:15`](../src/runtime/view/permission.cpp#L15) | `render_inline_permission` | Inline `maya::Permission` card |
| [`permission.cpp:43`](../src/runtime/view/permission.cpp#L43) | `render_checkpoint_divider` | `─── [↺ Restore checkpoint] ───` |
| [`cache.hpp`](../include/moha/runtime/view/cache.hpp) | various | View-side render caches |
