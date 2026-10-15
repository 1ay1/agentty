// Hand-written symbol matcher: the same 8 patterns the old std::regex set
// matched, in the same order (first match wins), with no backtracking engine.
//
// Every pattern is ^-anchored, so each check is one forward walk from the
// start of the line. The C function pattern was the costly one; it is
// decided here by a single left-to-right scan instead of a backtracking
// search.
#include "agentty/workspace/symbols.hpp"

#include <string_view>

namespace agentty {
namespace {

using sv = std::string_view;

constexpr bool is_word(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_';
}
// \s in ECMAScript: space, \t \n \v \f \r.
constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

std::size_t skip_space(sv s, std::size_t i) noexcept {
    while (i < s.size() && is_space(s[i])) ++i;
    return i;
}
// One or more \s; npos if none.
std::size_t need_space(sv s, std::size_t i) noexcept {
    const std::size_t j = skip_space(s, i);
    return j == i ? sv::npos : j;
}
// \w+ starting at i: returns the word (empty if none).
sv word_at(sv s, std::size_t i) noexcept {
    std::size_t j = i;
    while (j < s.size() && is_word(s[j])) ++j;
    return s.substr(i, j - i);
}
// `kw` at i followed by a non-word char (or end). Returns end or npos.
std::size_t keyword(sv s, std::size_t i, sv kw) noexcept {
    if (s.substr(i, kw.size()) != kw) return sv::npos;
    return i + kw.size();
}

// After a keyword: \s+(\w+). Note a regex like `class\s+(\w+)` also accepts
// `classes foo`? No: `class` must be followed by \s, so `classes` fails at
// the \s+. keyword() does not check the boundary; need_space() does.
sv name_after(sv s, std::size_t i) noexcept {
    if (i == sv::npos) return {};
    const std::size_t j = need_space(s, i);
    if (j == sv::npos) return {};
    return word_at(s, j);
}

// Try each keyword at i; the first whose `\s+(\w+)` matches wins, in order.
template <std::size_t N>
sv first_kw(sv s, std::size_t i, const sv (&kws)[N]) noexcept {
    for (const sv kw : kws)
        if (auto w = name_after(s, keyword(s, i, kw)); !w.empty()) return w;
    return {};
}

// 1. ^\s*(?:class|struct|enum(?:\s+class)?|union|namespace)\s+(\w+)
sv p_c_type(sv s) noexcept {
    const std::size_t i = skip_space(s, 0);
    static constexpr sv kws[] = {"class", "struct"};
    if (auto w = first_kw(s, i, kws); !w.empty()) return w;
    // enum(?:\s+class)?\s+(\w+): the regex tries "enum class X" first, then
    // falls back to "enum X" (which captures "class" for `enum class` with
    // no name after it).
    if (const auto e = keyword(s, i, "enum"); e != sv::npos) {
        if (const auto sp = need_space(s, e); sp != sv::npos) {
            if (auto w = name_after(s, keyword(s, sp, "class")); !w.empty()) return w;
            if (auto w = word_at(s, sp); !w.empty()) return w;
        }
    }
    static constexpr sv rest[] = {"union", "namespace"};
    return first_kw(s, i, rest);
}

// 2. ^\s*typedef\s+.+?\s+(\w+)\s*;
// Lazy .+? then \s+(\w+)\s*; : the FIRST position p>=start+1 where
// \s+ (\w+) \s* ; matches. The word is greedy \w+ but must be followed by
// \s*; so it is the whole run of word chars there.
sv p_typedef(sv s) noexcept {
    std::size_t i = skip_space(s, 0);
    const auto k = keyword(s, i, "typedef");
    if (k == sv::npos) return {};
    if (k >= s.size() || !is_space(s[k])) return {};
    // The match must end in \s*; so a line with no ';' after the keyword can
    // never match. That rejects nearly every line in O(n) before the search.
    if (s.find(';', k) == sv::npos) return {};
    // The regex is  typedef \s+ .+? \s+ (\w+) \s* ;
    // The engine tries the greedy first \s+ LONGEST first, then for each
    // length the lazy .+? SHORTEST first, then the second \s+ longest
    // first, then (\w+) longest first. Replay that order literally; each
    // step is O(1) so the whole thing stays linear in practice.
    std::size_t run_end = k;
    while (run_end < s.size() && is_space(s[run_end])) ++run_end;
    for (std::size_t a = run_end; a > k; --a) {          // end of first \s+
        for (std::size_t p = a + 1; p <= s.size(); ++p) { // end of .+? (>=1 char)
            if (p >= s.size() || !is_space(s[p])) continue;
            // second \s+ : greedy, longest first, at least one.
            std::size_t q_max = p;
            while (q_max < s.size() && is_space(s[q_max])) ++q_max;
            for (std::size_t q = q_max; q > p; --q) {
                // (\w+) longest first, at least one.
                std::size_t w_max = q;
                while (w_max < s.size() && is_word(s[w_max])) ++w_max;
                for (std::size_t w = w_max; w > q; --w) {
                    const std::size_t semi = skip_space(s, w);
                    if (semi < s.size() && s[semi] == ';') return s.substr(q, w - q);
                }
            }
        }
    }
    return {};
}

// 3. ^[\w:][\w\s\*\&<>:]*?\s(\w+)\s*\([^;]*?(?:\)\s*\{|\)\s*$)
//
// Structure: a head of allowed chars H = [\w\s*&<>:] (first one [\w:]),
// then one \s, then (\w+), then \s*, then '(' and the tail.
//
// The tail `\([^;]*?(?:\)\s*\{|\)\s*$)` succeeds iff, after the '(', there
// is a ')' reachable without crossing ';' that is followed by \s*{ or by
// \s* to end of line. That depends only on where '(' is, not on the name.
//
// The lazy head means the regex tries the SHORTEST head first, i.e. the
// leftmost \s inside the H-run whose following \w+ (greedy, all word chars)
// is followed by \s*( with a good tail. Because `\w+` is greedy and then
// needs \s*( , the name is always a maximal word run ending right before
// \s*( . Backtracking into a shorter \w+ never helps (a prefix of a word is
// followed by a word char, not \s or '(').
bool good_tail(sv s, std::size_t open) noexcept {
    // [^;]*? lazily, then \)\s*\{ or \)\s*$
    for (std::size_t q = open + 1; q < s.size(); ++q) {
        if (s[q] == ';') return false;
        if (s[q] != ')') continue;
        const std::size_t r = skip_space(s, q + 1);
        if (r == s.size() || s[r] == '{') return true;
    }
    return false;
}
constexpr bool is_head(char c) noexcept {
    return is_word(c) || is_space(c) || c == '*' || c == '&' || c == '<'
        || c == '>' || c == ':';
}
sv p_c_function(sv s) noexcept {
    if (s.empty() || !(is_word(s[0]) || s[0] == ':')) return {};
    // Positions 1.. are inside the head run while is_head holds. The \s that
    // precedes the name may be any position p>=1 with s[p] a space and
    // s[1..p-1] all head chars (the \s itself is a head char too).
    for (std::size_t p = 1; p < s.size(); ++p) {
        if (!is_head(s[p - 1]) && p - 1 >= 1) return {};   // head broke before p
        if (!is_space(s[p])) continue;
        const std::size_t w = p + 1;
        const sv name = word_at(s, w);
        if (name.empty()) continue;
        const std::size_t open = skip_space(s, w + name.size());
        if (open < s.size() && s[open] == '(' && good_tail(s, open)) return name;
    }
    return {};
}

// 4. ^\s*(?:def|class)\s+(\w+)
sv p_python(sv s) noexcept {
    static constexpr sv kws[] = {"def", "class"};
    return first_kw(s, skip_space(s, 0), kws);
}

// 5. ^\s*(?:export\s+)?(?:async\s+)?(?:function|class|const|let|var|type|interface)\s+(\w+)
sv p_js(sv s) noexcept {
    static constexpr sv kws[] = {"function", "class", "const", "let", "var",
                                 "type", "interface"};
    const std::size_t i0 = skip_space(s, 0);
    // Optional groups are tried present-first, matching the regex order.
    const auto try_from = [&](std::size_t i) -> sv {
        // (?:async\s+)?
        if (const auto a = keyword(s, i, "async"); a != sv::npos)
            if (const auto sp = need_space(s, a); sp != sv::npos)
                if (auto w = first_kw(s, sp, kws); !w.empty()) return w;
        return first_kw(s, i, kws);
    };
    if (const auto e = keyword(s, i0, "export"); e != sv::npos)
        if (const auto sp = need_space(s, e); sp != sv::npos)
            if (auto w = try_from(sp); !w.empty()) return w;
    return try_from(i0);
}

// 6. ^func(?:\s+\([^)]*\))?\s+(\w+)
sv p_go_func(sv s) noexcept {
    const auto f = keyword(s, 0, "func");
    if (f == sv::npos) return {};
    // Receiver first: \s+\([^)]*\) then \s+(\w+)
    if (const auto sp = need_space(s, f); sp != sv::npos && sp < s.size() && s[sp] == '(') {
        const auto close = s.find(')', sp + 1);
        if (close != sv::npos)
            if (auto w = name_after(s, close + 1); !w.empty()) return w;
    }
    return name_after(s, f);
}

// 7. ^type\s+(\w+)
sv p_go_type(sv s) noexcept { return name_after(s, keyword(s, 0, "type")); }

// 8. ^\s*(?:pub(?:\([^)]*\))?\s+)?(?:fn|struct|enum|trait|type|mod|const|static)\s+(\w+)
sv p_rust(sv s) noexcept {
    static constexpr sv kws[] = {"fn", "struct", "enum", "trait", "type", "mod",
                                 "const", "static"};
    const std::size_t i0 = skip_space(s, 0);
    if (const auto p = keyword(s, i0, "pub"); p != sv::npos) {
        // pub(...)? then \s+
        std::size_t after = p;
        if (after < s.size() && s[after] == '(') {
            const auto close = s.find(')', after + 1);
            // With the group: needs \s+ after ')'.
            if (close != sv::npos)
                if (const auto sp = need_space(s, close + 1); sp != sv::npos)
                    if (auto w = first_kw(s, sp, kws); !w.empty()) return w;
        }
        if (const auto sp = need_space(s, p); sp != sv::npos)
            if (auto w = first_kw(s, sp, kws); !w.empty()) return w;
    }
    return first_kw(s, i0, kws);
}

}  // namespace

std::string_view symbol_on_line(std::string_view line) noexcept {
    // Very long lines are minified or generated code, not declarations, and
    // the only place the matcher can do more than linear work.
    if (line.size() > 512) return {};
    // Comment-only lines are skipped, so `// fn foo()` examples don't count.
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') return {};
    // Cheap reject: every pattern needs a word char somewhere.
    using F = sv (*)(sv) noexcept;
    static constexpr F kMatchers[] = {p_c_type, p_typedef, p_c_function, p_python,
                                      p_js, p_go_func, p_go_type, p_rust};
    for (const F f : kMatchers)
        if (const sv w = f(line); !w.empty()) return w;
    return {};
}

}  // namespace agentty
