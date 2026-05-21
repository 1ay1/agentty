# Fix 4 — bottom-anchored composer (deferred design)

Status: **deferred.** Multi-day surgery. Not safe to ship in a single
session. This doc captures the analysis so the next person to pick it
up doesn't redo the survey.

## What ships today (idle and streaming)

agentty runs maya in **inline mode**: the rendered frame occupies the
last N rows of the terminal viewport, with the host's pre-existing
scrollback above. Each frame is composed by walking the canvas
top-to-bottom, diffing per-row against `state.prev_cells`, and emitting
per-row VT sequences (`\r\n` to advance, cursor moves + cell emit on
changed rows). See `maya/src/render/serialize.cpp ::
compose_inline_frame_impl`.

The composer is the **last** widget in `AppLayout`'s vertical column
(`src/runtime/view/view.cpp`), so its rows always land at the bottom of
the canvas. Its absolute viewport Y is `term_h - composer_h .. term_h -
1` only once the total frame is tall enough to fill the viewport;
before that it floats with the frame's bottom edge.

## The flicker the user perceives

Two compounding sources:

1. **Composer height jitter.** Single-char transitions
   (empty placeholder → typed 1 char → empty again, or 1 line → 2 lines
   on word-wrap) change `body_rows.size()`. The composer's box height
   changes → AppLayout's column reflows → every row above shifts by ±1
   canvas-Y → the per-row diff sees "different" at every row and
   repaints the whole frame.

   **Mitigation already shipped** (`91ef14a` + maya `6aee826`):
   `Composer::Config.min_body_rows = 2` pins the body floor so the
   empty↔first-char wobble no longer reshapes the box. Does not fix
   the wrap-driven height change, but kills the most frequent source.

2. **Thread growth shifts the composer's canvas-Y.** During streaming
   the thread region above the composer grows by 1 row per emitted
   line. Even though the composer's RENDERED BYTES are byte-identical
   to the previous frame, its CANVAS-Y has moved (was at row 47 in
   prev, now at row 48 in cur). The diff walks `cur[y]` vs `prev[y]`
   at fixed y, sees difference everywhere from `first_changed` down,
   and re-emits the composer every tick.

   **This is what Fix 4 would address.**

## The architectural change required for Fix 4

The diff in `compose_inline_frame_impl` is **position-tracking**: it
compares cells at the same `(y, x)` between frames. Bottom-anchoring
requires switching the composer's slice of the canvas to
**content-tracking**: compare the bottom K rows of `cur` against the
bottom K rows of `prev` regardless of where they sit on the canvas.

Two parts to that, both load-bearing:

### Part A — canvas layout: declare which rows are anchored

maya's `Canvas` today is a flat `width × content_rows` cell grid with
no notion of regions. The compose path knows `content_rows` and `W`
and nothing else about what those rows mean. To bottom-anchor the
composer we need:

- A marker on a subtree (`Element | anchor_bottom`) that the layout
  pass records as "this subtree's rows form an anchored region of
  height K."
- `Canvas` (or a sidecar struct passed alongside) carries
  `anchored_rows = K` so compose knows the bottom K rows of the
  canvas are the anchored region.
- AppLayout (in agentty `src/runtime/view/view.cpp`) marks the
  composer subtree with the anchor.

Affected files (maya): `include/maya/element/element.hpp`,
`include/maya/dsl.hpp`, `src/render/canvas.cpp`,
`include/maya/render/canvas.hpp`, plus whichever layout pass walks
elements and writes to `Canvas`.

### Part B — compose: content-tracking diff for the anchored tail

In `compose_inline_frame_impl` (serialize.cpp), the anchored tail
needs a separate code path:

1. Diff `cur[content_rows - K .. content_rows)` against
   `prev[prev_rows - K .. prev_rows)` — i.e. the bottom K rows of
   each, identifying them by their bottom-relative index, NOT their
   absolute canvas-Y.
2. If the anchored tail is byte-identical (the common case during
   streaming — the composer's text didn't change), emit **no bytes
   for those rows**. Their wire content is already correct; the
   `\r\n` walk from the changed upper rows naturally lands the
   cursor past the unchanged tail.
3. If the upper region grew, the upper rows' diff still uses
   position-tracking, but the cursor positioning math at the end
   must place the cursor at `(content_rows - 1, *)` for the
   `state.wire_cursor_rows_` invariant to hold — which means walking
   PAST the unchanged anchored rows without emitting them, and
   without scrolling.

The hard part is step 3 under inline mode's "growth past viewport
bottom scrolls native scrollback" semantics. Walking past unchanged
anchored rows by `\r\n` at the viewport bottom edge **scrolls** —
which is fine if rows above need to fall off, but the diff above also
already accounts for the scrolling, and now the row identities have
shifted between what compose THINKS shifted and what actually shifted.

### Part C — shadow-of-wire bookkeeping

`state.prev_cells_` today is indexed `[y * W + x]` with y the
canvas-Y. The bottom-K rows after a frame where prev_rows ≠
content_rows need their identity preserved across the index shift.
Three options:

1. **Memmove the anchored tail in prev_cells** so its absolute index
   matches its new canvas-Y. Cost: O(K × W) per growth frame. Plus a
   re-hash of `shadow_hash_`.
2. **Split `prev_cells_` into two slabs**, one for the floating upper
   region indexed [0, prev_rows - K), one for the anchored bottom
   indexed [0, K). The shadow-of-wire hash covers both. The verify
   loop walks both ranges.
3. **Keep prev_cells_ flat, but track `anchor_base_y` separately** so
   the bottom-K reads at `prev_cells_[(prev_anchor_base + i) * W +
   x]` regardless of where it sits in the flat buffer. Requires the
   most surgery in serialize.cpp's loop bounds but no memmove.

