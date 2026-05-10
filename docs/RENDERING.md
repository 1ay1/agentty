# Conversation rendering — agentty as controller, maya as view

How a `Model` becomes terminal cells. agentty is a pure data adapter: it
extracts state from the runtime model and emits **widget Configs**.
maya owns every Element, every chrome glyph, every layout decision,
every breathing animation. The host app constructs no Elements.

Read this alongside [`UI.md`](UI.md), which catalogs the maya DSL
primitives that the widgets are built on top of.

---

## 0. The screen, annotated

```
┌────────────────────────── terminal viewport ─────────────────────────────┐
│                                                                          │ ─┐
│  ─── [↺ Restore checkpoint] ─────────────────────────────────  ┐         │  │
│  ┃ ❯ You                                          12:34 · turn 1│        │  │
│  ┃                                                              │         │  │
│  ┃ refactor the login flow to use the new auth provider         │         │  │
│  ─────────────────────────────────────────────────────────────  ┘         │  │
│                                                                          │  │
│  ┃ ✦ Opus 4.7                              12:34 · 4.2s · turn 1│        │  │ Thread
│  ┃                                                              │         │  │ (welcome
│  ┃ I'll start by exploring the current auth structure.          │         │  │  if empty,
│  ┃                                                              │         │  │  conversation
│  ┃ ╭─ ACTIONS · 3/3 · 1.8s ─────────────────────────────╮       │         │  │  otherwise)
│  ┃ │ I N S P E C T 2 · M U T A T E 1                    │       │         │  │
│  ┃ │ ╭─ ✓ Read    src/auth/login.ts   42ms              │       │         │  │
│  ┃ │ │  │  import { Session } from './session';         │       │         │  │
│  ┃ │ │  │  ··· 80 hidden ···                            │       │         │  │
│  ┃ │ │  │  export default login;                        │       │         │  │
│  ┃ │ ├─ ✓ Grep    provider in src/auth   190ms          │       │         │  │
│  ┃ │ │  │  src/auth/login.ts:14: const provider = …     │       │         │  │
│  ┃ │ ╰─ ✓ Edit    src/auth/login.ts (+5 -2)   1.6s      │       │         │  │
│  ┃ │    │  - const provider = legacyAuth();             │       │         │  │
│  ┃ │    │  + const provider = await NewAuth.create({    │       │         │  │
│  ┃ │ ✓ DONE   3 actions   1.8s                          │       │         │  │
│  ┃ ╰────────────────────────────────────────────────────╯       │         │ ─┘
│                                                                          │
│  ╭─────────────────────────────────────────────────────────────────╮     │ ─┐
│  │ Changes (2 files)        Ctrl+R review  A accept  X reject      │     │  │ ChangesStrip
│  │ 2 files changed  +12  -3                                        │     │  │ (only when
│  │   ~ src/auth/login.ts        +5 -2                              │     │  │  pending)
│  │   + src/auth/types.ts        +7 -1                              │     │  │
│  ╰─────────────────────────────────────────────────────────────────╯     │ ─┘
│                                                                          │
│  ╭─ ⠋ — type to queue… ──────────────────────────────────╮              │ ─┐
│  │ ❯ ▎                                                   │              │  │ Composer
│  │                                                       │              │  │
│  │ ↵ send  ·  ⇧↵/⌥↵ newline  ·  ^E expand    ▎ Write    │              │ ─┘
│  ╰────────────────────────────────────────────────────────╯              │
│  ▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔             │ ─┐
│   ▎ Title  ·  ▌ ⠋ Streaming  4.2s    ⚡ 23.4 t/s  ▁▂▃▅▇  ●Opus  CTX 18% │  │
│                                                                          │  │ StatusBar
│   ^K palette  ·  ^J threads  ·  ^T todo  ·  ^N new  ·  ^C quit          │  │
│  ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁             │ ─┘
└──────────────────────────────────────────────────────────────────────────┘

  When a modal/picker is open, an Overlay floats above the base, centered
  horizontally, pinned to the bottom edge, with an opaque background.
```

