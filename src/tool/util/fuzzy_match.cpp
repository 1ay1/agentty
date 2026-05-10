#include "agentty/tool/util/fuzzy_match.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agentty::tools::util {

// ─────────────────────────────────────────────────────────────────────────
// Shared line scanner.
//
// `Line` records three offsets per line:
//   start       — first byte of the line (after the prior '\n').
//   end         — one past the '\n' (or file end).
//   indent_end  — offset of the first non-whitespace char (== end on a
//                 blank line).
//   trimmed_end — offset past the last non-whitespace char. On a blank
//                 line, indent_end == trimmed_end == start.
// ─────────────────────────────────────────────────────────────────────────

namespace {

struct Line {
    std::size_t start;
    std::size_t end;
    std::size_t indent_end;
    std::size_t trimmed_end;
};

constexpr bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r';
}

std::vector<Line> scan_lines(std::string_view s) {
    std::vector<Line> out;
    out.reserve(s.size() / 40 + 1);
    auto push = [&](std::size_t start, std::size_t end) {
        // trimmed_end: walk back over \r/space/tab.
        std::size_t te = end;
        if (te > start && s[te - 1] == '\n') --te;
        while (te > start && is_ws(s[te - 1])) --te;
        // indent_end: walk forward over space/tab. (Don't skip \r; that
        // can only occur on a blank CRLF line, in which case the check
        // below leaves indent_end at start.)
        std::size_t ie = start;
        while (ie < te && (s[ie] == ' ' || s[ie] == '\t')) ++ie;
        out.push_back({start, end, ie, te});
    };
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') { push(start, i + 1); start = i + 1; }
    }
    if (start < s.size()) push(start, s.size());
    return out;
}

// Byte-equal comparison over two substring ranges.
bool range_eq(std::string_view a, std::size_t a_lo, std::size_t a_hi,
              std::string_view b, std::size_t b_lo, std::size_t b_hi) noexcept {
    auto la = a_hi - a_lo, lb = b_hi - b_lo;
    if (la != lb) return false;
    for (std::size_t i = 0; i < la; ++i)
        if (a[a_lo + i] != b[b_lo + i]) return false;
    return true;
}

// Count exact occurrences of `needle` in `file` (used for strategy 1's
// uniqueness bookkeeping).
int count_occurrences(std::string_view file, std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > file.size()) return 0;
    int n = 0;
    std::size_t p = 0;
    while ((p = file.find(needle, p)) != std::string_view::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

// ── Strategy 2: CRLF-normalize, exact ────────────────────────────────────
//
// Windows-authored files and pastes from web tools frequently carry \r\n
// line endings while the model emits plain \n (or vice versa). We do the
// match on a \r-stripped view of the file AND the needle, then recover
// the original byte range by replaying the strip offsets.

struct StripResult {
    std::string          stripped;
    std::vector<std::size_t> src_of;  // src_of[i] = original byte offset
                                      // of stripped[i] (one past the end
                                      // is represented by the final entry
                                      // src_of[stripped.size()] = file.size()).
};

StripResult strip_cr(std::string_view s) {
    StripResult r;
    r.stripped.reserve(s.size());
    r.src_of.reserve(s.size() + 1);
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') continue;
        r.src_of.push_back(i);
        r.stripped.push_back(s[i]);
    }
    r.src_of.push_back(s.size());
    return r;
}

// ── Strategy 4/5 helpers ─────────────────────────────────────────────────
//
// Detect a uniform per-line indent delta between a needle block and the
// file lines it matched. Returns {have_delta, delta}. "Delta" means: for
// every non-blank line pair, file_indent == needle_indent prefixed-or-
// replaced by the same bytes. Concretely: we require there exists a
// string D such that for every non-blank pair, file_indent == D +
// needle_indent (the file is deeper) OR needle_indent == D + file_indent
// (the file is shallower), consistently across all lines.
//
// The common cases this captures:
//   - model output is at 2-space indent, file at 4-space → D = "  "
//     prepended to every new_text non-blank line.
//   - model output at 4-space, file at 2-space → strip first 2 spaces
//     from every new_text non-blank line.
//   - model tabs, file spaces, or vice versa → the "D" is computed
//     byte-wise so mixed tab/space works if it's consistent.
//
// If the delta isn't consistent (some lines deeper, others shallower, or
// differing prefixes), we return have_delta=false and leave new_text
// unchanged. Better to keep the model's text than to corrupt it.

