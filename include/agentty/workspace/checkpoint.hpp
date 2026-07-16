#pragma once
// Git-based workspace checkpoints — the Zed-agent "restore checkpoint"
// mechanism. Before each user turn the runtime snapshots the ENTIRE
// worktree state (tracked + untracked, .gitignore respected) as a
// parentless git commit pinned under refs/agentty/checkpoints/<id>.
// Restore makes the worktree byte-identical to that snapshot: files are
// rewritten from the tree AND files created since the snapshot are
// deleted (ignored files — build dirs etc. — are never touched).
//
// The snapshot never moves HEAD, never touches the user's real index,
// and never creates stash entries: everything runs against a throwaway
// GIT_INDEX_FILE seeded from a copy of the real index (so `add -A` only
// re-hashes changed files instead of the whole repo).
//
// All functions are safe to call outside a git repo — they return
// false/nullopt and do nothing.

#include <string>

namespace agentty::workspace {

// True iff the workspace root is inside a git repository. Cached after
// the first call (repo-ness doesn't change mid-session).
[[nodiscard]] bool in_git_repo();

// A one-glance summary of what the worktree has changed SINCE a
// checkpoint was taken — the diff between the pinned snapshot tree and
// the current working tree (tracked + newly-added; ignored files never
// counted). Powers the rewind picker's preview so a rewind is never
// blind. All zero / valid==false when not in a repo, the ref is gone,
// or nothing changed.
struct CheckpointDiff {
    bool valid          = false;   // the summary was computed successfully
    int  files_changed  = 0;
    int  insertions     = 0;
    int  deletions      = 0;
};
[[nodiscard]] CheckpointDiff checkpoint_summary(const std::string& id);

// Snapshot the current worktree and pin it at
// refs/agentty/checkpoints/<id>. Returns false when not in a repo or
// any git step fails. Also prunes the oldest checkpoint refs beyond a
// fixed keep-count so refs don't accumulate forever.
bool create_checkpoint(const std::string& id);

// True iff refs/agentty/checkpoints/<id> resolves to a commit.
[[nodiscard]] bool checkpoint_exists(const std::string& id);

// Make the worktree match the checkpoint exactly: rewrite every file
// recorded in the snapshot and delete non-ignored files that did not
// exist when it was taken. On failure returns false and, if `error` is
// non-null, writes a short human-readable reason into it.
bool restore_checkpoint(const std::string& id, std::string* error = nullptr);

} // namespace agentty::workspace
