// SPDX-License-Identifier: Apache-2.0
//
// checkpoint.cpp — git-based workspace checkpoints (see header for the
// contract). Design mirrors Zed's agent checkpoints: a parentless commit
// object pinned under refs/agentty/checkpoints/<id>, built against a
// THROWAWAY index file so the user's real index, HEAD, stash and reflog
// are never disturbed. The commit is invisible to normal git porcelain
// (no branch points at it) and survives until pruned by our own
// keep-count policy or a `git gc` after the ref is deleted.

#include "agentty/workspace/checkpoint.hpp"

#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/tool/util/subprocess.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace agentty::workspace {

namespace fs = std::filesystem;
namespace util = agentty::tools::util;

namespace {

// Keep at most this many checkpoint refs; oldest beyond the cap are
// dropped at create time. 64 user turns of history is plenty — restore
// targets are almost always the last handful of turns, and each ref
// pins whole blobs for every file that has since changed.
constexpr int kKeepCheckpoints = 64;

constexpr std::string_view kRefPrefix = "refs/agentty/checkpoints/";

struct RepoInfo {
    bool        in_repo = false;
    std::string root;      // worktree top-level (absolute)
    std::string git_dir;   // absolute .git dir (worktree-aware)
};

// Trim trailing newline(s) a git plumbing call leaves on single-line output.
std::string chomp(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Discover the enclosing repo ONCE per process. Repo-ness and layout
// don't change mid-session; caching avoids two subprocess spawns per
// checkpoint. (A user running `git init` mid-session picks it up on
// restart — acceptable.)
const RepoInfo& repo() {
    static const RepoInfo info = [] {
        RepoInfo r;
        const std::string ws = util::workspace_root().string();
        auto top = util::run_argv_s(
            {"git", "-C", ws, "rev-parse", "--show-toplevel"}, 8192,
            std::chrono::seconds{10});
        if (!top.started || top.exit_code != 0) return r;
        auto gd = util::run_argv_s(
            {"git", "-C", ws, "rev-parse", "--absolute-git-dir"}, 8192,
            std::chrono::seconds{10});
        if (!gd.started || gd.exit_code != 0) return r;
        r.root    = chomp(top.output);
        r.git_dir = chomp(gd.output);
        r.in_repo = !r.root.empty() && !r.git_dir.empty();
        return r;
    }();
    return info;
}

// Run git with GIT_INDEX_FILE pointing at our scratch index. POSIX gets
// the coreutils `env` prefix (always present on Linux/macOS); Windows
// routes through cmd's `set … &&` shell form. Output capped generously —
// plumbing output here is refs/paths, not file bodies.
util::SubprocessResult run_git_scratch(const std::string& index_file,
                                       std::vector<std::string> git_argv,
                                       std::size_t max_bytes = 512 * 1024) {
#ifdef _WIN32
    std::string cmd = "set GIT_INDEX_FILE=" + index_file + "&& ";
    for (const auto& a : git_argv) {
        cmd += '"';
        for (char c : a) { if (c == '"') cmd += '\\'; cmd += c; }
        cmd += "\" ";
    }
    return util::run_command_s(cmd, max_bytes, std::chrono::seconds{120});
#else
    std::vector<std::string> argv;
    argv.reserve(git_argv.size() + 2);
    argv.emplace_back("env");
    argv.emplace_back("GIT_INDEX_FILE=" + index_file);
    for (auto& a : git_argv) argv.push_back(std::move(a));
    return util::run_argv_s(argv, max_bytes, std::chrono::seconds{120});
#endif
}

util::SubprocessResult run_git(std::vector<std::string> tail,
                               std::size_t max_bytes = 512 * 1024) {
    std::vector<std::string> argv = {"git", "-C", repo().root};
    for (auto& a : tail) argv.push_back(std::move(a));
    return util::run_argv_s(argv, max_bytes, std::chrono::seconds{120});
}

bool ok(const util::SubprocessResult& r) {
    return r.started && !r.timed_out && r.exit_code == 0;
}

// Scratch index path inside the git dir. Seeded from a COPY of the real
// index so `add -A` only re-hashes files whose stat info changed —
// without the seed, every checkpoint would re-hash the entire worktree.
// Serialized by a process-wide mutex: checkpoint creation runs on a BG
// worker while restore runs on the UI thread, and two concurrent users
// of one scratch index would corrupt it.
std::mutex g_scratch_mu;

std::string prepare_scratch_index() {
    const fs::path idx = fs::path(repo().git_dir) / "agentty-checkpoint-index";
    std::error_code ec;
    fs::remove(idx, ec);
    fs::copy_file(fs::path(repo().git_dir) / "index", idx,
                  fs::copy_options::overwrite_existing, ec);
    // Copy failure (fresh repo with no index yet) is fine — git creates
    // the scratch index from scratch, just slower.
    return idx.string();
}

void drop_scratch_index(const std::string& idx) {
    std::error_code ec;
    fs::remove(fs::path(idx), ec);
}

std::string ref_for(const std::string& id) {
    return std::string{kRefPrefix} + id;
}

// Delete oldest checkpoint refs beyond kKeepCheckpoints. creatordate
// sorts oldest-first; everything before the keep-window tail goes.
void prune_old_checkpoints() {
    auto r = run_git({"for-each-ref", "--sort=creatordate",
                      "--format=%(refname)", std::string{kRefPrefix}});
    if (!ok(r)) return;
    std::vector<std::string> refs;
    std::istringstream in(r.output);
    for (std::string line; std::getline(in, line); ) {
        line = chomp(std::move(line));
        if (!line.empty()) refs.push_back(std::move(line));
    }
    if (static_cast<int>(refs.size()) <= kKeepCheckpoints) return;
    const std::size_t drop = refs.size() - static_cast<std::size_t>(kKeepCheckpoints);
    for (std::size_t i = 0; i < drop; ++i) {
        (void)run_git({"update-ref", "-d", refs[i]});
    }
}

// Repo-relative paths of every file recorded in the checkpoint tree.
bool snapshot_paths(const std::string& commit,
                    std::unordered_set<std::string>& out) {
    // -z: NUL-separated, immune to quoting of unusual filenames.
    auto r = run_git({"ls-tree", "-r", "-z", "--name-only", commit},
                     16 * 1024 * 1024);
    if (!ok(r)) return false;
    std::size_t start = 0;
    while (start < r.output.size()) {
        auto nul = r.output.find('\0', start);
        if (nul == std::string::npos) break;
        if (nul > start) out.emplace(r.output.substr(start, nul - start));
        start = nul + 1;
    }
    return true;
}

// Repo-relative paths of every CURRENT non-ignored file (tracked +
// untracked, .gitignore respected) — the deletion candidate set.
bool current_paths(std::vector<std::string>& out) {
    auto r = run_git({"ls-files", "-z", "-c", "-o", "--exclude-standard"},
                     16 * 1024 * 1024);
    if (!ok(r)) return false;
    std::size_t start = 0;
    while (start < r.output.size()) {
        auto nul = r.output.find('\0', start);
        if (nul == std::string::npos) break;
        if (nul > start) out.emplace_back(r.output.substr(start, nul - start));
        start = nul + 1;
    }
    return true;
}

} // namespace

bool in_git_repo() { return repo().in_repo; }

CheckpointDiff checkpoint_summary(const std::string& id) {
    CheckpointDiff out;
    if (!repo().in_repo || id.empty()) return out;

    // Resolve the checkpoint commit; bail quietly if it was pruned.
    auto rev = run_git({"rev-parse", "--verify", "--quiet",
                        ref_for(id) + "^{commit}"});
    if (!ok(rev)) return out;
    const std::string commit = chomp(rev.output);

    // Build a throwaway tree of the CURRENT worktree exactly like
    // create_checkpoint does (scratch index seeded from the real one so
    // `add -A` only re-hashes changed files), then numstat-diff the
    // checkpoint tree against it. Doing it tree-vs-tree (not
    // tree-vs-worktree) keeps the untracked/new-file accounting identical
    // to what a rewind would actually revert.
    std::lock_guard<std::mutex> lk(g_scratch_mu);
    const std::string idx = prepare_scratch_index();
    if (!ok(run_git_scratch(idx, {"git", "-C", repo().root, "add", "-A",
                                  "--ignore-errors"}))) {
        drop_scratch_index(idx);
        return out;
    }
    auto tree = run_git_scratch(idx, {"git", "-C", repo().root, "write-tree"});
    drop_scratch_index(idx);
    if (!ok(tree)) return out;
    const std::string cur_tree = chomp(tree.output);

    // --numstat: one line per changed file, "<added>\t<deleted>\t<path>".
    // Binary files report "-\t-\t<path>" — counted as a changed file, no
    // line deltas. -M enables rename detection so a moved file is one
    // change, not an add+delete pair.
    auto ns = run_git({"diff", "--numstat", "-M", commit, cur_tree},
                      16 * 1024 * 1024);
    if (!ok(ns)) return out;

    std::istringstream in(ns.output);
    for (std::string line; std::getline(in, line); ) {
        line = chomp(std::move(line));
        if (line.empty()) continue;
        ++out.files_changed;
        // Parse the two leading tab-separated integer columns; "-" (binary)
        // contributes zero line deltas but still a changed file.
        auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        auto to_int = [](std::string_view s) -> int {
            if (s == "-" || s.empty()) return 0;
            int v = 0;
            for (char c : s) { if (c < '0' || c > '9') return 0; v = v * 10 + (c - '0'); }
            return v;
        };
        out.insertions += to_int(std::string_view{line}.substr(0, t1));
        out.deletions  += to_int(std::string_view{line}.substr(t1 + 1, t2 - t1 - 1));
    }
    out.valid = true;
    return out;
}

bool create_checkpoint(const std::string& id) {
    if (!repo().in_repo || id.empty()) return false;
    std::lock_guard<std::mutex> lk(g_scratch_mu);

    const std::string idx = prepare_scratch_index();
    // Stage EVERYTHING (tracked changes + new files, deletions included)
    // into the scratch index. --ignore-errors: an unreadable file (perms,
    // vanished mid-scan) should degrade to "not in the snapshot", not
    // abort the checkpoint.
    if (!ok(run_git_scratch(idx, {"git", "-C", repo().root, "add", "-A",
                                  "--ignore-errors"}))) {
        drop_scratch_index(idx);
        return false;
    }
    auto tree = run_git_scratch(idx, {"git", "-C", repo().root, "write-tree"});
    drop_scratch_index(idx);
    if (!ok(tree)) return false;

    // Parentless commit: restore never walks history, and no parent
    // means dropping the ref makes the whole snapshot collectable.
    // Identity is forced inline so checkpoints work in repos where the
    // user hasn't configured user.name/email yet.
    auto commit = run_git({"-c", "user.name=agentty",
                           "-c", "user.email=checkpoint@agentty",
                           "commit-tree", chomp(tree.output),
                           "-m", "agentty checkpoint " + id});
    if (!ok(commit)) return false;

    if (!ok(run_git({"update-ref", ref_for(id), chomp(commit.output)})))
        return false;

    prune_old_checkpoints();
    return true;
}

bool checkpoint_exists(const std::string& id) {
    if (!repo().in_repo || id.empty()) return false;
    return ok(run_git({"rev-parse", "--verify", "--quiet",
                       ref_for(id) + "^{commit}"}));
}

bool restore_checkpoint(const std::string& id, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (!repo().in_repo) return fail("not inside a git repository");
    if (id.empty())      return fail("empty checkpoint id");

    auto rev = run_git({"rev-parse", "--verify", "--quiet",
                        ref_for(id) + "^{commit}"});
    if (!ok(rev)) return fail("checkpoint no longer exists (pruned?)");
    const std::string commit = chomp(rev.output);

    // Snapshot + current file sets FIRST — if either listing fails we
    // bail before touching a single byte of the worktree.
    std::unordered_set<std::string> snap;
    if (!snapshot_paths(commit, snap)) return fail("failed to list checkpoint contents");
    std::vector<std::string> now;
    if (!current_paths(now)) return fail("failed to list current files");

    std::lock_guard<std::mutex> lk(g_scratch_mu);

    // Rewrite every snapshotted file from the checkpoint tree via a
    // scratch index: read-tree populates it, checkout-index -a -f
    // force-writes the files. The real index and HEAD stay untouched —
    // `git status` afterwards shows the restored state as ordinary
    // working-tree changes, which is exactly what the user expects.
    const std::string idx = prepare_scratch_index();
    bool wrote =
        ok(run_git_scratch(idx, {"git", "-C", repo().root, "read-tree", commit}))
        && ok(run_git_scratch(idx, {"git", "-C", repo().root,
                                    "checkout-index", "-a", "-f"}));
    drop_scratch_index(idx);
    if (!wrote) return fail("failed to write checkpoint files");

    // Delete files that exist now but weren't in the snapshot — the
    // "agent created a file after this point" half of the rewind.
    // Ignored files (build outputs, caches) never appear in `now`, so
    // they survive. Deletion is best-effort; a failed unlink degrades
    // to a leftover file, not a failed restore.
    const fs::path root{repo().root};
    for (const auto& rel : now) {
        if (snap.contains(rel)) continue;
        std::error_code ec;
        fs::remove(root / rel, ec);
    }
    return true;
}

} // namespace agentty::workspace