---

## 1. The architectural rule

> **agentty constructs no Elements.** Every `Element{...}`, every
> `dsl::v(...)`, every `dsl::h(...)`, every `dsl::text(...)` lives in
> a maya widget. agentty extracts state into widget Configs and lets
> maya render.

Concrete: every `.cpp` under `src/runtime/view/` (except the legacy
overlay files in §13) contains **zero** Element construction. Each
file is a function `Model → SomeWidget::Config`, **one widget = one
adapter file**, and the directory layout mirrors the widget tree
(see §13).

The single exception is `cached_markdown_for` (private to
`thread/turn/turn.cpp`): it returns an `Element` because
`maya::StreamingMarkdown` is stateful and its block-cache must
persist across frames. agentty caches the widget *instance*, calls
`set_content()` on it, and slots the resulting `Element` into a Turn
body slot via the typed `Element` variant. No `Element{...}` literals
— only `widget.build()` calls.

---

## 2. Widget hierarchy

```
maya::AppLayout                               top-level chat-app frame
├── maya::Thread                              conversation viewport
│   │
│   ├── maya::WelcomeScreen                   (when messages.empty())
│   │       wordmark + tagline + chips +
│   │       starters card + hint row
│   │
│   └── maya::Conversation                    (when !messages.empty())
│       │       list of turns + dim dividers + optional in-flight
│       │
│       ├── maya::Turn[*]                     one speaker turn (rail + header + body)
│       │   │
│       │   ├── maya::CheckpointDivider       (above turn, outside rail)
│       │   │       "─── [↺ Restore checkpoint] ───"
│       │   │
│       │   └── body slots (typed variant — Turn auto-spaces between):
│       │       ├── PlainText                 user message text
│       │       ├── MarkdownText              maya::markdown(content)
│       │       ├── maya::AgentTimeline       Actions panel (one per assistant turn)
│       │       │   │
│       │       │   ├── stats row             "INSPECT 2 · MUTATE 1"
│       │       │   ├── per-event header      tree glyph + status icon + name + detail + duration
│       │       │   ├── maya::ToolBodyPreview body content under │ stripe
│       │       │   │     │   discriminated by Kind:
│       │       │   │     ├── CodeBlock        head+tail preview, dimmed
│       │       │   │     ├── EditDiff         multi-hunk per-side diff
│       │       │   │     ├── GitDiff          per-line +/-/@@ coloring
│       │       │   │     ├── TodoList         ✓ ◍ ○ checkbox list
│       │       │   │     └── Failure          red preview block
│       │       │   └── footer                 "✓ DONE  3 actions  1.4s"
│       │       │
│       │       ├── maya::Permission          inline permission card
│       │       │
│       │       └── Element                   escape hatch (cached StreamingMarkdown)
│       │
│       └── maya::ActivityIndicator          (optional, bottom of thread)
│               "▎ ⠋ streaming…" — only when active and no Timeline visible
│
├── maya::ChangesStrip                       pending-edits banner
│       │
│       ├── header row                       "Changes (2 files)  Ctrl+R review  A accept  X reject"
│       └── maya::FileChanges                file list with +/− line counts
│
├── maya::Composer                           bordered input box
│       │
│       ├── prompt + body rows               state-driven color (idle/streaming/awaiting)
│       └── hint row (width-adaptive)
│           │
│           ├── shortcuts (left)             ↵ send · ⇧↵ newline · ^E expand
│           └── ambient (right)              queue · words · tokens · profile chip
│
├── maya::StatusBar                          bottom panel (5 rows tall — fixed-height)
│       │
│       ├── maya::PhaseAccent (top)          ▔▔▔▔▔▔ in phase color, dim
│       │
│       ├── activity row (width-adaptive)
│       │   ├── maya::TitleChip              ▎ thread title (truncated)
│       │   ├── maya::PhaseChip              colored glyph + verb + elapsed (breathing)
│       │   ├── maya::TokenStreamSparkline   ⚡ rate · ▁▂▃▄ · total
│       │   ├── maya::ModelBadge             ● Opus / Sonnet / Haiku
│       │   └── maya::ContextGauge           CTX usage with green/amber/red zones
│       │
│       ├── maya::StatusBanner               error/toast banner (always 1 row → no jitter)
│       │
│       ├── maya::ShortcutRow                ^K palette · ^J threads · …
│       │
│       └── maya::PhaseAccent (bottom)       ▁▁▁▁▁▁ in phase color, dim
│
└── maya::Overlay                            (when a modal is open)
        │
        ├── base                             everything above (z-stacked underneath)
        └── overlay                          centered horizontally, pinned bottom,
                                             opaque background to mask the base
```

