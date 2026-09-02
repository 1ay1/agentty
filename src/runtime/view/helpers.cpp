#include "agentty/runtime/view/helpers.hpp"

#include <concepts>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <variant>
#include <vector>

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/model_name.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::Color profile_color(Profile p) noexcept {
    // Each profile gets a distinct, saturated identity hue. Minimal used
    // to render in `muted` (gray) which left it feeling like an absence
    // of state rather than a deliberate choice — bright_cyan claims an
    // identity for it without colliding with Write (magenta) or Ask
    // (blue). All three profile chips now carry a real color.
    switch (p) {
        case Profile::Write:   return role_brand;   // magenta
        case Profile::Ask:     return role_info;    // blue
        case Profile::Minimal: return code_path;    // bright_cyan
    }
    return fg;
}

std::string_view phase_glyph(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "●";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "◐";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "⚠";
        else                                                           return "▶";
    }, p);
}

std::string_view phase_verb(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> std::string_view {
        using T = std::decay_t<decltype(v)>;
        if      constexpr (std::same_as<T, phase::Idle>)               return "Ready";
        else if constexpr (std::same_as<T, phase::Streaming>)          return "Streaming";
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return "Awaiting";
        else                                                           return "Running";
    }, p);
}

maya::Color phase_color(const Phase& p) noexcept {
    return std::visit([](const auto& v) -> maya::Color {
        using T = std::decay_t<decltype(v)>;
        // Use bright ANSI variants for active phases so the pulsing
        // spinner in the status-bar chip reads as "alive" on every
        // palette, not just high-contrast dark themes. `highlight`
        // and `success` alone were landing on the desaturated end of
        // several popular light themes.
        if      constexpr (std::same_as<T, phase::Idle>)               return muted;
        else if constexpr (std::same_as<T, phase::Streaming>)          return maya::Color::bright_cyan();
        else if constexpr (std::same_as<T, phase::AwaitingPermission>) return maya::Color::bright_yellow();
        else                                                           return maya::Color::bright_green();
    }, p);
}

std::string small_caps(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        out.push_back(static_cast<char>(
            (c >= 'a' && c <= 'z') ? (c - 32) : c));
        if (i + 1 < s.size()) out.push_back(' ');
    }
    return out;
}

std::string tabular_int(int n, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%*d", width, n);
    return buf;
}

std::string format_elapsed_5(float secs) {
    // EVERY branch must produce EXACTLY 5 display columns — this label
    // lives in the status bar where any width change per frame reads as
    // jitter. Budgets by magnitude:
    //   <   10 s  → " 3.4s"   ( 1 + 3 + 1 = 5 )
    //   <  100 s  → "12.3s"   ( 4 + 1     = 5 )
    //   <  600 s  → " 234s"   ( 4 + 1     = 5 )
    //   < 600 m   → "59m9s" … needs "mm'ss" style — pick " 9m5s" /
    //                 "59m5s" (always 5 chars, seconds as single digit
    //                 rounded down, minutes 1–2 digits).
    //   else      → " >1hr"
    char buf[16];
    if (secs < 0.0f) secs = 0.0f;
    if      (secs <   10.0f)  std::snprintf(buf, sizeof(buf), " %.1fs", static_cast<double>(secs));
    else if (secs <  100.0f)  std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(secs));
    else if (secs <  600.0f)  std::snprintf(buf, sizeof(buf), "%4ds", static_cast<int>(secs));
    else if (secs < 3600.0f) {
        int m = static_cast<int>(secs) / 60;
        int s = (static_cast<int>(secs) / 10) % 6;   // tens of seconds, 0–5
        // "%2dm%ds" → e.g. " 9m3s" or "59m4s" — always 5 cols.
        std::snprintf(buf, sizeof(buf), "%2dm%ds", m, s);
    }
    else                      std::snprintf(buf, sizeof(buf), " >1hr");
    return buf;
}

std::string format_duration_compact(float secs) {
    char buf[24];
    if      (secs < 1.0f)
        std::snprintf(buf, sizeof(buf), "%.0fms", static_cast<double>(secs) * 1000.0);
    else if (secs < 60.0f)
        std::snprintf(buf, sizeof(buf), "%.1fs",  static_cast<double>(secs));
    else {
        int   mins = static_cast<int>(secs) / 60;
        float rest = secs - static_cast<float>(mins * 60);
        std::snprintf(buf, sizeof(buf), "%dm%.0fs", mins, static_cast<double>(rest));
    }
    return buf;
}

std::string pretty_model_label(std::string_view id) {
    // Thin delegate. Every rule that used to live here (family taxonomy,
    // version extraction, snapshot-date and `:latest` stripping, Ollama tag
    // handling, acronym casing) now lives in the domain SSOT so the turn
    // header, the composer chip, the welcome screen and the pickers cannot
    // disagree. See domain/model_name.hpp.
    return model_name::decode(id).full();
}

