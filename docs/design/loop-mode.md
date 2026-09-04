# Loop mode (`^B`) — repeat a prompt until you stop it

**Status:** SHIPPED (v0.7.0).

Some prompts are worth sending more than once: *"fix the next failing test"*,
*"keep refactoring until the build is clean"*, *"check whether the deploy
finished"*. Before this you held <kbd>Enter</kbd> or retyped. `^B` arms the
message and agentty re-sends it after **every completed turn** until you press
`^B` again.

## The shape

| Concern | Decision |
|---------|----------|
| What repeats | A **snapshot** taken at arm time (`loop_text` / `loop_attachments`) |
| When it repeats | After a turn that **ended normally**, from `finalize_turn` |
| On failure | **Back off**, don't stop and don't spin (see below) |
| On <kbd>Esc</kbd> | Stop outright — Esc means stop |
| Composer while armed | Shows the armed prompt, **read-only** |
| Wire visibility | **None** — an auto-send is a plain user turn |

All state lives on `ComposerState` (`m.ui.composer.loop_*`) and never leaves
the UI layer: it is not serialized into the thread, not sent as a header or a
body field, and no code under `src/provider/` branches on it.

## Why a snapshot, not the live composer

Arming copies the payload rather than re-reading `composer.text` each
iteration. Two reasons:

1. **You can keep typing.** The thing that repeats is the thing you armed —
   the only reading that stays true once the composer has moved on.
2. **The display can't lie.** Paired with the read-only lock below, the box
   and `loop_text` are the same bytes by construction.

Arming on an **empty** composer is refused rather than entering a state that
can never fire (`looping()` is `loop_armed && !loop_text.empty()` — an armed
loop with no payload is unrepresentable, not merely unlikely).

## Why the composer is read-only

Submit drains the composer, so each auto-send would otherwise leave an empty
box firing a prompt the user cannot see. The armed payload is therefore
restored after every submit — and once the box is a *readout*, letting it be
edited would mean the user reads one thing while agentty sends another.

The lock lives at `composer_update`'s single entry point: every mutating
message is dropped. `ComposerToggleLoop` is the deliberate exception — the way
out has to work from inside the locked state, or the mode is a trap.
<kbd>Esc</kbd> and every global chord live outside that reducer and are
unaffected. Visually the body dims and the caret parks (maya's
`Composer::read_only`), including suppressing the hardware caret, so "you
can't type here right now" is legible rather than discovered by pressing a key.

## Backoff — the part that matters

The first version gated only on `is_idle()`. It never asked *why* the turn
ended, so a rate-limited turn dropped to Idle, the loop re-sent instantly, and
agentty deepened the user's own rate limit on their behalf: a tight retry loop
with no delay and no stop condition. **This was observed in the field.**

A loop must never become an unattended retry. The rule now:

- **Success** re-sends immediately. A completed turn already took real
  wall-clock time; there is nothing to wait for.
- **Failure** arms `loop_wait_until_ms`, and the next send waits it out.
  - The provider's own `Retry-After` is obeyed **verbatim** when present. The
    server stated when to come back; any guess of ours is either rude or
    needlessly slow.
  - Otherwise `loop_failures` escalates a per-class schedule and resets to 0
    on the next success:

    | Class | Base | Cap |
    |-------|------|-----|
    | RateLimit / Auth | 30 s | 10 min |
    | Transient / other | 5 s | 2 min |

    Doubling per consecutive failure, so a hiccup costs seconds while a
    sustained outage decays to a slow poll instead of spinning.
- **Cancelled** disarms. Esc is unambiguous.

Rate-limit and auth share the slow lane deliberately: both mean "you are
asking too hard or wrongly", and retrying either quickly makes it worse.

### Waking a sleeping loop

`finalize_turn` is the normal re-send site, but it only runs when a turn
*ends* — a sleeping loop has no turn to end. The **Tick** resumes it:
`animation_demand` keeps the frame clock armed while `loop_wait_until_ms > 0`
(and only then, so an idle agentty still goes fully quiet), and the Tick arm
fires the moment the deadline passes and the session is idle.

## Ordering against the queue

The re-send is attempted **after** the queued-message drain. An explicitly
queued message is a fresh instruction and outranks the standing loop, which
fires again on the turn after it.

## UX

- `⟳ LOOP` while armed, `⟳ LOOP ×N` once it has re-fired — an unbounded loop
  with no visible progress is indistinguishable from a hang.
- `⟳ RETRY 24s` counting down while backing off, so a deliberately paused loop
  never reads as a wedged one.
- Brand-tinted border while idle-armed, so the gap between auto-sent turns
  doesn't look like nothing is happening. The live-phase color still wins while
  a turn is actually streaming.
- The chip has the highest keep-priority of the optional right-cluster
  segments: an app acting on its own must never render as idle.

## Tests

`composer_edit_test` pins:

- arming on an empty composer is refused;
- disarm clears every field and leaves the live composer text alone;
- typing and backspace are ignored while looping, `^B` still disarms from
  inside the lock, and editing resumes afterwards;
- a failed iteration is never ready immediately, consecutive failures
  escalate, success resets the escalation, rate-limit backs off harder than
  transient, and `Retry-After` is honoured verbatim.

## Files

- `include/agentty/runtime/model.hpp` — `ComposerState::loop_*`, `looping()`,
  `disarm_loop()`, `loop_ready()`, `loop_wait_secs()`, `loop_backoff()`.
- `src/runtime/app/update/composer.cpp` — arm/disarm arm + the read-only gate.
- `src/runtime/app/update/stream.cpp` — re-send on turn completion; backoff on
  terminal error.
- `src/runtime/app/update/meta.cpp` — Tick resume for a sleeping loop.
- `src/runtime/app/subscribe.cpp` — `^B` binding; `animation_demand` term.
- `src/runtime/view/composer.cpp` + `maya/include/maya/widget/composer.hpp` —
  the chip, the border tint, `read_only`.
