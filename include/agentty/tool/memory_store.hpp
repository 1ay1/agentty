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
    std::int64_t ts;         // unix seconds (UTC) — last-touched (bumps on dedup hit)
    Scope scope;
    std::string text;        // capped at kMaxTextBytes by `append`
    bool pinned = false;     // pinned records never roll on cap overflow
    std::vector<std::string> tags;  // optional grouping labels (lower-case, sorted, deduped)
    std::int32_t hits = 0;   // bumps on dedup append; signal of "this fact keeps coming up"
};

// Hard caps. Anything beyond these is rejected at append time so a
// runaway model can't fill the disk or poison every subsequent system
// prompt with megabytes of "memory".
inline constexpr std::size_t kMaxTextBytes        = 2u * 1024u;    // 2 KiB / record
inline constexpr std::size_t kMaxFileBytes        = 256u * 1024u;  // 256 KiB / file
inline constexpr std::size_t kMaxRecordsPerScope  = 200;           // hard cap on lines
inline constexpr std::size_t kTailLoadCount       = 50;            // load tail-N into prompt

// ── Prompt-budget caps (the defence against memory eating the context) ──
//
// load_recent_*() returns the tail-N records, but the system prompt must
// not grow without bound as the store fills up — a user who stores
// hundreds of facts would otherwise pay ~100 KiB of prompt on EVERY turn,
// pushing the conversation toward compaction far sooner. select_for_prompt
// applies a hard BYTE budget on top of tail-N, ranked so the
// highest-signal facts survive when the budget is tight:
//
//   1. pinned records are always kept (cap-exempt by design);
//   2. remaining budget filled by rank = (hits, then recency);
//   3. any single record longer than kPromptRecordCap is shown truncated
//      with an ellipsis note (the on-disk record stays whole — only the
//      prompt copy is clipped);
//   4. records that don't fit are dropped from the prompt (still on disk,
//      still editable, the dropped count is reported so the loader can
//      emit a "+N more stored" footer).
//
// Budget sized so the WHOLE learned-memory block (both scopes combined)
// stays a few KiB — a rounding error against a 200 K-token window — no
// matter how much has accumulated on disk.
inline constexpr std::size_t kPromptByteBudget = 6u * 1024u;   // ~6 KiB / scope block
inline constexpr std::size_t kPromptRecordCap  = 400;          // clip any one record to this in-prompt

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
//   • file size after append ≤ kMaxFileBytes (rolls oldest unpinned records)
//   • record count after append ≤ kMaxRecordsPerScope (rolls oldest unpinned)
//
// Dedup: if `text` (after whitespace normalisation) closely matches an
// existing record in the SAME scope, no new record is written. Instead
// the existing record's `ts` is refreshed, `hits` is incremented, and
// `tags` / `pinned` are merged with the incoming values. The returned
// id is the existing record's id (with `note` set to flag the dedup
// for the caller's UX). This is the primary defence against models
// repeatedly re-asserting the same fact across sessions.
//
// Pin: `pinned=true` marks the record as cap-exempt. The cap-rollover
// path walks oldest-first but skips pinned records, so a critical fact
// (the build command, a hard project convention) stays put even after
// hundreds of subsequent appends. A pinned record can still be removed
// by `forget` or `wipe`.
//
// Supersede: if `supersedes_id` is non-empty, the named record is
// removed atomically in the same rewrite that adds the new one.
// `forget`-then-`remember` would have the same effect but loses
// atomicity (a crash between them leaves the store in a half-state).
// If the supersede id doesn't exist, the new record is still written
// and a note flags the miss — we never refuse the append over a stale
// reference.
//
// Tags: passed in any case; normalised to lower-case ASCII, deduped,
// sorted. Empty tag strings are dropped. The system-prompt loader uses
// tags to group records under headings inside <learned-memory> so the
// model can scan by topic instead of by chronology.
//
// Concurrency: the append path takes a per-process mutex. Multi-process
// concurrent writes aren't synchronised — the only realistic writer is
// the agent itself; humans editing the JSONL in a text editor race
// with the agent's next write, the same way they race with any tool.
struct AppendOptions {
    bool pinned = false;
    std::vector<std::string> tags;
    std::string supersedes_id;   // empty ⇒ no supersede
    // When false (default) the dedup path is allowed; setting true
    // forces a fresh record even if a near-duplicate exists. Reserved
    // for `forget`+`remember` callers that explicitly want a new id.
    bool no_dedup = false;
};