(2) is probably the cleanest but most invasive — `InlineFrameState` is
a careful invariant carrier and its layout is depended on by
`verify_shadow`, `commit_prefix`, the witness chain. Anyone changing
it has to re-prove the invariants in inline_frame.hpp.

## Things you must NOT break while doing this

These are invariants the existing tests + production rely on. Every
change in this area has historically tripped at least one of them.

- **Inline-mode scrollback safety.** The host's pre-existing rows
  above the frame top must NEVER be overwritten. Today's compose has
  three explicit guards (case (A) `\r`-anchor, case (B) viewport-cap
  in force_redraw, the scroll_n math for oversized force_redraw); a
  Fix 4 implementation must add a fourth: walking past unchanged
  anchored rows by `\r\n` at the bottom edge must account for the
  scroll-off into native scrollback, OR use a non-scrolling cursor
  positioning sequence (absolute cursor positioning, but that breaks
  inline mode's "share the viewport with the host" contract — there
  is no absolute origin we can address).

- **Shadow-of-wire hash invariant.** Every cell of `prev_cells_[0,
  content_rows × W)` must, at end of compose, equal the cell the wire
  shows. Any path that emits bytes without updating prev_cells (or
  vice versa) lets the wire and the shadow drift; the next frame's
  diff then emits bytes computed against a lie. The
  `MAYA_DEBUG_SHADOW_VERIFY` build catches this; CI should run with
  it on during the Fix 4 development cycle.

- **ComponentElement cells-blit corruption** (the
  `cells_rows < content_h` path; see `maya/tests/test_card_border.cpp`).
  Setting `ev.hash_id` on terminal events for cached subtrees triggers
  the bug. AgentTimeline's hash_id branch is deliberately disabled in
  agentty for this reason. Fix 4 must NOT route the anchored tail
  through ComponentElement caching as a shortcut.

- **DECAWM-off persistence.** Compose emits `\x1b[?7l` once and
  persists `state.decawm_off_ = true` across frames. The Fix 4 path
  must not emit a frame that puts the wire into DECAWM-on without
  flipping the bit.

- **Cursor-row invariant for the next compose.** At end of every
  compose, `state.wire_cursor_rows_` must equal the row the terminal
  cursor is actually on (1-indexed from frame top). Walking past
  unchanged anchored rows must end with the cursor at
  `(content_rows - 1, x)` for some x, NOT mid-frame.

## Smaller wins to ship first

If the full Fix 4 is too big, these are intermediate steps that each
deliver some of the win without the canvas-region rewrite:

1. **Composer min height pin** — already shipped. Knocks out the
   keystroke-driven height wobble.
2. **Pin composer to a fixed height during streaming** (e.g. cap at
   3 body rows + chrome regardless of actual line count). Trades the
   user's ability to see a long composed message for stability.
   Reversible per-state.
3. **Re-introduce maya's bandwidth coalesce** (the old
   `min_changed_rows` hold), but gated on `phase.is_streaming()` AND
   `change is exactly the composer's K bottom rows`. Bypassed for
   any keystroke event so typing latency doesn't suffer. Requires
   threading the "this is a streaming-driven render" signal from
   agentty into maya — agentty already has it (`phase`); maya does
   not surface a knob today.
4. **Switch to alt-screen mode during long sessions.** Heavy-handed
   (drops the "inline with shell scrollback" UX) but eliminates the
   whole class of inline-mode scroll-driven repaints — the alt screen
   has fixed dimensions and absolute cursor positioning is free.
   Probably right for a future "expanded" mode but not the default.

## Estimated effort (full Fix 4)

- Part A (canvas marker + layout wiring): 1 day, mostly mechanical.
- Part B (compose path content-tracking diff): 2–3 days. The
  cursor-positioning math under inline-mode scroll semantics is the
  bulk of it; needs new unit tests in `maya/tests/test_scrollback.cpp`
  and `test_scrollback_vt.cpp` covering each growth/shrink/anchored
  combination.
- Part C (shadow-of-wire bookkeeping under anchor shift): 1 day if
  option (2) (split slabs) wins, 2 days if option (3) (anchor_base_y)
  wins.
- Smoke testing + flicker A/B against current build: 1 day.

Floor: 5 days for one engineer who already knows the maya render
pipeline well. Realistic: 7–10 days including the time to discover
which invariant the first attempt broke.

## How to validate it actually helped

There is no automated visual-diff harness. Manual A/B:

1. Build `main` and `fix-4` branches side-by-side.
2. Run identical sessions in both: stream a 50-line response, type a
   single character mid-stream, watch the composer.
3. Record the raw VT byte stream from each via the existing
   `MAYA_TRACE_BYTES=path` knob (see `maya/src/terminal/writer.cpp`).
   Compare byte counts per second. Fix 4 should drop composer-region
   bytes to ~0 during streaming (only the spinner cells re-emit).
4. Confirm scrollback content above the frame is identical in both
   builds — the easiest way to break Fix 4 is to scroll the host's
   prior output off the top accidentally.

## Owner / next step

Unowned. When picked up, start by reading:

1. `maya/src/render/serialize.cpp` lines 345 onward (the entire
   `compose_inline_frame_impl` function).
2. `maya/include/maya/render/inline_frame.hpp` — the witness chain
   type-state machine. New paths must thread through the same chain
   or the shadow-of-wire verification is bypassed.
3. `maya/tests/test_scrollback.cpp` + `test_scrollback_vt.cpp` —
   every invariant the current diff guards has at least one test
   here. Fix 4 must add corresponding tests for the anchored path
   BEFORE writing the implementation, not after.

Then sketch the diff loop on paper for the four cases (no growth,
growth above anchor, shrink above anchor, anchor itself changes)
before touching code. Most of the prior false starts in this area
came from skipping that step.
