# Fix 4 — bottom-anchored composer (REVERTED)

Status: **reverted**. Shipped briefly as maya `3ab6437` + agentty
`f3e991a`, both reverted after streaming-time ghosting + welcome-
screen scrolling regressions surfaced in real terminals.

## Why it was reverted

The DECSTBM-pinned anchored-region approach was sound on paper but
the anchored-steady upper-diff path mutated `prev_cells` and emitted
`\r\n` walks inside the scroll region. Inside DECSTBM there is no
local scrollback: rows scrolled off the top of the region are
discarded, not banked. That violated two assumptions:

1. The standard inline diff treats `prev_cells` as a faithful mirror
   of what's on the wire. The anchored-steady path bypassed the
   normal scroll path, so the mirror desynced once upper-region
   growth was sustained — manifesting as duplicated assistant
   prose and stray composer fragments mid-thread.
2. The welcome screen has `content_rows < term_h` so anchor was
   meant to stay inactive, but transient size matches activated
   it, then welcome-screen ticks (animation) `\r\n`-scrolled the
   ASCII wordmark inside the DECSTBM region, smearing the glyphs
   vertically.

## What the reference does instead

`third_party/maya/examples/agent_session.cpp` builds the composer + status bar
as the tail of a normal `vstack`, no anchor pipe. It exhibits zero
ghosting because the standard per-row diff already handles the
streaming case correctly when `prev_cells` is in sync — the bytes
written for the composer rows change only when the composer's
canvas content changes.

The original flicker symptom Fix 4 targeted (composer re-emit on
every streaming tick because canvas-Y shifts) is real, but the
right place to fix it is upstream: keep the composer's canvas-Y
stable by either pinning composer to fixed rows in `AppLayout`
or letting the diff treat trailing-stable runs as no-ops. Neither
needs DECSTBM.

## Files left clean by the revert

- `third_party/maya/include/maya/element/box.hpp` — `anchor_bottom` field gone.
- `third_party/maya/include/maya/dsl.hpp` — `| anchor_bottom()` pipe gone.
- `third_party/maya/include/maya/render/canvas.hpp` + `src/render/canvas.cpp` —
  `anchor_top_y_` field + resets gone.
- `third_party/maya/include/maya/render/serialize.hpp` — `anchor_rows_` /
  `anchor_term_h_` state fields gone.
- `third_party/maya/src/render/serialize.cpp` — DECSTBM decision, tear-down,
  activate hook, anchored-steady diff branch all gone. Back to
  legacy emit on every frame.
- `third_party/maya/include/maya/widget/app_layout.hpp` — composer group no
  longer wrapped.

## What stays from the session

- `composer.cpp::min_body_rows = 2` — pins composer body height
  across empty↔first-char so canvas-Y of the status bar doesn't
  jitter on the first keystroke. Independent fix, still wanted.
- `program.hpp::visual_hash()` 33 ms animation bucket — keeps
  welcome wordmark + idle pulses ticking. Independent fix, still
  wanted.

## If we revisit

Anchoring is doable but needs a different mechanism than DECSTBM:

- Track which canvas rows are "tail-stable" between frames and
  emit only `\x1b[K` + cursor reposition for them, never re-paint.
  The diff already short-circuits byte-identical rows; the bug is
  that canvas-Y shift makes them not-byte-identical at the same y.
- OR: reserve the composer's rows at fixed canvas-Y in the view
  layer and grow the assistant body upward via a fixed-height
  scrollback widget. This punts to view geometry, no terminal
  trickery needed, matches what the reference example already
  does with `Mode::Inline`.
