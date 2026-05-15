#include "agentty/runtime/view/status_bar/phase_chip.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

// ─────────────────────────────────────────────────────────────────────
// Phase chip — single-line "what is the app doing right now?" readout.
//
// The chip carries the WHOLE live state of the session in a single
// glyph + 10-column verb + elapsed slot. Priority ordering (highest
// wins) reflects what the user needs to know first:
//
//   1. AwaitingPermission     — blocks progress; must surface the tool
//   2. compacting             — long-running, replaces history
//   3. retry::Scheduled       — error backoff in flight
//   4. retry::StallFired      — stream went silent, watchdog tripped
//   5. ExecutingTool          — show the running tool's name
//   6. Streaming              — "thinking"
//   7. oauth_refresh_in_flight — composer queue gated on refresh
//   8. threads_loading        — background history load
//   9. queued + idle          — "+N queued" hint
//  10. autocompact_disabled   — degraded mode (idle only)
//  11. Idle                   — "Ready"
//
// Lower-priority signals that overlap an active phase (e.g. a queued
// message during streaming) are NOT shown here — the composer's own
// `❚ N queued` chip carries that, and stealing the phase verb to show
// queue count would mask the real activity.
// ─────────────────────────────────────────────────────────────────────

namespace {

// Walk the last assistant message and return the name of the tool
// call currently in `Running` state, if any.
std::string_view running_tool_name(const Model& m) {
    if (m.d.current.messages.empty()) return {};
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return {};
    for (const auto& tc : last.tool_calls) {
        if (tc.is_running()) return tc.name.value;
    }
    return {};
}

} // namespace

maya::PhaseChip::Config phase_chip_config(const Model& m) {
    const bool is_streaming = m.s.is_streaming() && m.s.active();
    const bool is_executing = m.s.is_executing_tool();
    const bool is_awaiting  = m.s.is_awaiting_permission();
    const bool phase_active = is_streaming || is_executing || is_awaiting;

    // Defaults — overwritten by whichever priority arm wins below.
    std::string  glyph;
    std::string  verb;
    maya::Color  color     = phase_color(m.s.phase);
    bool         breathing = phase_active;
    float        elapsed   = -1.0f;

    // Elapsed since the active phase started — only meaningful while a
    // turn is in flight. Computed once up front so each arm can opt in.
    float phase_elapsed = -1.0f;
    if (const auto* a = active_ctx(m.s.phase);
        a && phase_active && a->started.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - a->started).count();
        phase_elapsed = static_cast<float>(ms) / 1000.0f;
    }

    const auto spinner = std::string{m.s.spinner.current_frame()};

    // ── 1. AwaitingPermission ─────────────────────────────────────────
    // Highest priority: progress is blocked on the user. Show WHICH
    // tool needs approval, not just the generic "Awaiting". The full
    // permission prompt lives in the in-thread modal; this chip just
    // gives the user a glance-able "you have a decision to make".
    if (is_awaiting) {
        glyph     = "⚠";
        color     = status_warn;
        breathing = true;
        elapsed   = phase_elapsed;
        std::string tool;
        if (m.d.pending_permission) tool = m.d.pending_permission->tool_name.value;
        else                        tool = std::string{running_tool_name(m)};
        verb = tool.empty() ? std::string{"approve?"} : ("approve " + tool);
    }
    // ── 2. compacting ─────────────────────────────────────────────────
    // Long-running history summarisation. Survives across the synthetic
    // user/assistant pair and a normal streaming phase, so we key off
    // the dedicated flag rather than the phase variant.
    else if (m.s.compacting) {
        glyph     = spinner;
        verb      = "compacting";
        color     = role_info;            // blue — context/history axis
        breathing = true;
        elapsed   = phase_elapsed;
    }
    // ── 3. retry::Scheduled — exponential backoff between attempts ───
    else if (m.s.in_scheduled()) {
        glyph     = spinner;
        verb      = "retrying";
        color     = status_warn;
        breathing = true;
        elapsed   = phase_elapsed;
    }
    // ── 4. retry::StallFired — 120s silence watchdog tripped ─────────
    else if (m.s.in_stall_fired()) {
        glyph     = spinner;
        verb      = "stalled";
        color     = status_error;
        breathing = true;
        elapsed   = phase_elapsed;
    }
    // ── 5. ExecutingTool — show the running tool's name ──────────────
    else if (is_executing) {
        glyph     = spinner;
        verb      = std::string{running_tool_name(m)};
        if (verb.empty()) verb = "running";
        color     = status_ok;            // green — tool executing
        breathing = true;
        elapsed   = phase_elapsed;
    }
    // ── 6. Streaming — model is composing ────────────────────────────
    else if (is_streaming) {
        glyph     = spinner;
        verb      = "Streaming";
        color     = maya::Color::bright_cyan();
        breathing = true;
        elapsed   = phase_elapsed;
    }
    // ── 7. OAuth refresh in flight (idle, but submissions are gated) ──
    else if (m.s.oauth_refresh_in_flight) {
        glyph     = spinner;
        verb      = "auth…";
        color     = role_info;
        breathing = true;
    }
    // ── 8. Background thread-history load ────────────────────────────
    else if (m.s.threads_loading) {
        glyph     = spinner;
        verb      = "loading…";
        color     = muted;
        breathing = true;
    }
    // ── 9. Idle with queued messages waiting to send ─────────────────
    // The composer also paints a `❚ N queued` chip; this is the
    // status-bar mirror so a glance at the chrome confirms "yes, your
    // typed message is sitting in the queue, not lost".
    else if (!m.ui.composer.queued.empty()) {
        glyph     = "▸";
        verb      = "+" + std::to_string(m.ui.composer.queued.size()) + " queued";
        color     = role_brand_alt;
        breathing = false;
    }
    // ── 10. Idle but autocompaction is disabled (rapid-refill breaker) ─
    // Persistent degraded mode — the user should know context will
    // overflow without intervention.
    else if (m.s.autocompact_disabled) {
        glyph     = "!";
        verb      = "no-compact";
        color     = status_warn;
        breathing = false;
    }
    // ── 11. Plain idle ───────────────────────────────────────────────
    else {
        glyph     = std::string{phase_glyph(m.s.phase)};
        verb      = "Ready";
        color     = muted;
        breathing = false;
    }

    maya::PhaseChip::Config cfg;
    cfg.glyph        = std::move(glyph);
    cfg.verb         = std::move(verb);
    cfg.color        = color;
    cfg.breathing    = breathing;
    cfg.frame        = m.s.spinner.frame_index();
    cfg.verb_width   = 10;
    cfg.elapsed_secs = elapsed;
    return cfg;
}

} // namespace agentty::ui
