#pragma once
// moha::memory::HotFiles — recent-activity index for the workspace.
//
// Tracks which files have been touched in the last hour / day / week
// using two signals:
//   1. `git log --since=...` for commit-tracked changes (the canonical
//      source — survives across users and machines).
//   2. Filesystem mtime fallback for non-git workspaces or
//      uncommitted local edits.
//
// The transport layer reads these via `compose_block(...)` and folds
// them into the system prompt as a `<recent-activity>` section so the
// model automatically focuses on files the user is actively working
// on. Cached for 60 s so we don't re-fork git on every turn.

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace moha::memory {

struct HotEntry {
    std::string path;          // workspace-relative
    std::chrono::system_clock::time_point modified_at{};
};

class HotFiles {
public:
    void set_workspace(const std::filesystem::path& workspace);

    // Render the `<recent-activity>` block. Three buckets: last
    // 60 min, last 24 h, last 7 d. Empty bucket suppressed. Capped
    // at `max_bytes` total. Cached for 60 s.
    [[nodiscard]] std::string compose_block(std::size_t max_bytes = 2048) const;

    // Force a refresh on next compose. Useful after the user
    // modifies files and the cache hasn't expired yet.
    void invalidate();

    [[nodiscard]] bool ready() const noexcept;

private:
    mutable std::mutex                            mu_;
    std::filesystem::path                         workspace_;
    mutable std::string                           cache_;
    mutable std::chrono::steady_clock::time_point cache_at_{};

    void rebuild_cache_locked_(std::size_t max_bytes) const;
    [[nodiscard]] std::vector<HotEntry>
    git_log_since_(std::chrono::seconds since) const;
    [[nodiscard]] std::vector<HotEntry>
    mtime_scan_since_(std::chrono::seconds since) const;
};

[[nodiscard]] HotFiles& shared_hot_files();

} // namespace moha::memory
