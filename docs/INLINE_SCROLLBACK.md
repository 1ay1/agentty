# Inline scrollback rendering — current good state

> **Why this file exists.** Inline-mode rendering in agentty is the one
> subsystem where “make it work” is a multi-pin combination lock across
> two repos. Every pin is in the right position right now: streaming
> doesn't ghost, resume doesn't ghost, the canvas doesn't pile up
> forever, and the first frame after a thread swap is a memcpy-blit
> instead of a 500 ms paint. If you change any of the pins and the
> result looks wrong, re-read this file and put them back exactly as
> described. The pins are listed in dependency order so the recovery
> is incremental — fix pin 1 first, then pin 2, etc.
>
> Pair this with [`RENDERING.md`](RENDERING.md) (the data-adapter
> story) and `maya/docs/rendering-modes.md` (how `Mode::Inline`
> differs from `Mode::Fullscreen`). This file is the *integration*
> view: what agentty must do, what maya must do, and why each
> decision is the only one that works.

Last verified: commit `agentty 6eb50c4` + submodule `maya bc70e82`.
Build: `cmake --build build -j10`.

---

## TL;DR — the seven pins

If inline-mode rendering breaks, check these in order:

1. **Mode + AppLayout shape.** `Mode::Inline` at the runtime; flat
   `vstack` in `AppLayout` with **no** `overflow()`, **no** `shrink`,
   **no** wrapper boxes around children. (maya commit `54fde37`.)
2. **Frozen prefix is borrowed, not rebuilt.** `conversation_config`
   sets `cfg.frozen = &m.ui.frozen`. The host pushes entries; maya
   renders them through `list_ref` (zero-copy).
3. **Divider symmetry.** `frozen.cpp`, `build_live_tail`, and
   `build_queued_previews` all push **the same** `maya::Conversation::divider()`
   row between turns. Any height delta at a freeze instant ghosts.
   The `≡ Conversation compacted` divider obeys the same rule:
   `build_live_tail` must emit it at the `up_to_index` boundary just
   like `freeze_range`, or the post-compaction turn duplicates into
   scrollback at freeze time.
4. **Freeze gate.** `run_is_freezable` refuses to freeze a run that
   still contains a Pending / Approved / Running tool. Frozen
   Elements are immutable; freezing a non-terminal run would pin a
   spinner in scrollback forever.
5. **Hash-keyed cache.** Every frozen Turn has a stable
   `cfg.hash_id` built from the run's message IDs + render keys.
   The conversation divider has a fixed `hash_id`. Maya's
   `thread_local` `ComponentCache` blits cached cells when the key
   re-appears.
6. **Resume warmup.** `m.ui.needs_warmup_render` is set in
   `ThreadLoaded` after `rehydrate_frozen`. The reducer clears it on
   every non-`ThreadLoaded` step. Maya's run loop edge-detects the
   flag and calls `Runtime::warmup_render(view_root)` exactly once
   per swap, into a scratch canvas, populating the cache before the
   wire-bound render.
7. **Canvas shrink + trim.** `Runtime::render`'s inline path
   reallocates the canvas down to `content + 64` rows when
   `canvas.height() * 2 > shrink_target * 3` (1.5×). `trim_frozen_if_oversized`
   trims `m.ui.frozen` by ROWS — dropping oldest entries until
   `frozen_row_total` is under ~600 rows (and entry count under 60),
   keeping at least the most recent 3 entries — then issues
   `Cmd::commit_scrollback_overflow()`. The per-entry row count is
   estimated from each tool card's **args** body (write content, edit
   hunks, read/grep results), NOT its one-line output footer; counting
   output alone under-estimates a tall write to ~1 row and the cap
   never trips.

Each pin has a section below with the file/line, the rationale, and
the failure mode if you undo it.

---

## 1. Mode + AppLayout shape

**Pin location.**
- agentty: `maya::run<AgenttyApp>({.mode = Mode::Inline, …})` —
  set in `main.cpp`.
- maya: `include/maya/widget/app_layout.hpp`.

**Required shape (the only correct one for inline mode):**

