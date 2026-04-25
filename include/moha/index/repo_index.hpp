#pragma once
// moha::index — process-wide symbol index for the workspace.
//
// One walk of the tree on first query, regex-based per-language symbol
// extraction (no tree-sitter dependency), mtime-tracked refresh. The
// model uses this through three thin tools — `outline`, `repo_map`,
// `signatures` — and the system prompt embeds a compact map at session
// start so the agent has a table-of-contents without burning tool turns
// on `list_dir` tours of every directory.
//
// Coverage: C/C++, Python, JS/TS (incl. JSX/TSX), Go, Rust, Java, Ruby.
// Anything else is silently skipped — it falls back to `read`/`grep`
// which still work fine.
//
// The index is intentionally lossy: we capture the *names* and *line
// numbers* of declarations, not their bodies. That's the right
// compression: the model can ask "where is X?" or "what's in this
// file?" for a few hundred tokens instead of paging the file in.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moha::index {

enum class SymbolKind : std::uint8_t {
    Function,    // free function (any language)
    Method,      // member function — emitted only when the parser can tell
    Class,       // class
    Struct,      // struct (C++/C/Go/Rust)
    Enum,        // enum / enum class
    Union,       // C/C++ union
    Namespace,   // C++ namespace, JS module, Python module
    Typedef,     // typedef / using / type alias
    Trait,       // Rust trait
    Interface,   // TS interface, Go interface, Java interface
    Module,      // Rust mod, Ruby module
    Const,       // top-level const/let
    Macro,       // C/C++ #define
    Impl,        // Rust impl block
};

[[nodiscard]] std::string_view to_string(SymbolKind k) noexcept;

struct Symbol {
    std::string name;
    SymbolKind  kind;
    int         line = 0;          // 1-based line where the declaration starts
    std::string signature;         // trimmed source line; empty for noise we
                                   // could parse but didn't (Macros, large
                                   // function bodies — avoid bloat)
};

// A logical "module" — a directory grouping that the agent can think
// about as a unit instead of as a list of files. Auto-detected by
// `detect_modules()` from the existing file index. At huge scale
// (100k+ files) modules become the natural granularity for the
// system-prompt repo map.
struct Module {
    std::filesystem::path                  dir;        // workspace-relative
    std::string                            name;       // basename of dir
    std::vector<std::filesystem::path>     files;      // direct + nested
    int                                    score = 0;  // sum of file scores
    int                                    file_count = 0;
    std::vector<std::string>               top_symbols; // 3-6 highest-centrality
};

struct FileIndex {
    std::filesystem::path                 path;       // workspace-relative
    std::filesystem::file_time_type       mtime{};
    std::vector<Symbol>                   symbols;
    // Top-of-file description: the first prose-shaped comment block,
    // capped at ~120 chars. Empty if the file has no leading comment
    // or the comment is just a license/header-guard incantation.
    std::string                           description;
    // Centrality score: sum of cross-file inbound mentions to the
    // symbols this file *defines*. Computed in `rebuild_importance_`
    // after every refresh. A score of 0 means "nothing else in the
    // workspace references anything this file defines" (typically a
    // top-level main, vendored code, or a leaf test file).
    int                                   score = 0;
};

class RepoIndex {
public:
    // Walk `root` (defaults to the workspace cwd), respecting the standard
    // skip-list (.git, node_modules, build, target, vendor, etc.). Re-uses
    // cached entries whose mtime is unchanged. Thread-safe.
    void refresh(const std::filesystem::path& root);

    // One file. If the file isn't in the cache (or its mtime moved), parse
    // it now. Returns empty if the path isn't a code file or doesn't exist.
    [[nodiscard]] std::vector<Symbol>
    outline(const std::filesystem::path& path);

    // All cached files. Deterministic (sorted by path).
    [[nodiscard]] std::vector<FileIndex> all_files() const;

    // Compact human/model-readable repo map, capped at `max_bytes`. Format:
    //   src/runtime/
    //     update.cpp [Step do_things, Msg classify, ...]
    //     view.cpp   [void render(...), ...]
    // Files are listed flat under each directory; symbols are joined with
    // commas and truncated with "…" once the per-file budget is hit.
    [[nodiscard]] std::string
    compact_map(std::size_t max_bytes = 4096) const;

    // Symbols matching `pattern` (case-insensitive substring). Returns up
    // to `limit` (path, symbol) pairs.
    [[nodiscard]] std::vector<std::pair<std::filesystem::path, Symbol>>
    find_symbols(std::string_view pattern, std::size_t limit = 50) const;

