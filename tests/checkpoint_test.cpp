// checkpoint_test — end-to-end coverage of workspace::checkpoint against a
// REAL scratch git repository (the module shells out to git; nothing less
// exercises it). Creates an isolated repo under the system temp dir, chdirs
// into it for the duration (repo() caches its root on first call, so this
// test must run in its own process — MODE standalone), and walks the whole
// lifecycle: create → mutate (edit + delete + add + rename-ish) → summary →
// restore → idempotence → missing-id failure → ignored-file survival.
//
// The transcript-side flow (RestoreCheckpoint reducer) is not covered here —
// it needs a full Model; this file owns the git/worktree half, which is
// where the destructive operations live.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <unistd.h>   // getpid
#else
#include <process.h>
#define getpid _getpid
#endif

#include "agentty/workspace/checkpoint.hpp"
#include "agentty/tool/util/subprocess.hpp"

namespace fs = std::filesystem;
using namespace agentty;

static int g_fails = 0;
static void check(bool ok, const std::string& msg) {
    if (!ok) { ++g_fails; std::printf("  FAIL: %s\n", msg.c_str()); }
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}
static void put(const fs::path& p, const std::string& s) {
    if (p.has_parent_path()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::trunc);
    f << s;
}
static void sh(const std::string& cmd) {
    (void)tools::util::run_command_s(cmd, 65536, std::chrono::seconds{30});
}

int main() {
    // ── Isolated scratch repo; cwd moved INTO it before first repo() ─────
    fs::path root = fs::temp_directory_path()
                  / ("agentty-cp-test-" + std::to_string(::getpid()));
    fs::create_directories(root);
    fs::current_path(root);
    sh("git init -q .");
    // repo() must work without user identity configured (commit-tree gets
    // inline -c identity); leave user.name/email unset on purpose.

    check(workspace::in_git_repo(), "scratch repo detected");

    // ── t0 content: nested dir, spaces in names, then checkpoint ────────
    put("a.txt", "original-a\n");
    put("sub/b.txt", "original-b\n");
    put("spaced name.txt", "original-spaced\n");
    check(workspace::create_checkpoint("cp1"), "create cp1");
    check(workspace::checkpoint_exists("cp1"), "cp1 exists");
    check(!workspace::checkpoint_exists("zzz"), "unknown id does not exist");

    // ── mutate: edit, delete, add ───────────────────────────────────────
    put("a.txt", "MUTATED-a\n");
    fs::remove("sub/b.txt");
    put("new.txt", "agent-created\n");
    put("spaced name.txt", "MUTATED-spaced\n");

    auto d = workspace::checkpoint_summary("cp1");
    check(d.valid, "summary computes");
    check(d.files_changed == 4,
          "summary counts 4 changed files (got "
          + std::to_string(d.files_changed) + ")");

    // ── restore: worktree returns to t0 exactly ─────────────────────────
    std::string err;
    check(workspace::restore_checkpoint("cp1", &err),
          "restore cp1 (" + err + ")");
    check(slurp("a.txt") == "original-a\n", "edited file restored");
    check(slurp("sub/b.txt") == "original-b\n", "deleted file restored");
    check(slurp("spaced name.txt") == "original-spaced\n",
          "filename with spaces restored");
    check(!fs::exists("new.txt"), "file created after checkpoint removed");
    if (fs::exists("new.txt")) {
        auto ls = tools::util::run_command_s(
            "git ls-files -z -c -o --exclude-standard | tr '\\0' '\\n'",
            65536, std::chrono::seconds{30});
        std::printf("  [diag] ls-files:\n%s\n", ls.output.c_str());
        auto lt = tools::util::run_command_s(
            "git ls-tree -r --name-only refs/agentty/checkpoints/cp1",
            65536, std::chrono::seconds{30});
        std::printf("  [diag] snapshot tree:\n%s\n", lt.output.c_str());
        std::printf("  [diag] cwd=%s\n", fs::current_path().string().c_str());
        auto rr = tools::util::run_command_s(
            "git rev-parse --show-toplevel", 65536, std::chrono::seconds{30});
        std::printf("  [diag] toplevel=%s\n", rr.output.c_str());
    }

    // Post-restore the summary must be clean (0 changed files).
    auto d2 = workspace::checkpoint_summary("cp1");
    check(d2.valid && d2.files_changed == 0,
          "post-restore summary is clean (got "
          + std::to_string(d2.files_changed) + ")");

    // ── idempotence ─────────────────────────────────────────────────────
    check(workspace::restore_checkpoint("cp1", &err), "second restore ok");

    // ── failure paths surface errors, touch nothing ─────────────────────
    put("canary.txt", "untouched\n");
    check(!workspace::restore_checkpoint("nope", &err),
          "missing checkpoint fails");
    check(!err.empty(), "failure carries a reason");
    check(slurp("canary.txt") == "untouched\n",
          "failed restore leaves worktree alone");

    // ── ignored files survive both create and restore ───────────────────
    put(".gitignore", "ignored.log\n");
    put("ignored.log", "v1\n");
    check(workspace::create_checkpoint("cp2"), "create cp2");
    put("ignored.log", "v2\n");
    check(workspace::restore_checkpoint("cp2", &err), "restore cp2");
    check(slurp("ignored.log") == "v2\n",
          "ignored file untouched by restore");

    // ── the real index and HEAD stay untouched throughout ───────────────
    auto st = tools::util::run_command_s("git status --porcelain", 65536,
                                         std::chrono::seconds{30});
    check(st.started && st.exit_code == 0, "git status runs");
    // Every file should show as untracked/modified relative to the EMPTY
    // real index — restore must not have staged anything.
    auto staged = tools::util::run_command_s(
        "git diff --cached --name-only", 65536, std::chrono::seconds{30});
    check(staged.started && staged.output.empty(),
          "restore staged nothing into the real index");

    // ── cleanup ─────────────────────────────────────────────────────────
    fs::current_path(fs::temp_directory_path());
    std::error_code ec;
    fs::remove_all(root, ec);

    if (g_fails) {
        std::printf("CHECKPOINT TESTS FAILED (%d)\n", g_fails);
        return 1;
    }
    std::printf("ALL CHECKPOINT TESTS PASSED\n");
    return 0;
}
