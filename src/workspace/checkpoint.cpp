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
#include <chrono>
#include <cstdio>
#include <atomic>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>   // GetCurrentProcessId
#else
#  include <unistd.h>    // getpid
#endif

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
//
// We probe from the PROCESS CWD, not util::workspace_root(). The
// workspace root is the filesystem *access* boundary and is routinely
// widened (`-w /` for full-disk power) — but `-w /` must not make the
// checkpoint layer think the project is `/` (which is never a repo, so
// checkpoints would silently die). cwd is the directory the user
// launched agentty from — i.e. their project — and agentty never
// chdir's away from it, so `git -C <cwd> rev-parse` walks up to the
// real enclosing repo regardless of how wide the sandbox gate is.
// Falls back to the workspace root if cwd is somehow unreadable.
const RepoInfo& repo() {
    static const RepoInfo info = [] {
        RepoInfo r;
        // The ACTIVE PROJECT (cwd clamped inside the access boundary), not
        // the raw boundary: `-w /` must not make the checkpoint layer think
        // the project is `/`. project_root() centralises this cwd-vs-boundary
        // resolution (shared with git tools + normalize_path).
        std::string ws = util::project_root().string();
        if (ws.empty()) ws = util::workspace_root().string();
        // ONE spawn, not two. `git rev-parse` prints each requested value on
        // its own line in option order, so a single invocation yields both
        // the worktree top-level (line 1) and the absolute git dir (line 2).
        // Process creation is the expensive part on Windows, and this probe
        // sits on the first turn's critical path (submit_message ->
        // in_git_repo) -- halving the spawns halves that cost.
        auto res = util::run_argv_s(
            {"git", "-C", ws, "rev-parse", "--show-toplevel",
             "--absolute-git-dir"}, 8192, std::chrono::seconds{10});
        if (!res.started || res.exit_code != 0) return r;
        std::string out = res.output;
        const auto nl = out.find('\n');
        if (nl == std::string::npos) return r;   // need both lines
        r.root    = chomp(out.substr(0, nl));
        r.git_dir = chomp(out.substr(nl + 1));
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

// Scratch index files left behind by a process killed mid-operation. A git
// call here may run 120s, so only files idle well past that are stale; a
// live operation (this process or another agentty) touches its file.
// Swept every 16th new one, which is often enough.
void sweep_stale_scratch_indexes() {
    std::error_code ec;
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::minutes{10};
    for (const auto& e : fs::directory_iterator(fs::path(repo().git_dir), ec)) {
        const auto name = e.path().filename().string();
        if (!name.starts_with("agentty-checkpoint-index")) continue;
        std::error_code tec;
        const auto t = fs::last_write_time(e.path(), tec);
        if (!tec && t < cutoff) fs::remove(e.path(), tec);
    }
}

// Scratch index path inside the git dir, UNIQUE per call. Seeded from a
// COPY of the real index so `add -A` only re-hashes files whose stat info
// changed — without the seed, every checkpoint would re-hash the entire
// worktree. Distinct files per operation replace the old guarded lock:
// its body spanned the git subprocesses, and exec.cpp's Windows
// stdin-feeder join mints thread_root inside run_child — a capability
// minted under a held lock aborts (jaal), which killed the process on the
// first submit in a repo. With one index file per operation the lock has
// nothing left to guard, and the create-vs-restore worktree race stays
// excluded at the app level (restore runs only from Idle with
// thread_loading set; submit refuses while it is).
std::string prepare_scratch_index() {
    static std::atomic<unsigned> seq{0};
    if (seq.load() % 16 == 0) sweep_stale_scratch_indexes();
    // pid in the name too: two agentty in one repo must not share a file.
#ifdef _WIN32
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    const fs::path idx = fs::path(repo().git_dir)
        / ("agentty-checkpoint-index." + std::to_string(pid)
           + "." + std::to_string(seq.fetch_add(1)));
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
//
// Two creates can prune at once now (no lock). Each delete names the commit
// the ref held when listed, so git drops it only if nobody moved it since;
// a ref another prune already deleted just fails that one call.
void prune_old_checkpoints() {
    auto r = run_git({"for-each-ref", "--sort=creatordate",
                      "--format=%(refname) %(objectname)", std::string{kRefPrefix}});
    if (!ok(r)) return;
    std::vector<std::pair<std::string, std::string>> refs;   // ref, sha
    std::istringstream in(r.output);
    for (std::string line; std::getline(in, line); ) {
        line = chomp(std::move(line));
        const auto sp = line.rfind(' ');
        if (sp != std::string::npos) refs.emplace_back(line.substr(0, sp), line.substr(sp + 1));
    }
    if (static_cast<int>(refs.size()) <= kKeepCheckpoints) return;
    const std::size_t drop = refs.size() - static_cast<std::size_t>(kKeepCheckpoints);
    for (std::size_t i = 0; i < drop; ++i)
        (void)run_git({"update-ref", "-d", refs[i].first, refs[i].second});
}

// Repo-relative paths of every file recorded in the checkpoint tree.
//
// NEWLINE-separated, NOT -z: the subprocess runner scrubs terminal control
// bytes from all captured output (strip_terminal_controls), and NUL is a
// C0 control — every \0 separator was silently dropped, the parser found
// no separators, and both listings came back EMPTY. Restore's delete-half
// (remove files created after the checkpoint) never ran, and the summary
// baseline was wrong. -c core.quotePath=false emits raw UTF-8 bytes (no
// C-style quoting of non-ASCII names); a filename containing a NEWLINE is
// still ambiguous in this format, so such entries are skipped defensively
// (git quotes them even with quotePath=false, and a quoted entry starts
// with '"' — filter those rather than mis-delete).
bool snapshot_paths(const std::string& commit,
                    std::unordered_set<std::string>& out) {
    auto r = run_git({"-c", "core.quotePath=false",
                      "ls-tree", "-r", "--name-only", commit},
                     16 * 1024 * 1024);
    if (!ok(r)) return false;
    std::istringstream in(r.output);
    for (std::string line; std::getline(in, line); ) {
        line = chomp(std::move(line));
        if (line.empty() || line.front() == '"') continue;
        out.emplace(std::move(line));
    }
    return true;
}

// Repo-relative paths of everything present in the worktree right now:
// tracked (-c) plus untracked-but-not-ignored (-o --exclude-standard).
// Same newline/quotePath rationale as snapshot_paths.
bool current_paths(std::vector<std::string>& out) {
    auto r = run_git({"-c", "core.quotePath=false",
                      "ls-files", "-c", "-o", "--exclude-standard"},
                     16 * 1024 * 1024);
    if (!ok(r)) return false;
    std::istringstream in(r.output);
    for (std::string line; std::getline(in, line); ) {
        line = chomp(std::move(line));
        if (line.empty() || line.front() == '"') continue;
        out.emplace_back(std::move(line));
    }
    return true;
}

} // namespace

bool in_git_repo(Io) { return repo().in_repo; }

// Set true once repo() has been forced at least once (by the startup
// prewarm OR by any earlier blocking caller). The submit path reads this to
// decide whether in_git_repo() would answer instantly (cache warm) or would
// have to spawn `git` on the UI thread (cache cold). See
// in_git_repo_if_ready().
namespace {
std::atomic<bool> g_repo_ready{false};
}

std::optional<bool> in_git_repo_if_ready(Io) {
    if (!g_repo_ready.load(std::memory_order_acquire)) return std::nullopt;
    return repo().in_repo;   // cache already built: this is a pure field read
}

void prewarm_repo_info(std::stop_token stop) {
    // SYNCHRONOUS. Touching repo() forces its function-local static to
    // initialise, running the two `git rev-parse` probes here instead of
    // on the first submit.
    //
    // Runs as Cmd::task_isolated. At shutdown jaal stops `stop` and waits
    // for us to return; the probe fills a static, so skip it once stopped
    // or a fast exit races static destruction.
    if (stop.stop_requested()) return;
    (void)repo();
    g_repo_ready.store(true, std::memory_order_release);
}

namespace {
bool create_checkpoint_locked(const std::string& id);

// Tree id of the current worktree, built in a scratch index (seeded from
// the real one so `add -A` only re-hashes changed files). nullopt on failure.
std::optional<std::string> worktree_tree() {
    const std::string idx = prepare_scratch_index();
    // --ignore-errors: an unreadable file (perms, vanished mid-scan) degrades
    // to "not in the tree", not a failure.
    if (!ok(run_git_scratch(idx, {"git", "-C", repo().root, "add", "-A",
                                  "--ignore-errors"}))) {
        drop_scratch_index(idx);
        return std::nullopt;
    }
    auto tree = run_git_scratch(idx, {"git", "-C", repo().root, "write-tree"});
    drop_scratch_index(idx);
    if (!ok(tree)) return std::nullopt;
    return chomp(tree.output);
}
} // namespace

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
    const auto tree = worktree_tree();
    if (!tree) return out;
    const std::string& cur_tree = *tree;

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
    return create_checkpoint_locked(id);
}

namespace {
bool create_checkpoint_locked(const std::string& id) {
    // Stage EVERYTHING (tracked changes + new files, deletions included).
    const auto tree = worktree_tree();
    if (!tree) return false;

    // Parentless commit: restore never walks history, and no parent
    // means dropping the ref makes the whole snapshot collectable.
    // Identity is forced inline so checkpoints work in repos where the
    // user hasn't configured user.name/email yet.
    auto commit = run_git({"-c", "user.name=agentty",
                           "-c", "user.email=checkpoint@agentty",
                           "commit-tree", *tree,
                           "-m", "agentty checkpoint " + id});
    if (!ok(commit)) return false;

    if (!ok(run_git({"update-ref", ref_for(id), chomp(commit.output)})))
        return false;

    prune_old_checkpoints();
    return true;
}
} // namespace

bool checkpoint_exists(const std::string& id) {
    if (!repo().in_repo || id.empty()) return false;
    return ok(run_git({"rev-parse", "--verify", "--quiet",
                       ref_for(id) + "^{commit}"}));
}

bool restore_checkpoint(const std::string& id,
                        std::string* error) {
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

    // Rewrite every snapshotted file from the checkpoint tree via a
    // scratch index: read-tree populates it, checkout-index -a -f
    // force-writes the files. The real index and HEAD stay untouched —
    // `git status` afterwards shows the restored state as ordinary
    // working-tree changes, which is exactly what the user expects.
    const std::string idx = prepare_scratch_index();
    const bool wrote =
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
