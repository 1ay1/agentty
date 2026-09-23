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