Every name above is a real widget at `maya/include/maya/widget/<name>.hpp`.

---

## 3. Data flow — `view(m)` to terminal cells

```
agentty::ui::view(m)                                       [view.cpp]
    ↓
    builds maya::AppLayout::Config { … }
        .thread          = thread_config(m)             [thread/thread.cpp]
        .changes_strip   = changes_strip_config(m)      [changes_strip.cpp]
        .composer        = composer_config(m)           [composer.cpp]
        .status_bar      = status_bar_config(m)         [status_bar/status_bar.cpp]
        .overlay         = pick_overlay(m)              [view.cpp]
    ↓
    AppLayout{cfg}.build()
        ↓
        v(
            Thread{cfg.thread}.build()         | grow(1.0f),
            ChangesStrip{cfg.changes_strip}.build(),
            Composer{cfg.composer}.build(),
            StatusBar{cfg.status_bar}.build()
        ) | pad<1> | grow(1.0f)
        ↓
        Overlay{base, cfg.overlay}.build()
        ↓
        Element  (one tree of BoxElement / TextElement / ComponentElement)
        ↓
    maya layout engine → Canvas → terminal cells
```

Each `*_config` adapter is its own file matching the widget name; the
directory tree mirrors the widget hierarchy (§13).

`view()` is one declarative struct expression. No imperative chaining,
no `if` branches around `zstack` / `vstack`, no element construction —
just a `Config` populated from `Model` data.

```cpp
// src/runtime/view/view.cpp — the entire body of view():
maya::Element view(const Model& m) {
    return maya::AppLayout{{
        .thread        = thread_config(m),
        .changes_strip = changes_strip_config(m),
        .composer      = composer_config(m),
        .status_bar    = status_bar_config(m),
        .overlay       = pick_overlay(m),
    }}.build();
}
```

---

## 4. Inside `maya::AppLayout::build()`

```cpp
auto base = (v(
    v(Thread{cfg_.thread}.build()) | grow(1.0f),
    ChangesStrip{cfg_.changes_strip}.build(),
    Composer{cfg_.composer}.build(),
    StatusBar{cfg_.status_bar}.build()
) | pad<1> | grow(1.0f)).build();

Overlay::Config oc;
oc.base = std::move(base);
if (cfg_.overlay) { oc.overlay = *cfg_.overlay; oc.present = true; }
return Overlay{std::move(oc)}.build();
```

That's the whole top-level layout. Four sections in a vstack with the
Thread growing to fill, all wrapped in `pad<1>`. If `overlay` is
present, `Overlay` z-stacks it on top with center-bottom alignment.

---

## 5. Inside `maya::Thread::build()`

```cpp
if (cfg_.is_empty)
    return WelcomeScreen{cfg_.welcome}.build();
return Conversation{cfg_.conversation}.build();
```

`Thread::Config` nests `WelcomeScreen::Config` *and*
`Conversation::Config`; the widget just picks the branch. Each
sub-widget gets its own agentty adapter (`thread/welcome_screen.cpp`,
`thread/conversation.cpp`).

`Conversation::Config` itself nests typed sub-configs:

```cpp
struct Conversation::Config {
    std::vector<Turn::Config>                turns;
    std::optional<ActivityIndicator::Config> in_flight;
};
```

The widget builds each Turn from its config internally — the host
never assembles a `vector<Element>` of pre-built turns.

