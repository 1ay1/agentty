// exec_policy_test — the two clocks, pinned.
//
// ── WHY ──────────────────────────────────────────────────────────────────
//
// This session began with a command that ran forever. The timeout was an
// IDLE watchdog, reset by every byte, so anything that kept printing kept
// itself alive; past the output cap the reader discarded what it read, so
// memory didn't bound it either. One `yes` left a spinning process and a
// stuck worker for the rest of the session.
//
// The fix is not a bigger number. Silence and total runtime are different
// questions and need separate clocks:
//
//   idle  reset by output. Catches a HANG. Cannot catch a chatty runaway,
//         because output is precisely what keeps it alive.
//   wall  never reset. Catches the runaway. Must be generous, or it fires
//         on the long builds that are the normal case.
//
// Both, or neither works. That is the property these checks exist to keep,
// because it is invisible from reading either clock alone, and the obvious
// "simplification" — one timeout — silently reinstates the bug.
//
// The other half is honesty about WHICH clock fired. "Timed out" sends you
// hunting for a hang when the command was fine and just needed longer, so
// the outcome names the budget and the two get opposite advice.
#include "agtest.hpp"

#include "agentty/tool/util/exec.hpp"
#include "agentty/tool/util/sandbox.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/registry.hpp"

#include <nlohmann/json.hpp>
#include <csignal>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>

#include <filesystem>

#include <chrono>
#include <string>
#include <thread>
#include <variant>

using namespace std::chrono_literals;
namespace mt = mcp::tools;
using json = nlohmann::json;

namespace {

// Note what this does NOT need: no jaal headers. The capability is stated
// in mcp-cpp's vocabulary and implemented over jaal behind the seam, so a
// caller -- including this test -- never sees the platform layer.
std::shared_ptr<mt::Exec> exec() {
    static auto e = agentty::tools::util::make_exec();
    return e;
}

mt::ExecResult sh(std::string script, mt::Budgets b) {
    return exec()->run(mt::Call{}, {.program = {"/bin/sh", {"-c", std::move(script)}},
                        .budgets = b,
                        .max_output_bytes = 64u * 1024u});
}

bool timed_out_on(const mt::ExecResult& r, mt::TimedOut::budget which) {
    const auto* t = std::get_if<mt::TimedOut>(&r.outcome);
    return t && t->which == which;
}

} // namespace

TEST_CASE("exec: a chatty runaway is reaped by the wall clock") {
    // THE bug. Output resets the idle clock forever, so only an absolute
    // ceiling can end this. Without one the call never returns.
    const auto t0 = std::chrono::steady_clock::now();
    auto r = sh("while :; do echo spin; done", {.idle = 30s, .wall = 2s});
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK_MESSAGE(timed_out_on(r, mt::TimedOut::budget::wall),
                  "a runaway that never goes quiet can only be caught by the "
                  "clock that output does not reset");
    CHECK_MESSAGE(elapsed < 15s, "reaped at its own ceiling, not the idle one");
    CHECK_MESSAGE(r.truncated,
                  "it blew the output cap -- and the cap must not be what "
                  "stops the reaping");
}

TEST_CASE("exec: a long chatty job is NOT cut off") {
    // The convenience guarantee, and the reason the idle clock is the
    // default rather than a wall clock. Runtime here exceeds the idle
    // budget several times over; progress is what keeps it alive, and that
    // is correct.
    auto r = sh("for i in 1 2 3 4 5 6; do echo step$i; sleep 0.2; done",
                {.idle = 1s, .wall = 60s});

    CHECK_MESSAGE(r.ok(), "progress keeps rolling the idle window forward");
    CHECK_MESSAGE(r.output.find("step6") != std::string::npos,
                  "and every byte it printed survived");
}

TEST_CASE("exec: a silent hang is caught by the idle clock") {
    auto r = sh("sleep 30", {.idle = 1s, .wall = 60s});
    CHECK(timed_out_on(r, mt::TimedOut::budget::idle));
    CHECK_FALSE(timed_out_on(r, mt::TimedOut::budget::wall));
}

TEST_CASE("exec: which budget fired is part of the answer") {
    // The two want opposite advice -- "it hung, go look at why" versus "it
    // was working fine, give it longer or run it in the background". A
    // single timed_out flag forces the caller to guess, and it guesses
    // wrong half the time.
    auto hung    = sh("sleep 30", {.idle = 1s, .wall = 60s});
    auto runaway = sh("while :; do echo x; done", {.idle = 30s, .wall = 2s});

    const auto* a = std::get_if<mt::TimedOut>(&hung.outcome);
    const auto* b = std::get_if<mt::TimedOut>(&runaway.outcome);
    REQUIRE(a);
    REQUIRE(b);
    CHECK_MESSAGE(a->which != b->which,
                  "two different failures must not report as one");
}

