#pragma once
// moha::ui — small pure helpers shared by view modules.

#include <chrono>
#include <string>
#include <string_view>

#include <maya/style/color.hpp>

#include "moha/runtime/model.hpp"

namespace moha::ui {

// Enum reflection — delegates to moha::to_string().
[[nodiscard]] inline std::string_view profile_label(Profile p) noexcept { return to_string(p); }
[[nodiscard]] maya::Color profile_color(Profile p) noexcept;
[[nodiscard]] inline std::string_view phase_label(const Phase& p) noexcept { return to_string(p); }

// Status-bar styling — glyph + verb form for the current phase, and a
// terminal color picked to communicate urgency at a glance.
[[nodiscard]] std::string_view phase_glyph(const Phase& p) noexcept;
[[nodiscard]] std::string_view phase_verb(const Phase& p) noexcept;
[[nodiscard]] maya::Color      phase_color(const Phase& p) noexcept;

[[nodiscard]] std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp);

// ── Typographic primitives ────────────────────────────────────────────────
// Letter-spaced uppercase ("S E C T I O N") — the typographic shorthand
// for "this is a section header" in CLI tools that lack real small-caps.
// ASCII-only; non-ASCII bytes pass through unchanged. Only useful for
// short labels — long strings get wide fast (each char gains a space).
[[nodiscard]] std::string small_caps(std::string_view s);

// Right-aligned fixed-width integer. Use for any number that updates
// in place (token counts, durations, pcts, counters) so the surrounding
// row never dances horizontally. Falls back to natural width if `n` is
// wider than `width`.
[[nodiscard]] std::string tabular_int(int n, int width);

// Compact, ALWAYS-5-display-column elapsed-time formatter. Picks the
// best unit for the magnitude:
//     0.0–9.9 s  →  " 4.2s"   (leading space)
//   10.0–99.9 s  →  "12.3s"
//      100–599 s →  " 234s"   (whole seconds)
//        ≥600 s  →  " 9m05s"  (m/s)
//        ≥3600 s →  " >1hr"
// Stable width is the whole point — drop into any always-on indicator
// (phase chip elapsed, tool duration, etc.) and the surrounding layout
// will not shift as the value ticks.
[[nodiscard]] std::string format_elapsed_5(float secs);

// Variable-width compact elapsed-time formatter for non-fixed-width
// surfaces (turn meta, timeline title/footer):
//   < 1 s  → "234ms"
//   < 60 s → "4.2s"
//   else   → "1m20s"
// Use this in any non-status-bar surface where exact width isn't
// required and "234ms" / "4.2s" reads better than padded forms.
[[nodiscard]] std::string format_duration_compact(float secs);

// Context window size for a given model id. Defaults to 200 K but bumps
// to 1 M when the model id carries the moha-internal `[1m]` tag (which
// triggers the `context-1m-2025-08-07` beta on the wire). Used by the
// status-bar ctx % calculation so the bar doesn't read "180 %" after
// switching to a 1 M-window model with the old 200 K cap baked in.
[[nodiscard]] int context_max_for_model(std::string_view model_id) noexcept;

// UTF-8 helpers.
[[nodiscard]] std::string utf8_encode(char32_t cp);
[[nodiscard]] int utf8_prev(std::string_view s, int byte_pos) noexcept;
[[nodiscard]] int utf8_next(std::string_view s, int byte_pos) noexcept;

} // namespace moha::ui
