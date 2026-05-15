#include "agentty/runtime/view/thread/activity_indicator.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

namespace {

// Lower-case the phase verb for the activity row. The status-bar
// PhaseChip wants Title-Case ("Streaming") because it lives in a
// chrome strip surrounded by other Title-Cased chips; the in-thread
// activity row wants a softer typographic register since it sits
// directly under the assistant turn body. "streaming\u2026" reads as
// ambient "still working" chrome; "Streaming\u2026" reads as a
// section header.
//
// We special-case "Streaming" to the friendlier verb "thinking" —
// during the SSE stream the model is generally still composing the
// reply (tool decisions, prose), and "thinking" matches the user's
// mental model better than the protocol-level word "streaming".
std::string soft_verb(std::string_view raw) {
    if (raw == "Streaming") return "thinking";
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

std::optional<maya::ActivityIndicator::Config>
activity_indicator_config(const Model& m) {
    if (!m.s.active()) return std::nullopt;
    if (m.d.current.messages.empty()) return std::nullopt;
    const auto& last = m.d.current.messages.back();
    if (last.role != Role::Assistant) return std::nullopt;
    bool tl_visible =
        !last.tool_calls.empty()
        && std::any_of(last.tool_calls.begin(), last.tool_calls.end(),
                       [](const auto& tc){ return !tc.is_terminal(); });
    if (tl_visible) return std::nullopt;

    const auto caps = ModelCapabilities::from_id(m.d.model_id.value);
    maya::Color edge = caps.is_opus()   ? accent
                     : caps.is_sonnet() ? info
                     : caps.is_haiku()  ? success
                                        : highlight;
    maya::ActivityIndicator::Config cfg;
    cfg.edge_color    = edge;
    cfg.spinner_glyph = std::string{m.s.spinner.current_frame()};
    cfg.label         = soft_verb(phase_verb(m.s.phase));

    // Elapsed seconds since the active phase started — same source the
    // status-bar PhaseChip reads. Shown as a muted "· 3.4s" tail so the
    // user gets a sense of how long the model has been working without
    // having to glance up at the status bar. Skipped when the phase
    // just started (< 1s) to avoid "0.0s" flicker on the first frame.
    if (const auto* a = active_ctx(m.s.phase);
        a && a->started.time_since_epoch().count() != 0) {
        auto now = std::chrono::steady_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - a->started).count();
        if (ms >= 1000) {
            float secs = static_cast<float>(ms) / 1000.0f;
            // Trim format_elapsed_5's leading padding — here we want a
            // tight variable-width string ("3.4s", "42s", "2m 10s").
            std::string e = format_elapsed_5(secs);
            std::size_t i = e.find_first_not_of(' ');
            if (i != std::string::npos) e.erase(0, i);
            cfg.detail = std::move(e);
        }
    }
    return cfg;
}

} // namespace agentty::ui
