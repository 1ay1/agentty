#include "moha/view/helpers.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>

#include "moha/view/palette.hpp"

namespace moha::ui {

maya::Color profile_color(Profile p) noexcept {
    switch (p) {
        case Profile::Write:   return accent;
        case Profile::Ask:     return info;
        case Profile::Minimal: return muted;
    }
    return fg;
}

std::string_view phase_glyph(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:               return "●";
        case Phase::Streaming:          return "◐";
        case Phase::AwaitingPermission: return "⚠";
        case Phase::ExecutingTool:      return "▶";
    }
    return "●";
}

std::string_view phase_verb(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:               return "Ready";
        case Phase::Streaming:          return "Streaming";
        case Phase::AwaitingPermission: return "Awaiting";
        case Phase::ExecutingTool:      return "Running";
    }
    return "Ready";
}

maya::Color phase_color(Phase p) noexcept {
    switch (p) {
        case Phase::Idle:               return muted;
        case Phase::Streaming:          return highlight;
        case Phase::AwaitingPermission: return warn;
        case Phase::ExecutingTool:      return success;
    }
    return fg;
}

std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string utf8_encode(char32_t cp) {
    std::string out;
    auto u = static_cast<uint32_t>(cp);
    if (u < 0x80) {
        out.push_back(static_cast<char>(u));
    } else if (u < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else if (u < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (u >> 12)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (u >> 18)));
        out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
    }
    return out;
}

int utf8_prev(std::string_view s, int byte_pos) noexcept {
    if (byte_pos <= 0) return 0;
    int p = byte_pos - 1;
    while (p > 0 && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80) --p;
    return p;
}

int utf8_next(std::string_view s, int byte_pos) noexcept {
    int n = static_cast<int>(s.size());
    if (byte_pos >= n) return n;
    int p = byte_pos + 1;
    while (p < n && (static_cast<uint8_t>(s[p]) & 0xC0) == 0x80) ++p;
    return p;
}

} // namespace moha::ui
