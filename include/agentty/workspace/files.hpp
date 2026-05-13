#pragma once
// Workspace file enumeration — walk the active workspace root and
// return relative paths suitable for the @mention picker, attachment
// resolution, and any other UI that surfaces "files the user might
// want to reference".
//
// Lives under workspace/ rather than runtime/ because it's pure
// filesystem I/O (same family as tool/util/fs_helpers.cpp): the
// runtime owns the UI state (mention::Open holds the captured
// snapshot), but the act of walking the disk is a separate concern.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agentty {

// Walk the workspace, return up to `cap` workspace-relative file
// paths with binary files / common build & VCS dirs filtered out.
// Cheap on reasonable repos (low thousands of files) — runs
// synchronously on open. If a future repo blows past the cap,
// increase the cap or add async/incremental loading.
[[nodiscard]] std::vector<std::string>
list_workspace_files(std::size_t cap = 5000);

// Case-insensitive substring filter over a path list. Returned
// indices point into the original `files` vector — the dispatcher
// uses one to resolve cursor → path identically to how the view
// rendered the rows.
[[nodiscard]] std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query);

} // namespace agentty