    // True iff at least one refresh has populated the cache.
    [[nodiscard]] bool ready() const noexcept;

    // Workspace root the cache was last built against. Empty if never.
    [[nodiscard]] std::filesystem::path workspace() const;

    // Per-symbol cross-file mention count — populated by
    // `rebuild_importance_`. Returned as a const reference so
    // tests / debug-dump code can introspect without a copy. Empty
    // before the first refresh.
    [[nodiscard]] const std::unordered_map<std::string, int>&
    symbol_scores() const noexcept;

    // ── Reference graph ─────────────────────────────────────────
    // Bidirectional cross-file usage map populated by the same
    // tokenisation pass that builds importance scores. Lets
    // `find_usages(symbol)` answer in zero tool calls.

    // Files that mention `symbol_name` (excluding the file that
    // *defines* it). Returns empty vector when the symbol isn't in
    // the universe or has no cross-file references.
    [[nodiscard]] std::vector<std::filesystem::path>
    files_using(std::string_view symbol_name) const;

    // Symbols that `path` references (defined elsewhere in the
    // workspace). Useful for "what does this file depend on?"
    // queries.
    [[nodiscard]] std::vector<std::string>
    symbols_used_by(const std::filesystem::path& path) const;

    // Number of distinct cross-file references for a given symbol.
    // O(1) lookup. Returns 0 when symbol isn't in the index.
    [[nodiscard]] int reference_count(std::string_view symbol_name) const;

    // Group files into modules by directory (cap depth so a deeply
    // nested workspace doesn't produce one-file-per-module noise).
    // Returns modules sorted by score (sum of contained file scores)
    // descending. Filters to modules with ≥ `min_files` direct
    // children — singleton "modules" aren't useful.
    //
    // Cap-by-depth lets the agent think at the right abstraction:
    //   - `src/runtime/view/`         (depth 3) → one module
    //   - `src/runtime/view/widget/`  (depth 4) → its own module
    //   - …deeper than that gets folded into its ancestor.
    [[nodiscard]] std::vector<Module>
    detect_modules(int max_depth = 4, int min_files = 2) const;

    // The whole-repo "compact map" but ZOOMED to a subdirectory.
    // Used by `repo_map(path=...)` to drill from the module overview
    // into a focused per-subtree view.
    [[nodiscard]] std::string
    subtree_map(const std::filesystem::path& subtree,
                std::size_t max_bytes = 4096) const;

    // Hierarchical map: module overview block first, then a tree of
    // remaining files. Used as the default repo_map output for huge
    // codebases — fits in the same byte budget as compact_map but
    // the per-byte information density is way higher because the
    // unit is "module" not "file".
    [[nodiscard]] std::string
    hierarchical_map(std::size_t max_bytes = 4096) const;

private:
    mutable std::mutex                                            mu_;
    std::filesystem::path                                          root_;
    std::unordered_map<std::string, FileIndex>                     by_path_;  // key = path.string()
    std::unordered_map<std::string, int>                           symbol_score_;  // name → cross-file mentions
    // Reference graph (Tier-1 intelligence multiplier):
    //   symbol_to_users_[name] = set of file paths that mention it
    //   path_to_symbols_used_[path] = set of symbol names referenced
    // Both populated in rebuild_importance_'s tokenisation pass —
    // adds zero asymptotic cost (we already walk every byte).
    std::unordered_map<std::string, std::vector<std::string>>      symbol_to_users_;
    std::unordered_map<std::string, std::vector<std::string>>      path_to_symbols_used_;
    std::chrono::steady_clock::time_point                          last_refresh_{};

    // Compute every symbol's cross-file mention count and per-file
    // score after a refresh. Runs under `mu_` already held.
    // Algorithm: build the universe of defined symbol names (set), then
    // tokenise each file's content once and bump the count for every
    // matching identifier that ISN'T defined in the same file (so a
    // file's local helpers don't pump up their own importance). File
    // score = sum of per-symbol scores for everything it defines, with
    // a 2× multiplier for files modified within the last hour so the
    // map reflects what the user is actively touching.
    void rebuild_importance_();
};

// Process-wide singleton — the runtime + tools + transport all share one
// view of the index. Never null; constructed on first call.
[[nodiscard]] RepoIndex& shared();

// One-shot extractor. Public so the `outline` tool can run it on a file
// that's outside the cached workspace (e.g. an absolute path the user
// pasted). Cheap — a single read + a few regex scans.
[[nodiscard]] std::vector<Symbol>
extract_symbols(const std::filesystem::path& path);

} // namespace moha::index