// MIRROR of mcp::tools::MemoryAppendResult (mcp-cpp/include/mcp/tools/host.hpp):
// the two carry identical fields and are bridged field-by-field in
// AgenttyMemoryStore::append (src/tool/mcp_tools_backends.cpp), which holds a
// static_assert that trips if their sizes drift. Every field needs an in-class
// default: the no-rollover path returns without ever touching `rolled`, so an
// uninitialised `rolled` was read as garbage and printed as e.g.
// "(140696944861232 old record(s) rolled.)". Add/remove a field here and you
// update the mirror + the bridge mapping.
struct AppendResult {
    std::string id;          // 8 hex chars on success (may be an existing id on dedup)
    std::string error;       // empty on success
    std::string note;        // non-empty when text was truncated, deduped, supersede missed, etc.
    std::size_t rolled = 0;  // number of old records dropped to fit caps
    bool deduped = false;    // true ⇒ hit an existing record; id is the existing one
};
[[nodiscard]] AppendResult append(Scope s, std::string_view text,
                                  AppendOptions opts = {});

// Read all records in a scope, oldest first. Skips lines that fail to
// parse. Returns empty vector on missing file.
[[nodiscard]] std::vector<Record> load_all(Scope s);

// Read the tail-N most recent records across BOTH scopes, oldest first
// within each scope. Used by the system-prompt builder. mtime-cached
// the same way CLAUDE.md is.
[[nodiscard]] std::vector<Record> load_recent_user();
[[nodiscard]] std::vector<Record> load_recent_project();

// Budgeted prompt selection. Takes the records load_recent_*() returned
// for ONE scope and pares them down to fit kPromptByteBudget, keeping all
// pinned records and then the highest-signal remainder (by hits, then
// recency). Returns the chosen records in oldest-first stable order (so
// the prompt still reads chronologically), plus the number dropped so the
// caller can emit a "+N more stored" footer. Records whose text exceeds
// kPromptRecordCap are returned with text clipped + an ellipsis marker;
// the on-disk record is untouched. This is THE bound that stops a growing
// memory store from inflating every system prompt.
struct PromptSelection {
    std::vector<Record> records;   // clipped, oldest-first, within budget
    std::size_t         dropped = 0;   // records elided to fit the budget
};
[[nodiscard]] PromptSelection select_for_prompt(
    std::vector<Record> recent,
    std::size_t byte_budget = kPromptByteBudget);

// Forget by exact id. Returns count removed (0 if not found).
[[nodiscard]] std::size_t forget_by_id(std::string_view id);

// Forget by substring (case-sensitive) across both scopes. Returns
// count removed. Refuses to run with an empty/whitespace pattern so
// a stray `forget {}` doesn't nuke everything.
[[nodiscard]] std::size_t forget_by_substring(std::string_view needle);

// Preview a substring forget without writing. Returns the records
// that WOULD be removed. Used by the `forget` tool's `dry_run` path
// so the model can confirm a non-trivial wipe before committing.
[[nodiscard]] std::vector<Record> preview_forget_by_substring(std::string_view needle);

// Wipe an entire scope. The hard "start afresh on this codebase"
// switch — removes every record in the scope file (the file itself is
// truncated, not deleted, so subsequent appends don't race a directory
// rebuild). Returns the count of records removed, or std::nullopt if
// the scope path is unresolvable (no $HOME, no workspace, etc.). The
// `confirm` gate lives in the tool layer; this function always wipes.
[[nodiscard]] std::optional<std::size_t> wipe(Scope s);

// Render a record for the <learned-memory> block in the system prompt.
// Format: `[<id>]` + (optional `★ ` for pinned) + text.
[[nodiscard]] std::string render_for_prompt(const Record& r);

// Lookup by id across both scopes. Returns the record + its resolved
// scope (which may differ from any caller-assumed scope), or nullopt
// if no record carries that id. Used by the supersede path to find
// the predecessor wherever it lives.
[[nodiscard]] std::optional<std::pair<Record, Scope>>
    find_by_id(std::string_view id);

} // namespace agentty::tools::memory
