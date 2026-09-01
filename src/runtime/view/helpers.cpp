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
    // Drop the agentty extended-context markers anywhere in the id. Both
    // spellings exist ([1m] today, [2m] reserved — wire_model_id strips both);
    // a marker the wire strips must never leak into the UI either.
    for (std::string_view marker : {"[1m]", "[2m]"}) {
        if (auto pos = id.find(marker); pos != std::string_view::npos) {
            std::string stripped{id.substr(0, pos)};
            stripped += id.substr(pos + marker.size());
            return pretty_model_label(stripped);
        }
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
        // Mixed-case brand names that read wrong plain title-cased.
        if (lower_eq(w, "chatgpt"))  { out.append("ChatGPT");  return; }
        if (lower_eq(w, "deepseek")) { out.append("DeepSeek"); return; }
        if (lower_eq(w, "openai"))   { out.append("OpenAI");   return; }
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

    // Collect words FIRST so neighbor-aware rules (version joins, snapshot
    // dates) can see the whole id instead of streaming blind.
    std::vector<std::string_view> words;
    std::size_t w0 = 0;
    for (std::size_t i = 0; i <= id.size(); ++i) {
        const bool boundary =
            (i == id.size() || id[i] == '-' || id[i] == '_' || id[i] == ' ');
        if (!boundary) continue;
        if (i > w0) words.push_back(id.substr(w0, i - w0));
        w0 = i + 1;
    }

    auto all_digits = [&](std::string_view w) {
        if (w.empty()) return false;
        for (char c : w) if (!is_digit(c)) return false;
        return true;
    };

    // Drop a trailing release SNAPSHOT — provenance, not identity. Two
    // spellings in the wild:
    //   claude-3-5-haiku-20241022   one 8-digit "20…" word
    //   gpt-4o-2024-08-06           a 4-2-2 digit triple
    if (!words.empty() && words.back().size() == 8 &&
        all_digits(words.back()) && words.back().substr(0, 2) == "20") {
        words.pop_back();
    } else if (words.size() >= 4) {   // ≥4: never reduce an id to nothing
        const auto y = words[words.size() - 3];
        const auto m = words[words.size() - 2];
        const auto d = words[words.size() - 1];
        if (y.size() == 4 && all_digits(y) && y.substr(0, 2) == "20" &&
            m.size() == 2 && all_digits(m) && d.size() == 2 && all_digits(d))
            words.resize(words.size() - 3);
    }
    // A trailing literal "latest" is an alias pointer, not part of the name
    // (chatgpt-4o-latest, codex-mini-latest) — same rule as the :latest tag.
    if (words.size() > 1 && lower_eq(words.back(), "latest"))
        words.pop_back();
    // Server display names spell the same alias parenthetically —
    // "GPT-5.3 Chat (latest)" — which our '-_ ' split leaves as a
    // "(latest)" word. Drop a trailing parenthesized alias/status tag
    // ((latest), (preview), (deprecated)) so a name-derived label reads
    // as cleanly as an id-derived one.
    if (words.size() > 1) {
        std::string_view last = words.back();
        if (last.size() >= 2 && last.front() == '(' && last.back() == ')') {
            std::string_view inner = last.substr(1, last.size() - 2);
            if (lower_eq(inner, "latest") || lower_eq(inner, "preview") ||
                lower_eq(inner, "deprecated") || lower_eq(inner, "beta") ||
                lower_eq(inner, "exp") || lower_eq(inner, "experimental"))
                words.pop_back();
        }
    }

    std::string out;
    out.reserve(id.size() + tag.size() + 1);
    bool prev_short_number = false;
    for (const auto& w : words) {
        const bool short_number = all_digits(w) && w.size() <= 2;
        // Version join: adjacent short pure-digit words are ONE dotted
        // version, not two numbers — `claude-sonnet-4-5` is Sonnet 4.5,
        // `claude-3-5-haiku` is 3.5. Size/quant words (9b, 8x7b) contain
        // letters, so they never join; snapshot dates were dropped above.
        if (short_number && prev_short_number) {
            out.push_back('.');
            out.append(w);
        } else {
            if (!out.empty()) out.push_back(' ');
            emit_word(out, w);
        }
        prev_short_number = short_number;
    }
    if (out.empty()) out = std::string{id};   // pathological all-delim id

    if (!tag.empty()) {
        // An Ollama tag can chain size + variant + quant: `70b-instruct-q4_K_M`.
        // Keep the parts a human distinguishes models BY (size `70b`, variant
        // `instruct`/`coder`) and drop pure quantization noise (`q4_K_M`,
        // `Q8_0`, `fp16`) — the quant changes fidelity, not identity, and the
        // raw spelling reads like line noise in a picker row.
        auto is_quant = [](std::string_view p) {
            if (p.size() < 2) return false;
            const char c0 = p.front();
            if ((c0 == 'q' || c0 == 'Q') &&
                p.size() >= 2 && p[1] >= '0' && p[1] <= '9')
                return true;                     // q4_K_M / Q8_0 / q5_1
            return p == "fp16" || p == "fp32" || p == "bf16";
        };
        std::size_t p0 = 0;
        std::string cleaned;
        for (std::size_t i = 0; i <= tag.size(); ++i) {
            if (i != tag.size() && tag[i] != '-') continue;
            std::string_view part = tag.substr(p0, i - p0);
            p0 = i + 1;
            if (part.empty() || is_quant(part)) continue;
            if (!cleaned.empty()) cleaned.push_back(' ');
            // Title-case a variant word (instruct → Instruct); size runs
            // (70b, 8x7b) are digit-led and stay lowercase.
            if (part.front() >= 'a' && part.front() <= 'z' &&
                !(part.front() >= '0' && part.front() <= '9')) {
                cleaned.push_back(static_cast<char>(part.front() - 'a' + 'A'));
                cleaned.append(part.substr(1));
            } else {
                cleaned.append(part);
            }
        }
        if (!cleaned.empty()) {
            out.push_back(' ');
            out.append(cleaned);
        }
    }
    return out;
}