std::string model_display_label(std::string_view id,
                                std::string_view display_name) {
    // Thin delegate — the decoder owns the id-vs-server-name reconciliation.
    return model_name::decode(id, display_name).full();
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

std::string timestamp_full(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    // "Jan 14 09:15" — 12 visible cols, fixed-width on all locales
    // because we hand-format the month from a small table instead of
    // strftime %b (which is locale-dependent and can produce 3- or
    // 4-character abbreviations in some locales).
    static constexpr const char* kMonth[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"};
    const int m = (tm.tm_mon >= 0 && tm.tm_mon < 12) ? tm.tm_mon : 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%s %2d %02d:%02d",
                  kMonth[m], tm.tm_mday, tm.tm_hour, tm.tm_min);
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

int context_max_for_model(std::string_view model_id) noexcept {
    // ModelCapabilities owns the model-id parsing; this consumes the typed
    // window. Sonnet-4+ auto-detects the 1M window (the `context-1m` beta the
    // transport already sends), Opus/Haiku stay 200k, and the `[1m]` suffix
    // still forces 1M for any model. Unknown families (local / OpenAI-compat)
    // report 0 here — the caller prefers a real probed window in that case, so
    // fall back to the historical 200k default only when nothing is known.
    const int w = ModelCapabilities::from_id(model_id).context_window();
    return w > 0 ? w : 200'000;
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

int chip_prev(std::string_view s, int byte_pos) noexcept {
    if (byte_pos <= 0) return 0;
    // Closing sentinel of a placeholder at byte_pos - 1 → jump past
    // the whole token in one step. attachment::placeholder_len_ending_at
    // returns 0 when the bytes don't form a valid placeholder, so the
    // fallback to utf8_prev keeps non-placeholder cases unchanged.
    if (auto len = attachment::placeholder_len_ending_at(
            s, static_cast<std::size_t>(byte_pos)); len > 0) {
        return byte_pos - static_cast<int>(len);
    }
    return utf8_prev(s, byte_pos);
}

int chip_next(std::string_view s, int byte_pos) noexcept {
    int n = static_cast<int>(s.size());
    if (byte_pos >= n) return n;
    if (auto len = attachment::placeholder_len_at(
            s, static_cast<std::size_t>(byte_pos)); len > 0) {
        return byte_pos + static_cast<int>(len);
    }
    return utf8_next(s, byte_pos);
}

namespace {
// Natural three-way compare of two labels: split each into alternating
// non-digit / digit chunks and compare chunk-by-chunk — digit chunks as
// INTEGERS (so "10" > "2"), non-digit chunks case-folded lexically. The
// leading non-digit chunk is the family; the first numeric chunk is the
// generation, etc. Returns <0 / 0 / >0.
[[nodiscard]] int natural_cmp(std::string_view a, std::string_view b) noexcept {
    auto lower = [](char c) -> char {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[i]));
        const bool db = std::isdigit(static_cast<unsigned char>(b[j]));
        if (da && db) {
            // Compare digit runs as integers: skip leading zeros, then
            // longer run wins; equal length → lexical over the digits.
            std::size_t ia = i, jb = j;
            while (ia < a.size() && a[ia] == '0') ++ia;
            while (jb < b.size() && b[jb] == '0') ++jb;
            std::size_t ea = ia; while (ea < a.size() && std::isdigit((unsigned char)a[ea])) ++ea;
            std::size_t eb = jb; while (eb < b.size() && std::isdigit((unsigned char)b[eb])) ++eb;
            const std::size_t la = ea - ia, lb = eb - jb;
            if (la != lb) return la < lb ? -1 : 1;
            for (std::size_t k = 0; k < la; ++k)
                if (a[ia + k] != b[jb + k])
                    return a[ia + k] < b[jb + k] ? -1 : 1;
            // advance past the FULL digit runs (including the zeros)
            i = ea; j = eb;
            while (i < a.size() && std::isdigit((unsigned char)a[i])) ++i;   // no-op after ea
            while (j < b.size() && std::isdigit((unsigned char)b[j])) ++j;
        } else if (da != db) {
            // A number sorts before letters at the same position so
            // "gpt-4" and "gpt-4o" group before "gpt-image".
            return da ? -1 : 1;
        } else {
            const char ca = lower(a[i]), cb = lower(b[j]);
            if (ca != cb) return ca < cb ? -1 : 1;
            ++i; ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// The leading non-digit run, case-folded — the "family" ("claude sonnet",
// "gpt", "gemini"). Trailing separators are trimmed so "gpt-" == "gpt".
[[nodiscard]] std::string family_of(std::string_view s) noexcept {
    std::string out;
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c))) break;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-'
                            || out.back() == '_' || out.back() == '.'))
        out.pop_back();
    return out;
}
}  // namespace

bool model_order_less(std::string_view a, std::string_view b) noexcept {
    // 1. Family ASCENDING (all Sonnets together, alphabetical across
    //    families) — the categorized look.
    const std::string fa = family_of(a), fb = family_of(b);
    if (fa != fb) return fa < fb;
    // 2. Within a family, NEWEST FIRST: natural compare, DESCENDING, so
    //    the higher version number floats to the top.
    const int c = natural_cmp(a, b);
    if (c != 0) return c > 0;
    // 3. Fully equal under natural compare (e.g. "latest" vs a dated
    //    alias of the same model) — raw byte order for a stable result.
    return a < b;
}

} // namespace agentty::ui
