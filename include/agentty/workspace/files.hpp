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

// Kick the workspace-file walk on a background thread (single-flight, safe
// to call repeatedly). After it lands, list_workspace_files() and the
// picker's first open are instant. Call at startup.
void prewarm_workspace_files(std::size_t cap = 5000);

// Join the prewarm walk if it is still running. Called from main()'s teardown
// before CRT/heap destruction so a fast pipe-EOF exit can't leave the detached
// walk touching freed state (Windows 0xC0000005). No-op if it never ran or
// already finished.
void join_workspace_prewarm();

// Cooperative shutdown for BOTH prewarm walks (files here + symbols). The
// full-tree file scan and the multi-threaded symbol regex pass are otherwise
// uncancellable, so join_workspace_*_prewarm() at teardown would block ^C
// until they finish on a large repo. Teardown calls request_prewarm_cancel()
// before the joins; the walk loops poll prewarm_cancelled() and bail early, so
// the join still runs (keeping the Windows UAF guard) but returns promptly.
void request_prewarm_cancel() noexcept;
[[nodiscard]] bool prewarm_cancelled() noexcept;

// Non-blocking: has the file list been built yet? The composer opens the
// `@` picker INSTANTLY and shows an "indexing…" hint until this is true.
[[nodiscard]] bool files_ready();

// Record that the user referenced `path` (selected it in the picker), so a
// later `@` ranks it near the top. Frecency — recently-used-first.
void note_file_referenced(std::string_view path);

// ── Git awareness ────────────────────────────────────────
// A file's relationship to git — the strongest "this file matters right
// now" signal. Computed once at prewarm; drives both ranking (dirty files
// float to the top of a blank `@`) and the row tag the picker renders.
enum class GitTag {
    None,
    Modified,           // worktree change (dirty)  — highest priority
    Staged,             // index change (git add'd)
    Untracked,          // new file, not yet tracked
    RecentlyCommitted,  // touched in the last ~20 commits (weaker)
};

// The git tag for a workspace-relative path (None when clean/unknown or
// the signal walk hasn't landed). Cheap map lookup, safe on any thread.
[[nodiscard]] GitTag file_git_tag(std::string_view path);

// Rebuild the git-status map SYNCHRONOUSLY against the current project
// root. Prewarm calls this on a background thread; call it directly after
// a run of tool edits so a follow-up `@` reflects the new working set, or
// from a test that just mutated a fixture repo.
void refresh_git_signals();

// A one-word label + whether this path is "hot" (dirty/staged/untracked)
// for the picker's row rendering. Empty label ⇒ no tag.
[[nodiscard]] std::string_view git_tag_label(GitTag tag);

// Case-insensitive substring filter over a path list. Returned
// indices point into the original `files` vector — the dispatcher
// uses one to resolve cursor → path identically to how the view
// rendered the rows.
[[nodiscard]] std::vector<std::size_t>
filter_files(const std::vector<std::string>& files, std::string_view query);

} // namespace agentty
