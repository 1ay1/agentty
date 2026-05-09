#pragma once
// #symbol picker — opens above the composer when the user types `#`
// at a word boundary. Same shape as MentionPaletteState (Closed | Open
// {query, index, candidates}) but the candidate set is workspace
// symbols instead of file paths. Symbol = (name, path, line) — the
// chip we attach on select carries all three so submit-time expansion
// can splice an excerpt of the file around the declaration.
//
// The candidate set is built once per process by walking the
// workspace and applying a small set of language regex patterns
// (find_definition tool's pattern set, in lighter form). The walk
// runs synchronously the first time `#` is hit; subsequent opens
// reuse the cached vector. Workspaces with thousands of source files
// take ~1 second on first open — visible but bounded; the user gets
// a "scanning workspace…" message until the walk finishes.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace moha {

struct SymbolEntry {
    std::string name;        // identifier, e.g. "submit_message"
    std::string path;        // workspace-relative file path
    int         line_number = 0;  // 1-based
};

namespace symbol_palette {

struct Closed {};

struct Open {
    std::string              query;
    int                      index = 0;
    std::vector<SymbolEntry> entries;
};

} // namespace symbol_palette

using SymbolPaletteState = std::variant<symbol_palette::Closed, symbol_palette::Open>;

[[nodiscard]] inline bool symbol_palette_is_open(const SymbolPaletteState& s) noexcept {
    return std::holds_alternative<symbol_palette::Open>(s);
}
[[nodiscard]] inline       symbol_palette::Open* symbol_palette_opened(SymbolPaletteState& s)       noexcept { return std::get_if<symbol_palette::Open>(&s); }
[[nodiscard]] inline const symbol_palette::Open* symbol_palette_opened(const SymbolPaletteState& s) noexcept { return std::get_if<symbol_palette::Open>(&s); }

// Walk the workspace once, return all definitions matched by the
// language regex set. Cached per-process: first call walks the
// disk; subsequent calls return the cached vector by const-ref. Cap
// at `cap` entries to bound the picker's working set on huge repos.
[[nodiscard]] const std::vector<SymbolEntry>&
list_workspace_symbols(std::size_t cap = 50000);

// Case-insensitive substring filter on the symbol NAME (not path).
// Returns indices into `entries` so the dispatcher resolves the
// cursor → (name, path, line) using the same view the picker rendered.
[[nodiscard]] std::vector<std::size_t>
filter_symbols(const std::vector<SymbolEntry>& entries, std::string_view query);

} // namespace moha
