#pragma once
// moha::ui — small pure helpers shared by view modules.

#include <chrono>
#include <string>
#include <string_view>

#include <maya/style/color.hpp>

#include "moha/model.hpp"

namespace moha::ui {

// Enum reflection — delegates to moha::to_string().
[[nodiscard]] inline std::string_view profile_label(Profile p) noexcept { return to_string(p); }
[[nodiscard]] maya::Color profile_color(Profile p) noexcept;
[[nodiscard]] inline std::string_view phase_label(Phase p) noexcept    { return to_string(p); }

[[nodiscard]] std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp);

// UTF-8 helpers.
[[nodiscard]] std::string utf8_encode(char32_t cp);
[[nodiscard]] int utf8_prev(std::string_view s, int byte_pos) noexcept;
[[nodiscard]] int utf8_next(std::string_view s, int byte_pos) noexcept;

} // namespace moha::ui
