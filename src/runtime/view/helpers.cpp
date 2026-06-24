#include "agentty/runtime/view/helpers.hpp"

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <variant>

#include "agentty/domain/catalog.hpp"
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
    // Drop the agentty extended-context marker anywhere in the id.
    if (auto pos = id.find("[1m]"); pos != std::string_view::npos) {
        std::string stripped{id.substr(0, pos)};
        stripped += id.substr(pos + 4);
        return pretty_model_label(stripped);
    }

    // Strip provider namespace: keep the segment after the last '/'.
    if (auto slash = id.find_last_of('/'); slash != std::string_view::npos)
        id.remove_prefix(slash + 1);

    // Split off an optional `:tag` (Ollama size/quant selector).
    std::string_view tag;
    if (auto colon = id.find(':'); colon != std::string_view::npos) {
        tag = id.substr(colon + 1);
        id  = id.substr(0, colon);
    }
    // `:latest` carries no information — every Ollama pull defaults to it.
    if (tag == "latest") tag = {};

    auto is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto is_upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    auto is_lower = [](char c) { return c >= 'a' && c <= 'z'; };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto lower_eq = [&](std::string_view w, std::string_view lit) {
        if (w.size() != lit.size()) return false;
        for (std::size_t i = 0; i < w.size(); ++i) {
            char a = w[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != lit[i]) return false;
        }
        return true;
    };

    // Emit one word, title-cased, with these refinements:
    //   • a word that's ALREADY an all-caps acronym (≤4 chars) is kept
    //     verbatim (GPT, GLM, SQL).
    //   • a curated set of well-known lowercase acronyms is upper-cased
    //     (gpt → GPT, glm → GLM) — these read wrong title-cased.
    //   • a DIGIT-LED word (4o, 8x7b, 70b, 9b, 2.5) keeps every letter
    //     lowercase — these are version/size runs, not names.
    //   • otherwise: upper-case the first letter, lower-case the rest
    //     handled implicitly (we only touch the leading alpha).
    auto emit_word = [&](std::string& out, std::string_view w) {
        if (w.empty()) return;
        bool all_caps = true, has_alpha = false;
        for (char c : w) {
            if (is_alpha(c)) { has_alpha = true; if (!is_upper(c)) all_caps = false; }
        }
        if (has_alpha && all_caps && w.size() <= 4) {
            out.append(w);                       // GPT / GLM / SQL acronym
            return;
        }
        if (lower_eq(w, "gpt") || lower_eq(w, "glm") || lower_eq(w, "sql") ||
            lower_eq(w, "tts")  || lower_eq(w, "vl")) {
            for (char c : w) out.push_back(
                is_lower(c) ? static_cast<char>(c - 'a' + 'A') : c);
            return;
        }
        // Digit-led word: version/size run — keep letters lowercase.
        if (is_digit(w.front())) {
            for (char c : w) out.push_back(
                is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c);
            return;
        }
        // OpenAI o-series reasoning models (o1 / o3 / o4 / o4-mini): a
        // lone 'o' followed by a digit is conventionally lowercase.
        if (w.size() >= 2 && (w[0] == 'o' || w[0] == 'O') && is_digit(w[1])) {
            out.push_back('o');
            out.append(w.substr(1));
            return;
        }
        bool cased = false;
        for (char c : w) {
            if (!cased && is_lower(c)) { c = static_cast<char>(c - 'a' + 'A'); cased = true; }
            else if (is_alpha(c))      { cased = true; }
            out.push_back(c);
        }
    };

    std::string out;
    out.reserve(id.size() + tag.size() + 1);
    std::size_t w0 = 0;
    for (std::size_t i = 0; i <= id.size(); ++i) {
        const bool boundary =
            (i == id.size() || id[i] == '-' || id[i] == '_' || id[i] == ' ');
        if (!boundary) continue;
        if (i > w0) {
            if (!out.empty()) out.push_back(' ');
            emit_word(out, id.substr(w0, i - w0));
        }
        w0 = i + 1;
    }
    if (out.empty()) out = std::string{id};   // pathological all-delim id

    if (!tag.empty()) {
        out.push_back(' ');
        out.append(tag);
    }
    return out;
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
    // ModelCapabilities owns the model-id parsing; this just consumes
    // the typed flag. If new models with different windows ship,
    // extend ModelCapabilities or branch on caps.family/generation
    // here rather than re-introducing substring sniffing.
    return ModelCapabilities::from_id(model_id).extended_context_1m
         ? 1'000'000 : 200'000;
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

} // namespace agentty::ui
