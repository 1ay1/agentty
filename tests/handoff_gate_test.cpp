// Does the trust-handoff row actually enforce anything?
//
// It did not, for its entire life before this file. The pane offered
// "Agent writes host-executed files" with refuse/warn/allow, persistence
// round-tripped it, and `is_host_trusted()` had zero callers outside its own
// unit test. So the control saved a value nothing read -- the same bug class
// sandbox_pane_test was written for, one level up: not "the switch was set
// wrong" but "the switch said refuse and the write went through".
//
// What this covers, in the order the chain runs:
//
//   decide    the policy maps path shape + policy to a verdict
//   refuse    the default actually stops the write
//   feed      every handoff is recorded, allowed ones included
//   shape     matching is on the SHAPE, so a renamed .git is still caught
//   threads   the feed survives concurrent writers (it is cross-thread)
//
// What it does NOT cover is the mcp bridge dispatch that calls check() --
// that needs a live tool provider. The bridge call site is three lines and
// exercised by every real write; the interesting logic is all here.

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>   // getpid — unique symlink sandbox per run
#endif

#include "agentty/tool/util/handoff_gate.hpp"
#include "agentty/tool/util/sandbox.hpp"   // Mode::Off, for the "off means off" case

using namespace agentty;
namespace hg = tools::util::handoff;
using Policy = sandbox_cfg::HandoffPolicy;
using Kind = sandbox_cfg::TrustKind;

namespace {

// Clear before each case: the feed is process-global (it is a session log), so
// a test that inherits another's events is testing the wrong thing.
struct Fresh {
    Fresh() { hg::clear_handoff_feed(); }
    ~Fresh() { hg::clear_handoff_feed(); }
};

}  // namespace

TEST_CASE("handoff: --sandbox off turns the gate off too") {
    Fresh fresh;

    // "off" has to mean off. The gate is not a sandbox WALL -- it lives on the
    // tool call rather than in the kernel -- but it is part of the same
    // control, and a user who disabled sandboxing did not opt into one
    // surviving piece of it refusing their writes.
    //
    // Driven through check() rather than check_with(), because the mode test
    // IS the thing under test: check_with() takes an explicit policy and
    // deliberately knows nothing about modes.
    namespace sb = agentty::tools::util::sandbox;
    sb::reset_config_for_test();
    sandbox_cfg::Config cfg;
    cfg.configured = true;
    cfg.handoff = sandbox_cfg::HandoffPolicy::Refuse;   // the strictest setting
    sb::set_config(cfg);
    sb::init(sb::Mode::Off);

    auto v = hg::check("/work/.vscode/tasks.json", "write");
    CHECK(v.allowed);
    CHECK_FALSE(v.is_handoff);

    // And nothing is recorded: a disabled control should not be filling a feed
    // the pane will render as if it were watching.
    CHECK(hg::handoff_feed().empty());
}

// ── The policy ───────────────────────────────────────────────────────────

TEST_CASE("handoff: an ordinary path is not a handoff under any policy") {
    Fresh fresh;

    // The common case, and the one that must stay cheap and silent. If this
    // ever starts flagging, the shape table has grown a rule that matches
    // normal source files and the gate becomes noise the user turns off.
    for (auto p : {Policy::Refuse, Policy::Warn, Policy::Allow}) {
        auto v = hg::check_with("/work/src/main.cpp", "write", p);
        CHECK(v.allowed);
        CHECK_FALSE(v.is_handoff);
        CHECK(v.reason.empty());
    }
    CHECK(hg::handoff_feed().empty());
}

TEST_CASE("handoff: refuse actually refuses, and says what to do instead") {
    Fresh fresh;

    auto v = hg::check_with("/work/.vscode/tasks.json", "write", Policy::Refuse);
    CHECK_FALSE(v.allowed);
    CHECK(v.is_handoff);
    CHECK(v.kind == Kind::EditorTask);

    // The message is load-bearing, not decoration. A refusal that only says
    // "no" gets retried by the model, and a retry loop against a security gate
    // is worse than either outcome -- so it must name the mechanism AND the
    // alternative. Checking for both means a future reword cannot quietly drop
    // the half that ends the loop.
    CHECK(v.reason.find("refused") != std::string::npos);
    CHECK(v.reason.find("executes") != std::string::npos);
    CHECK(v.reason.find("Sandbox pane") != std::string::npos);
}

