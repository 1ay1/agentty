# Fix 4 — bottom-anchored composer

Status: **shipped** (maya `3ab6437`, agentty `f3e991a`). DECSTBM-based
implementation. This doc records the design choices and the known
edge cases that still fall back to legacy emit.

## Goal

Kill the composer-region flicker that streaming-driven thread growth
used to produce. Pre-Fix-4 every emitted upper-region line shifted
the composer's canvas-Y by 1; the per-row diff (which compares
`prev[y]` vs `cur[y]` at fixed y) saw every composer row as
"changed" and re-emitted the entire composer on every streaming
tick.

Post-Fix-4 the composer's bytes on the wire are pinned to a fixed
set of viewport rows by a DECSTBM scroll region; streaming-driven
upper-region growth scrolls only those upper rows, leaving the
anchored bytes untouched.

## What ships

### Element side

- `BoxElement::anchor_bottom` flag (`maya/include/maya/element/box.hpp`).
- `| anchor_bottom()` DSL pipe (`maya/include/maya/dsl.hpp`).
- `AppLayout` wraps `ChangesStrip + Composer + StatusBar` in a single
  `vstack` marked `| anchor_bottom()`. The anchored region therefore
  covers all three of those when present.

### Canvas side

- `Canvas::set_anchor_top(y)` + `Canvas::anchor_top_y()` accessor
  (`maya/include/maya/render/canvas.hpp`).
- The painter (`maya/src/render/renderer.cpp::paint_element`'s
  BoxElement lambda) calls `canvas.set_anchor_top(ay)` when it visits
  a box with `anchor_bottom == true`. Multiple anchored boxes —
  smallest non-negative y wins (topmost anchor covers the most rows).
- `clear()` / `clear_rows()` / `resize()` all reset `anchor_top_y_` to
  -1 so each frame's paint starts with a clean slate.

### Compose side

In `maya/src/render/serialize.cpp::compose_inline_frame_impl`:

- Up-front decision: `K = content_rows - anchor_top_y` if anchor set;
  `want_anchor = K > 0 && K < term_h && content_rows >= term_h`. The
  `>= term_h` gate skips activation until the frame fills the viewport,
  so the early frames don't push host scrollback off-screen.
- Three branches:
  1. **Anchored steady-state** (`anchor_compatible`): K matches state's
     remembered K and term_h matches. Splits the canvas into upper
     [0, A_cur) and anchored [A_cur, content_rows) regions. Diffs the
     upper region position-tracked within the DECSTBM scroll region
     (`\r\n` at the bottom of the region scrolls only the upper area).
     Diffs the anchored region bottom-relatively (cur[A_cur + i] vs
     prev[A_prev + i]); for changed anchored rows, absolute-positions
     the cursor to the anchored viewport row, emits the row, then
     repositions cursor back to the bottom of the upper region.
     **This is the fast path** — streaming ticks land here and emit
     ONLY the upper-region delta + (rarely) the anchored deltas.
  2. **Anchor tear-down** (`anchor_active && !anchor_compatible`):
     emits `\x1b[r` to reset DECSTBM, then `\x1b[2J\x1b[H` to wipe
     viewport, then drops to legacy path for a fresh repaint. One-
     frame visible flash. Fires on composer-height changes (K shifts)
     and on terminal resize.
  3. **Anchor activation** (legacy path's tail, when `want_anchor &&
     state.anchor_rows_ == 0`): after the legacy emit completes
     (cursor assumed at viewport bottom), emits `\x1b[1;<upper_h>r`
     to set DECSTBM, then absolute-positions cursor to viewport row
     `upper_h` (= bottom of upper region) so the next compose's
     anchored-steady path finds the cursor where it expects.

### InlineFrameState additions

- `anchor_rows_` — K from last compose (0 = no anchor active).
- `anchor_term_h_` — term_h that anchor was set up for; mismatch
  triggers tear-down.
- Both reset in `reset_state()` and `abandoned_for_recovery()`.
- `finalize()` emits `\x1b[r` to restore the full scroll region
  before the cursor/DECAWM restores at shutdown so the host shell
  isn't trapped inside a partial scroll region.

## Known limitations / edge cases

- **Tear-down does a viewport wipe.** Composer-height changes (e.g.
  word-wrap pushing body from 2 to 3 rows) trigger a full-viewport
  flash. Mitigated upstream: `composer_config` sets
  `min_body_rows = 2` so empty↔first-char doesn't wobble. Real wrap
  events still flash once.
- **Activation gate `content_rows >= term_h`.** Until the frame fills
  the viewport (early frames of a session), the anchor is inactive
  and the legacy flicker still applies. Acceptable because flicker
  is only perceivable during streaming, which only happens once the
  frame is much larger than viewport.
- **DECSTBM is reset by terminal resize.** maya's resize handler
  already drives a hard-reset path; the next compose enters with
  `state.anchor_rows_ = 0` (post-reset), legacy path repaints, then
  activation fires.
- **`\x1b[2J\x1b[H` in tear-down destroys host scrollback above the
  viewport.** Inline mode's contract was "don't touch host
  scrollback"; tear-down violates that. Trade-off accepted because
  tear-down is rare and the visible result (frame redraws cleanly)
  is better than the alternative (cursor drift + ghost rows).
- **No DECSC/DECRC.** The anchored-row out-of-band emit uses explicit
  cursor positioning instead. DECSC's SGR-save semantics would leak
  upper-region SGR into the anchored emit.

## Files touched

- `maya/include/maya/element/box.hpp` — `anchor_bottom` field.
- `maya/include/maya/dsl.hpp` — `RAnchor` pipe + plumbing.
- `maya/include/maya/render/canvas.hpp` — anchor_top_y accessors.
- `maya/src/render/canvas.cpp` — anchor_top_y reset in clear/resize.
- `maya/src/render/renderer.cpp` — paint-time hook into canvas.
- `maya/include/maya/render/serialize.hpp` — InlineFrameState fields.
- `maya/src/render/serialize.cpp` — the actual compose-path logic.
- `maya/include/maya/widget/app_layout.hpp` — anchored cluster.

## How to validate

No automated visual test. Manual:

1. Open agentty in a terminal that honors DECSTBM (Kitty, WezTerm,
   Ghostty, alacritty, iTerm 3.5+, vte-based, Windows Terminal).
2. Send a prompt that produces a long streaming response (30+
   lines). Watch the composer area; pre-Fix-4 it visibly redrew on
   every token, post-Fix-4 it stays still.
3. Trigger a composer height change (type a long enough line to
   wrap). Expect one visible viewport flash, then steady state.
4. Resize the terminal. Expect one full repaint; subsequent
   streaming should re-enter the anchored fast path.