---

## 6. Inside `maya::Turn::build()` — the body-slot dispatch

The most interesting widget. `Turn::Config::body` is a typed variant:

```cpp
using BodySlot = std::variant<
    PlainText,             // user/plain text path
    MarkdownText,          // string → maya::markdown()
    AgentTimeline::Config, // tool-calls Actions panel
    Permission::Config,    // inline permission card
    Element                // escape hatch (cached StreamingMarkdown)
>;
std::vector<BodySlot> body;
```

Turn:
1. Renders the header row (`<glyph> <label> ___ <meta>`).
2. Walks each body slot, dispatches via `std::visit` to the right
   widget invocation, and inserts a blank line between consecutive
   non-empty slots — callers don't push spacers.
3. If `error` is non-empty, appends a `⚠ <message>` row.
4. Wraps everything in the bold left-only border (the speaker rail) in
   `rail_color`.
5. If `checkpoint_above`, prepends a `CheckpointDivider` outside the
   rail (between-turns concern, not inside the rail).

Per-slot widget invocation:

| BodySlot variant      | Renderer inside Turn                |
|-----------------------|-------------------------------------|
| `PlainText`           | `text(content, fg)`                 |
| `MarkdownText`        | `maya::markdown(content)`           |
| `AgentTimeline::Config` | `AgentTimeline{cfg}.build()`      |
| `Permission::Config`  | `Permission{cfg}.build()`           |
| `Element`             | the Element itself (escape hatch)   |

The escape-hatch `Element` slot exists for one reason: cross-frame
caching. `maya::StreamingMarkdown` keeps a per-block parse cache that
must survive between renders, so agentty holds the widget instance in
its `MessageMdCache` and feeds the resulting `Element` back through
the slot list. That's the only `Element`-producing call agentty makes.

---

## 7. Inside `maya::AgentTimeline::build()`

The Actions panel. Composition:

```
╭─ ACTIONS · 3/5 · Bash ─────────────────────────────╮
│  I N S P E C T  2  ·  M U T A T E  1               │   ← stats row (only when events > 1)
│                                                    │
│  ╭─ ⠋ Bash    npm test          1.2s               │   ← per-event header
│  │   │  PASS test/foo.test.ts                      │   ← ToolBodyPreview rows under │ stripe
│  │   │  ✓ all 5 tests passed                       │
│  │                                                  │   ← inter-event connector (next status color)
│  ├─ ✓ Read    src/foo.ts        38ms               │
│  │   │  import { bar } from './bar';               │
│  ╰─ ✓ Edit    src/foo.ts        210ms              │
│   │  edit 1/2  ·  −1 / +3                          │
│   │  - const provider = …                          │
│   │  + const provider = await …                    │
│                                                    │
│  ✓ DONE   3 actions   1.4s                          │   ← footer (only when all settled)
╰─────────────────────────────────────────────────────╯
```

Each `AgentTimelineEvent` carries:

```cpp
struct AgentTimelineEvent {
    std::string             name;              // "Bash", "Read", …
    std::string             detail;            // "npm test  ·  exit 0"
    float                   elapsed_seconds;
    Color                   category_color;    // inspect/mutate/execute/plan/vcs
    AgentEventStatus        status;            // Pending/Running/Done/Failed/Rejected
    ToolBodyPreview::Config body;              // typed body — no Elements
};
```

For each event the widget:
1. Picks the tree glyph (`──` / `╭─` / `├─` / `╰─` based on position).
2. Picks the status icon (10-frame braille spinner for active states;
   `✓ ✗ ⊘` for terminal).
3. Renders `name + detail + (optional duration)`.
4. Builds the body via `ToolBodyPreview{ev.body}.build()` and stripes
   each line with the `│` connector, in `event_connector_color(status)`.
5. Inserts a short inter-event connector colored by the *next*
   event's status (so the lane visually flows into the upcoming
   event).

Footer is rendered only when every event is terminal: `✓ DONE` /
`✗ N FAILED` / `⊘ N REJECTED` + count + total elapsed.