std::string model_display_label(std::string_view id,
                                std::string_view display_name) {
    // ── Extended-context variants must stay DISTINGUISHABLE ─────────
    // `[1m]` is a picker-only marker: agentty appends it to offer the 1M
    // context window as a separate choice, and wire_model_id strips it before
    // the id reaches the wire. pretty_model_label strips it too — correctly,
    // since a wire marker must never leak as literal "[1m]" — but that made
    // `claude-opus-4-8` and `claude-opus-4-8[1m]` render as the SAME string.
    // Two identical rows, and the only difference (the context column) sat in
    // reference-weight text at the far end of the row.
    //
    // The variant IS the point of the row, so it belongs in the name. Recurse
    // on the stripped id and re-attach a human suffix.
    for (const auto& [marker, suffix] :
         {std::pair<std::string_view, std::string_view>{"[1m]", " · 1M"},
          std::pair<std::string_view, std::string_view>{"[2m]", " · 2M"}}) {
        if (auto pos = id.find(marker); pos != std::string_view::npos) {
            std::string bare{id.substr(0, pos)};
            bare += id.substr(pos + marker.size());
            // The server rarely names the variant; when it does (e.g.
            // "Claude Opus 4.8 (1M Context)") that name already says it, so
            // don't double up.
            std::string base = model_display_label(bare, display_name);
            const bool already_says =
                base.find("1M") != std::string::npos
                || base.find("2M") != std::string::npos;
            return already_says ? base : base + std::string{suffix};
        }
    }

    // No server name (OpenAI-compat / Ollama that echo the id) → the
    // id-normalized label is the only source.
    if (display_name.empty() || display_name == id)
        return pretty_model_label(id);

    const std::string from_id   = pretty_model_label(id);
    const std::string from_name = pretty_model_label(display_name);

    // Is the server name just a re-cased / re-punctuated spelling of the
    // id ("GPT-4o" for gpt-4o, "Hy-MT2-30B-A3B" for hy-mt2-30b-a3b)? If
    // so, both normalize to the same tidy form and we prefer the
    // id-derived one for cross-provider CONSISTENCY. We compare the
    // alphanumeric-only, case-folded skeletons: identical skeleton ⇒
    // same model, no marketing signal.
    auto skeleton = [](std::string_view s) {
        std::string k;
        k.reserve(s.size());
        for (char c : s)
            if (std::isalnum(static_cast<unsigned char>(c)))
                k.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
        return k;
    };
    if (skeleton(from_id) == skeleton(from_name) ||
        skeleton(display_name) == skeleton(id))
        return from_id;

    // The names genuinely diverge — a marketing alias the id can't
    // reconstruct ("Nano Banana Pro" for gemini-3-pro-image). Keep the
    // human name, but NORMALIZED so its own cruft ((latest), casing)
    // is cleaned to match the rest of the list's typography.
    return from_name;
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