// Detect a structural indentation shift between the needle and the file
// lines it matched. We model it as: every non-blank needle line has the
// form `needle_base + tail_i`, every corresponding file line has the form
// `file_base + tail_i`, where `tail_i` is identical on both sides and
// `needle_base`/`file_base` are the common leading indent of each side
// respectively. If such a decomposition exists, we return it; `new_text`
// is then transformed by stripping `needle_base` from each non-blank line
// (if present) and prepending `file_base`.
//
// This handles the common realistic case where the model emits a block
// at "outdented" zero or shallow indent (e.g. a method body shown without
// its class wrapper) and the file has it at deeper indent — the per-line
// byte delta differs (outer line gains 4 spaces, body line gains 4+2=6),
// but the STRUCTURAL delta "drop the first N chars, add the first M chars"
// is uniform.

struct IndentDelta {
    bool        have = false;
    std::string needle_base;   // strip this prefix from each non-blank new_text line
    std::string file_base;     // then prepend this one
};

// Longest common prefix of two byte strings.
std::size_t common_prefix_len(std::string_view a, std::string_view b) noexcept {
    std::size_t n = std::min(a.size(), b.size());
    std::size_t k = 0;
    while (k < n && a[k] == b[k]) ++k;
    return k;
}

IndentDelta detect_indent_delta(std::string_view file_text,
                                const std::vector<Line>& matched_file_lines,
                                std::string_view needle_text,
                                const std::vector<Line>& nl) {
    // 1. Find needle_base: the longest common whitespace prefix of every
    //    non-blank needle line's indent.
    std::string_view needle_base;
    bool first = true;
    for (const auto& N : nl) {
        if (N.indent_end == N.trimmed_end) continue;
        std::string_view ind{needle_text.data() + N.start, N.indent_end - N.start};
        if (first) { needle_base = ind; first = false; }
        else       { needle_base = needle_base.substr(0, common_prefix_len(needle_base, ind)); }
    }
    if (first) return {};   // no non-blank lines — nothing to infer

    // 2. Same for file_base.
    std::string_view file_base;
    first = true;
    for (std::size_t i = 0; i < nl.size(); ++i) {
        const auto& F = matched_file_lines[i];
        const auto& N = nl[i];
        if (N.indent_end == N.trimmed_end) continue;
        if (F.indent_end == F.trimmed_end) continue;
        std::string_view ind{file_text.data() + F.start, F.indent_end - F.start};
        if (first) { file_base = ind; first = false; }
        else       { file_base = file_base.substr(0, common_prefix_len(file_base, ind)); }
    }
    if (first) return {};

    // 3. Verify the transformation is consistent: for every non-blank
    //    pair, needle_line has `needle_base + tail` and file_line has
    //    `file_base + tail` with the SAME tail (including the tail's own
    //    leading whitespace, which represents real intra-block indentation).
    for (std::size_t i = 0; i < nl.size(); ++i) {
        const auto& N = nl[i];
        const auto& F = matched_file_lines[i];
        if (N.indent_end == N.trimmed_end) continue;
        if (F.indent_end == F.trimmed_end) return {};  // needle non-blank maps to file blank — weird
        std::string_view nind{needle_text.data() + N.start, N.indent_end - N.start};
        std::string_view find{file_text.data()   + F.start, F.indent_end - F.start};
        // Peel the bases.
        if (nind.size() < needle_base.size()) return {};
        if (find.size() < file_base.size())   return {};
        std::string_view ntail = nind.substr(needle_base.size());
        std::string_view ftail = find.substr(file_base.size());
        if (ntail != ftail) return {};
        // And the post-indent content must match byte-for-byte (already
        // guaranteed by strategy 4's match, but we double-check indirectly
        // by comparing trimmed content ranges).
        if (!range_eq(file_text, F.indent_end, F.trimmed_end,
                      needle_text, N.indent_end, N.trimmed_end))
            return {};
    }

    IndentDelta d;
    d.have        = true;
    d.needle_base.assign(needle_base);
    d.file_base.assign(file_base);
    return d;
}

