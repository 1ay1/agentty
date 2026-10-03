// dirs_test.cpp — agentty::dirs, the write-side storage primitive.
//
// The properties worth pinning, and the bug each one guards:
//
//   1. default resolution + provenance (Origin::Default)
//   2. an absolute override wins                      (#58's whole point)
//   3. a RELATIVE override resolves against the ROOT, not the cwd — the
//      "one directory, not one per launch dir" rule
//   4. an EMPTY override counts as unset (exported-but-blank is a common
//      shell accident and must relocate nothing)
//   5. an UNUSABLE override warns and falls back, reporting
//      Origin::OverrideFellBack rather than silently papering over it
//   6. a non-overridable Spec ignores the environment entirely (credentials
//      must not relocate because of a line in a shell profile)
//   7. owner_only forces 0700 even when the override points at a 0755 dir
//   8. project_anchor() WALKS UP to a marker — the `cd src/ && agentty`
//      duplicate-index bug (#61). This is the one project_root() cannot do,
//      since it is the cwd clamped, and the reason dirs has its own anchor.
//   9. a markerless tree falls back to the cwd, so behaviour is unchanged
//      from before this primitive existed
//  10. Root::Project hangs off <anchor>/.agentty
//  11. resolve_dry() creates nothing
//  12. Lifecycle::sweeps() is the structural gate for #62
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "agentty/dirs/dirs.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using namespace agentty::dirs;

namespace {

fs::path g_sandbox;
int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << "x";
}

#ifndef _WIN32
unsigned mode_of(const fs::path& p) {
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return st.st_mode & 07777;
}
#endif

}  // namespace

