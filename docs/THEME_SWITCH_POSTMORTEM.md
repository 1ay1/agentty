# The theme switch that skipped every other entry

A postmortem. Four fixes, three of them for real bugs that were not *this*
bug, and one measurement that ended it in ten minutes.

The lesson is in the ratio.

---

## The report

> "Live theme switch sometimes doesn't work. But it always works when
> something else is animating, like the welcome screen."

and later, on a long thread:

> "After 12 downs Voltage missed rendering. One skips, the next one renders.
> Why can't we force refresh on every down?"

---

## The answer

`demote_to_stale()` changes state. It does not paint.

```cpp
if (retheme_repaint_) {
    ...
    return std::move(arm).demote_to_stale();   // ← emits nothing
}
```

So a theme swap takes **two frames**: one to demote the inline frame out of
`Synced`, and a second one where the `Stale` arm actually writes the ~28 KB
repaint.

Nothing guaranteed the second frame happened.

Hold Down, and the next keypress lands inside that gap. It publishes a new
theme, this arm demotes *again*, and the paint is deferred once more. The
screen sits exactly one keystroke behind — which looks like "every other
entry skips" — and only catches up when you stop pressing.

That is also why an animation hid it. A RAF-driven widget keeps frames
flowing unconditionally, so the second frame always arrived within ~16 ms.
Idle is the only state where a missing frame is actually missing.

And why it needed a long thread: the repaint is ~28 KB. On a short thread
the gap between demote and paint is too small for a key repeat to land in.

## The fix

Latch the debt at the demote, and make the run loop owe a frame until
something paints.

```cpp
// at the demote
retheme_paint_owed_ = true;

// the loop's "do I owe a frame?" predicate
return coalesced_last_render_ || pending_retheme_ || retheme_paint_owed_;

// cleared by the first frame that did not itself demote — i.e. the painter
if (!retheme_repaint_) retheme_paint_owed_ = false;
```

Three lines of logic. Verified: 60 moves → 60 `Stale` repaints, and
`owed=0` on 185 frames, so the latch settles and never spins.

---

## How it was actually found

By printing four numbers. Not by reading code.

The instrumentation was ~20 lines at four points along the pipeline:

| probe | question it answered |
|---|---|
| `[rd] move` | did the keypress reach the reducer and change the theme? |
| `[rt] note` | did the renderer detect the swap? |
| `[rt] CONSUMED` | did it decide to repaint? |
| `[rt] emit` | **how many bytes actually reached the wire?** |

The first three all said yes, every time — 44/44, 45/45, 45/45. Which was
itself the finding: everything I had spent days fixing was already correct.

The fourth one ended it:

```
[rd] move idx=470 next='SeedFlip Voltage' CHANGE
[rt] note fired=1 pending=1
[rt] inline CONSUMED
[rt] frame coh=2->3 bytes=0 retheme=1      ← demoted, emitted NOTHING
[rt] emit Stale bytes=27769                ← paint lands on the NEXT frame
```

`bytes=0` on the frame that decided to repaint. The whole bug is on that
line.

One practical note: agentty `freopen`s stderr to `~/.agentty/logs/stderr.log`
at startup, so `2>/tmp/x.log` captures nothing. That cost a round trip.

---

## The three fixes that were not this bug

All three were real. None of them was the reported symptom. Keeping them
listed because "I fixed a real bug" is not evidence you fixed *the* bug.

### 1. Early-bound colours — shipped, correct, wrong target

Committed markdown blocks stored fully rendered `Element`s with ~58 colour
slots resolved at commit time. A settled transcript could not follow a theme
switch at all.

Real, and worth fixing (`docs/LATE_BINDING.md`). Made colours late-bound,
which deleted `invalidate_colours()` and the whole per-switch rebuild.

But the user's symptom persisted, because the transcript was being repainted
correctly and the repaint was not reaching the wire.

### 2. The eager re-render — a fix that became a new bug

My first attempt re-rendered every committed block on each theme change.
`O(transcript)` per keystroke: **2.42 ms → 4.65 ms** per keypress on a
100-message thread, scaling with length.

Past one key-repeat slot (33 ms at 30/s) keys outrun frames and the browser
visibly skips entries — so I *introduced* a second cause of the exact
symptom I was chasing. Reverted.

### 3. The coalesce hoist — real, latent, unrelated

`StylePool::retheme()` is an edge detector that consumes the edge as it
reports it. It was called past the inline coalesce gate, so on a congested
wire a swap could be lost outright when two keypresses straddled one
deferred frame.

Genuinely broken. Kept. But the log showed **zero** coalesces ever collided
with a swap in the failing sessions, so it was never the cause.

---

## What I should have done differently

**Measure before the second fix, not after the fourth.**

The first fix was justified: I had a failing test, it passed, the reasoning
was sound. When the user said "still broken", that was the moment to
instrument. Instead I re-read the same code three more times and found three
more things that *could* cause it.

Reading code finds bugs that match your hypothesis. Measurement finds the bug
that is there.

**Symptom similarity is not evidence.** "Skips every other entry" was
genuinely consistent with early binding, with frame coalescing, *and* with a
two-frame repaint. Three plausible causes, one true. Nothing but a
measurement distinguishes them.

**Suspect the code you just changed.** When my own fix made per-keypress
render 2x more expensive on the exact gesture being reported, that should
have been the first hypothesis, not something I discovered while measuring
something else.

**Ask what the last unverified step is.** Every probe I added confirmed a
stage was already fine. The bug is always in the stage nobody has printed
yet — and the final stage, "did the bytes reach the terminal", is the one
that sits below where source-reading naturally stops.

---

## The general shape

Both of the real bugs here are the same class in different clothes:

> A state change was mistaken for the effect it was supposed to cause.

- `demote_to_stale()` **marks** that a repaint is needed. Something else has
  to perform it.
- `retheme()` **reports** a swap and consumes the edge. Something else has to
  act on it.

In both cases the decision was recorded correctly, and the work it implied
was not guaranteed to happen. That is the bug to look for whenever a system
"decides" something and the decision is right but the outcome is missing:
find the step between the decision and the effect, and ask what guarantees
it runs.

---

## Fixes retained

| commit | what |
|---|---|
| maya `0adb8ad` | colours resolve at paint; `MAYA_ASSERT_LATE_BOUND` makes early binding a compile error |
| agentty `9af90536` | `ui::Slot` hands back a symbolic slot |
| agentty `4cd0a9d2` | theme switch stops rebuilding the transcript (net −9 lines) |
| maya `1eb42c9` | theme swap survives a coalesced frame |
| maya (this) | **the actual fix** — a demoted repaint is guaranteed a painting frame |