```cpp
const int term_h = available_height();
auto base = (vstack()
    .min_height(Dimension::fixed(term_h))
    .padding(1)
    (
        Thread{cfg_.thread}.build(),
        ChangesStrip{cfg_.changes_strip}.build(),
        Composer{cfg_.composer}.build(),
        StatusBar{cfg_.status_bar}.build()
    )).build();
```

**What must NOT appear:**
- No `overflow(Overflow::Hidden)` anywhere.
- No `shrink(…)` on the outer vstack or any child.
- No per-child wrapper `vstack`/`box`.
- No `grow(1.0f)` on the AppLayout outer — the `Conversation`
  widget owns its own grow (see pin 2).

**Why.** Maya's inline renderer commits viewport overflow row-by-row
into the terminal's native scrollback (see `maya/src/render/serialize.cpp`'s
case-B path and the `compose_inline_frame` notes in
`maya/src/render/serialize.cpp:345+`). For that mechanism to work,
layout **must** be allowed to grow past the viewport. If anything
clips or compresses the thread:

- Content piles up inside the same viewport slot.
- Stale cells from a previously-taller frame leak through the
  shorter slot.
- The seam at thread↔composer ghosts during streaming.

The `min_height(term_h)` pin ensures empty/short threads still fill
the viewport (composer rides the bottom edge); the absence of any
overflow/shrink ensures tall content overflows naturally and the
inline renderer can commit the overflow into scrollback.

**Failure signature if you regress this:** ghosted text at the
composer's top border during assistant streaming, or a phantom
duplicate of the last tool-call card just above the composer.

**Commit reference.** maya `54fde37` — “app_layout: drop
overflow:Hidden + shrink(1.0f) on thread_box, let inline-mode
scrollback overflow happen naturally.”

---

## 2. Borrowed frozen prefix

**Pin location.** `src/runtime/view/thread/conversation.cpp` —
`conversation_config()`.

```cpp
maya::Conversation::Config cfg;
cfg.frozen = &m.ui.frozen;      // borrowed pointer; zero-copy
int running_turn = m.ui.frozen_turn + 1;
build_live_tail(m, running_turn, cfg.live_tail);
build_queued_previews(m, running_turn, cfg.live_tail);
cfg.in_flight = std::nullopt;   // see pin 8 — placeholder lives in-Turn
```

**Why.** maya's `Conversation` widget, when given a `frozen`
pointer, does `rows.push_back(list_ref(*cfg_.frozen))` — a single
`list_ref` Element referencing the host's vector. Per-frame cost is
O(visible live tail) regardless of how long the session has run.
The `frozen` vector grows only at freeze instants; it never shrinks
during normal append (only on trim — pin 7).

**The two invariants the host owes maya:**
- `frozen` outlives every `build()` call. ✓ It's `m.ui.frozen`,
  same lifetime as the Model.
- Inter-turn dividers belong to the host. The `Conversation` widget
  adds **no** dividers when `frozen != nullptr`. See pin 3.

**Failure signature.** If you switch to `cfg.built_turns` or
`cfg.turns`, every frame rebuilds every settled turn — frame cost
goes from O(visible live tail) to O(N turns × tool cards). 200-turn
session at ~3 ms/turn = 600 ms/frame visible as input lag.

---

## 3. Divider symmetry

**Pin locations.** Three sites must push **byte-identical** divider
Elements with **the same** `hash_id`:

| Site                                                | File                                                  |
|-----------------------------------------------------|-------------------------------------------------------|
| Frozen prefix: between settled turns                | `src/runtime/app/update/frozen.cpp` (`freeze_range`)  |
| Live tail: between unfrozen turns                   | `src/runtime/view/thread/conversation.cpp` (`build_live_tail`) |
| Live tail: before each queued-message preview       | `src/runtime/view/thread/conversation.cpp` (`build_queued_previews`) |

All three call `gap_row()`, which forwards to
`maya::Conversation::divider()` (defined in
`maya/include/maya/widget/conversation.hpp:177`). That static
function returns a width-aware indented `─` rule with a fixed
`hash_id` of `"maya.conversation.divider"`.