TEST_CASE("handoff: warn allows the write but still reports it") {
    Fresh fresh;

    auto v = hg::check_with("/work/.git/config", "edit", Policy::Warn);
    CHECK(v.allowed);          // the write proceeds
    CHECK(v.is_handoff);       // and it is still an event
    CHECK(v.kind == Kind::GitConfig);
    CHECK_FALSE(v.reason.empty());
}

TEST_CASE("handoff: allow is silent in the verdict but NOT in the feed") {
    Fresh fresh;

    auto v = hg::check_with("/work/.envrc", "write", Policy::Allow);
    CHECK(v.allowed);
    CHECK(v.is_handoff);
    CHECK(v.reason.empty());   // nothing to warn about, the user chose this

    // The load-bearing half. A feed that only recorded refusals would go blank
    // exactly when the gate is off, which is when the user most needs to see
    // what the agent is authoring. Recording happens before the policy branch
    // for this reason.
    auto feed = hg::handoff_feed();
    REQUIRE(feed.size() == 1);
    CHECK(feed[0].write.path == "/work/.envrc");
    CHECK(feed[0].kind == Kind::ShellInit);
}

// ── Shape matching, not name matching ────────────────────────────────────

TEST_CASE("handoff: a git dir under another name is still caught") {
    Fresh fresh;

    // Cursor 3.0.0's bug, exactly: "git directories do not have to be called
    // .git". An exact-name table would miss this, which is why
    // is_host_trusted matches on shape.
    for (auto path : {"/work/.git/config",
                      "/work/nested/.git/hooks/pre-commit",
                      "/work/sub/.git/config"}) {
        auto v = hg::check_with(path, "write", Policy::Refuse);
        CHECK_FALSE(v.allowed);
        CHECK(v.is_handoff);
    }
}

TEST_CASE("handoff: the known escape shapes are all refused") {
    Fresh fresh;

    // One per TrustKind that has a concrete CVE or advisory behind it. Named
    // individually rather than looped over the enum so that ADDING a kind
    // without a matching rule fails review here rather than silently shipping
    // an unenforced category.
    struct Case { const char* path; Kind kind; };
    const Case cases[] = {
        {"/work/.vscode/tasks.json",        Kind::EditorTask},
        {"/work/.git/config",               Kind::GitConfig},
        {"/work/.envrc",                    Kind::ShellInit},
    };
    for (const auto& c : cases) {
        auto v = hg::check_with(c.path, "write", Policy::Refuse);
        CHECK_FALSE(v.allowed);
        CHECK(v.kind == c.kind);
    }
}

// ── The feed ─────────────────────────────────────────────────────────────

TEST_CASE("handoff: the same path twice is one entry, not two") {
    Fresh fresh;

    // A handoff is a fact about a FILE, not a sequence of events: the agent
    // rewriting tasks.json three times in a turn is still "the agent authored
    // tasks.json". This differs from the broker's feed on purpose -- there the
    // order is the information, here the path is.
    hg::check_with("/work/.vscode/tasks.json", "write", Policy::Allow);
    hg::check_with("/work/.vscode/tasks.json", "edit", Policy::Allow);
    hg::check_with("/work/.vscode/tasks.json", "write", Policy::Allow);

    auto feed = hg::handoff_feed();
    CHECK(feed.size() == 1);
    // The newest write wins the metadata, so the recorded tool is the last one.
    CHECK(feed[0].write.command == "write");
}

TEST_CASE("handoff: distinct paths are kept apart and bounded") {
    Fresh fresh;

    // Well past the cap, so this checks the bound AND that it drops the oldest
    // rather than refusing new ones -- a feed that fills up and goes deaf would
    // hide the most recent handoff, which is the one being debugged.
    for (int i = 0; i < 80; ++i)
        hg::check_with("/work/sub" + std::to_string(i) + "/.envrc", "write",
                       Policy::Allow);

    auto feed = hg::handoff_feed();
    CHECK(feed.size() <= 32);
    CHECK_FALSE(feed.empty());
    // Newest kept: the last path written must still be present.
    CHECK(feed.back().write.path == "/work/sub79/.envrc");
}

