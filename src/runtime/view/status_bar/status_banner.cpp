#include "agentty/runtime/view/status_bar/status_banner.hpp"

#include <chrono>
#include <string_view>

namespace agentty::ui {

namespace {

// Severity for a transient status message. Splits the toast band
// into three meanings instead of the old binary error/non-error:
//   Error → red    ("error: …", terminal failures)
//   Warn  → yellow ("retrying …", "awaiting …", "transient — …")
//   Info  → phase  (everything else: "context compacted", "cancelled", …)
//
// The old code routed ALL non-error messages through the streaming
// phase color (bright_cyan) with bright_white text — unreadable on
// most themes, and it told the user nothing about severity. Retries
// in particular look identical to the calm "compacted" toast despite
// being a thing the user might actually want to act on.
maya::StatusBanner::Kind classify(std::string_view status) noexcept {
    if (status.rfind("error:", 0) == 0) return maya::StatusBanner::Kind::Error;

    constexpr std::string_view warn_prefixes[] = {
        "retrying",          // "retrying (upstream cut off)…"
        "transient",         // "transient — retrying in 21s (attempt 5/6)…"
        "rate limit",        // "rate limit — retrying …"
        "awaiting",
    };
    for (auto p : warn_prefixes) {
        if (status.size() >= p.size()
         && std::string_view(status.data(), p.size()) == p) {
            return maya::StatusBanner::Kind::Warn;
        }
    }
    return maya::StatusBanner::Kind::Info;
}

} // namespace

// Toast notifications ride on the StatusBanner row, which maya::StatusBar
// now uses to take over the activity_row slot entirely (full-width, bright
// foreground on accent background) whenever `text` is non-empty. The
// shortcut row was retired; shortcuts moved to the welcome screen.
//
// We surface `m.s.status` here as long as it's both non-empty AND still
// inside its TTL window (`status_active`). "ready" is the calm/idle
// pseudo-status and shouldn't strobe the toast band, so it's filtered
// out. `kind` keys off the message text so the widget picks the right
// tint (red for errors, yellow for retry/awaiting, phase-color info for
// everything else — "context compacted", "cancelled", …).
maya::StatusBanner::Config status_banner_config(const Model& m) {
    maya::StatusBanner::Config cfg;
    if (m.s.status.empty() || m.s.status == "ready") return cfg;
    if (!m.s.status_active(std::chrono::steady_clock::now())) return cfg;
    cfg.text     = m.s.status;
    cfg.kind     = classify(m.s.status);
    cfg.is_error = (cfg.kind == maya::StatusBanner::Kind::Error);
    return cfg;
}

} // namespace agentty::ui
