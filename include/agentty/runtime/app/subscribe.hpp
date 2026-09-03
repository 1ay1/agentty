#pragma once
// agentty::app::subscribe — input → Msg routing.
//
// Pure function of Model: snapshots which modal (if any) owns the keyboard,
// then routes keys / paste / tick into the right Msg.

#include <chrono>

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace agentty::app {

// The streaming Tick cadence — the SINGLE source of truth for how
// often the loop wakes while a turn is active.
//
// This is consumed in TWO places that MUST agree, or animation either
// wastes renders or freezes-until-keypress:
//   1. subscribe() — the `Sub::every(period, Tick{})` interval.
//   2. Program::visual_hash() — the fine-animation time bucket, which
//      must be PHASE-LOCKED to this period so the render gate advances
//      exactly once per loop wake (see program.hpp).
// Deriving both from one function makes the phase-lock structural
// rather than a hand-maintained invariant across two files.
//
// Cadence (33 ms ≈ 30 fps on DEC-2026 sync terminals for a smooth
// spinner; 100 ms ≈ 10 fps elsewhere to cut progressive-paint
// flicker; clamped to ≥ 80 ms over SSH where the wire, not local
// paint, is the bottleneck). Computed once — the inputs (terminal
// sync support, SSH env) are immutable for the session.
[[nodiscard]] std::chrono::milliseconds streaming_tick_period() noexcept;

// ── Animation demand — the SINGLE definition of "something is moving" ──
//
// Three gates decide whether time-based animation runs, and a silent
// animation death (or a flicker) is what happens when they drift:
//   1. subscribe()            — arms the Tick timer at all.
//   2. Program::visual_hash() — lets a wake actually reach view().
//   3. meta.cpp's Tick arm    — advances reducer-side animation state.
// Each used to restate its own subset of "is anything animating" inline,
// and every reveal/caret/spinner bug in the file histories is one of
// those restatements drifting from the others ("Thinking gets stuck",
// "reveal freezes mid-glide", "caret flickers under two clocks").
//
// These predicates are defined ONCE here so the gates cannot disagree
// about WHAT is animating — they may still differ in what they DO about
// it (arm a timer / pick a hash bucket / step a spinner), which is
// per-gate policy, not shared truth.

// Live wire bytes on the tail message (streaming_text / pending_stream
// not yet drained into the settled body). The earliest and cheapest
// "the reveal has something to chew on" signal.
[[nodiscard]] bool tail_has_live_bytes(const Model& m) noexcept;

// The reveal typewriter needs frames NOW: wire bytes are arriving on the
// answer channel, or — during a pure-reasoning phase — on the reasoning
// channel (which streams through the same reveal machinery and froze
// "until a keypress" when this term was restated without it).
[[nodiscard]] bool reveal_needs_frames(const Model& m) noexcept;

// The wire is quiet but the reveal is still DRAINING: end-of-turn, deltas
// settled into `text`, while the widget's cursor is still gliding to the
// edge (is_finalizing / reveal_in_progress), or a settle-freeze/cooldown
// is waiting on it. In this window BOTH m.s.active() and
// reveal_needs_frames() are false — the frame demand comes purely from
// animation state, not from bytes. Consumed by visual_hash's fast bucket
// (each armed RAF frame must actually render, or the tail snaps in on
// the next keypress) and by extension the Tick arm via animation_demand.
[[nodiscard]] bool reveal_draining(const Model& m) noexcept;

// Something time-based is animating and the Tick clock must run: an
// active turn, a loading spinner, live/undrained reveal state, or a
// deferred settle waiting on the reveal to finish. The union feeding
// gate 1; gates 2 and 3 consume its terms via the predicates above.
[[nodiscard]] bool animation_demand(const Model& m) noexcept;

[[nodiscard]] maya::Sub<Msg> subscribe(const Model& m);

} // namespace agentty::app
