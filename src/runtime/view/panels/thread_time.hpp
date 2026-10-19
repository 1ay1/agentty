#pragma once
// thread_time.hpp — the ^J list's day groups and short ages.

#include <chrono>
#include <ctime>
#include <string>

namespace agentty::ui {

// Which band a thread falls in, by the local calendar day it was last
// active, relative to `now`. Ordered newest first.
enum class DayGroup { Today, Yesterday, ThisWeek, ThisMonth, Older };

[[nodiscard]] inline const char* day_group_label(DayGroup g) noexcept {
    switch (g) {
        case DayGroup::Today:     return "Today";
        case DayGroup::Yesterday: return "Yesterday";
        case DayGroup::ThisWeek:  return "This week";
        case DayGroup::ThisMonth: return "This month";
        case DayGroup::Older:     return "Older";
    }
    return "Older";
}

namespace thread_time_detail {
// Days since the epoch for `tp`'s local calendar date.
[[nodiscard]] inline long local_day(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    tm.tm_hour = 12; tm.tm_min = 0; tm.tm_sec = 0;   // noon dodges DST edges
    return static_cast<long>(std::mktime(&tm) / 86400);
}
}  // namespace thread_time_detail

[[nodiscard]] inline DayGroup day_group(std::chrono::system_clock::time_point when,
                                        std::chrono::system_clock::time_point now) {
    const long d = thread_time_detail::local_day(now) - thread_time_detail::local_day(when);
    if (d <= 0) return DayGroup::Today;
    if (d == 1) return DayGroup::Yesterday;
    if (d < 7)  return DayGroup::ThisWeek;
    if (d < 31) return DayGroup::ThisMonth;
    return DayGroup::Older;
}

// "now", "5m", "3h", "2d", "6w", "4mo", "2y": one unit, fits a narrow column.
[[nodiscard]] inline std::string short_age(std::chrono::system_clock::time_point when,
                                           std::chrono::system_clock::time_point now) {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(now - when).count();
    if (s < 60)            return "now";
    if (s < 3600)          return std::to_string(s / 60) + "m";
    if (s < 86400)         return std::to_string(s / 3600) + "h";
    if (s < 86400 * 14)    return std::to_string(s / 86400) + "d";
    if (s < 86400 * 60)    return std::to_string(s / (86400 * 7)) + "w";
    if (s < 86400 * 365)   return std::to_string(s / (86400 * 30)) + "mo";
    return std::to_string(s / (86400 * 365)) + "y";
}

}  // namespace agentty::ui
