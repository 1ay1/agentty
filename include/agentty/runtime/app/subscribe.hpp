#pragma once
// agentty::app::subscribe — input → Msg routing.
//
// Pure function of Model: snapshots which modal (if any) owns the keyboard,
// then routes keys / paste / tick into the right Msg.

#include <chrono>

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

[[nodiscard]] Sub subscribe(const Model& m);

// ── The fields subscribe() reads ───────────────────────────────────────
//
// jaal calls subscribe() again only when this value CHANGES (the optional
// `subs_key` hook — core/program.hpp). Without it the kernel rebuilds and
// re-diffs the whole subscription tree after every single message: every
// keystroke, every SSE delta, every 30 fps tick. subscribe() is not cheap
// here — it walks the message list twice and snapshots four form panes — so
// that is real work on the input path, which is exactly the lag the
// FormFocus/optional<login::State> snapshots were introduced to avoid.
//
// A VALUE, not a hash: a collision would silently keep a stale subscription
// (a timer that should have stopped, a router closed over old text), and
// equality cannot be wrong in that direction.
//
// THE RULE: this must cover everything subscribe() reads AND everything its
// routers CAPTURE. The key router captures by `[=]`, so every local computed
// in subscribe() before it is part of the dependency — that is the half that
// bites, because a captured copy going stale is invisible until a key routes
// against last frame's state.
struct SubsKey {
    // Which overlay owns the keyboard, and the per-pane modes the routers
    // close over.
    int  active_panel      = 0;
    bool settings_adding   = false;
    bool appearance_picking = false;
    // focus_of() per form-backed pane: open/editing/choosing, packed.
    unsigned form_modes    = 0;

    // Gates on the turn.
    bool streaming         = false;
    bool turn_active       = false;
    bool animation_demand  = false;

    // Composer-derived predicates the router branches on.
    bool text_empty        = true;
    bool has_queued        = false;
    bool in_history        = false;
    bool has_history       = false;
    bool peeking_queue     = false;

    // The Ctrl+U target, and the login payload's IDENTITY.
    //
    // login::State is a variant of up to a dozen strings; it has no
    // operator== and copying it per frame is what the optional snapshot
    // exists to avoid. The router only reads the ALTERNATIVE INDEX plus
    // "is the OAuth code box empty", so those two are the whole dependency
    // — carrying them is both correct and cheap.
    std::optional<MessageId> live_retrieved_id;
    int  login_alt         = -1;    // -1 = login doesn't own the keyboard
    bool login_code_empty  = true;

    // The login worker's SOURCE key: (attempt, provider). subscribe()
    // returns a keyed stream while a login is waiting, so unlike everything
    // above this doesn't just feed a capture — it decides whether a
    // background poll loop RUNS. Empty when no login is in flight.
    std::optional<std::pair<std::uint64_t, std::string>> login_worker;

    [[nodiscard]] bool operator==(const SubsKey&) const = default;
};

[[nodiscard]] SubsKey subs_key(const Model& m) noexcept;

} // namespace agentty::app
