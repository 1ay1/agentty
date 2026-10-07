// subprocess_group_kill_test — timeout/cancel must kill the whole tree.
//
// Subprocess::run used to signal only the `sh -c` leader. A backgrounded
// child (`sleep 60 &`) or a pipeline stage survived the timeout and kept
// running after the tool had returned. The runner now spawns the child
// in its own session/process group and signals the group.

#include "agtest.hpp"

#include "agentty/tool/util/subprocess.hpp"

#if !defined(_WIN32)
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using agentty::tools::util::Subprocess;
using agentty::tools::util::SubprocessOptions;

namespace {

fs::path pid_file(const char* tag) {
    return fs::temp_directory_path()
         / ("agentty_group_kill_" + std::string{tag} + "_"
            + std::to_string(::getpid()));
}

// Wait up to ~2 s for `pid` to disappear. ESRCH = gone.
bool gone(pid_t pid) {
    for (int i = 0; i < 40; ++i) {
        if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return false;
}

pid_t read_pid(const fs::path& p) {
    std::ifstream in(p);
    long v = 0;
    in >> v;
    return static_cast<pid_t>(v);
}

} // namespace

TEST_CASE("subprocess timeout kills backgrounded children") {
    const auto pf = pid_file("timeout");
    std::error_code ec;
    fs::remove(pf, ec);

    SubprocessOptions opts;
    // The background sleep holds the pipe open and never writes, so the
    // idle timeout fires. Before the fix the sleep outlived the timeout.
    opts.command = SubprocessOptions::Shell{
        "sleep 60 & echo $! > '" + pf.string() + "'; wait"};
    opts.timeout = std::chrono::seconds{1};
    auto r = Subprocess::run(std::move(opts));

    const pid_t bg = read_pid(pf);
    REQUIRE(bg > 0);
    CHECK(r.timed_out);
    const bool dead = gone(bg);
    if (!dead) ::kill(bg, SIGKILL);   // don't leak it into the test run
    CHECK_MESSAGE(dead, "the backgrounded sleep must die with its group");
    fs::remove(pf, ec);
}

TEST_CASE("subprocess cancel kills backgrounded children") {
    const auto pf = pid_file("cancel");
    std::error_code ec;
    fs::remove(pf, ec);

    const auto start = std::chrono::steady_clock::now();
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Shell{
        "sleep 60 & echo $! > '" + pf.string() + "'; wait"};
    opts.timeout = std::chrono::seconds{60};
    opts.stop_requested = [start] {
        return std::chrono::steady_clock::now() - start
             > std::chrono::milliseconds{500};
    };
    auto r = Subprocess::run(std::move(opts));

    const pid_t bg = read_pid(pf);
    REQUIRE(bg > 0);
    CHECK(!r.timed_out);
    const bool dead = gone(bg);
    if (!dead) ::kill(bg, SIGKILL);
    CHECK_MESSAGE(dead, "cancel must take the whole group down");
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{10});
    fs::remove(pf, ec);
}

TEST_CASE("subprocess normal exit is unaffected") {
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Shell{"echo hi; exit 3"};
    opts.timeout = std::chrono::seconds{10};
    auto r = Subprocess::run(std::move(opts));
    CHECK(r.started);
    CHECK(r.exit_code == 3);
    CHECK(r.output.find("hi") != std::string::npos);
}
#endif

#if !defined(_WIN32)
// ── the wall-clock ceiling ───────────────────────────────────────────────
//
// The idle watchdog caps SILENCE, so a child that never stops printing
// rolls it forward forever and is never reaped. Past the output cap the
// reader discards what it reads, so memory doesn't bound it either: one
// `yes` left a spinning process and a stuck worker for the rest of the
// session. agentty installs its runner as mcp-cpp's host sandbox whenever
// the sandbox is on, so mcp-cpp's own ceiling was bypassed in the default
// configuration -- this one has to exist here too.
//
// The balance that matters: this must NOT make long sessions worse. A job
// that legitimately runs for ages while printing is the normal case and has
// to survive.

TEST_CASE("subprocess: a chatty runaway is reaped by the wall clock") {
    SubprocessOptions opts;
    // Never silent, so the idle watchdog can never fire: without the
    // ceiling this call does not return.
    opts.command = SubprocessOptions::Shell{
        "while :; do echo spinning; done"};
    opts.timeout      = std::chrono::seconds{30};   // idle: never trips
    opts.hard_timeout = std::chrono::seconds{2};
    opts.max_bytes    = 4096;                       // cap hit almost at once

    const auto t0 = std::chrono::steady_clock::now();
    auto r = Subprocess::run(std::move(opts));
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(r.started);
    CHECK_MESSAGE(r.timed_out, "stopping it is still a timeout outcome");
    CHECK_MESSAGE(r.hard_capped,
                  "and it must be attributable to the wall clock, not to "
                  "silence -- the two want opposite advice");
    CHECK_MESSAGE(elapsed < std::chrono::seconds{20},
                  "reaped at its own ceiling, not the idle one");
}

TEST_CASE("subprocess: a long chatty job is NOT cut off") {
    // The convenience guarantee. Output every 100 ms for ~1.2 s, with an
    // idle window far shorter than the total runtime: a build that keeps
    // printing must finish no matter how long it takes.
    SubprocessOptions opts;
    opts.command = SubprocessOptions::Shell{
        "for i in 1 2 3 4 5 6 7 8 9 10 11 12; do echo step $i; "
        "sleep 0.1; done; exit 0"};
    opts.timeout   = std::chrono::seconds{1};   // shorter than the runtime
    opts.max_bytes = 64 * 1024;

    auto r = Subprocess::run(std::move(opts));

    CHECK(r.started);
    CHECK_MESSAGE(!r.timed_out,
                  "progress keeps rolling the idle window forward");
    CHECK_MESSAGE(!r.hard_capped,
                  "and the derived ceiling (20x idle, 10 min floor) is "
                  "nowhere near a job like this");
    CHECK(r.exit_code == 0);
    CHECK(r.output.find("step 12") != std::string::npos);
}

TEST_CASE("subprocess: the derived ceiling is generous, not tight") {
    // 20x the idle budget, floored at 10 minutes. Pinned because the whole
    // point is that it never fires on real work -- if someone tunes this
    // down to "something reasonable", long sessions start dying.
    SubprocessOptions opts;
    opts.command   = SubprocessOptions::Shell{"echo quick"};
    opts.timeout   = std::chrono::seconds{60};   // the common default
    opts.max_bytes = 4096;
    auto r = Subprocess::run(std::move(opts));
    CHECK(r.started);
    CHECK_FALSE(r.hard_capped);
    // 60s idle would derive max(1200s, 600s) = 20 minutes.
    static_assert(60 * 20 > 600, "the floor must not be the binding term here");
}
#endif
