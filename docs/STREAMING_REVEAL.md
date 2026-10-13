# Streaming Reveal — the typewriter animation, its bugs, and how they were fixed

This documents the live markdown "typewriter" reveal (the text gliding in as
the model streams), the burst/stutter bugs that plagued it, and — most
importantly — the *method* that finally fixed them after several failed
attempts. Read the method section first if you're here to debug a new reveal
issue: the tooling is the point.

## The pipeline in one paragraph

Wire deltas arrive bursty (fat SSE chunks with idle gaps). agentty does **no
host-side pacing**: on each Tick it moves `pending_stream → streaming_text`
and feeds the widget the full `text + streaming_text + pending_stream`
(`turn.cpp`). All animation lives in maya's `StreamingMarkdown` reveal-fx: a
fractional **reveal cursor** (`reveal_cp_`, integrated by
`maya::anim::RateCursor`) walks left-to-right; `build()` clips the rendered
tail to the cursor (`reveal_byte_clip_`) and an overlay paints the
scramble/gradient/caret on the trailing edge. Display should always be
`buffer[0 : cursor]`, with the cursor gliding at a readable rate independent
of how bursty the wire is — a jitter buffer, like a video player.

## The method (this is the important part)

The reveal bug was "fixed" many times against instruments that **lied**, and
each time it came back. What actually worked:

1. **Do not trust a synthetic probe.** `reveal_smoothness_probe` fed bytes
   smoothly (a few per frame), so a *burst never happened* — it reported
   "smooth" while the real app bursted. A test that can't reproduce the
   failure will bless a broken fix.

2. **Replay REAL recorded bytes, deterministically.** `anthropic_md_stream`
   has recorded fixtures (`tests/fixtures/anthropic_md_tour.jsonl` — real
   Anthropic delta sizes + timing). Its `det` mode is single-threaded on a
   **frozen anim clock**: it appends each delta at its recorded `t_ms`,
   renders every frame, and measures the per-frame visible-char delta. Every
   value (source / clip / cursor / visible) is read at one consistent
   instant — no producer-thread race, no wall-clock jitter. An earlier
   `--trace` mode used a producer thread + wall clock and its numbers raced
   (it once printed `src=9` and `visible=303` in the same line — garbage).
   **Determinism is non-negotiable for a measurement you'll tune against.**

   ```
   ./build/anthropic_md_stream det tests/fixtures/anthropic_md_tour.jsonl
   # → max_frame_delta=… frames_over_24=…   (bounded per-frame delta = smooth)
   ```

