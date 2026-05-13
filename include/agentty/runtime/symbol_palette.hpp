#pragma once
// #symbol picker — opens above the composer when the user types `#`
// at a word boundary. Same shape as MentionPaletteState (Closed | Open
// {query, index, candidates}) but the candidate set is workspace
// symbols instead of file paths. Symbol = (name, path, line) — the
// chip we attach on select carries all three so submit-time expansion
// can splice an excerpt of the file around the declaration.
//
// The workspace scanner that produces SymbolEntry (and the filter
// helper) live in `workspace/symbols.hpp`; this header is
// UI-state-only and re-imports SymbolEntry for the Open variant's
// vector.

#include <string>
#include <variant>
#include <vector>

#include "agentty/workspace/symbols.hpp"  // SymbolEntry

namespace agentty {

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

} // namespace agentty
