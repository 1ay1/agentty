#pragma once
// agentty::tools::memory — the storage layer behind the `remember` and
// `forget` tools, and the loader the system-prompt builder reads on
// every turn.
//
// Storage shape: newline-delimited JSON ("JSONL"). One record per line:
//
//   {"id":"a1b2c3d4","ts":1731860000,"scope":"project","text":"prefer fish"}
//
// JSONL was picked over a single JSON document for three reasons:
//
//   1. Appending a new record is one `O_APPEND` write of `<record>\n` —
//      no read-modify-write of the whole file, no atomic-rename dance,
//      no risk of clobbering a concurrent edit.
//   2. The file is line-addressable: `forget` strips matching lines
//      and rewrites the survivors. A corrupted line affects only that
//      record; loaders skip-and-continue on parse failures.
//   3. The on-disk format is grep-friendly for a human auditing what
//      the agent has stored about them.
//
// Two scopes:
//
//   User    ~/.agentty/memory.jsonl                  shared across workspaces
//   Project <workspace>/.agentty/memory.jsonl        per-project, gitignored
//
// `local` scope is intentionally NOT exposed via these tools — the
// equivalent in agentty's existing memory hierarchy is the user-authored
// CLAUDE.local.md, which the human owns. Letting the model write to
// "local" felt like blurring the boundary.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agentty::tools::memory {

namespace fs = std::filesystem;

enum class Scope : std::uint8_t { User, Project };

[[nodiscard]] constexpr std::string_view to_string(Scope s) noexcept {
    return s == Scope::User ? "user" : "project";
}

// Returns std::nullopt if the string isn't a recognised scope.
[[nodiscard]] std::optional<Scope> parse_scope(std::string_view s) noexcept;

struct Record {
    std::string id;          // 8 hex chars; assigned at append time
    std::int64_t ts;         // unix seconds (UTC)
    Scope scope;
    std::string text;        // capped at kMaxTextBytes by `append`
};

// Hard caps. Anything beyond these is rejected at append time so a
// runaway model can't fill the disk or poison every subsequent system
// prompt with megabytes of "memory".
inline constexpr std::size_t kMaxTextBytes        = 2u * 1024u;    // 2 KiB / record
inline constexpr std::size_t kMaxFileBytes        = 256u * 1024u;  // 256 KiB / file
inline constexpr std::size_t kMaxRecordsPerScope  = 200;           // hard cap on lines
inline constexpr std::size_t kTailLoadCount       = 50;            // load tail-N into prompt

// Resolve the on-disk path for a scope. Creates parent directories
// lazily on the first `append` — this function is pure and never
// touches the filesystem.
//
//   User    $HOME/.agentty/memory.jsonl    (or %USERPROFILE% on Windows)
//   Project <workspace_root>/.agentty/memory.jsonl
//
// Returns an empty path if the relevant base directory can't be
// resolved (no $HOME on POSIX, workspace_root() empty before init).
[[nodiscard]] fs::path path_for(Scope s);

// Append a record to the scope file. Generates an id, stamps the
// current time. On success returns the assigned id; on failure returns
// a human-readable error. Enforces:
//
//   • text non-empty after trim
//   • text size ≤ kMaxTextBytes (truncates with a note rather than failing)
//   • file size after append ≤ kMaxFileBytes (rolls oldest records)
//   • record count after append ≤ kMaxRecordsPerScope (rolls oldest)
//
// Concurrency: the append path takes a per-process mutex. Multi-process
// concurrent writes aren't synchronised — the only realistic writer is
// the agent itself; humans editing the JSONL in a text editor race
// with the agent's next write, the same way they race with any tool.
struct AppendResult {
    std::string id;          // 8 hex chars on success
    std::string error;       // empty on success
    std::string note;        // non-empty when text was truncated, etc.
    std::size_t rolled;      // number of old records dropped to fit caps
};
[[nodiscard]] AppendResult append(Scope s, std::string_view text);

// Read all records in a scope, oldest first. Skips lines that fail to
// parse. Returns empty vector on missing file.
[[nodiscard]] std::vector<Record> load_all(Scope s);

// Read the tail-N most recent records across BOTH scopes, oldest first
// within each scope. Used by the system-prompt builder. mtime-cached
// the same way CLAUDE.md is.
[[nodiscard]] std::vector<Record> load_recent_user();
[[nodiscard]] std::vector<Record> load_recent_project();

// Forget by exact id. Returns count removed (0 if not found).
[[nodiscard]] std::size_t forget_by_id(std::string_view id);

// Forget by substring (case-sensitive) across both scopes. Returns
// count removed. Refuses to run with an empty/whitespace pattern so
// a stray `forget {}` doesn't nuke everything.
[[nodiscard]] std::size_t forget_by_substring(std::string_view needle);

// Render a record for the <learned-memory> block in the system prompt.
// Format: `[<id>] <text>` — id present so the model can refer to a
// specific record when calling `forget`.
[[nodiscard]] std::string render_for_prompt(const Record& r);

} // namespace agentty::tools::memory