3. **Measure the REAL running binary when the harness can't reproduce it.**
   The last two bugs only appeared in the real app (bursty wire with idle
   gaps + tool cards) which the fixture didn't capture. `AGENTTY_STREAM_PROF=1
   ./build/agentty` logs one line per streaming frame to
   `/tmp/agentty-stream-prof.log`:

   ```
   [stream] src=182 clip=182 dclip=+142 live=1 finalizing=0 settled=0 fastpath=0 build_us=230
   ```

   `dclip` (jump in the real reveal cursor) is the honest burst signal:
   `+2..+8` is a glide, `+100+` is a paste. `live` / `finalizing` / `settled`
   pinpoint *which* mechanism. This log — read from an actual turn the user
   saw burst — is what found bugs #2 and #3.

4. **The screen is the only real metric.** "frames_over_24" and "idle-at-edge
   %" are proxies. A human watching one turn is the final arbiter; the tests
   exist to stop *regressions* after that confirmation.

## The four bugs (in the order they were peeled back)

### Bug 1 — the in-progress line pasted whole (`build.cpp`)

`visible_end` rounded the reveal clip UP to end-of-line. But the *in-progress*
final line has no trailing `\n` yet (true of every delta as it arrives), so it
rounded to the source end — revealing the whole just-arrived line in one
frame. The cursor gated nothing.

**Fix:** round up to end-of-line ONLY for *completed* lines (a `\n` exists —
needed so a row scrolling into immutable scrollback is never frozen
half-revealed). The in-progress line clips AT the cursor and types out
glyph-by-glyph. A **monotonic clamp** (`visible_end` never below the previous
frame's `cached_tail_clip_`) prevents a height-shrink when a line completes.
`det`: `frames_over_24` 104 → 21.

### Bug 2 — the tool-boundary paste (`turn.cpp` + `snap_reveal_to_edge`)

At a tool card, `turn.cpp` called `snap_reveal_to_edge()` which hard-set
`reveal_cp_ = total_cp` — pasting the whole typed-but-unrevealed backlog in
one frame ("first char sticks, then it all appears with the next tool").
Snapping is load-bearing for scrollback safety (a growing card must not strand
a lagged inline row — `scrollback_oracle_test`), so it can't simply be
removed.

**Fix:** `snap_reveal_to_edge(glide_ms)`. With `glide_ms>0` it arms a
HARD-deadline finalize ramp so the cursor SPRINTS to the edge over ~150 ms — a
fast but VISIBLE catch-up instead of a paste — still landing quickly enough to
be scrollback-safe. `glide_ms=0` keeps the instant snap for the discrete
resize-safety path. turn.cpp's tool-exit uses `snap_reveal_to_edge(150)` and
defers `finish()` to the existing phase-1/phase-2 exit once the glide lands.
Reproduce with `det --snap-at N [--snap-glide M]`.

### Bug 3 — premature drain settled the widget mid-stream (`turn.cpp`)

`text_gone_quiet` called `request_finalize()` whenever bytes paused >120 ms.
But a slowly-streaming model routinely pauses that long BETWEEN deltas while
still mid-message. The finalize ramp completed and flipped the widget
`live_` off; the next delta re-lived it and pasted the whole delta. The
profiler was decisive: **161 of 427 frames were wrongly `live=0`** during a
live stream, and every `+100`-cell burst was immediately preceded by a
`live=0` idle run.

**Fix:** gate `text_gone_quiet` on `!wire_streaming_here`
(`!m.s.is_streaming()`) so it only drains at the true text→tool seam, not on a
normal inter-delta gap. `text_block_closed` (explicit end-of-text) still
drains. `live=0` frames: 161 → 3.

### Bug 4 — the cursor outran the wire and FROZE (pacing, `turn.cpp` + RateCursor)

With the settle fixed, the reveal glided each fat delta to the edge at the
90 cps floor, then sat **frozen** until the next delta arrived — profiled at
~57 % of frames idle-at-edge (`dclip=+0 live=1`). That reads as stop-and-go,
not a typewriter. The reveal can only show bytes that have arrived; at 90 cps
the cursor drains faster than a bursty wire delivers, so it perpetually
catches up and waits.

**Fix (two parts):**
- maya `RateCursor`: on idle frames (`backlog<=0`) **decay `smoothed_rate_`
  toward `floor_rate_`** instead of early-returning with a stale high rate, so
  each new delta begins its glide at the readable floor.
- `turn.cpp` pacing: **45 cps / 0.40 s lag** (was 90 / 0.15). A lower floor +
  larger lag holds a continuous buffer so the cursor glides THROUGH the idle
  gaps instead of freezing. Reproduced idle-gap pattern: idle-at-edge 54 % →
  <1 %. This was only safe because Bug 2's fix replaced the tool-boundary
  paste with a bounded glide, so the larger steady-state backlog no longer
  bursts at a tool card (`scrollback_oracle_test` green).

## The load-bearing constraint (don't undo this by accident)

Committed rows scroll into the terminal's **native, immutable scrollback**.
Once a row is there it can never be repainted. So the reveal must never leave a
row *half-revealed* if it might scroll off, and its rendered bytes must be
byte-identical to the eventual committed render. This is why:
- completed lines round the clip up (never a partial committed row),
- the ghost band uses `conceal` (occupy width, paint nothing) rather than
  truncating (which would change height and reflow),
- the tool boundary must reach the edge within a bounded window.

`reveal_scrollback_test`, `scrollback_oracle_test`, and `scrollback_wire_fuzz`
guard this. If you change reveal pacing or clipping, run them.

## Robustness features (added after the core fixes)

- **Adaptive floor** (`RateCursor::set_adaptive`, enabled in `turn.cpp`): the
  reveal estimates the wire's actual delivery rate (EMA over wall time, so
  idle gaps pull it down) and auto-tunes its floor to match, clamped to a
  readable band (25..180 cps). This makes the glide smooth across models of
  very different throughput without a hand-picked constant. The fixed
  45 cps / 0.40 s is now only the cold-start seed. A/B with `det --adaptive`.
- **Speed-scaled shimmer**: the scramble/gradient trail's time constants scale
  inversely with the glide rate (`RateCursor::effective_rate()`), so the shimmer
  covers a consistent *spatial* window whether text flies or crawls. The
  finalize settle gate is widened by the max scale so a slow-wire scramble
  can't freeze glyphs on settled text.
- **CI gate** (`reveal_stream_gate` ctest): `anthropic_md_stream det
  --assert-max-delta N` fails if any streaming (non-finalizing) frame reveals
  more than N cells — the guard that would have caught every burst bug.
  Finalizing frames (the deliberate land-the-tail glide) are excluded.
- **Capture** (`anthropic_md_stream capture <out.jsonl>`): records a fresh
  real fixture (live billed API call — manual dev action).

## Audit fixes (second pass, code-review sweep)

A close re-read of the cursor/ramp/host seams found six latent issues; all
fixed together (reveal_fx.cpp / animation.hpp / stream.cpp / turn.cpp):

1. **Hard vs adaptive deadline** (`finalize_hard_`): `snap_reveal_to_edge`'s
   glide promised a hard wall-clock window but shared `finalize_deadline_ms_`
   with `request_finalize`, so the #4 adaptive re-eval stretched a 150 ms
   tool-seam snap to up to 2.5 s on a big backlog (deferred tool cards stayed
   hidden the whole time). Hard ramps are now exempt from the stretch; late
   GROWTH still completes on time via the deadline rate.
2. **Ramp disarm on resume**: `finalize_armed_` was only cleared at settle, so
   a mid-message `request_finalize` (text→tool gap, `text_block_closed`)
   left the widget in ramp mode for the REST of the turn once more text
   arrived — cursor cruising ≥2× floor, jitter buffer collapsed. Growth
   arriving after the cursor reached the armed-time edge
   (`reveal_edge_reached_ms_ != 0 && size > finalize_armed_size_`) now
   disarms the ramp. The genuine late-delta case (cursor still behind the
   edge) is untouched.
3. **Post-idle carry burst**: the residual-time carry decided "owed vs idle"
   from the CURRENT frame's backlog — but frames stop 4 s after the edge
   settles (caret window), so a chunk arriving after a long model silence saw
   one frame with seconds of stale `reveal_us_` elapsed and drained it at
   ~15× real time (a paste). Growth that lands while the cursor was at-edge
   now re-stamps the µs clock (`reveal_us_ = 0` → re-init on next advance).
4. **Adaptive cold-start seed**: `RateCursor`'s adaptive branch clamped a
   cold (≈0) wire estimate up to `adapt_floor_min_` (25) and never consulted
   the documented seed (`floor_rate_`, 45) — every message's first ~τ typed
   at the band minimum, and a post-pause decayed estimate crawled the same
   way. Below-band estimates now fall back to the seed (then band-clamp).
5. **End-of-turn paste**: `finalize_turn`'s unconditional `settle_message_md`
   (finish() at message_stop) pasted the steady-state backlog (≈wire_cps ×
   0.40 s — a line+) in one frame — tripled in size by the 0.15→0.40 drain
   retune after that trade was accepted. On interactive (non-SSH) terminals
   the turn now ends with the widget's own bounded `request_finalize(200)`
   glide; the deferred settle-freeze (already gated on
   `live_tail_reveal_settled`) settles + freezes after the widget flips
   `live_` off itself. Over SSH/fps=0 the immediate-finish path is kept
   (sparse frames can't hold the height mid-glide — the original stranded-
   duplicate bug). Escape hatch: `AGENTTY_NO_REVEAL_GLIDE=1`.
6. **Instant-snap clock skew**: `snap_reveal_to_edge(0)` stamped only
   `reveal_ms_`; per-frame dt integrates off `reveal_us_`, so bytes arriving
   right after a mid-stream resize snap burst-integrated up to 250 ms. Both
   clocks are stamped now.

Plus two hardenings: the O(1) append-proof in `set_content` samples a third
(midpoint) window so a same-length mid-buffer divergence can't alias as
"unchanged", and `finish()`/`set_live(false)`/settle all clear the new ramp
state so no path leaks `finalize_hard_`.

### Verification hardening (same sweep, follow-up)

The fixes above initially shipped with only *negative* verification (nothing
broke). Three additions make them positively enforced:

- **Finalize-glide gates in the det harness**: `--assert-finalize-max N`
  (no finalizing frame reveals > N cells — catches a #5-class end-of-turn
  paste) and `--assert-finalize-ms M` (the ramp must LAND — `live_` off —
  within M ms of arming; catches a wedged ramp (#2 class) and an unbounded
  deadline stretch (#1 class)). Wired into `reveal_stream_gate` (caps 40 /
  3600 ms; measured healthy: 8–14 cells, 1150–1750 ms) and a new
  `reveal_stream_gate_prod` arm that runs the SHIPPED pacing
  (45 cps / 0.40 s / adaptive) instead of the harness default. A third arm,
  `reveal_stream_gate_snap`, fires a HARD tool-boundary glide
  (`--snap-at 40 --snap-glide 150`) mid-stream at production pacing —
  exercising `finalize_hard_` on real bytes (an unpaced snap measures as a
  56-cell paste and fails the cap; the glide measures 2).
- **`reveal_resume_test`** (ctest): drives the real widget on the frozen
  clock across the three timing edges steady streaming never crosses —
  mid-message finalize then resume (must disarm + restore the jitter-buffer
  lag), hard snap against a ~600 cp backlog (must land by the 150 ms
  deadline), and an 8 s FRAMELESS idle gap then a chunk (per-frame step must
  stay glide-sized). Mutation-tested: reverting any one of the three widget
  fixes fails exactly its check.
- **One settle predicate**: the four-way union (live ∥ finalizing ∥
  reveal-gliding ∥ parsing) that was hand-mirrored across
  `live_tail_reveal_settled`, the view's `reveal_settled` cache-key gate,
  the is_idle settle-skip, and the frame re-arm is now a single widget
  method, `StreamingMarkdown::is_animating()`. The "MUST agree" comments
  are enforced by construction instead of by review.
- **Cadence-gated glide policy**: `reveal_end_glide_enabled()` requires
  BOTH dense-frame signals the streaming tick derives its cadence from —
  not-SSH **and** synchronized-output support. A non-sync terminal ticks at
  100 ms (a 200 ms ramp ≈ 2 frames — the same height-drift geometry as
  SSH), so it keeps the immediate-finish path.

## The block-boundary pop — FIXED (maya 4c47249)

For a while a completed block popped into view whole in one frame at a block
boundary (the ~+170 mid-body pop in the tour fixture, plus eager table/quote
rows under a burst). The prior analysis blamed "the overlay decorates only the
tail leaf" and sketched an invasive per-block source-provenance side-table.
That was the WRONG root cause. The real cause was one line in `build()`.

**Root cause: build() rounded the reveal clip up to end-of-line.** The clip
that gates the rendered tail was rounded UP to the next `\n` whenever that line
was complete ("a completed line renders whole for scrollback safety"). But
Markdown prose is **one source line per paragraph** that soft-WRAPS at render
time — and `build()` has no width, so it cannot know where the wraps fall.
`find('\n')` on a cursor sitting mid-paragraph returns the paragraph's
TERMINATING newline, hundreds of bytes ahead. Rounding to it dumped the ENTIRE
remaining paragraph the instant its closing `\n` arrived (a fat delta). The
reveal cursor gated nothing. Worse, the now-"settled" multi-line block went
through `render_tail`'s component path, so `find_last_text` bailed and the
overlay's eager arm (`line_bounded`) showed every wrapped row above the cursor
whole — exactly the pop.

**Fix: gate the rendered tail EXACTLY at the reveal cursor** — never round a
settled-but-uncommitted line whole (`build.cpp`). This is scrollback-safe
because the tail slice `[committed_, clip)` is entirely uncommitted and
redrawn in place each frame; only `commit_range` (reveal-paced, block-aligned)
freezes bytes into scrollback, and by then the cursor is past the block. A
block-boundary cap (`bcap`) additionally prevents the clip / monotonic clamp
from crossing a blank-line (`\n\n`) boundary the cursor hasn't reached, so a
completed block always stays the tail's last leaf where the overlay conceal
can reach it.

**Measured** (`anthropic_md_stream det ... --adaptive`):

| fixture | before | after | gate (--assert-max-delta 24) |
|---------|--------|-------|------------------------------|
| tour    | 172    | 22    | pass |
| smoke   | 27     | 6     | pass |

Smooth-feed probe PASS; bursty probe (`PROBE_BURSTY=1`) PASS (worst +26, was
+285); `reveal_stream_gate` PASS; `reveal_scrollback_test` (10970 checks) and
`scrollback_wire_fuzz` (20384 checks) green. Reproduce/inspect a frame with
`DET_DUMP_FRAME=N ./build/anthropic_md_stream det tests/fixtures/anthropic_md_tour.jsonl --adaptive`.

### Why the per-block-provenance side-table was NOT needed

The abandoned plan (publish each tail block's `[offset,end)` from
`render_tail`, walk every leaf, byte-compare against the cursor) would have
worked but was far more invasive than the defect warranted, with real
scrollback-corruption blast radius in the 2400-line `render_tail.cpp`. Once
the clip stops rounding, the in-progress block stays a flat `TextElement` that
the existing prose ghost band glides through per-glyph — no provenance table,
no leaf walk, no `render_tail` change. Threads were also never the fix
(profiling: build ~12 µs median / 351 µs max vs a 16 000 µs frame budget — no
bottleneck; the defect was correctness).

## Pre-existing failure: scrollback_oracle_test (NOT this path)

`scrollback_oracle_test` (ctest #60) fails with ~58 "gate recovery"
corruptions on clean HEAD — a long-standing failure that predates the whole
reveal-gate series (verified back through maya `d2d80a8`). The failures are on
tool-turn Running-progress-grow frames (`t1-r1-runNN`, "committed-row mutation
slipped past the type guards") in the TOOL-PANEL commit path, not the prose
reveal path. The clip-gating fix actually improves the count (58 → 36). This
tool-panel corruption is a separate, unfixed defect; do not conflate a red
oracle with a broken reveal.

## Quick reference — debugging a new reveal complaint

1. `AGENTTY_STREAM_PROF=1 ./build/agentty`, do the offending turn, then
   inspect `/tmp/agentty-stream-prof.log`:
   - big `dclip` with `live=1 finalizing=0` → a mid-stream paste (clip/cursor).
   - `dclip=+0 live=1` for many frames → the cursor is frozen (pacing too fast
     for the wire).
   - `live=0` runs during streaming → something is settling the widget early.
   - big `dclip` with `finalizing=1` or `settled=1` → end-of-turn/tool-seam
     glide or flush (usually fine).
2. Reproduce deterministically with `anthropic_md_stream det` (+`--snap-at` for
   tool boundaries, `--cps`/`--drain` to sweep pacing).
3. Fix, re-measure, then confirm on a live turn.
4. Run the scrollback tests before committing.