---

## 8. Inside `maya::ToolBodyPreview::build()`

A discriminated body widget. `Config::kind` picks the renderer:

| Kind        | Inputs              | Rendering                                                   |
|-------------|---------------------|-------------------------------------------------------------|
| `None`      | —                   | empty Element (skipped)                                     |
| `CodeBlock` | `text`, `text_color`| head+tail preview (4+3 lines) with `··· N hidden ···` mark  |
| `Failure`   | `text`              | same as CodeBlock but in `Color::red()`                     |
| `EditDiff`  | `hunks[]`           | per-hunk header `edit i/N · −k / +m`, head+tail per side    |
| `GitDiff`   | `text`              | per-line styling (+green / -red / @@dim / context plain)    |
| `TodoList`  | `todos[]`           | `✓` completed (dim), `◍` in-progress, `○` pending           |

All elision math (split lines → keep first `head` + last `tail` →
insert dim middle marker) lives inside the widget. agentty just provides
the raw `text` / `hunks[]` / `todos[]`.

---

## 9. The other top-level widgets

### `maya::WelcomeScreen` — empty-thread splash

```
                    ┌─┐┌─┐┌─┐┌┐ ┌┬┐┌┬┐┬ ┬
                    ├─┤│ ┐├─ │└┐ │  │ └┬┘
                    ┴ ┴└─┘└─┘┘ ┘ ┴  ┴  ┴ 

                a calm middleware between you and the model

                ● Opus 4.7              ▌ WRITE ▐                  ← chips row

              ╭─ T R Y ──────────────────────────────╮
              │                                       │
              │ • Implement a small feature           │
              │ • Refactor or clean up this file      │
              │ • Explain what this code does         │
              │ • Write tests for ...                 │
              ╰───────────────────────────────────────╯

      type to begin  ·  ^K palette  ·  ^J threads  ·  ^N new
```

agentty supplies brand content (wordmark glyphs, tagline, starter
prompts, hint keys); the widget owns the layout, the wordmark gradient
("last row dim"), the small-caps title, the centering.

### `maya::ChangesStrip` — pending edits banner

```
╭─────────────────────────────────────────────────────╮
│ Changes (2 files)   Ctrl+R review  A accept  X reject│
│ 2 files changed  +12  -3                             │
│   ~ src/auth/login.ts        +5 -2                   │
│   + src/auth/types.ts        +7 -1                   │
╰─────────────────────────────────────────────────────╯
```

When `cfg.changes` is empty, the widget renders to an empty Element so
the slot collapses cleanly without an `if` in the host.

### `maya::Composer` — bordered input box

State-driven: border + prompt color reflect activity (idle / awaiting
permission / streaming / executing tool); placeholder text changes
("type a message…" / "running tool — type to queue…"); height pins to
`min_rows` during activity to prevent vertical jitter from layout
reflows above. Hint row is width-adaptive — drops `expand` then
`newline` keys as width shrinks; right side carries queue depth /
word-and-token counters / profile chip.

### `maya::StatusBar` — bottom panel

`StatusBar::Config` nests **typed sub-widget Configs**, so each
sub-widget gets its own agentty adapter (one widget = one adapter file):

```cpp
struct StatusBar::Config {
    Color                        phase_color;       // top/bottom PhaseAccent + leading rail
    TitleChip::Config            breadcrumb;        // empty title = hide
    PhaseChip::Config            phase;
    TokenStreamSparkline::Config token_stream;
    Element                      model_badge;       // pre-built (ModelBadge has its own adapter)
    ContextGauge::Config         context;           // max=0 = hide
    StatusBanner::Config         status_banner;     // empty text = blank slot
    ShortcutRow::Config          shortcuts;
    // … width thresholds …
};
```

Five fixed rows in the layout:
1. `PhaseAccent` (top) — ▔▔▔▔ in phase color, dim
2. activity row — `TitleChip` · `PhaseChip` · `TokenStreamSparkline` · `ModelBadge` · `ContextGauge`
3. `StatusBanner` — toast banner or blank (always 1 row tall to prevent jitter)
4. `ShortcutRow` — width-adaptive key/label list
5. `PhaseAccent` (bottom) — ▁▁▁▁ in phase color, dim

