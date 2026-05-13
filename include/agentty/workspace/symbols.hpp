#pragma once
// Workspace symbol enumeration — walk the active workspace root,
// apply a small language-aware regex set, and return all named
// definitions found. Same family as files.hpp: a pure-I/O scanner
// that the runtime's `#` picker consumes.
//
// SymbolEntry lives here (not in runtime/symbol_palette.hpp)
// because its shape is dictated by what the scanner emits; the UI
// state is downstream — symbol_palette::Open just holds a vector
// of these.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agentty {

struct SymbolEntry {
    std::string name;             // identifier, e.g. "submit_message"
    std::string path;             // workspace-relative file path
    int         line_number = 0;  // 1-based
};

// Walk the workspace once, return all definitions matched by the
// language regex set. Cached per-process: first call walks the
// disk; subsequent calls return the cached vector by const-ref.
// Cap at `cap` entries to bound the picker's working set on huge
// repos.
[[nodiscard]] const std::vector<SymbolEntry>&
list_workspace_symbols(std::size_t cap = 50000);

// Case-insensitive substring filter on the symbol NAME (not path).
// Returns indices into `entries` so the dispatcher resolves the
// cursor → (name, path, line) using the same view the picker
// rendered.
[[nodiscard]] std::vector<std::size_t>
filter_symbols(const std::vector<SymbolEntry>& entries, std::string_view query);

} // namespace agentty