TEST_CASE("exec: output printed before the exit survives it") {
    // The exit says the bytes are finished, not that we have them: what was
    // written before it is still in the pipe. jaal's conformance suite pins
    // this for the platform; this pins that the policy layer honours it.
    auto r = sh("printf 'last-bytes'; exit 3", {.idle = 5s, .wall = 30s});

    const auto* e = std::get_if<mt::Exited>(&r.outcome);
    REQUIRE(e);
    CHECK(e->code == 3);
    CHECK(r.output == "last-bytes");
    CHECK_FALSE(r.ok());
}

TEST_CASE("exec: a program that never ran is not a program that failed") {
    // The distinction a bare exit code cannot express, and the reason the
    // outcome is a sum. "127" could be the shell's not-found or the
    // program's own choice; StartFailed cannot be either.
    auto r = exec()->run(mt::Call{}, {.program = {"/definitely/not/a/program", {}}});

    CHECK(std::holds_alternative<mt::StartFailed>(r.outcome));
    CHECK_FALSE(r.ok());
    const auto* f = std::get_if<mt::StartFailed>(&r.outcome);
    REQUIRE(f);
    CHECK_MESSAGE(!f->reason.empty(), "and it says why");
}

TEST_CASE("exec: a clean command reports no timeout at all") {
    // The negative case, so a bug that reports every run as timed-out does
    // not hide behind the checks above.
    auto r = sh("printf 'ok'", {.idle = 5s, .wall = 30s});
    CHECK(r.ok());
    CHECK_FALSE(std::holds_alternative<mt::TimedOut>(r.outcome));
    CHECK_FALSE(r.truncated);
    CHECK(r.output == "ok");
}

// ── the sandboxed path ──────────────────────────────────────────────────
//
// The checks above exercise the direct spawn. This one exercises the other
// way in: claybin clones with namespace flags, installs a seccomp filter and
// enters a cgroup, then jaal ADOPTS the pid and pidfd it hands back. After
// that line both paths are the same object, so what needs proving is that
// the hand-off works at all -- and that the policy still applies on top of
// it, since a sandbox you cannot observe the exit of would have been reason
// to keep a second loop.
//
// Skips where no sandbox can be built, because a check that silently passes
// on an unconfined host is worse than no check.
TEST_CASE("exec: the sandboxed path spawns, is watched, and still obeys the clocks") {
    namespace sb = agentty::tools::util::sandbox;
    if (!sb::init(sb::Mode::On) || !sb::is_active()) return;

    auto e = agentty::tools::util::make_exec();

    // 1. it runs at all -- the adopt() hand-off works.
    auto ran = e->run(mt::Call{}, {.program = {"/bin/sh", {"-c", "echo inside"}},
                       .budgets = {.idle = 10s, .wall = 30s}});
    CHECK_MESSAGE(ran.ok(), "a sandboxed command still runs and is reaped");
    CHECK(ran.output.find("inside") != std::string::npos);

    // 2. the exit is observable -- this is the bit adopt() exists for. A
    //    pidfd we could not watch would show up as a wall-clock timeout
    //    instead of a clean exit, so ok() above already proves it; assert it
    //    directly too, because that inference is not obvious.
    CHECK(std::holds_alternative<mt::Exited>(ran.outcome));

    // 3. the wall clock still reaps a runaway INSIDE the sandbox. The idle
    //    clock cannot (output keeps resetting it), and a sandboxed child is
    //    exactly where an unreaped runaway would be hardest to notice.
    const auto t0 = std::chrono::steady_clock::now();
    auto runaway = e->run(mt::Call{}, {.program = {"/bin/sh", {"-c", "while :; do echo s; done"}},
                           .budgets = {.idle = 30s, .wall = 2s},
                           .max_output_bytes = 32u * 1024u});
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(timed_out_on(runaway, mt::TimedOut::budget::wall));
    CHECK_MESSAGE(elapsed < 15s, "reaped at its ceiling, not left spinning");
}

