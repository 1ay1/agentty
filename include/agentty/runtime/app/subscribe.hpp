#pragma once
// agentty::app::subscribe — timers and workers, plus input → Msg routing.
//
// subscribe(m) asks for the Tick timer and the login worker. Terminal input
// goes through on_key / on_paste / on_focus / on_resize, which jaal calls with the model as it is
// when the event arrives (jaal D43), so routing can't read a stale copy.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <maya/maya.hpp>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/cmd.hpp"
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
// paint, is the bottleneck). A pure function of the launch environment
// init() captured, so subscribe() and visual_hash() agree by reading it.
[[nodiscard]] std::chrono::milliseconds
streaming_tick_period(const Model::Env& env) noexcept;

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

// Whether ^A/^X (accept/reject all changes) are live on the main screen.
[[nodiscard]] bool strip_chords_armed(const Model& m) noexcept;

[[nodiscard]] Sub subscribe(const Model& m);

// Terminal input. Each reads the live model; nullopt drops the event.
[[nodiscard]] std::optional<Msg> on_key(const Model& m, const maya::KeyEvent& ev);
[[nodiscard]] Msg on_paste(const Model& m, const maya::PasteEvent& pe);
[[nodiscard]] Msg on_focus(const maya::FocusEvent& fe) noexcept;
[[nodiscard]] Msg on_resize(const maya::ResizeEvent& re) noexcept;

// ── The fields subscribe() reads ───────────────────────────────────────
//
// jaal re-runs subscribe() only when this value changes (subs_key hook,
// jaal core/program.hpp), and debug builds check it covers subscribe().
// Input routing is not in here: on_key and friends read the live model.
// A value, not a hash, so a collision can't keep a stale timer or worker.
struct SubsKey {
    // Arms the Tick timer.
    bool animation_demand = false;
    // The tick period is read from m.env, which init() sets once; keyed
    // anyway so a future change to it can't leave the timer at the old rate.
    std::chrono::milliseconds tick_period{0};
    // The login worker's source key: (attempt, provider). Empty when no
    // login is in flight.
    std::optional<std::pair<std::uint64_t, std::string>> login_worker;

    [[nodiscard]] bool operator==(const SubsKey&) const = default;
};

[[nodiscard]] SubsKey subs_key(const Model& m) noexcept;

} // namespace agentty::app