// Apply the structural shift to `text`: strip `needle_base` from each
// non-blank line's leading bytes if present, then prepend `file_base`.
// Blank lines are preserved verbatim so we don't introduce trailing
// whitespace on them.
std::string apply_indent_delta(std::string_view text, const IndentDelta& d) {
    if (!d.have) return std::string{text};
    if (d.needle_base.empty() && d.file_base.empty()) return std::string{text};
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t line_start = i;
        while (i < text.size() && text[i] != '\n') ++i;
        std::size_t line_end = i;
        if (i < text.size()) ++i;  // consume '\n'
        bool blank = true;
        for (std::size_t k = line_start; k < line_end; ++k)
            if (!is_ws(text[k])) { blank = false; break; }
        if (blank) {
            out.append(text.data() + line_start, line_end - line_start);
        } else {
            std::size_t strip = 0;
            if (!d.needle_base.empty()
                && line_end - line_start >= d.needle_base.size()
                && std::string_view{text.data() + line_start, d.needle_base.size()} == d.needle_base) {
                strip = d.needle_base.size();
            }
            out.append(d.file_base);
            out.append(text.data() + line_start + strip,
                       line_end - line_start - strip);
        }
        if (line_end < text.size()) out.push_back('\n');
    }
    return out;
}

// Normalize a byte string for strategy 5:
//   - strip carriage returns
//   - replace common unicode gotchas with ASCII
//       NBSP (U+00A0, C2 A0)         → ' '
//       Narrow NBSP (U+202F, E2 80 AF)
//       Figure space / en / em / thin → ' '
//       Left/right single quote       → '\''
//       Left/right double quote       → '"'
//       En/em dash                    → '-'
//       Ellipsis (U+2026)             → '...'
//   - collapse any run of [space tab] into a single space
// Newlines are preserved 1:1. We also record a parallel src_of[] mapping
// from every emitted byte back to the original offset so the caller can
// recover the file-coordinate range of a match.

struct SquashResult {
    std::string              text;
    std::vector<std::size_t> src_of; // size == text.size() + 1
};

