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
// semantically matches `needle`, if one exists and is unique. It tries
// increasingly permissive strategies, each gated on uniqueness:
//
//   1. Exact match. If unique, return it.
//   2. CRLF-normalized exact match (handles Windows files, mixed endings).
//   3. Line-window match trimming TRAILING whitespace per line.
//   4. Line-window match trimming BOTH sides per line (tolerates
//      indentation drift: tabs vs spaces, 2 vs 4 spaces, etc.). When the
//      matched block's own indent is uniform, `new_text` is re-indented
//      by the same delta so the splice keeps the file's convention.
//   5. Line-window match after collapsing *all* internal whitespace runs
//      to a single space and normalizing common unicode gotchas (NBSP,
//      smart quotes). Last resort before giving up.
//
// At every step, multiple matches → {false, 0, 0, count}. The caller
// reports ambiguity.
//
// The returned range is in the *original* file coordinates — the edit tool
// rebuilds the output by splicing `new_text` between `[0, pos)` and
// `[pos+len, end)`, so the caller never has to reason about normalization.
// When strategy 4 adjusts indent, the adjusted replacement is returned via
// `adjusted_new_text` and the caller should splice that instead of the
// raw `new_text`.

#include <cstddef>
#include <string>
#include <string_view>

namespace moha::tools::util {

struct FuzzyMatch {
    bool        ok;      // true iff exactly one match
    std::size_t pos;     // byte offset into file (0 if !ok)
    std::size_t len;     // bytes of file that correspond to `needle`
    int         count;   // total matches seen (1 on ok; >1 means ambiguous)
    // Populated only when the match required re-indenting `new_text` to
    // fit the file's indentation convention (strategy 4). Empty means the
    // caller should splice the original `new_text` unchanged.
    std::string adjusted_new_text;
    // Which strategy produced the hit — useful for diagnostics and tests.
    // 0 = none, 1 = exact, 2 = CRLF-normalized, 3 = trailing-ws,
    // 4 = both-sides-trim (may carry adjusted_new_text),
    // 5 = whitespace-squash + unicode-normalize.
    int         strategy = 0;
};

// Convenience overload — back-compat with callers that don't need the
// replacement re-indent (they pass their own `new_text` separately).
FuzzyMatch fuzzy_find(std::string_view file, std::string_view needle);

// Full-fidelity API — also accepts the intended replacement so strategy 4
// can return a `new_text` re-indented to match the file's convention. The
// caller MUST splice `result.adjusted_new_text` when it is non-empty;
// otherwise splice their original `new_text` unchanged.
FuzzyMatch fuzzy_find(std::string_view file,
                      std::string_view needle,
                      std::string_view new_text);

} // namespace moha::tools::util