Owned-by-widget behaviour:

- `PhaseChip` — breathing animation cadence (32-frame cycle, bold half /
  dim half — perceptible motion below resting heart-rate).
- `ContextGauge` — green/amber/red zones (`<60%` safe, `60–80%` warn,
  `>80%` danger) plus a placeholder slot when no usage data has arrived
  yet (so the right-side chips don't shove leftward when the first
  usage event fires mid-stream).
- `TokenStreamSparkline` — fixed 37-cell ⚡ rate · ▁▂▃▄ · total. The
  ring buffer behind it persists across sub-turns and tool gaps so the
  bar shows a continuous trace of generation rate over the session;
  only the per-burst rate accumulator resets on `StreamStarted`.
- `StatusBanner` — empty `text` renders a 1-cell blank placeholder so
  the row count stays fixed regardless of toast presence.
- `ShortcutRow` — priority-sorted dropping (`S-Tab` and `^/` go first
  on narrow widths) and key-only mode below `label_min_width`.

The activity row's width-adaptive logic (drop breadcrumb < 130, drop
token stream < 110, drop ctx bar < 55) lives in `StatusBar::build()`,
which patches per-frame copies of the sub-configs based on terminal
width before invoking each sub-widget.

### `maya::Overlay` — modal layer

A thin coordinator: `present=false` collapses to just `base`. When
present, z-stacks `overlay` on top, centered horizontally, pinned to
the bottom edge, with an opaque background to mask the base.

---

## 10. agentty's adapter side — one widget, one adapter file

Every adapter file under `src/runtime/view/` has the same shape: one
function `Model → SomeWidget::Config`. Filenames mirror the widget
they adapt; the directory tree mirrors the widget hierarchy.

| Adapter file | Function | Returns |
|---|---|---|
| `view.cpp` | `view(m)` | `Element` (the one `.build()`) |
| `view.cpp` | `pick_overlay(m)` | `optional<Element>` |
| `thread/thread.cpp` | `thread_config(m)` | `Thread::Config` |
| `thread/welcome_screen.cpp` | `welcome_screen_config(m)` | `WelcomeScreen::Config` |
| `thread/conversation.cpp` | `conversation_config(m)` | `Conversation::Config` |
| `thread/activity_indicator.cpp` | `activity_indicator_config(m)` | `optional<ActivityIndicator::Config>` |
| `thread/turn/turn.cpp` | `turn_config(msg, idx, n, m)` | `Turn::Config` |
| `thread/turn/permission.cpp` | `inline_permission_config(pp,tc)` | `Permission::Config` |
| `thread/turn/agent_timeline/agent_timeline.cpp` | `agent_timeline_config(msg, frame, c)` | `AgentTimeline::Config` |
| `thread/turn/agent_timeline/tool_body_preview.cpp` | `tool_body_preview_config(tc)` | `ToolBodyPreview::Config` |
| `composer.cpp` | `composer_config(m)` | `Composer::Config` |
| `changes_strip.cpp` | `changes_strip_config(m)` | `ChangesStrip::Config` |
| `status_bar/status_bar.cpp` | `status_bar_config(m)` | `StatusBar::Config` |
| `status_bar/title_chip.cpp` | `title_chip_config(m)` | `TitleChip::Config` |
| `status_bar/phase_chip.cpp` | `phase_chip_config(m)` | `PhaseChip::Config` |
| `status_bar/token_stream_sparkline.cpp` | `token_stream_sparkline_config(m)` | `TokenStreamSparkline::Config` |
| `status_bar/context_gauge.cpp` | `context_gauge_config(m)` | `ContextGauge::Config` |
| `status_bar/status_banner.cpp` | `status_banner_config(m)` | `StatusBanner::Config` |
| `status_bar/shortcut_row.cpp` | `shortcut_row_config(m)` | `ShortcutRow::Config` |
| `status_bar/model_badge.cpp` | `model_badge_config(m)` | `maya::ModelBadge` |