SquashResult squash_normalize(std::string_view s) {
    SquashResult r;
    r.text.reserve(s.size());
    r.src_of.reserve(s.size() + 1);
    bool in_ws = false;
    auto emit = [&](char c, std::size_t src) {
        if (c == ' ' || c == '\t') {
            if (in_ws) return;        // collapse
            in_ws = true;
            r.src_of.push_back(src);
            r.text.push_back(' ');
        } else {
            in_ws = false;
            r.src_of.push_back(src);
            r.text.push_back(c);
        }
    };
    for (std::size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r') { ++i; continue; }
        if (c == '\n') {
            in_ws = false;
            r.src_of.push_back(i);
            r.text.push_back('\n');
            ++i;
            continue;
        }
        if (c < 0x80) {
            emit(static_cast<char>(c), i);
            ++i;
            continue;
        }
        // 2-byte UTF-8: U+0080..U+07FF.
        if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            std::uint32_t cp = ((c & 0x1Fu) << 6) | (c2 & 0x3Fu);
            if (cp == 0x00A0) { emit(' ', i); i += 2; continue; }
            // Pass through as bytes.
            emit(static_cast<char>(c),  i);
            emit(static_cast<char>(c2), i + 1);
            i += 2;
            continue;
        }
        // 3-byte UTF-8: U+0800..U+FFFF.
        if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 2]);
            std::uint32_t cp = ((c & 0x0Fu) << 12)
                             | ((c2 & 0x3Fu) << 6)
                             |  (c3 & 0x3Fu);
            switch (cp) {
                case 0x2002: case 0x2003: case 0x2004: case 0x2005:
                case 0x2006: case 0x2007: case 0x2008: case 0x2009:
                case 0x200A: case 0x202F: case 0x205F: case 0x3000:
                    emit(' ', i);   i += 3; continue;
                case 0x2018: case 0x2019: case 0x201A: case 0x201B:
                    emit('\'', i);  i += 3; continue;
                case 0x201C: case 0x201D: case 0x201E: case 0x201F:
                    emit('"',  i);  i += 3; continue;
                case 0x2013: case 0x2014: case 0x2212:
                    emit('-',  i);  i += 3; continue;
                case 0x2026:
                    emit('.', i); emit('.', i); emit('.', i);
                    i += 3; continue;
                default: break;
            }
            emit(static_cast<char>(c),  i);
            emit(static_cast<char>(c2), i + 1);
            emit(static_cast<char>(c3), i + 2);
            i += 3;
            continue;
        }
        // 4-byte and invalid: pass through.
        emit(static_cast<char>(c), i);
        ++i;
    }
    r.src_of.push_back(s.size());
    return r;
}

// Slide `needle_lines` across `file_lines` comparing byte ranges with the
// supplied predicate. Return {ok, pos, len, count} in *file* coordinates.
// `eq` is a line-level equality: (fline, nline) -> bool.
template <class LineEq>
FuzzyMatch sliding_match(std::string_view file_text,
                         const std::vector<Line>& fl,
                         std::string_view needle_text,
                         const std::vector<Line>& nl,
                         LineEq eq) {
    FuzzyMatch r{false, 0, 0, 0, {}, 0};
    if (nl.empty() || nl.size() > fl.size()) return r;

    std::size_t found_pos = 0, found_end = 0;
    int count = 0;
    for (std::size_t i = 0; i + nl.size() <= fl.size(); ++i) {
        bool all = true;
        for (std::size_t k = 0; k < nl.size(); ++k) {
            if (!eq(file_text, fl[i + k], needle_text, nl[k])) {
                all = false; break;
            }
        }
        if (!all) continue;

        std::size_t pos = fl[i].start;
        std::size_t end = fl[i + nl.size() - 1].end;
        // Needle-had-trailing-newline? If not, strip the file's trailing
        // \n from the match range so the splice length matches what the
        // caller's `new_text` expects.
        bool needle_had_trailing_nl = !needle_text.empty() && needle_text.back() == '\n';
        if (!needle_had_trailing_nl && end > pos && file_text[end - 1] == '\n')
            --end;

        if (count == 0) { found_pos = pos; found_end = end; }
        ++count;
        if (count > 1) { r.count = count; return r; }
    }
    if (count == 1) {
        r.ok = true;
        r.pos = found_pos;
        r.len = found_end - found_pos;
        r.count = 1;
    }
    return r;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────

FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle) {
    return fuzzy_find(file, needle, {});
}

FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text) {
    if (needle.empty()) return {false, 0, 0, 0, {}, 0};

    // ── 1. Exact match, uniqueness check ────────────────────────────────
    {
        int n = count_occurrences(file, needle);
        if (n == 1) {
            auto pos = file.find(needle);
            return {true, pos, needle.size(), 1, {}, 1};
        }
        if (n >= 2) return {false, 0, 0, n, {}, 0};
    }

    // ── 2. CRLF-normalized exact ────────────────────────────────────────
    //
    // Only worth the cost if there's actually a \r in either side.
    if (file.find('\r') != std::string_view::npos
        || needle.find('\r') != std::string_view::npos) {
        auto sf = strip_cr(file);
        auto sn = strip_cr(needle);
        int n = count_occurrences(sf.stripped, sn.stripped);
        if (n == 1) {
            auto p = sf.stripped.find(sn.stripped);
            std::size_t src_lo = sf.src_of[p];
            std::size_t src_hi = sf.src_of[p + sn.stripped.size()];
            return {true, src_lo, src_hi - src_lo, 1, {}, 2};
        }
        if (n >= 2) return {false, 0, 0, n, {}, 0};
    }

    // ── Line windows ────────────────────────────────────────────────────
    auto fl = scan_lines(file);
    auto nl = scan_lines(needle);

    // ── 3. Trailing-whitespace tolerant ─────────────────────────────────
    {
        auto m = sliding_match(file, fl, needle, nl,
            [](std::string_view f, const Line& a,
               std::string_view n, const Line& b) {
                return range_eq(f, a.start, a.trimmed_end,
                                n, b.start, b.trimmed_end);
            });
        if (m.ok)        { m.strategy = 3; return m; }
        if (m.count >= 2){ return m; }
    }

    // ── 4. Both-sides-trim (indentation drift) ──────────────────────────
    //
    // Compare line *content* (between indent_end and trimmed_end). If we
    // land a unique hit and the caller supplied `new_text`, re-indent it
    // from the needle's uniform prefix to the file's uniform prefix so
    // the splice keeps the file's convention.
    {
        auto m = sliding_match(file, fl, needle, nl,
            [](std::string_view f, const Line& a,
               std::string_view n, const Line& b) {
                // Blank lines: both must be blank.
                bool ablank = (a.indent_end == a.trimmed_end);
                bool bblank = (b.indent_end == b.trimmed_end);
                if (ablank != bblank) return false;
                if (ablank) return true;
                return range_eq(f, a.indent_end, a.trimmed_end,
                                n, b.indent_end, b.trimmed_end);
            });
        if (m.count >= 2) return m;
        if (m.ok) {
            m.strategy = 4;
            if (!new_text.empty()) {
                // Find the matched file lines so we can read their indent.
                // sliding_match doesn't return the window index, so rescan
                // by pos.
                std::size_t wi = 0;
                while (wi < fl.size() && fl[wi].start != m.pos) ++wi;
                if (wi < fl.size() && wi + nl.size() <= fl.size()) {
                    std::vector<Line> matched(fl.begin() + static_cast<std::ptrdiff_t>(wi),
                                              fl.begin() + static_cast<std::ptrdiff_t>(wi + nl.size()));
                    auto d = detect_indent_delta(file, matched, needle, nl);
                    if (d.have && d.needle_base != d.file_base) {
                        m.adjusted_new_text = apply_indent_delta(new_text, d);
                    }
                }
            }
            return m;
        }
    }

    // ── 5. Whitespace-squash + unicode-normalize ────────────────────────
    //
    // Last-chance match. We compare per-line AFTER normalizing both sides
    // with `squash_normalize` (all internal whitespace runs collapsed to
    // a single space, NBSP/smart-quote/dash-ish unicode folded to ASCII).
    {
        auto sf = squash_normalize(file);
        auto sn = squash_normalize(needle);
        auto slf = scan_lines(sf.text);
        auto sln = scan_lines(sn.text);
        auto m = sliding_match(sf.text, slf, sn.text, sln,
            [](std::string_view f, const Line& a,
               std::string_view n, const Line& b) {
                return range_eq(f, a.indent_end, a.trimmed_end,
                                n, b.indent_end, b.trimmed_end);
            });
        if (m.count >= 2) return m;
        if (m.ok) {
            // Lift squashed coords back to original file coords.
            std::size_t lo = sf.src_of[m.pos];
            std::size_t hi = sf.src_of[m.pos + m.len];
            return {true, lo, hi - lo, 1, {}, 5};
        }
    }

    return {false, 0, 0, 0, {}, 0};
}

} // namespace agentty::tools::util
