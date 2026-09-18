#pragma once
// Workspace symbol enumeration — walk the active workspace root,
// apply a small language-aware regex set, and return all named
// definitions found. Same family as files.hpp: a pure-I/O scanner
// that the runtime's `#` picker consumes.
//
// SymbolEntry lives here (not in runtime/panel/symbol.hpp)
// because its shape is dictated by what the scanner emits; the UI
// state is downstream — symbol::Open just holds a vector
// of these.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/util/snapshot.hpp"

namespace agentty {

struct SymbolEntry {
    std::string name;             // identifier, e.g. "submit_message"
    std::string path;             // workspace-relative file path
    int         line_number = 0;  // 1-based
};

// Walk the workspace once, return all definitions matched by the
// language regex set. Cached per-process: first call walks the
// disk; subsequent calls share the cached buffer. Cap at `cap`
// entries to bound the picker's working set on huge repos.
//
// Returns a SNAPSHOT rather than a const-ref. The const-ref form was a
// latent use-after-free: it copied the owning shared_ptr into a block-scoped
// local and returned a reference that outlived it. A Snapshot carries the
// ownership out with the data, so the buffer cannot be freed while a caller
// still holds the handle. See util/snapshot.hpp.
[[nodiscard]] util::Snapshot<std::vector<SymbolEntry>>
list_workspace_symbols(std::size_t cap = 50000);

// Kick the (parallel) symbol scan on a background thread pool — single-
// flight, safe to call repeatedly. Call at startup so the first `#` is
// instant instead of freezing the UI for a multi-second regex scan.
void prewarm_workspace_symbols(std::size_t cap = 50000);

// Join the prewarm scan if still running — see join_workspace_prewarm() in
// files.hpp for the fast-exit UAF this closes.
void join_workspace_symbols_prewarm();

// Non-blocking: is the symbol index built yet? The composer opens the
// `#` picker INSTANTLY and shows "indexing…" until this returns true.
[[nodiscard]] bool symbols_ready();

// Case-insensitive substring filter on the symbol NAME (not path).
// Returns indices into `entries` so the dispatcher resolves the
// cursor → (name, path, line) using the same view the picker
// rendered.
[[nodiscard]] std::vector<std::size_t>
filter_symbols(const std::vector<SymbolEntry>& entries, std::string_view query);

} // namespace agentty