**Why symmetry is the invariant.** At a freeze instant, rows
N-1, N-2, … of the live tail are physically on the wire (some may
already be in native scrollback). When `freeze_range` runs, those
rows must reappear in the frozen prefix with **identical heights**
or maya's row-diff will see a vertical shift and either:

- Repaint downward (visible flash + duplicate row in scrollback), or
- Repaint upward (visible erase of a row that's no longer part of
  the live tail), leaving a “ghost” of the old separator visible at
  the seam.

Because `Conversation::divider()` is a `component(...)` with a
fixed `hash_id`, every divider at a given terminal width renders to
byte-identical cells, and maya's cache blits the same cells in all
three sites.

**Failure signature.** A one-row vertical jitter at the
frozen↔live boundary the instant a turn settles. Usually visible as
the composer “snapping” up or down by one row when an assistant
turn finishes.

**The compaction divider is a SECOND divider that obeys the same
rule.** Besides the inter-turn `gap_row()`, `freeze_range` pushes a
single-row `≡ Conversation compacted` rule (`compaction_divider_row()`)
immediately before the run that begins on a `CompactionRecord`'s
`up_to_index`. `build_live_tail` **must** push a byte-identical
`compaction_divider_row()` at the same boundary, in the same order
(divider before the gap). If only `freeze_range` pushes it, the
divider does not exist in the live tail while the post-compaction turn
streams — it materialises only at the freeze instant, adding one row
at the TOP of an already-overflowed turn. Maya's row-diff repaints
downward, and the pre-shift copy of the turn that already committed to
native scrollback is stranded there permanently: a **duplicate of the
entire post-compaction turn** (user bubble + assistant timeline), plus
a clipped/empty neighbouring tool panel where the two paints overlap.
This is the post-compaction duplicate-turn bug. `build_queued_previews`
does NOT need it — queued previews are synthetic rows past
`messages.size()` that are never frozen, so they have no freeze seam.

---

## 4. Freeze gate

**Pin location.** `src/runtime/app/update/frozen.cpp` —
`run_is_freezable()` + the gate in `freeze_range()`.

```cpp
if (!run_is_freezable(m, i, run_end)) {
    m.ui.frozen_through = i;   // resume here next time
    return;
}
```

A run is freezable iff every tool in every assistant message of
the run is in a terminal state (Succeeded / Failed / Denied /
TimedOut / Cancelled). Pending / Approved / Running runs are NOT
frozen; the live tail keeps rendering them until they settle.

**Why.** Frozen entries are `maya::Element` *values* — immutable
once pushed. Their `hash_id` is stamped once and never recomputed
(see pin 5). If we freeze a Running tool, the live model's status
updates later (when `ToolExecOutput` lands) but the rendered
Element in `m.ui.frozen` keeps the pre-update state forever. The
user sees a permanently-spinning tool card in scrollback.

**Where the freeze trigger fires.** `freeze_through(m, live_start)`
is called from `submit_message` (i.e. at the start of every new
user turn) — see `src/runtime/app/update/internal.hpp`. Mid-stream
freezing is **forbidden**: splitting an in-flight assistant run
across the frozen / live boundary produces two visual Turns where
the user should see one. The single freeze site is on user-message
submit (see the comment block in
`src/runtime/app/cmd_factory.cpp:584+`).

**Failure signature.** A scrolled-off tool card stays spinning
forever in the terminal's scrollback. The live tail shows the
correct status; only the frozen copy is wrong.

---

## 5. Hash-keyed cache for frozen entries

**Pin location.** `src/runtime/app/update/frozen.cpp` —
`freeze_range()`.

For each frozen Turn:

```cpp
maya::CacheIdBuilder kb;
kb.add(std::string_view{"agentty.turn.assistant_run"})
  .add(static_cast<std::uint64_t>(run_end - i));
for (std::size_t j = i; j < run_end; ++j) {
    kb.add(std::string_view{m.d.current.messages[j].id.value});
    kb.add(m.d.current.messages[j].compute_render_key());
}
cfg.hash_id = kb.build();
```

User / compaction-summary turns get a simpler key with just
`"agentty.turn" + msg.id + compute_render_key()`.