TEST_CASE("handoff: concurrent writers do not corrupt the feed") {
    Fresh fresh;

    // The feed is written from tool worker threads and read from the reducer
    // thread, so it is the one genuinely cross-thread surface here. Same
    // reasoning as the broker's feed: `maya::guarded` makes "forgot the lock"
    // unrepresentable, and this test is what proves the guard is load-bearing
    // rather than decorative -- run under TSan it reports nothing, and reports
    // plenty if the guard is removed.
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([t, &go] {
            while (!go.load()) { /* line them up */ }
            for (int i = 0; i < 100; ++i)
                hg::check_with("/work/w" + std::to_string(t) + "_" +
                                   std::to_string(i) + "/.envrc",
                               "write", Policy::Allow);
        });
    }
    // Read concurrently too: a snapshot taken mid-write is the actual race.
    std::thread reader([&go] {
        while (!go.load()) {}
        for (int i = 0; i < 200; ++i) (void)hg::handoff_feed();
    });

    go.store(true);
    for (auto& w : workers) w.join();
    reader.join();

    auto feed = hg::handoff_feed();
    CHECK(feed.size() <= 32);
    CHECK_FALSE(feed.empty());
    // Every surviving entry is intact: no torn path, no default-constructed
    // record from a partially published push.
    for (const auto& h : feed) {
        CHECK_FALSE(h.write.path.empty());
        CHECK(h.write.at_ms > 0);
    }
}

TEST_CASE("handoff: clearing the feed empties it") {
    Fresh fresh;

    hg::check_with("/work/.envrc", "write", Policy::Allow);
    CHECK_FALSE(hg::handoff_feed().empty());
    hg::clear_handoff_feed();
    CHECK(hg::handoff_feed().empty());
}

TEST_CASE("handoff: a symlinked directory cannot hide a trusted shape") {
    Fresh fresh;

    // is_host_trusted() is a pure path-shape walk, so it only sees the spelling
    // it was handed. That let ONE symlink step around the gate entirely:
    //
    //     ln -s .git/hooks innocuous
    //     write innocuous/post-merge      <- installs a real git hook
    //
    // Measured before the fix: "innocuous/post-merge" read as PLAIN while
    // ".git/hooks/post-merge" read as trusted, and the write reached the hook
    // directory either way. Creating a symlink inside your own workspace is
    // ordinary work, so nothing else stopped it.
    //
    // check() now also tries the RESOLVED path, but only when the spelling
    // looks innocent -- the common case must stay string compares with no I/O.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("agentty_handoff_symlink_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / ".git" / "hooks");
    fs::create_directories(root / "src");

    fs::create_directory_symlink(root / ".git" / "hooks", root / "innocuous", ec);
    if (ec) return;   // no symlink support here; nothing to assert

    // Direct spelling: caught before and after the fix.
    CHECK(hg::check_with((root / ".git" / "hooks" / "post-merge").string(),
                         "write", Policy::Refuse).is_handoff);

    // THE REGRESSION: same destination, spelled through the symlink.
    auto v = hg::check_with((root / "innocuous" / "post-merge").string(),
                           "write", Policy::Refuse);
    CHECK_MESSAGE(v.is_handoff,
                  "a trusted path reached through a symlinked directory must "
                  "still be gated -- otherwise one `ln -s` defeats the gate");
    CHECK_FALSE(v.allowed);

    // And resolution must not start gating ordinary files: a check that fires
    // on everything is as useless as one that fires on nothing.
    CHECK_FALSE(hg::check_with((root / "src" / "main.cpp").string(),
                               "write", Policy::Refuse).is_handoff);
    CHECK_FALSE(hg::check_with((root / "README.md").string(),
                               "write", Policy::Refuse).is_handoff);

    fs::remove_all(root, ec);
}
