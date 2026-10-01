// agentty_tests — the single test binary.
//
// Every unit test in tests/ is compiled as a doctest TEST_CASE and linked into
// THIS one executable, which links the shared agentty object set exactly once.
// The previous model built ~70 separate executables, each statically re-linking
// the whole object set — that link fan-out was the dominant CI cost. doctest
// auto-registers every TEST_CASE, so this file only supplies main().
//
// ctest still runs and filters individual cases: doctest_discover_tests()
// registers each TEST_CASE as its own ctest entry (`ctest -j` parallelism and
// per-case failure reporting are preserved), and `agentty_tests --test-case=X`
// runs one in isolation.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <maya/core/anim_clock.hpp>
#include <jaal/kernel/loop.hpp>   // loop_identity: the harness stands in for the loop

int main(int argc, char** argv) {
    // (No rendering policy is terminal-derived any more — the streaming
    // reveal defaults ON everywhere, tmux included — so the suite behaves
    // identically inside and outside a multiplexer without a gate here.)

    // ── Central user-root isolation ────────────────────────────────
    // Point the ENTIRE binary at a throwaway ~/.agentty before any test
    // runs. Tests write credentials, account registries, threads,
    // settings and caches; without this they mutate the developer's real
    // store. (That is not hypothetical: five credential tests isolated
    // via XDG_CONFIG_HOME silently lost their sandbox when the
    // single-root consolidation moved config_dir() to $AGENTTY_HOME, and
    // the suite began overwriting live credentials + accounts.)
    //
    // Per-process unique so parallel ctest workers never share state.
    // Tests that want their OWN sandbox still setenv AGENTTY_HOME
    // themselves — this is only the default floor. AGENTTY_UNDER_TEST
    // additionally arms the tripwire in util/user_root.cpp, which aborts
    // if anything ever reaches the real root despite this.
    //
    // REMOVED on the way out. This used to leak: one directory per process,
    // and ctest runs this binary once per test case, so a single full suite
    // left ~1150 of them behind. Found the hard way — /tmp is a 16 GB tmpfs
    // here and it hit 100% with 89,373 `agentty_tests_home_*` directories,
    // which then failed an unrelated BUILD with "No space left on device".
    // A test harness must not need a janitor.
    namespace fs = std::filesystem;
    fs::path sandbox;
    {
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        sandbox = fs::temp_directory_path() /
            ("agentty_tests_home_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(sandbox, ec);
#if defined(_WIN32)
        _putenv_s("AGENTTY_HOME", sandbox.string().c_str());
        _putenv_s("AGENTTY_UNDER_TEST", "1");
#else
        ::setenv("AGENTTY_HOME", sandbox.string().c_str(), 1);
        ::setenv("AGENTTY_UNDER_TEST", "1", 1);
#endif
    }
    // Sweep anything an EARLIER run abandoned (a crash or a SIGKILL skips the
    // cleanup below, and those are exactly the runs worth investigating — so
    // this only removes directories older than a day, never a sibling ctest
    // worker's live one).
    //
    // Matches the whole `agentty_*` namespace, not just this binary's own
    // sandbox. That breadth is the point: individual tests mint their own temp
    // dirs (agentty_smarttune_, agentty_acctsw_, agentty_mcp_, … 47 distinct
    // prefixes at last count) and most of them never clean up. Fixing each
    // call site is whack-a-mole and the next test to be written will leak
    // again; sweeping the namespace is one rule that covers all of them,
    // including the ones nobody has written yet.
    //
    // Measured before this existed: 3537 stale directories, on top of the 4956
    // the standalone binary leaked — enough to fill a 16 GB tmpfs and fail an
    // unrelated build with "No space left on device".
    {
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        for (fs::directory_iterator it(fs::temp_directory_path(), ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            // Only directories, and only ours. A FILE named agentty-something
            // is not the harness's to delete, and the prefix is specific
            // enough that nothing else on a dev box collides.
            //
            // Both spellings. Tests use `agentty_` and `agentty-`
            // interchangeably (agentty-subagent-test-, agentty-test-), and a
            // sweep that knows only one silently leaves the other growing --
            // which is exactly how 183 agentty-subagent dirs survived the
            // first version of this.
            if (!name.starts_with("agentty_") &&
                !name.starts_with("agentty-")) continue;
            std::error_code dec;
            if (!fs::is_directory(it->path(), dec) || dec) continue;
            std::error_code sec;
            const auto mt = fs::last_write_time(it->path(), sec);
            if (sec || now - mt < std::chrono::hours(24)) continue;
            std::error_code rec;
            fs::remove_all(it->path(), rec);
        }
    }
    // Pin the animation clock for the whole binary. Several render/seam tests
    // (midrun_*, turn_settle, reveal) drive frames synchronously and assert on
    // committed-scrollback stability; they require maya::anim_now_ms() frozen
    // so render is a pure function of the model instead of racing wall-clock.
    // Harmless for tests that don't read it. Formerly each such test froze it
    // in its own main(); with one shared binary we do it once here.
    maya::testing::freeze_anim_clock();

    // Stand in for the kernel's loop thread, for the same reason the clock is
    // frozen above: these tests drive view() directly instead of running a
    // kernel, and view() legitimately touches loop-bound state (the render
    // caches in agent_timeline.cpp, the frozen-build flag in
    // tool_body_preview.cpp).
    //
    // Without this the render tests abort — which is the mechanism working,
    // not a false positive: off the loop there is no token, and loop_bound
    // aborts rather than silently handing back a second thread's empty copy.
    // Arming the identity here says "this thread IS the loop for this
    // process", which is true: doctest runs the cases on the main thread.
    //
    // It does NOT weaken the guarantee for the thing it protects. A WORKER
    // inside a test still has no token and still aborts, because the identity
    // is per-thread — that is exactly what loop_affinity_test asserts in a
    // forked child.
    const jaal::kernel::loop_identity loop_id;

    const int rc = doctest::Context(argc, argv).run();
    // Best-effort: a leaked sandbox is a slow leak, a failed remove is not
    // worth failing a green suite over.
    if (!sandbox.empty()) {
        std::error_code ec;
        fs::remove_all(sandbox, ec);
    }
    return rc;
}
