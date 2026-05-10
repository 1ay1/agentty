#pragma once
// @file mention picker — opens above the composer when the user types
// `@` on an empty buffer. The Open alternative carries the typed query,
// the cursor index into the visible row list, and a snapshot of the
// workspace's file paths captured at open time (so subsequent typing
// just filters the snapshot rather than rewalking the disk).
//
// Same shape as command_palette.hpp — Closed/Open variant, query +
// index in Open, plus a `files` vector since unlike the static command
// catalog, the candidate set comes from disk.

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace agentty {

namespace mention {

struct Closed {};

struct Open {
    /// Filter buffer — what the user has typed since opening.
    std::string query;
    /// Cursor into the visible (filtered) rows.
    int         index = 0;
    /// Snapshot of workspace-relative file paths at open time. Sorted
    /// once for deterministic order; filtering walks this list each
    /// frame against the current query.
    std::vector<std::string> files;
};

} // namespace mention

using MentionPaletteState = std::variant<mention::Closed, mention::Open>;

[[nodiscard]] inline bool mention_is_open(const MentionPaletteState& s) noexcept {
    return std::holds_alternative<mention::Open>(s);
}
[[nodiscard]] inline       mention::Open* mention_opened(MentionPaletteState& s)       noexcept { return std::get_if<mention::Open>(&s); }
[[nodiscard]] inline const mention::Open* mention_opened(const MentionPaletteState& s) noexcept { return std::get_if<mention::Open>(&s); }

// Walk the workspace, return up to `cap` workspace-relative file paths
// with binary files / common build & VCS dirs filtered out. Cheap on
// reasonable repos (low thousands of files) — runs synchronously on
// open. If a future repo blows past the cap, increase the cap or add
// async/incremental loading.
[[nodiscard]] std::vector<std::string>
list_workspace_files(std::size_t cap = 5000);

// Case-insensitive substring filter over a path list. Returned indices
// point into the original `files` vector — the dispatcher uses one to
// resolve cursor → path identically to how the view rendered the rows.
[[nodiscard]] std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query);

} // namespace agentty
