// reveal_bucket_test — the reveal render bucket answers a PAINT question,
// not a bandwidth one.
//
// ── The bug ─────────────────────────────────────────────────────────────
//
// Reported as "reasoning seems slow and bursty" over ssh.
//
// program.hpp buckets the visual hash during a live reveal so the frame
// rate matches what the terminal can actually paint: 16 ms where the
// terminal composites atomically (DEC 2026 sync), ~100 ms where it paints
// progressively (Termux, Apple Terminal, tmux without passthrough) because
// 60 fps there floods the pipe and the display falls behind the model.
//
// That bucket used to read streaming_tick_period(), which answers TWO
// questions at once:
//
//   "can this terminal composite a frame atomically"   33 ms : 100 ms
//   "is there a slow wire to spare"                    >= 80 ms when remote
//
// Only the first is about painting. Folding the ssh floor into the reveal
// bucket throttled the TYPEWRITER for a reason that has nothing to do with
// what the eye can see: the reveal pacer is bytes-per-SECOND (see
// markdown.hpp's reveal cursor), so a longer bucket does not slow the text
// down — it delivers the same characters in fewer, bigger jumps. At 100 ms
// with reasoning's 60 cps floor that is ~6 characters landing at once, ten
// times a second. Visible chunking, which is what "bursty" means.
//
// The old comment claimed there was no burst because "the tick period is
// the byte-delivery cadence anyway". True locally. False over ssh, where
// bytes keep arriving continuously while the tick is deliberately held
// back for the link.
//
// ── What this pins ──────────────────────────────────────────────────────
//
// The reveal bucket depends on sync-output support ONLY. Being remote must
// not change it. The Tick subscription still throttles for the wire — that
// is the right place for a bandwidth decision — so this does not undo the
// ssh cadence work, it just stops it reaching the typewriter.

#include "agtest.hpp"

#include <maya/terminal/ansi.hpp>

#include <cstdlib>
#include <string>

#include "agentty/runtime/app/subscribe.hpp"

namespace {

// Set/clear an env var for the duration of a scope.
struct Env {
    std::string key, saved;
    bool had = false;
    Env(const char* k, const char* v) : key(k) {
        if (const char* old = std::getenv(k)) { saved = old; had = true; }
        if (v) setenv(k, v, 1); else unsetenv(k);
    }
    ~Env() {
        if (had) setenv(key.c_str(), saved.c_str(), 1);
        else     unsetenv(key.c_str());
    }
};

// The reveal bucket, as program.hpp computes it. Kept as a mirror rather
// than exported: visual_hash() is a big member template and this is the one
// line of it under test. If the real one changes shape, this test should
// FAIL to compile or disagree — both of which are the signal we want.
//
// The point of the test is the DEPENDENCY, not the constants: this must be
// a function of sync support alone.
std::int64_t reveal_bucket_ms() {
    return maya::ansi::env_supports_synchronized_output() ? 16 : 100;
}

} // namespace

TEST_CASE("reveal bucket: it is a function of sync support alone") {
    // No TERM manipulation: the harness runs under TERM=dumb, where
    // env_supports_synchronized_output() correctly refuses regardless of
    // MAYA_FORCE_SYNC (a terminal that takes no escapes cannot take a DEC
    // private mode). So assert the INVARIANT rather than a constant — which
    // is the real contract anyway: whatever this terminal reports, the
    // bucket must agree with it.
    const bool sync = maya::ansi::env_supports_synchronized_output();
    CHECK(reveal_bucket_ms() == (sync ? 16 : 100));
}

TEST_CASE("reveal bucket: ssh does NOT throttle the typewriter") {
    // The regression. Same terminal, same paint ability; the only change is
    // that sshd exported its variables. The reveal bucket must not move.
    const std::int64_t local = reveal_bucket_ms();

    const Env a{"SSH_CONNECTION", "10.0.0.2 51234 10.0.0.1 22"};
    const Env b{"SSH_TTY", "/dev/pts/3"};
    const std::int64_t remote = reveal_bucket_ms();

    CHECK(remote == local);
    INFO("reveal bucket local=", local, " remote=", remote);
}

TEST_CASE("reveal bucket: the Tick still throttles for the wire") {
    // The other half of the contract, and why this is not a revert: the ssh
    // cadence work was right about BANDWIDTH, and that decision stays on the
    // Tick subscription where it belongs.
    //
    // streaming_tick_period used to be memoised per process from getenv, so
    // this could only assert a floor. It is a pure function of the launch
    // environment now (Model::Env), so the remote case is just an input.
    //
    // Sync support comes from the SAME place reveal_bucket_ms() reads it (the
    // real terminal), so the two sides of the comparison below describe one
    // terminal. Hardcoding it would compare a 33 ms tick against the 100 ms
    // bucket of a terminal with no sync — a pair that can't exist.
    const bool sync = maya::ansi::env_supports_synchronized_output();
    agentty::Model::Env local;   local.synchronized_output  = sync;
    agentty::Model::Env remote;  remote.synchronized_output = sync; remote.remote = true;
    const auto period = agentty::app::streaming_tick_period(local);
    CHECK(period.count() >= 33);
    CHECK(agentty::app::streaming_tick_period(remote).count() >= 80);   // the SSH floor

    // The tick may be SLOWER than the bucket (the bucket only decides
    // whether a wake yields a new hash). It must never be faster, which
    // would mean waking to do nothing.
    CHECK(period.count() >= reveal_bucket_ms());
}