**Why.** maya's renderer holds a `thread_local ComponentCache`
keyed by `(hash_id, width)`. The first paint of a Turn captures
its cells into the cache; every subsequent paint at the same width
is a memcpy-blit of those cells. Three things must be true:

- The key must be **stable** for the lifetime of the entry. ✓
  Frozen entries are immutable, and `compute_render_key()` is
  computed against the immutable message contents.
- The key must **change** when the visible content changes. ✓
  Every message ID + render key feeds the builder.
- Width-keyed: a resize invalidates entries automatically. ✓
  Cache implementation in maya.

**The divider has its own key** — `"maya.conversation.divider"` —
so all N dividers in a long frozen prefix share **one** cached
cells-blit. See pin 3.

**Failure signature.** If you remove `hash_id` or use a non-stable
key (e.g. one that includes a timestamp), every frame re-runs the
full layout + paint for every frozen Turn. 200-turn session →
hundreds of ms per frame.

---

## 6. Resume warmup (the resume-ghosting fix)

**Pin locations.** Three files cooperate:

### 6a. Model field
`include/agentty/runtime/model.hpp` — `UI::needs_warmup_render`.
Default `false`. Documented in-place.

### 6b. Setter
`src/runtime/app/update/picker.cpp` — `ThreadLoaded` handler:

```cpp
m.d.current = std::move(e.thread);
rehydrate_frozen(m);
m.ui.needs_warmup_render = !m.ui.frozen.empty();
// ...
return {std::move(m), Cmd<Msg>::commit_scrollback_overflow()};
```

### 6c. Clearer
`src/runtime/app/update.cpp` — top of `update(Model, Msg)`:

```cpp
const bool is_thread_load = std::visit([](const auto& x) {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, msg::ThreadListMsg>) {
        return std::holds_alternative<::agentty::ThreadLoaded>(x);
    }
    return false;
}, msg);
if (!is_thread_load) m.ui.needs_warmup_render = false;
```

> The `::agentty::ThreadLoaded` qualification is load-bearing — there is
> no `msg::ThreadLoaded` alias. `ThreadLoaded` lives in the
> `::agentty` namespace and is wrapped into `msg::ThreadListMsg`.

### 6d. Program hook
`include/agentty/runtime/app/program.hpp`:

```cpp
static bool needs_warmup(const Model& m) {
    return m.ui.needs_warmup_render;
}
```

### 6e. Maya-side consumer
`maya/include/maya/app/app.hpp` (the `run<P>` loop, around the
`view(model)` call):

```cpp
Element view_root = P::view(model);
if constexpr (detail::HasNeedsWarmup<P>::value) {
    const bool want = P::needs_warmup(model);
    if (want && !last_warmup_done) {
        rt.warmup_render(view_root);
    }
    last_warmup_done = want;
}
auto status = rt.render(view_root);
```

### 6f. Maya-side implementation
`maya/src/app/app.cpp` — `Runtime::warmup_render(const Element&)`,
right after `Runtime::render`'s closing brace (around line 559).
Same `pool_`, same width as `canvas_`; renders into a scratch
canvas that's dropped at function end. The `thread_local`
`ComponentCache` retains the captured cells.

**Why it fixes resume-ghosting.** Before warmup, the first
post-`ThreadLoaded` render paid full layout + paint over the
rehydrated frozen tree (tens to hundreds of ms). That render
emitted a huge byte stream against a stale `prev_cells` shadow,
straddling the writer's pacing window — old picker-frame residue
could interleave with the new transcript bytes. After warmup, the
real render is a sub-ms cell-blit, completes in one coherent write,
and the shadow is clean.

**Edge-trigger semantics.** `last_warmup_done` is a local bool in
the run loop. The reducer-side clear (pin 6c) ensures the next
thread swap gets a clean `false → true` edge so warmup fires
exactly once per swap.

**Thread-locality gotcha.** `ComponentCache` is `thread_local`.
Warmup **must** run on the UI thread (the same one that runs
`render`). That's why `warmup_render` is a Runtime method invoked
from the run loop, not a background-thread pre-bake. Do not move
this to a worker.

**Style-pool invariant.** Warmup uses the same `pool_` as `render`,
so cached cells reference valid `StyleId`s when blitted later.
Allocating a separate pool would silently corrupt subsequent
paints.