int main() {
    g_sandbox = fs::temp_directory_path() /
                ("agentty_dirs_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(g_sandbox, ec);

    const fs::path home = g_sandbox / "home";
    fs::create_directories(home);
    ::setenv("HOME", home.c_str(), 1);
    ::unsetenv("AGENTTY_HOME");
    ::unsetenv("XDG_CONFIG_HOME");
    ::unsetenv("AGENTTY_TEST_DIR");

    const fs::path root = home / ".agentty";

    // ── 1. default resolution ────────────────────────────────────────────
    {
        const Spec s{.root = Root::User, .leaf = "widgets", .env = "AGENTTY_TEST_DIR"};
        auto r = resolve(s);
        check(r.has_value(), "default: resolved");
        if (r) {
            check(r->path == root / "widgets", "default: path under root");
            check(r->origin == Origin::Default, "default: origin");
            check(!r->overridden(), "default: not overridden");
            check(fs::is_directory(r->path), "default: created");
            check(r->root == root, "default: root reported");
        }
    }

    // ── 2. absolute override wins ────────────────────────────────────────
    {
        const fs::path elsewhere = g_sandbox / "elsewhere";
        ::setenv("AGENTTY_TEST_DIR", elsewhere.c_str(), 1);
        const Spec s{.root = Root::User, .leaf = "widgets", .env = "AGENTTY_TEST_DIR"};
        auto r = resolve(s);
        check(r.has_value(), "abs override: resolved");
        if (r) {
            check(r->path == elsewhere, "abs override: path honoured");
            check(r->origin == Origin::Override, "abs override: origin");
            check(fs::is_directory(elsewhere), "abs override: created");
        }
    }

    // ── 3. a relative override resolves against the ROOT, not the cwd ────
    // The rule that makes an override mean ONE directory rather than a
    // different one for every directory agentty is launched from.
    {
        ::setenv("AGENTTY_TEST_DIR", "widgets2", 1);
        const fs::path cwd_before = fs::current_path();
        fs::create_directories(g_sandbox / "unrelated");
        fs::current_path(g_sandbox / "unrelated");

        const Spec s{.root = Root::User, .leaf = "widgets", .env = "AGENTTY_TEST_DIR"};
        auto r = resolve(s);
        check(r.has_value(), "rel override: resolved");
        if (r) {
            check(r->path == root / "widgets2", "rel override: under root, not cwd");
            check(r->path.parent_path() != g_sandbox / "unrelated",
                  "rel override: did NOT resolve against cwd");
        }
        fs::current_path(cwd_before);
    }

    // ── 4. empty means unset ─────────────────────────────────────────────
    {
        ::setenv("AGENTTY_TEST_DIR", "", 1);
        const Spec s{.root = Root::User, .leaf = "widgets", .env = "AGENTTY_TEST_DIR"};
        auto r = resolve(s);
        check(r.has_value(), "empty override: resolved");
        if (r) {
            check(r->path == root / "widgets", "empty override: treated as unset");
            check(r->origin == Origin::Default, "empty override: origin is Default");
        }
    }

    // ── 5. unusable override warns + falls back, and SAYS so ─────────────
    // A file in the way is the single most likely mistake. The warning text
    // is checked by eye; what the test pins is that we fall back AND report
    // it, rather than silently serving the default as if nothing happened.
    {
        const fs::path blocker = g_sandbox / "a-file-not-a-dir";
        touch(blocker);
        ::setenv("AGENTTY_TEST_DIR", blocker.c_str(), 1);
        const Spec s{.root = Root::User, .leaf = "widgets", .env = "AGENTTY_TEST_DIR"};
        auto r = resolve(s);
        check(r.has_value(), "bad override: still resolves");
        if (r) {
            check(r->path == root / "widgets", "bad override: fell back to default");
            check(r->origin == Origin::OverrideFellBack,
                  "bad override: origin records the fallback");
            check(r->overridden(), "bad override: overridden() is true");
        }
        ::unsetenv("AGENTTY_TEST_DIR");
    }

    // ── 6. a non-overridable Spec ignores the environment ────────────────
    {
        ::setenv("AGENTTY_TEST_DIR", (g_sandbox / "ignored").c_str(), 1);
        const Spec s{.root = Root::User, .leaf = "secrets"};  // no .env
        auto r = resolve(s);
        check(r.has_value(), "no-env: resolved");
        if (r) {
            check(r->path == root / "secrets", "no-env: env ignored");
            check(r->origin == Origin::Default, "no-env: origin is Default");
        }
        ::unsetenv("AGENTTY_TEST_DIR");
    }

    // ── 7. owner_only forces 0700 on an override we did not create ───────
#ifndef _WIN32
    {
        const fs::path loose = g_sandbox / "loose";
        fs::create_directories(loose);
        ::chmod(loose.c_str(), 0755);
        check(mode_of(loose) == 0755, "owner_only: precondition is 0755");

        ::setenv("AGENTTY_TEST_DIR", loose.c_str(), 1);
        const Spec s{.root = Root::User, .leaf = "private",
                     .env = "AGENTTY_TEST_DIR", .owner_only = true};
        auto r = resolve(s);
        check(r.has_value(), "owner_only: resolved");
        if (r) check(mode_of(r->path) == 0700, "owner_only: tightened to 0700");
        ::unsetenv("AGENTTY_TEST_DIR");
    }
#endif

    // ── 8. the anchor WALKS UP to a marker ───────────────────────────────
    // The #61 duplicate-index bug, and the whole reason this is not
    // project_root(): launch from a subdir and the anchor must NOT move.
    {
        const fs::path proj = g_sandbox / "proj";
        const fs::path deep = proj / "src" / "runtime";
        fs::create_directories(deep);
        fs::create_directories(proj / ".git");

        const fs::path cwd_before = fs::current_path();

        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(proj);
        const fs::path from_top = project_anchor();

        fs::current_path(deep);
        agentty::tools::util::set_workspace_root(deep);
        const fs::path from_deep = project_anchor();

        check(from_top == fs::weakly_canonical(proj), "anchor: found from top");
        check(from_deep == fs::weakly_canonical(proj),
              "anchor: WALKED UP from src/runtime to the .git root");
        check(from_top == from_deep,
              "anchor: stable regardless of launch dir (the #61 fix)");

        // And the storage path that follows from it.
        const Spec s{.root = Root::Project, .leaf = "rag"};
        auto r = resolve(s);
        check(r.has_value(), "project: resolved");
        if (r) {
            check(r->path == fs::weakly_canonical(proj) / ".agentty" / "rag",
                  "project: <anchor>/.agentty/<leaf>");
            check(fs::is_directory(r->path), "project: created");
        }

        fs::current_path(cwd_before);
    }

    // ── 9. a markerless tree falls back to the cwd ───────────────────────
    // Behaviour must be unchanged from before this primitive existed, so a
    // tree with no .git still stores rather than refusing.
    {
        const fs::path bare = g_sandbox / "bare";
        fs::create_directories(bare);
        const fs::path cwd_before = fs::current_path();

        fs::current_path(bare);
        agentty::tools::util::set_workspace_root(bare);
        const fs::path a = project_anchor();
        check(a == fs::weakly_canonical(bare), "markerless: falls back to cwd");

        fs::current_path(cwd_before);
    }

    // ── 10. resolve_dry creates nothing ──────────────────────────────────
    {
        const Spec s{.root = Root::User, .leaf = "never-made"};
        auto r = resolve_dry(s);
        check(r.has_value(), "dry: resolved");
        if (r) {
            check(r->path == root / "never-made", "dry: path computed");
            check(!fs::exists(r->path), "dry: nothing created");
        }
    }

    // ── 11. Lifecycle is the gate for #62 ────────────────────────────────
    // Pure, so it is checked by value rather than against the disk.
    {
        check(!Lifecycle{}.sweeps(), "lifecycle: default never sweeps");
        check(!Lifecycle{.rebuildable = true}.sweeps(),
              "lifecycle: rebuildable alone does not sweep (keep_last=0 ⇒ keep all)");
        check(!Lifecycle{.keep_last = 1}.sweeps(),
              "lifecycle: keep_last WITHOUT rebuildable never sweeps — the "
              "blobs::gc asymmetry, unrecoverable data is never auto-deleted");
        check((Lifecycle{.rebuildable = true, .keep_last = 1}.sweeps()),
              "lifecycle: rebuildable + keep_last sweeps");
    }

    // ── markers are non-empty and include the common case ────────────────
    {
        auto m = project_markers();
        check(!m.empty(), "markers: non-empty");
        bool has_git = false;
        for (auto s : m) if (s == ".git") has_git = true;
        check(has_git, "markers: .git present");
    }

    fs::remove_all(g_sandbox, ec);

    if (g_failures == 0) {
        std::fprintf(stderr, "all dirs checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d dirs check(s) failed\n", g_failures);
    return 1;
}
