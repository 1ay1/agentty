#pragma once
// agentty::stats — Unit and format(): the ONE place a number becomes text.
//
// Every statistic in the panel is a double plus a unit. This file owns the
// translation, and owning it once is the point: without it, a token count
// gets spelled "12.4k" on one tab and "12400" on the next, a duration is
// "3.2s" here and "3200ms" there, and the eleventh statistic arrives with
// a twelfth spelling. That drift is invisible in review — each site looks
// correct on its own — and glaring in use.
//
// The widget (maya::StatSheet) deliberately does NOT do this. It takes
// strings, because units belong to the domain that owns the numbers; a
// widget that knew what a kilotoken was would need a new enumerator every
// time a host learned a new unit.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace agentty::stats {

enum class Unit : std::uint8_t {
    Count,    // 1, 47, 1.2k        — turns, calls, errors
    Tokens,   // 847, 12.4k, 1.3M   — same ramp as Count, named for intent
    Bytes,    // 512 B, 4.1 KB      — 1024-based
    Millis,   // 340ms, 3.2s, 4m12s — reads as a duration, never as a number
    Ratio,    // 0.62 → "62%"
    Rate,     // 1.2k/s
    Usd,      // $0.42
};

namespace detail {

// Thousands ramp shared by Count / Tokens / Rate.
//
// One decimal below 10 and none above it: "1.2k" and "12k" both fit four
// columns, so a table of them stays aligned without padding, and the
// precision that gets dropped at 12k was never being read anyway.
[[nodiscard]] inline std::string si(double v) {
    char buf[32];
    const double a = v < 0 ? -v : v;
    if (a < 1000.0) {
        // Integers print as integers. "47.0 turns" implies a precision the
        // count does not have.
        if (a == std::floor(a)) {
            std::snprintf(buf, sizeof buf, "%.0f", v);
        } else {
            std::snprintf(buf, sizeof buf, "%.1f", v);
        }
    } else if (a < 1'000'000.0) {
        const double k = v / 1000.0;
        std::snprintf(buf, sizeof buf, (k < 10.0 && k > -10.0) ? "%.1fk" : "%.0fk", k);
    } else if (a < 1'000'000'000.0) {
        const double mm = v / 1'000'000.0;
        std::snprintf(buf, sizeof buf, (mm < 10.0 && mm > -10.0) ? "%.1fM" : "%.0fM", mm);
    } else {
        std::snprintf(buf, sizeof buf, "%.1fB", v / 1'000'000'000.0);
    }
    return buf;
}

// Durations read as durations. 214000 is a number; "3m34s" is a fact about
// how long you waited, and the reader should not have to divide.
[[nodiscard]] inline std::string duration(double ms) {
    char buf[32];
    if (ms < 0) ms = 0;
    if (ms < 1000.0) {
        std::snprintf(buf, sizeof buf, "%.0fms", ms);
    } else if (ms < 60'000.0) {
        const double s = ms / 1000.0;
        std::snprintf(buf, sizeof buf, s < 10.0 ? "%.1fs" : "%.0fs", s);
    } else if (ms < 3'600'000.0) {
        const int total = static_cast<int>(ms / 1000.0 + 0.5);
        std::snprintf(buf, sizeof buf, "%dm%02ds", total / 60, total % 60);
    } else {
        const int total = static_cast<int>(ms / 60'000.0 + 0.5);
        std::snprintf(buf, sizeof buf, "%dh%02dm", total / 60, total % 60);
    }
    return buf;
}

[[nodiscard]] inline std::string bytes(double b) {
    char buf[32];
    const double a = b < 0 ? -b : b;
    if (a < 1024.0)             { std::snprintf(buf, sizeof buf, "%.0f B", b); }
    else if (a < 1024.0 * 1024) { std::snprintf(buf, sizeof buf, "%.1f KB", b / 1024.0); }
    else if (a < 1024.0 * 1024 * 1024) {
        std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    } else {
        std::snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
    }
    return buf;
}

}  // namespace detail

// The single formatter. Every number the panel shows passes through here.
[[nodiscard]] inline std::string format(Unit u, double v) {
    switch (u) {
        case Unit::Count:
        case Unit::Tokens:
            return detail::si(v);
        case Unit::Bytes:
            return detail::bytes(v);
        case Unit::Millis:
            return detail::duration(v);
        case Unit::Ratio: {
            char buf[16];
            // Whole percents. A share is read as a proportion, and "61.7%"
            // spends a column on precision nobody acts on. But a non-zero
            // share never prints as "0%" — same rule as the bar that is
            // not allowed to round to empty: "almost none" and "none" are
            // different readings.
            const double pct = v * 100.0;
            if (pct > 0.0 && pct < 1.0) return "<1%";
            if (pct < 0.0 && pct > -1.0) return ">-1%";
            std::snprintf(buf, sizeof buf, "%.0f%%", pct);
            return buf;
        }
        case Unit::Rate:
            return detail::si(v) + "/s";
        case Unit::Usd: {
            char buf[24];
            const double a = v < 0 ? -v : v;
            // Sub-cent amounts are real on cheap models and rounding them
            // to "$0.00" reports a free turn that was not free.
            std::snprintf(buf, sizeof buf, a < 0.01 && a > 0 ? "$%.4f" : "$%.2f", v);
            return buf;
        }
    }
    return detail::si(v);
}

}  // namespace agentty::stats
