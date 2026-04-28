#include "moha/runtime/view/phase_chip.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include "moha/runtime/view/helpers.hpp"
#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

namespace {

// Walk the last assistant message and return the name of the tool
// call currently in `Running` state, if any. Lets the phase chip
// render "▌ ⠋ bash ▐" when ExecutingTool is active.
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
    const bool active       = is_streaming || is_executing;

    std::string glyph = active
        ? std::string{m.s.spinner.current_frame()}
        : std::string{phase_glyph(m.s.phase)};
    std::string verb{phase_verb(m.s.phase)};
    if (is_executing) {
        if (auto tn = running_tool_name(m); !tn.empty())
            verb = std::string{tn};
    }

    float elapsed = -1.0f;
    if (active && m.s.started.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m.s.started).count();
        elapsed = static_cast<float>(ms) / 1000.0f;
    }

    maya::PhaseChip::Config cfg;
    cfg.glyph        = std::move(glyph);
    cfg.verb         = std::move(verb);
    cfg.color        = phase_color(m.s.phase);
    cfg.breathing    = active;
    cfg.frame        = m.s.spinner.frame_index();
    cfg.verb_width   = 10;
    cfg.elapsed_secs = elapsed;
    return cfg;
}

} // namespace moha::ui
