// SPDX-License-Identifier: Apache-2.0
#pragma once
// agentty::fuzzy — one scored subsequence matcher shared by every incremental
// picker (command palette, @-files, #-symbols, model / provider). Ranks a
// candidate by HOW WELL the query matches it (leading prefix > word-boundary /
// acronym > consecutive run > scattered subsequence; exact wins), and returns
// the matched character offsets so the view can highlight them. Extracted from
// the command palette so every picker feels equally intelligent, and so
// "why did this rank here" lives in exactly one place.

#include <cctype>
#include <climits>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::fuzzy {

struct Match {
    int              score = INT_MIN;  // higher = better; INT_MIN = no match
    std::vector<int> positions;        // matched byte offsets into the candidate
    [[nodiscard]] bool matched() const noexcept { return score != INT_MIN; }
};

// A word boundary is the start of an acronym-typable segment. Covers the
// separators real labels / paths / identifiers use.
[[nodiscard]] inline bool is_word_boundary(std::string_view s, std::size_t i) noexcept {
    if (i == 0) return true;
    char p = s[i - 1];
    if (p == ' ' || p == '-' || p == '/' || p == '_' || p == '.'
        || p == '(' || p == ':') return true;
    // camelCase / PascalCase hump: lower→Upper.
    char c = s[i];
    return (p >= 'a' && p <= 'z') && (c >= 'A' && c <= 'Z');
}

// Score `needle` (need NOT be pre-lowercased) as a subsequence of `hay`.
// Greedy left-to-right — suboptimal for pathological cases but fast and good
// enough for live filtering, matching the established @-picker scorer. Returns
// no-match if `needle` isn't a subsequence at all.
[[nodiscard]] inline Match score(std::string_view hay, std::string_view needle) {
    Match m;
    if (needle.empty()) { m.score = 0; return m; }
    if (needle.size() > hay.size()) return m;
    // ASCII fast-path lowercase. The candidates are model ids / provider
    // labels / paths — ASCII — so a branch beats std::tolower, which drags in
    // locale machinery on every char and dominated the per-keystroke cost of
    // ranking a large list. Non-ASCII bytes pass through unchanged (they were
    // never lowercased meaningfully by the C-locale tolower here either).
    auto lc = [](char c) noexcept -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };

    int s = 0, skipped = 0;
    std::size_t hi = 0, ni = 0;
    bool prev = false;
    m.positions.reserve(needle.size());
    while (ni < needle.size() && hi < hay.size()) {
        if (lc(hay[hi]) == lc(needle[ni])) {
            s += 16;
            if (prev)                       s += 18;  // consecutive run
            if (is_word_boundary(hay, hi))  s += 30;  // acronym / word start
            if (hi == 0)                    s += 25;  // leading prefix
            m.positions.push_back(static_cast<int>(hi));
            prev = true; ++ni; ++hi;
        } else { prev = false; ++skipped; ++hi; }
    }
    if (ni < needle.size()) return {};         // not a subsequence
    s -= skipped;                               // mild gap penalty
    if (hay.size() == needle.size()) s += 40;   // exact
    m.score = s;
    return m;
}

// Convenience: does `needle` fuzzy-match `hay` at all (subsequence)? Drop-in
// for the old boolean fuzzy_contains, but subsequence (not substring) so
// "sp4" matches "gpt-sonnet-4" the way users expect.
[[nodiscard]] inline bool matches(std::string_view hay, std::string_view needle) {
    return score(hay, needle).matched();
}

} // namespace agentty::fuzzy