Pure helpers (no maya types touched): under
`thread/turn/agent_timeline/tool_helpers.cpp` (display name, category
color/label, event status, timeline detail) and
`thread/turn/agent_timeline/tool_args.cpp` (arg parsers); shared
helpers (`format_duration_compact`, `small_caps`, `phase_*`) live in
`helpers.cpp`.

The single `Element`-returning function inside an adapter is
`cached_markdown_for` (private to `thread/turn/turn.cpp`) — required
because `maya::StreamingMarkdown` is stateful and its block-cache
must persist across frames.

---

## 11. Caching and persistent state

One thread-local cache + one persistent ring buffer carry across
frames.

### Markdown cache — `cache.hpp` / `cache.cpp`

| Cache            | Key                           | Holds                                                    |
|------------------|-------------------------------|----------------------------------------------------------|
| `message_md_cache(tid, idx)` | `(thread_id, msg_idx)`        | `shared_ptr<Element>` (finalized) + `shared_ptr<StreamingMarkdown>` (live) |

`StreamingMarkdown` is the only widget held across frames — its
internal block-boundary cache makes each delta `O(new_chars)` rather
than re-parsing the full transcript. agentty keeps the instance alive,
calls `set_content(streaming_text)` each frame, slots
`instance.build()` into the Turn body via the `Element` variant.

Once `finalize_turn` moves `streaming_text` → `text`, the next render
takes the finalized branch: builds `maya::markdown(text)` once,
caches the resulting `Element`, returns the same pointer every
subsequent frame.

### Streaming-rate ring buffer — `StreamState::rate_history`

