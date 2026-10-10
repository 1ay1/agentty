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
//  13. sweep: keep_last + live-variant protection + sidecars + the legacy
//      untagged file, and a bounded blast radius
//  14. a NON-rebuildable store is never swept, whatever keep_last says
//  15. min_age holds back variants a concurrent process may still be writing
#include <array>
#include <chrono>
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
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);
        const fs::path from_top = project_anchor();

        fs::current_path(deep);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), deep);
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
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), bare);
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

    // ── 12. the RAG Spec shape: empty leaf + override ────────────────────
    // rag's Specs use leaf="" (they want the .agentty root itself, then add
    // their own filename) and share $AGENTTY_RAG_DIR. Pinned here because an
    // empty leaf is an easy thing to break with a stray path join, and
    // because this is the exact shape #61's fix ships.
    {
        const fs::path proj = g_sandbox / "ragproj";
        const fs::path deep = proj / "src" / "rag";
        fs::create_directories(deep);
        fs::create_directories(proj / ".git");
        const fs::path cwd_before = fs::current_path();

        const Spec rag{.root = Root::Project, .leaf = "", .env = "AGENTTY_RAG_DIR"};

        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);
        auto top = resolve(rag);

        fs::current_path(deep);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), deep);
        auto sub = resolve(rag);

        check(top.has_value() && sub.has_value(), "rag spec: resolved from both");
        if (top && sub) {
            check(top->path == fs::weakly_canonical(proj) / ".agentty",
                  "rag spec: empty leaf yields the .agentty root itself");
            check(top->path == sub->path,
                  "rag spec: ONE index dir from root and from src/rag (#61)");
        }

        // And the override relocates it, which is the #61 feature half.
        const fs::path moved = g_sandbox / "ragmoved";
        ::setenv("AGENTTY_RAG_DIR", moved.c_str(), 1);
        auto over = resolve(rag);
        check(over.has_value(), "rag spec: override resolved");
        if (over) {
            check(over->path == moved, "rag spec: $AGENTTY_RAG_DIR honoured");
            check(over->origin == Origin::Override, "rag spec: origin is Override");
        }
        ::unsetenv("AGENTTY_RAG_DIR");

        fs::current_path(cwd_before);
    }

    // ── 13. sweep: the #62 lifecycle, enforced ───────────────────────────
    {
        const fs::path proj = g_sandbox / "sweepproj";
        fs::create_directories(proj / ".git");
        const fs::path cwd_before = fs::current_path();
        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);

        const Spec swept{
            .root = Root::Project, .leaf = "",
            .life = {.rebuildable = true, .keep_last = 1},
        };
        auto dir = resolve(swept);
        check(dir.has_value(), "sweep: dir resolved");
        const fs::path d = dir->path;

        // Four variants + a live one + sidecars + a legacy untagged file.
        auto mk = [&](const std::string& n, int age_days) {
            touch(d / n);
            touch(d / (n + ".meta.json"));
            auto t = fs::file_time_type::clock::now()
                   - std::chrono::hours{24 * age_days};
            std::error_code tec;
            fs::last_write_time(d / n, t, tec);
        };
        mk("rag_code.live.ragdb", 0);
        mk("rag_code.aaaa.ragdb", 1);   // newest stale → kept by keep_last=1
        mk("rag_code.bbbb.ragdb", 5);
        mk("rag_code.cccc.ragdb", 9);
        touch(d / "rag_code.ragdb");               // legacy, untagged
        touch(d / "rag_docs.other.ragdb");         // different stem, untouched
        touch(d / "unrelated.txt");

        const std::array<std::string, 1> keep{"rag_code.live.ragdb"};
        static constexpr std::array<std::string_view, 1> side{".meta.json"};
        const SweepRequest req{
            .spec = swept, .stem = "rag_code.", .suffix = ".ragdb",
            .keep = keep, .sidecar_suffixes = side, .include_untagged = true,
        };

        // DRY RUN first: reports, touches nothing. The property that makes
        // this safe to develop against a real store.
        auto dry = sweep(req, /*dry_run=*/true);
        check(dry.deleted == 3, "sweep dry: would delete 3 (bbbb, cccc, legacy)");
        check(fs::exists(d / "rag_code.cccc.ragdb"), "sweep dry: deleted nothing");
        check(fs::exists(d / "rag_code.ragdb"), "sweep dry: legacy still there");

        auto st = sweep(req);
        check(st.deleted == 3, "sweep: deleted 3");
        check(st.failed == 0, "sweep: no failures");

        // The live one and the newest stale variant survive.
        check(fs::exists(d / "rag_code.live.ragdb"), "sweep: live kept");
        check(fs::exists(d / "rag_code.aaaa.ragdb"),
              "sweep: newest stale kept (keep_last=1 keeps the A/B warm)");
        check(!fs::exists(d / "rag_code.bbbb.ragdb"), "sweep: older deleted");
        check(!fs::exists(d / "rag_code.cccc.ragdb"), "sweep: oldest deleted");

        // Sidecars go with their principal.
        check(!fs::exists(d / "rag_code.cccc.ragdb.meta.json"),
              "sweep: sidecar deleted with its principal");
        check(fs::exists(d / "rag_code.live.ragdb.meta.json"),
              "sweep: live sidecar kept");

        // The legacy untagged file: #62's actual 47 MB.
        check(!fs::exists(d / "rag_code.ragdb"), "sweep: legacy untagged deleted");

        // Blast radius: a different stem and an unrelated file are untouched.
        check(fs::exists(d / "rag_docs.other.ragdb"), "sweep: other stem untouched");
        check(fs::exists(d / "unrelated.txt"), "sweep: unrelated file untouched");

        fs::current_path(cwd_before);
    }

    // ── 14. a non-rebuildable store is NEVER swept ───────────────────────
    // The blobs::gc asymmetry, enforced. keep_last alone must do nothing:
    // feedback data is accumulated from real usage and nothing regenerates
    // it, so no sweep may ever collect it.
    {
        const fs::path proj = g_sandbox / "nosweep";
        fs::create_directories(proj / ".git");
        const fs::path cwd_before = fs::current_path();
        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);

        const Spec precious{
            .root = Root::Project, .leaf = "",
            .life = {.rebuildable = false, .keep_last = 1},   // NOT rebuildable
        };
        auto dir = resolve(precious);
        check(dir.has_value(), "no-sweep: resolved");
        touch(dir->path / "rag_code.aaaa.ragdb");
        touch(dir->path / "rag_code.bbbb.ragdb");

        const SweepRequest req{
            .spec = precious, .stem = "rag_code.", .suffix = ".ragdb",
        };
        auto st = sweep(req);
        check(st.deleted == 0, "no-sweep: nothing deleted");
        check(st.examined == 0, "no-sweep: did not even look");
        check(fs::exists(dir->path / "rag_code.bbbb.ragdb"),
              "no-sweep: non-rebuildable data survives keep_last");

        fs::current_path(cwd_before);
    }

    // ── 15. min_age holds back a fresh variant ───────────────────────────
    // Another process may be mid-save; a concurrent one may legitimately be
    // running a different embedder.
    {
        const fs::path proj = g_sandbox / "graceproj";
        fs::create_directories(proj / ".git");
        const fs::path cwd_before = fs::current_path();
        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);

        // keep_last must be >0 or sweeps() is false and this test would
        // pass vacuously -- it would be measuring the gate, not the grace
        // window. Two fresh variants with keep_last=1: without min_age the
        // older would be deleted, so holding BOTH proves the window works.
        const Spec graced{
            .root = Root::Project, .leaf = "",
            .life = {.rebuildable = true, .keep_last = 1,
                     .min_age = std::chrono::seconds{3600}},
        };
        auto dir = resolve(graced);
        check(dir.has_value(), "min_age: resolved");
        touch(dir->path / "rag_code.fresh1.ragdb");   // mtime = now
        touch(dir->path / "rag_code.fresh2.ragdb");   // mtime = now

        const SweepRequest req{
            .spec = graced, .stem = "rag_code.", .suffix = ".ragdb",
        };
        auto st = sweep(req);
        check(st.examined == 2, "min_age: both variants examined");
        check(st.too_new == 2, "min_age: both held back by the grace window");
        check(st.deleted == 0, "min_age: nothing deleted");
        check(fs::exists(dir->path / "rag_code.fresh1.ragdb")
                  && fs::exists(dir->path / "rag_code.fresh2.ragdb"),
              "min_age: fresh variants survive even beyond keep_last");

        fs::current_path(cwd_before);
    }

    // ── resolve_dry agrees with resolve ─────────────────────────────────
    //
    // The observer must predict the writer. `agentty config` resolves dry so
    // that asking where the logs go does not MAKE a logs directory -- but a
    // dry resolve that skips the viability checks reports a path the writer
    // would reject, which is worse than creating the directory: the user is
    // told a location that can never exist.
    //
    // The case that caught it: $AGENTTY_HOME naming a FILE. `<file>/threads`
    // is merely ABSENT, which reads as "fine, we'll create it" unless you
    // walk up to the nearest existing ancestor and find a file in the way.
    {
        const fs::path base = g_sandbox / "dry";
        fs::create_directories(base);
        const fs::path afile = base / "a-file";
        { std::ofstream f(afile); f << "x"; }

        const Spec s{.root = Root::User, .leaf = "threads",
                     .env = "AGENTTY_TEST_DRY"};

        // Override points at a FILE -> the writer warns and falls back, so
        // the dry answer must be the fallback, flagged as such.
        ::setenv("AGENTTY_TEST_DRY", afile.string().c_str(), 1);
        auto dry = resolve_dry(s);
        auto wet = resolve(s);
        check(dry.has_value() && wet.has_value(), "dry: both resolve");
        if (dry && wet) {
            check(dry->path == wet->path,
                  "dry: the path matches what the writer chose");
            check(dry->origin == wet->origin,
                  "dry: the origin matches too (fell-back, not override)");
            check(dry->origin == Origin::OverrideFellBack,
                  "dry: a file-valued override is reported as fallen back");
        }
        ::unsetenv("AGENTTY_TEST_DRY");

        // And a dry resolve still creates NOTHING -- that is the whole
        // reason it exists.
        const fs::path probe = base / "never-made";
        const Spec q{.root = Root::User, .leaf = "", .env = "AGENTTY_TEST_DRY2"};
        ::setenv("AGENTTY_TEST_DRY2", probe.string().c_str(), 1);
        (void)resolve_dry(q);
        check(!fs::exists(probe), "dry: resolves without creating");
        ::unsetenv("AGENTTY_TEST_DRY2");
    }

    // ── the project root ignores itself ───────────────────────────────
    //
    // <project>/.agentty holds derived state -- tens of MB of retrieval index
    // -- next to a user's source. agentty's own .gitignore protects agentty's
    // repo and nobody else's: in any other checkout, the first `git add -A`
    // after a warm index stages the lot. Shipping the rule INSIDE the
    // directory makes it structural instead of folklore.
    {
        const fs::path proj = g_sandbox / "ignoreproj";
        fs::create_directories(proj / ".git");
        const fs::path cwd_before = fs::current_path();
        fs::current_path(proj);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), proj);

        const Spec s{.root = Root::Project, .leaf = ""};
        auto d = resolve(s);
        check(d.has_value(), "ignore: project spec resolves");
        if (d) {
            const fs::path ig = d->root / ".gitignore";
            check(fs::exists(ig), "ignore: .gitignore written on creation");
            std::ifstream in(ig);
            std::string body((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
            check(body.find("\n*\n") != std::string::npos
                      || body.rfind("*\n") != std::string::npos,
                  "ignore: the rule ignores everything in here");

            // NEVER clobbered. A user who wants to commit part of .agentty
            // (a shared skills/ dir is a real use) edits this file, and a
            // tool that rewrote it every launch would be a tool that argues.
            {
                std::ofstream out(ig, std::ios::trunc);
                out << "# mine\n*.ragdb\n";
            }
            (void)resolve(s);
            std::ifstream again(ig);
            std::string after((std::istreambuf_iterator<char>(again)),
                               std::istreambuf_iterator<char>());
            check(after == "# mine\n*.ragdb\n",
                  "ignore: a user-edited file is left alone");
        }

        // The USER root gets no such file -- ~/.agentty is not inside a
        // repository, so the rule would be noise.
        const Spec u{.root = Root::User, .leaf = "threads"};
        if (auto ud = resolve(u))
            check(!fs::exists(ud->root / ".gitignore"),
                  "ignore: user root is left alone");

        fs::current_path(cwd_before);
    }

    // ── one override location, many projects ────────────────────────────
    //
    // #61's worry: point two checkouts at one directory and they land on
    // the same filenames. Nothing corrupts -- the index meta records its
    // corpus root and refuses a foreign one -- but each switch then costs a
    // full re-index, which is worse than the disk it saved.
    {
        const fs::path p1 = g_sandbox / "shareda";
        const fs::path p2 = g_sandbox / "sharedb";
        const fs::path shared = g_sandbox / "onedir";
        fs::create_directories(p1 / ".git");
        fs::create_directories(p2 / ".git");
        const fs::path cwd_before = fs::current_path();

        const Spec s{.root = Root::Project, .leaf = "cache"};
        ::setenv("AGENTTY_PROJECT_DIR", shared.string().c_str(), 1);

        fs::current_path(p1);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), p1);
        auto a = resolve(s);

        fs::current_path(p2);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), p2);
        auto b = resolve(s);

        ::unsetenv("AGENTTY_PROJECT_DIR");
        check(a.has_value() && b.has_value(), "shared: both resolve");
        if (a && b) {
            check(a->path != b->path,
                  "two projects under one override get separate directories");
            check(a->path.string().find("shareda-") != std::string::npos,
                  "the basename stays readable");
        }

        // And the DEFAULT keeps no hash -- each project already has its own
        // .agentty, so there is nothing to disambiguate.
        fs::current_path(p1);
        agentty::tools::util::set_workspace_root(::agentty::IoAccess::grant(), p1);
        if (auto d = resolve(s))
            check(d->path == p1 / ".agentty" / "cache",
                  "the default path is plain");

        fs::current_path(cwd_before);
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
