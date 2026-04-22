#include "moha/tool/util/fuzzy_match.hpp"

#include <vector>

namespace moha::tools::util {

namespace {

struct Line {
    std::size_t start;   // byte offset of first char
    std::size_t end;     // byte offset one past last char (including \n)
    std::size_t trimmed_end; // end of content after stripping trailing ws
};

std::vector<Line> scan_lines(std::string_view s) {
    std::vector<Line> out;
    out.reserve(s.size() / 40 + 1);
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            std::size_t te = i;
            while (te > start && (s[te - 1] == ' ' || s[te - 1] == '\t' ||
                                   s[te - 1] == '\r'))
                --te;
            out.push_back({start, i + 1, te});
            start = i + 1;
        }
    }
    if (start < s.size()) {
        std::size_t te = s.size();
        while (te > start && (s[te - 1] == ' ' || s[te - 1] == '\t' ||
                               s[te - 1] == '\r'))
            --te;
        out.push_back({start, s.size(), te});
    }
    return out;
}

bool trimmed_eq(std::string_view a, std::size_t a_lo, std::size_t a_hi,
                std::string_view b, std::size_t b_lo, std::size_t b_hi) {
    auto len_a = a_hi - a_lo;
    auto len_b = b_hi - b_lo;
    if (len_a != len_b) return false;
    for (std::size_t i = 0; i < len_a; ++i)
        if (a[a_lo + i] != b[b_lo + i]) return false;
    return true;
}

} // namespace

FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle) {
    if (needle.empty()) return {false, 0, 0, 0};

    // 1. Exact match, uniqueness check.
    {
        auto first = file.find(needle);
        if (first != std::string_view::npos) {
            auto second = file.find(needle, first + 1);
            if (second == std::string_view::npos)
                return {true, first, needle.size(), 1};
            int count = 2;
            auto p = second;
            while ((p = file.find(needle, p + 1)) != std::string_view::npos)
                ++count;
            return {false, 0, 0, count};
        }
    }

    // 2. Line-trim tolerant match. We split both sides into lines, trim
    //    trailing whitespace, and slide the needle window through the file.
    auto file_lines = scan_lines(file);
    auto ndl_lines  = scan_lines(needle);
    if (ndl_lines.empty() || ndl_lines.size() > file_lines.size())
        return {false, 0, 0, 0};

    std::size_t found_pos = 0;
    std::size_t found_len = 0;
    int count = 0;

    for (std::size_t i = 0; i + ndl_lines.size() <= file_lines.size(); ++i) {
        bool all = true;
        for (std::size_t k = 0; k < ndl_lines.size(); ++k) {
            const auto& fl = file_lines[i + k];
            const auto& nl = ndl_lines[k];
            if (!trimmed_eq(file, fl.start, fl.trimmed_end,
                             needle, nl.start, nl.trimmed_end)) {
                all = false; break;
            }
        }
        if (!all) continue;

        // Range covers the matched file lines exactly — including their
        // newline terminators (so splicing new_text in-place works cleanly).
        std::size_t pos = file_lines[i].start;
        std::size_t end = file_lines[i + ndl_lines.size() - 1].end;
        // If the needle's last line had no trailing newline, strip the
        // newline off the file's last line too so the splice length matches
        // the caller's expectation of `new_text`.
        const auto& last_ndl = ndl_lines.back();
        bool needle_had_trailing_nl =
            (last_ndl.end == needle.size() && !needle.empty()
             && needle.back() == '\n');
        if (!needle_had_trailing_nl && end > pos && file[end - 1] == '\n')
            --end;

        if (count == 0) { found_pos = pos; found_len = end - pos; }
        ++count;
        if (count > 1) return {false, 0, 0, count};
    }

    if (count == 1) return {true, found_pos, found_len, 1};
    return {false, 0, 0, 0};
}

} // namespace moha::tools::util