The `TokenStreamSparkline` reads its history from a 16-slot ring
buffer in `StreamState`. The ring buffer **persists across sub-turns
and tool gaps** — only the per-burst rate accumulator
(`live_delta_bytes`, `first_delta_at`, `rate_last_sample_*`) resets on
`StreamStarted`. So the rate *number* measures only the current burst
(not polluted by the previous turn's bytes), but the sparkline *bars*
trace generation rate continuously over the whole session.

---

## 12. Data flow for one assistant turn with tools

End to end, what happens when an assistant turn with two tool calls
needs rendering:

```
view(m)                                    ┐
   AppLayout::Config{ .thread = ..., …}    │
   AppLayout{cfg}.build()                  │ host
      ↓                                    ┘
   Thread{thread_cfg}.build()
      ↓
   for each Turn::Config in thread_cfg.turns:
      Turn{tc}.build()
         ↓
      header = h(glyph, label, meta) | grow(1.0f)
      for each BodySlot:
         visit(slot):
            MarkdownText →   maya::markdown(content)
                               (or cached StreamingMarkdown.build()
                                via Element variant)
            AgentTimeline::Config →
                AgentTimeline{cfg}.build()
                   ↓
                for each AgentTimelineEvent:
                    header row (tree glyph, status icon, name, detail, duration)
                    ToolBodyPreview{event.body}.build()
                       ↓
                       switch(kind):
                          CodeBlock  → head_tail(text, 4, 3) → vstack of lines
                          EditDiff   → for each hunk: header + push_diff_side(old, '-', red) + push_diff_side(new, '+', green)
                          GitDiff    → per-line pick_style (+/-/@@) + head+tail
                          TodoList   → for each: glyph + content (status-styled)
                          Failure    → CodeBlock in red
                    inter-event connector (colored by next status)
                stats row (if events > 1)
                footer (if all terminal)
                | border<Round> | bcolor(rail_color) | btext("ACTIONS · 3/3 …")
            Permission::Config →   Permission{cfg}.build()
      | rail (Bold left border in rail_color)
      → Element
   collected as Conversation::Config.turns
   Conversation{conv_cfg}.build()
      → list of turn Elements separated by dim ─── dividers
      + optional ActivityIndicator at bottom
   → Element
   (slotted into AppLayout's vstack alongside changes_strip / composer / status_bar)
```

Every transition is `widget.build()` returning an `Element`. agentty
participates only at the entry: building the top-level Config tree.

---

## 13. Files

### maya widgets — flat directory, hierarchy in headers

```
maya/include/maya/widget/
├── app_layout.hpp                top-level frame: Thread + ChangesStrip + Composer + StatusBar + Overlay
├── thread.hpp                    welcome | conversation branch
├── conversation.hpp              list of typed Turn::Configs + optional in-flight
├── turn.hpp                      single turn: rail + header + typed body slot variant
├── checkpoint_divider.hpp        "─── [↺ Restore checkpoint] ───"
├── activity_indicator.hpp        "▎ ⠋ streaming…"
├── welcome_screen.hpp            wordmark + chips + starters + hints
├── agent_timeline.hpp            Actions panel for tool calls
├── tool_body_preview.hpp         discriminated tool body (CodeBlock/EditDiff/GitDiff/TodoList/Failure)
├── permission.hpp                inline permission card
├── changes_strip.hpp             pending edits banner
├── composer.hpp                  bordered input box (state-driven color, hint row)
├── status_bar.hpp                bottom panel — nests typed sub-widget Configs
├── phase_accent.hpp              soft horizontal rule in phase color
├── phase_chip.hpp                breathing colored glyph + verb + elapsed
├── title_chip.hpp                ▎ + bold title with middle-truncation
├── token_stream_sparkline.hpp    ⚡ rate · ▁▂▃▄ · total — fixed 37-cell slot
├── context_gauge.hpp             CTX usage fuel-gauge with zones
├── status_banner.hpp             toast/error row (always 1 row tall)
├── shortcut_row.hpp              width-adaptive keyboard hints
├── model_badge.hpp               ● colored model chip
└── overlay.hpp                   z-stack base + centered modal
```

### agentty adapters — directory tree mirrors the widget hierarchy

```
src/runtime/view/
├── view.cpp                              # AppLayout
├── changes_strip.cpp                     # ChangesStrip
├── composer.cpp                          # Composer
├── cache.cpp · helpers.cpp               # shared (not adapters)
├── login.cpp · pickers.cpp · diff_review.cpp   # legacy modals (pending widgetization)
├── thread/
│   ├── thread.cpp                        # Thread
│   ├── welcome_screen.cpp                # WelcomeScreen      (empty branch)
│   ├── conversation.cpp                  # Conversation       (non-empty branch)
│   ├── activity_indicator.cpp            # ActivityIndicator  (bottom of conversation)
│   └── turn/
│       ├── turn.cpp                      # Turn
│       ├── permission.cpp                # Permission         (body slot)
│       └── agent_timeline/
│           ├── agent_timeline.cpp        # AgentTimeline      (body slot)
│           ├── tool_body_preview.cpp     # ToolBodyPreview    (per-event body)
│           ├── tool_helpers.cpp          # per-tool helpers
│           └── tool_args.cpp             # arg parsers
└── status_bar/
    ├── status_bar.cpp                    # StatusBar
    ├── title_chip.cpp                    # TitleChip          (activity row)
    ├── phase_chip.cpp                    # PhaseChip          (activity row)
    ├── token_stream_sparkline.cpp        # TokenStreamSparkline (activity row)
    ├── context_gauge.cpp                 # ContextGauge       (activity row)
    ├── model_badge.cpp                   # ModelBadge         (activity row)
    ├── status_banner.cpp                 # StatusBanner       (status row)
    └── shortcut_row.cpp                  # ShortcutRow        (shortcut row)
```

Headers mirror the same layout under `include/agentty/runtime/view/`.

`login.cpp`, `pickers.cpp`, `diff_review.cpp` are modal overlays that
still construct elements directly — they predate the controller-only
refactor. Future widgetization: `maya::LoginModal`, `maya::Picker`
(or `CommandPalette` / `ThreadList` / `ModelPicker` / `TodoModal`),
`maya::DiffReview`.