**Failure signature without warmup.** On thread switch to a
tool-heavy thread:
1. Picker disappears.
2. ~500 ms of black.
3. Transcript paints from top, fast-scrolling to bottom.
4. Composer snaps into place.
5. Often a residual “ghost” of the picker visible behind the
   transcript for a frame or two.

With warmup: the transcript appears in one frame, no ghosting,
sub-millisecond paint.

**Measured wins** (see commit `agentty 6eb50c4` body for the
detailed table): 6 turns × 800 lines: 80 ms → 0.57 ms; 80 turns ×
500 lines: 687 ms → 5.69 ms.

---

## 7. Canvas shrink + frozen trim

Two independent caps keep long sessions from drifting into
per-frame multi-ms territory.

### 7a. Canvas shrink (maya side)

**Pin location.** `maya/src/app/app.cpp` — `Runtime::render`, inline
path, around line 320:

```cpp
constexpr int kMinCanvasHeight = 500;
const int prev_content_rows = content_height(canvas_);
const int shrink_target = std::max(kMinCanvasHeight,
                                   prev_content_rows + 64);
const bool oversized = canvas_.height() * 2 > shrink_target * 3;

if (canvas_.width() != w
    || canvas_.height() < kMinCanvasHeight
    || oversized) {
    canvas_.set_style_pool(&pool_);
    const int target_h = oversized
        ? shrink_target
        : std::max(kMinCanvasHeight, canvas_.height());
    canvas_.resize(w, target_h);
}
```

**Why.** `canvas_.clear()` streams over `width × height` cells every
frame. Once a tall transcript bumps the canvas to e.g. 6000 rows
(via the grow-and-retry path right below it), `clear()` keeps
costing 6000 × ~100 cells per frame forever — even after a trim
drops the actual content back to a few hundred rows. The shrink
trigger reclaims memory eagerly: `height × 2 > target × 3` is
1.5× — tighter than 2×, which on tool-heavy sessions where
all-time-peak content was a 500-line write that has since trimmed
leaves the canvas oversized.

1.5× still avoids resize thrash on normal grow/shrink cycles: a
30-row turn appended on top of a 400-row steady-state lands at
430, well below the 600 trigger.

**Failure signature if you regress.** Steady-state per-frame time
creeps up over a long session. Profile with `MAYA_FRAME_PROF=1` —
look at `rt=` (render_tree) vs total; `clear()` time is hidden but
proportional to `canvas.height()`.

### 7b. Frozen trim (agentty side)

**Pin location.** `src/runtime/app/update/frozen.cpp` —
`trim_frozen_if_oversized()`.

```cpp
constexpr std::size_t kFrozenMaxRows    = 600;   // primary: bound canvas height
constexpr std::size_t kFrozenMaxEntries = 60;    // secondary: pathological tiny entries
constexpr std::size_t kKeepMinEntries   = 3;     // never drop the immediate context

if (frozen_row_total <= kFrozenMaxRows
    && frozen.size() <= kFrozenMaxEntries) return maya::Cmd<Msg>::none();
// drop oldest WHOLE entries (front) until both caps satisfied,
// keeping >= kKeepMinEntries; frozen / frozen_rows / frozen_row_total
// stay in lockstep...
return maya::Cmd<Msg>::commit_scrollback_overflow();
```

**Why ROWS, not entries.** `m.ui.frozen` is borrowed by maya every
frame via `list_ref`, and the inline canvas auto-sizes to
`frozen_row_total + chrome`. Maya then re-runs THREE O(rows×width)
passes per frame: `render_tree` (layout/measure), `canvas.clear()`,
and the canvas/shadow witness scan. So per-frame cost — and the
spinner/input lag the user feels on a long thread — scales with
total frozen ROWS, not entry count. A single full `write`/`edit`
body is hundreds of rows in ONE entry, so an entry-count cap alone
can't bound the canvas. ~600 rows ≈ a few full viewports; warm
per-frame render stays ~0.2 ms there (vs ~25–240 ms when a tall
body is left un-capped). Trimmed entries are NOT lost — they remain
on disk in `m.d.current.messages` AND in the terminal's native
scrollback (painted live once, full body and all). Only the in-app
re-render window shrinks; `show_all` bodies are never collapsed.

