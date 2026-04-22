#pragma once
// Whitespace-tolerant substring matching for the edit tool.
//
// Exact `std::string::find` fails whenever the model's `old_text` disagrees
// with the file on trivial whitespace (trailing spaces on a line, tabs vs.
// spaces, extra blank line). The model is usually correct about what it
// wants to change; penalizing it for indentation drift just forces a retry
// loop.
//
// `fuzzy_find(file, needle)` returns the exact byte range in `file` that
// semantically matches `needle`, if one exists and is unique. The rules:
//
//   1. Exact match wins; no normalization needed.
//   2. Otherwise split both sides into lines, trim each line's trailing
//      whitespace, and slide the needle's line window across the file. A
//      window matches when every line is byte-equal post-trim.
//   3. Multiple matches → {false, 0, 0, count}. Caller reports ambiguity.
//
// The returned range is in the *original* file coordinates — the edit tool
// rebuilds the output by splicing `new_text` between `[0, pos)` and
// `[pos+len, end)`, so the caller never has to reason about normalization.

#include <cstddef>
#include <string_view>

namespace moha::tools::util {

struct FuzzyMatch {
    bool        ok;      // true iff exactly one match
    std::size_t pos;     // byte offset into file (0 if !ok)
    std::size_t len;     // bytes of file that correspond to `needle`
    int         count;   // total matches seen (1 on ok; >1 means ambiguous)
};

FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle);

} // namespace moha::tools::util
