// complexity.cpp — the turn-complexity classifier (see complexity.hpp).
//
// Pure heuristic, no I/O. Deliberately conservative: Standard is the fallback,
// only strong signals move a turn up to Complex or down to Trivial. The goal
// is not perfect classification — it's a cheap, well-calibrated bias so the
// orchestrator spends reasoning effort where it pays off and skips it on
// throwaway turns, exactly as the multi-agent research prescribes.

#include "agentty/domain/complexity.hpp"

#include <array>
#include <cctype>

namespace agentty::smart {

namespace {

// Lowercase a byte (ASCII); leaves UTF-8 continuation bytes alone.
constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// Case-insensitive substring search over a lowercased haystack.
bool has(std::string_view hay_lower, std::string_view needle) noexcept {
    if (needle.size() > hay_lower.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= hay_lower.size(); ++i) {
        std::size_t j = 0;
        for (; j < needle.size(); ++j)
            if (hay_lower[i + j] != needle[j]) break;
        if (j == needle.size()) return true;
    }
    return false;
}

// Count words (whitespace-separated runs).
std::size_t word_count(std::string_view s) noexcept {
    std::size_t n = 0;
    bool in = false;
    for (char c : s) {
        const bool ws = (c == ' ' || c == '\t' || c == '\n' || c == '\r');
        if (!ws && !in) { ++n; in = true; }
        else if (ws) in = false;
    }
    return n;
}

// Strong "this needs real thinking" vocabulary. Design/architecture/debugging/
// explanation intent, and multi-step verbs.
constexpr std::array kComplexTerms = {
    std::string_view{"architect"}, std::string_view{"design"},
    std::string_view{"refactor"},  std::string_view{"redesign"},
    std::string_view{"debug"},     std::string_view{"root cause"},
    std::string_view{"why "},      std::string_view{" why"},
    std::string_view{"trade-off"}, std::string_view{"tradeoff"},
    std::string_view{"strategy"},  std::string_view{"approach"},
    std::string_view{"end to end"},std::string_view{"end-to-end"},
    std::string_view{"across the"},std::string_view{"whole"},
    std::string_view{"investigate"},std::string_view{"diagnose"},
    std::string_view{"optimize"},  std::string_view{"optimise"},
    std::string_view{"race condition"}, std::string_view{"deadlock"},
    std::string_view{"plan "},     std::string_view{"compare"},
    std::string_view{"evaluate"},  std::string_view{"review"},
    std::string_view{"state of the art"}, std::string_view{"deep"},
    std::string_view{"implement all"},
};

// Clearly-throwaway acknowledgements / one-word imperatives.
constexpr std::array kTrivialExact = {
    std::string_view{"yes"}, std::string_view{"no"}, std::string_view{"ok"},
    std::string_view{"okay"}, std::string_view{"yep"}, std::string_view{"yeah"},
    std::string_view{"thanks"}, std::string_view{"thank you"}, std::string_view{"ty"},
    std::string_view{"go"}, std::string_view{"go ahead"}, std::string_view{"do it"},
    std::string_view{"run it"}, std::string_view{"continue"}, std::string_view{"proceed"},
    std::string_view{"commit"}, std::string_view{"commit it"}, std::string_view{"push"},
    std::string_view{"stop"}, std::string_view{"cancel"}, std::string_view{"sure"},
    std::string_view{"nice"}, std::string_view{"perfect"}, std::string_view{"great"},
    std::string_view{"lgtm"}, std::string_view{"undo"}, std::string_view{"retry"},
    std::string_view{"again"}, std::string_view{"next"},
};

// Count enumerated asks ("1.", "2)", leading "- ", " and "): a proxy for a
// multi-part request that deserves more thinking.
std::size_t enumerated_asks(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        // line-leading list marker
        const bool bol = (i == 0) || s[i - 1] == '\n';
        if (bol) {
            char c = s[i];
            if (c == '-' || c == '*' ) ++n;
            else if (c >= '1' && c <= '9'
                     && i + 1 < s.size() && (s[i + 1] == '.' || s[i + 1] == ')'))
                ++n;
        }
    }
    return n;
}

} // namespace

Complexity classify_complexity(std::string_view text) noexcept {
    // Trim leading/trailing whitespace for the exact-match trivial check.
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    std::string_view trimmed = text.substr(b, e - b);

    if (trimmed.empty()) return Complexity::Trivial;

    // Lowercase copy for keyword scans (bounded; user turns are small).
    std::string low;
    low.reserve(trimmed.size());
    for (char c : trimmed) low.push_back(lower(c));

    const std::size_t words = word_count(trimmed);

    // 1. Exact trivial acknowledgements / one-word imperatives (no '?').
    if (words <= 3 && trimmed.find('?') == std::string_view::npos) {
        for (auto t : kTrivialExact)
            if (low == t) return Complexity::Trivial;
    }

    // 2. Strong complexity vocabulary → Complex.
    for (auto term : kComplexTerms)
        if (has(low, term)) return Complexity::Complex;

    // 3. Size / structure signals → Complex. A long turn, or several
    //    enumerated asks, is inherently multi-step.
    if (words >= 60 || enumerated_asks(trimmed) >= 3)
        return Complexity::Complex;

    // 4. A short, single-clause request → Simple.
    if (words <= 8 && trimmed.find('\n') == std::string_view::npos)
        return Complexity::Simple;

    // 5. Conservative default.
    return Complexity::Standard;
}

} // namespace agentty::smart