**The row count is estimated from tc.args, NOT tc.output().** A tool
card renders its body from its ARGS — `write` shows `args["content"]`
(the whole new file), `edit` shows every hunk under `args["edits"]`,
`read`/`grep` show their result text. `tc.output()` is only the
one-line "wrote N lines" footer. The original estimate counted only
output, so a 3000-line write was scored at ~1 row — `frozen_row_total`
read tiny, the cap never tripped, and the canvas ballooned to
thousands of rows while the model believed the thread was small.
This was THE root cause of the long-thread render lag; see
`estimate_msg_rows` in `frozen.cpp`.

**The `commit_scrollback_overflow` Cmd is the safe variant.** It
lets maya derive the safe row count itself
(`max(0, prev_rows - term_h)`); the older row-counted
`commit_scrollback` was retired because no caller outside the
renderer can know the right physical-row count (see
`maya/docs/scrollback-corruption-audit.md` finding #1, if present).

**Where it's called.** The trim Cmd is issued from the same
reducer steps that grow `m.ui.frozen` — primarily after
`freeze_through` runs in `submit_message`. Search for
`trim_frozen_if_oversized` to find the call sites.

---

## 8. Activity indicator placement (subtle but related)

**Pin location.** `build_live_tail` in
`src/runtime/view/thread/conversation.cpp`.

When the head of the live tail is a freshly-pushed Assistant with
no text + no streaming bytes + no tool calls, the “thinking…”
indicator is injected **into the Turn body** as a
`maya::ActivityIndicator` Element. `conversation_config` sets
`cfg.in_flight = std::nullopt` — the indicator is NOT a separate
free-floating row below the conversation.

**Why.** A free-floating `in_flight` indicator below the
conversation list would either:

- Add a row to the live tail that disappears the instant the first
  streaming byte arrives → row diff ghost.
- Stay present and double-render alongside the assistant's body.

By living *inside* the assistant Turn's body, the indicator's
disappearance is just the Turn rebuilding without it — a clean
content swap inside a fixed-height (or growing) Turn slot.

**Failure signature.** A spurious row flicker the instant
streaming starts on a fresh assistant turn.

---

## 9. The maya inline render state machine (read-only context)

You don't normally touch this, but understanding it helps when
something looks wrong on the wire.

**Pin location.** `maya/include/maya/render/inline_frame.hpp` and
`maya/src/render/inline_frame.cpp`.

Six type-states: `Empty → Fresh → Synced → (Stale | HardReset) → Sealed`.
The runtime stores an `InlineCoherence` variant. Every legal
transition is a member function consuming the predecessor by
value-move; every illegal transition is a compile error.

Key transitions:
- `Empty.seed() → Fresh` (first render about to fire).
- `Fresh.render() → Synced | Stale | HardReset` (case A: paint from
  the cursor's current position downward).
- `Synced.verify() → optional<ShadowWitness>` (shadow hash check).
- `Synced.render(witness) → Synced | Stale | HardReset`.
- `Stale.render() → …` (case B: cursor walks to the frame top,
  paint in place, erase below — **no** scrollback wipe).
- `HardReset.render() → …` (emits `\x1b[2J\x1b[3J\x1b[H` first —
  wipes viewport AND saved lines — then case A).

`commit(ScrollbackMarker)` on `Synced` releases overflowed rows
into native scrollback by shifting `prev_cells` up.

**When the runtime promotes to HardReset:**
- Terminal resize (in `Runtime::handle_resize` — fs goes Divergent,
  inline Synced/Stale → HardReset; Empty/Fresh stay put).
- Hard I/O error in the writer (residue drain failure).

**When the runtime demotes to Stale:**
- `verify()` returns `nullopt` (shadow hash mismatch — host wrote
  to the wire bypassing the renderer, or memory was clobbered).
- The application calls `force_redraw()`.

You should rarely have to think about this layer from agentty —
the agentty-side `commit_scrollback_overflow()` Cmd is the only
direct hook into it. Just know that if a state machine
transition looks wrong on the wire, the bug is almost certainly
in the *content* (one of pins 1–8), not in the state machine.

---

## 10. Recovery checklist

If inline scrollback rendering is broken after your changes:

1. **Diff against this file.** Run `git diff` against the verified
   commits at the top. If you've touched `app_layout.hpp` or
   `conversation_config()`, revisit pins 1 and 2.
2. **Build clean.** `cmake --build build -j10`. Examples in maya
   are pre-broken (`StreamingMarkdown` move-assign on `std::mutex`);
   ignore those errors — `libmaya.a` itself builds.
3. **Profile.** Set `MAYA_FRAME_PROF=1` (writes to
   `/tmp/maya-frame-prof-<pid>.log`). Look at `rt=` and `cf=` —
   anything over ~2 ms steady-state is suspicious. Also
   `AGENTTY_VIEW_PROF=1` (writes to `/tmp/agentty-view-prof.log`)
   for frozen/live-tail sizes per `conversation_config` call. And
   `AGENTTY_STREAM_PROF=1` (writes to `/tmp/agentty-stream-prof.log`)
   for the in-flight StreamingMarkdown widget's per-frame
   `set_content+finish+build()` cost — isolates the live-tail widget
   from the timeline render; skips the settled fast-path.
4. **Resume profile.** `AGENTTY_LOAD_PROF=1` (writes to
   `/tmp/agentty-load-prof.log`) — surfaces `rehydrate_frozen` +
   `release_to_kernel` timings on `ThreadLoaded`.
5. **Confirm warmup is firing.** Add a `fprintf(stderr, ...)` in
   `Runtime::warmup_render` (or watch the `MAYA_FRAME_PROF` log
   for a “double-render” pattern at thread swap). Without warmup,
   the next-frame `cf=` after `ThreadLoaded` will be tens to
   hundreds of ms; with warmup, it's sub-ms.

If the symptoms match a known failure signature above, restore
that pin from this doc.

---

## 11. File map

| Concern                                      | File                                                            |
|----------------------------------------------|------------------------------------------------------------------|
| Mode + outer layout                          | `maya/include/maya/widget/app_layout.hpp`                       |
| Borrowed frozen prefix wiring                | `src/runtime/view/thread/conversation.cpp`                       |
| Frozen builder + trim                        | `src/runtime/app/update/frozen.cpp`                              |
| Divider source of truth                      | `maya/include/maya/widget/conversation.hpp` (`divider_rule`)    |
| `needs_warmup_render` field                  | `include/agentty/runtime/model.hpp`                              |
| `needs_warmup_render` set                    | `src/runtime/app/update/picker.cpp` (`ThreadLoaded`)             |
| `needs_warmup_render` clear                  | `src/runtime/app/update.cpp` (top of `update`)                   |
| Program hook                                 | `include/agentty/runtime/app/program.hpp`                        |
| Maya warmup consumer (run loop)              | `maya/include/maya/app/app.hpp` (`Program<P>::run`)             |
| Maya warmup implementation                   | `maya/src/app/app.cpp` (`Runtime::warmup_render`)                |
| Canvas shrink trigger                        | `maya/src/app/app.cpp` (`Runtime::render`, inline path)         |
| Inline state machine (read-only)             | `maya/include/maya/render/inline_frame.hpp`                      |
| Compose / serialize internals (read-only)    | `maya/src/render/serialize.cpp` (`compose_inline_frame_impl`)   |

---

## 12. What this doc deliberately does NOT cover

- Widget-level layout inside individual Turns (agent timeline,
  tool body previews). See `RENDERING.md` + `UI.md`.
- The composer's chip/cursor rendering. Owned by
  `maya::Composer`; agentty just constructs a Config.
- Fullscreen mode. agentty uses inline exclusively.
- Per-event `hash_id` in `AgentTimeline`. Tested + rejected as a
  net regression for typical workloads. Don't add it without
  benchmarking against the failing test
  `test_agent_timeline_per_event_hash_id_bounds_cost`.
- Backpressure / non-blocking write residue. Handled inside maya
  (`writer_->has_residue()` / `try_drain_residue()`). Agentty
  doesn't need to think about it.