// ── a session: started here, polled later ───────────────────────────────
//
// process_start/poll/stop used to own a thread per session and spawn
// through a different code path from every other tool — which after the
// bwrap removal meant a background process was confined by nothing while a
// foreground one was confined by claybin. Now it goes through the same
// capability, so there is one boundary and one place that knows how to
// watch a child.
//
// Driven through the real registry rather than the capability directly:
// what matters is that the three tools still compose, since the lifetime
// spans three separate calls.
TEST_CASE("process session: start, poll, stop through the registry") {
    auto start = agentty::tool::DynamicDispatch::execute(
        "process_start",
        json{{"command", "for i in 1 2 3; do echo tick$i; sleep 0.15; done; echo done"}});
    if (!start) MESSAGE("process_start failed: " << start.error().detail);
    REQUIRE(start.has_value());

    const auto at = start->text.find("proc-");
    REQUIRE(at != std::string::npos);
    std::string id = start->text.substr(at);
    id = id.substr(0, id.find_first_of(" )\n"));

    // Poll until it ends or we run out of patience. The session keeps
    // producing for ~450ms, so a few polls should see output and then the end.
    std::string seen = start->text;
    bool ended = false;
    for (int i = 0; i < 40 && !ended; ++i) {
        auto p = agentty::tool::DynamicDispatch::execute(
            "process_poll", json{{"id", id}, {"wait_ms", 100}});
        REQUIRE(p.has_value());
        seen += p->text;
        if (p->text.find("exited") != std::string::npos
            || p->text.find("not running") != std::string::npos) ended = true;
    }

    CHECK_MESSAGE(seen.find("tick1") != std::string::npos,
                  "output produced between polls must survive to the next one "
                  "-- the host drains continuously so a full pipe never "
                  "stalls the child");
    CHECK_MESSAGE(seen.find("done") != std::string::npos,
                  "and the tail written just before exit is not lost");

    // Stopping an already-finished session is not an error.
    auto stop = agentty::tool::DynamicDispatch::execute(
        "process_stop", json{{"id", id}});
    CHECK(stop.has_value());
}

// A spawned tool must not inherit the host's file descriptors.
//
// Access rights attach to the open file DESCRIPTION, not to the path, so a
// descriptor the host holds open survives exec and voids the sandbox's file
// policy: the child can read a file it was denied, even one that no longer
// has a path. agentty holds credential stores and logs open while it runs
// tools, so the exec has to close everything but the child's own three.
// (Moved from mcp-cpp's fd_leak_test when spawning moved to agentty.)
#if defined(__linux__)
TEST_CASE("exec: a child cannot read the host's open descriptors") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("fd_leak_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const auto secret = dir / "secret.txt";
    {
        std::FILE* f = std::fopen(secret.c_str(), "w");
        REQUIRE(f);
        std::fputs("FLAG{fd-inherited}\n", f);
        std::fclose(f);
    }
    // Opened before the spawn and deliberately without O_CLOEXEC, standing
    // in for a long-lived host fd we don't control.
    const int fd = ::open(secret.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    const std::string peek = "cat /proc/self/fd/" + std::to_string(fd) + " 2>/dev/null; true";

    auto r = sh(peek, mt::Budgets{10s, 10s});
    CHECK_MESSAGE(r.output.find("FLAG{fd-inherited}") == std::string::npos,
                  "child read an inherited fd");

    // The sharp case: unlinked, so no path rule can name it.
    fs::remove(secret);
    auto r2 = sh(peek, mt::Budgets{10s, 10s});
    CHECK_MESSAGE(r2.output.find("FLAG{fd-inherited}") == std::string::npos,
                  "child read an UNLINKED file through an inherited fd");

    // The child's own descriptors still work: stdout reaches us and stdin
    // is at EOF rather than hanging (a sweep that closed 0/1/2 would pass
    // the checks above by breaking every tool).
    auto r3 = sh("echo alive; cat; echo done", mt::Budgets{10s, 10s});
    CHECK(r3.output.find("alive") != std::string::npos);
    CHECK(r3.output.find("done")  != std::string::npos);

    ::close(fd);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

// A running tool's output reaches the UI as it is produced, and Esc stops
// it. Both travel in the call's CallContext; exec has to honour them. From
// Oct 7 until this test existed it did neither: shell output arrived only
// at the end, and Esc waited for the command to finish on its own.
TEST_CASE("exec: a tool call streams progress and stops on cancel") {
    using agentty::tool::DynamicDispatch;
    int calls = 0;
    {
        agentty::tools::CallContext ctx;
        ctx.progress = [&](std::string_view) { ++calls; };
        auto r = DynamicDispatch::execute(
            "shell", json{{"command", "for i in 1 2 3; do echo tick$i; sleep 0.2; done"}}, ctx);
        CHECK(r.has_value());
    }
    CHECK_MESSAGE(calls >= 3, "output streamed while the command ran");

    std::stop_source src;
    std::jthread trip([&] { std::this_thread::sleep_for(300ms); src.request_stop(); });
    const auto t0 = std::chrono::steady_clock::now();
    {
        agentty::tools::CallContext ctx;
        ctx.cancel.push_back(src.get_token());
        (void)DynamicDispatch::execute("shell", json{{"command", "sleep 20; echo done"}}, ctx);
    }
    CHECK_MESSAGE(std::chrono::steady_clock::now() - t0 < 5s, "Esc stopped the command");
}
