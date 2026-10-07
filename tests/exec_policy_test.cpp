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

#include <chrono>
#include <string>
#include <variant>

using namespace std::chrono_literals;
namespace mt = mcp::tools;

namespace {

// Note what this does NOT need: no jaal headers. The capability is stated
// in mcp-cpp's vocabulary and implemented over jaal behind the seam, so a
// caller -- including this test -- never sees the platform layer.
std::shared_ptr<mt::Exec> exec() {
    static auto e = agentty::tools::util::make_unsandboxed_exec();
    return e;
}

mt::ExecResult sh(std::string script, mt::Budgets b) {
    return exec()->run({.program = {"/bin/sh", {"-c", std::move(script)}},
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
    auto r = exec()->run({.program = {"/definitely/not/a/program", {}}});

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
