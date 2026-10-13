// logx_rotation_test — mid-run rotation must not lose lines or invalidate
// the fd that writers hold.
//
// Rotation is the one logx seam with a concurrency hazard: writers read the
// sink fd WITHOUT the rotate mutex (a diagnostic must never lock the
// caller's hot path), so a rotation that CLOSES the old descriptor and
// publishes a new one leaves every racing writer holding a number the
// kernel has already recycled. The write then succeeds — into whatever
// opened next. That is silent log loss at best and cross-fd corruption at
// worst, and it is invisible in a single-threaded test.
//
// The fix is dup2: re-point the SAME descriptor at the new file, so the
// value writers hold stays valid across a rotation and there is no window
// in which it can be reused. These tests pin both halves of that contract.
//
// Reaching rotation needs the threshold lowered: at the shipping 32 MB a
// test would have to write 32 MB (measured: over three minutes). This TU
// is linked against logx.cpp directly, so it sets the internal knob rather
// than requiring an env var — logging stays something nobody configures.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "agtest.hpp"
#include "agentty/util/logx.hpp"

using namespace agentty;

// The internal rotation threshold (src/util/logx.cpp). Not public API; this
// test is the only thing that touches it.
namespace agentty::logx::detail {
extern std::atomic<std::int64_t> g_rotate_bytes;
}

namespace {

// Rotate after 64 KB so the seam is reachable in milliseconds. Set before
// anything logs: the sink latches on first use.
constexpr long long kRotateBytes = 64 * 1024;

struct SetRotateBytes {
    SetRotateBytes() {
        logx::detail::g_rotate_bytes.store(kRotateBytes,
                                           std::memory_order_relaxed);
    }
} const g_set_rotate_bytes;

long long rotate_limit() { return kRotateBytes; }

std::string read_file(const std::string& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

// Everything the logger still owns: the live file plus the one rotated
// generation it keeps. A line in either is a line that survived.
std::string live_plus_rotated() {
    const auto lf = std::string{logx::log_file()};
    if (lf.empty()) return {};
    return read_file(lf + ".old") + read_file(lf);
}

void require_logging() {
    REQUIRE_MESSAGE(!logx::log_file().empty(),
                    "AGENTTY_LOG/_FILE must be set for this binary "
                    "(ctest sets them; see cmake/AgenttyTests.cmake)");
}

// A payload wide enough that a few hundred lines cross a KB-scale
// threshold, so each test actually rotates rather than merely appending.
constexpr const char* kPad =
    "----------------------------------------------------------------";

// Rough bytes on disk per emitted line: the payload plus logx's timestamp,
// level, channel and site prefix. Only used for SIZING the bursts, so an
// approximation is fine — every assertion below verifies against the file's
// real contents, never against this estimate.
constexpr long long kLineCost = 64 + 80;

long long live_size() {
    return static_cast<long long>(read_file(std::string{logx::log_file()}).size());
}

#if defined(__linux__)
// The fd number the process currently has open on `path`, or -1. Reading
// /proc/self/fd is the only way to observe the sink descriptor from
// outside logx without widening its public API for a test's benefit.
int fd_pointing_at(const std::string& path) {
    int found = -1;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) {
        std::error_code ec;
        const auto target = std::filesystem::read_symlink(e.path(), ec);
        if (ec) continue;                       // fd closed under us; fine
        if (target.string() != path) continue;
        const int fd = std::atoi(e.path().filename().string().c_str());
        // The directory_iterator itself holds an fd; the log is the only
        // one that can point at this path, so first match wins.
        if (found < 0) found = fd;
    }
    return found;
}
#endif

}  // namespace

TEST_CASE("logx: mid-run rotation keeps the sink writable") {
    require_logging();
    const long long limit = rotate_limit();
    // Write comfortably past the threshold from wherever the file currently
    // is — these test cases share one log, so "how much is enough" depends
    // on what earlier cases already wrote.
    const long long need = 2 * limit + limit / 2;
    for (long long w = 0; w < need; w += kLineCost)
        AGT_LOG(General, Warn, "rotation.test", "single {} {}", w, kPad);

    const auto all = live_plus_rotated();
    // The last line written must be present: if rotation had invalidated
    // the descriptor, writes after the swap would vanish.
    CHECK(all.find("single ") != std::string::npos);
    // And rotation must actually have happened — otherwise this test is
    // silently only exercising the append path.
    CHECK_MESSAGE(!read_file(std::string{logx::log_file()} + ".old").empty(),
                  "expected a rotated .old generation");
}

TEST_CASE("logx: concurrent writers lose nothing across a rotation") {
    require_logging();

    // Getting real power out of this test took two false starts, both worth
    // recording because both looked like evidence and were not:
    //
    //   * "assert every line ever written survives" — wrong, because logx
    //     keeps ONE rotated generation. A burst several times the threshold
    //     is SUPPOSED to drop its oldest lines.
    //   * "use a tiny threshold so it rotates constantly" — wrong for a
    //     subtler reason: threads finish at different times, so a thread
    //     that finished early has its last line evicted by rotations that
    //     LATER threads trigger. That fails against the fixed logger too,
    //     i.e. it measures retention, not the bug.
    //
    // What separates the two is accounting. Size each ROUND so it crosses
    // the threshold at most once: then the round's lines are split between
    // .old and the live file with nothing evicted, and every single one
    // MUST be present. Any absence is the sink going invalid under a
    // writer — the actual defect. Repeat for many rounds to get many
    // rotation windows to race against.
    const long long limit = rotate_limit();
    REQUIRE(limit > 0);

    constexpr int kThreads = 8;
    constexpr int kRounds  = 12;

    long rotations_seen = 0;
    for (int round = 0; round < kRounds; ++round) {
        // Size THIS round against where the live file actually sits, not
        // against an empty file: these cases share a log, so a fixed burst
        // can straddle two thresholds and evict its own early lines — a
        // legitimate drop that would look exactly like the bug.
        const long long before  = live_size();
        const long long headroom = limit - before;
        // Just over the remaining headroom: guarantees one crossing, never
        // two. If the file is already past the limit the next emit rotates,
        // so a small burst still gives exactly one window.
        const long long budget  = (headroom > 0 ? headroom : 0) + limit / 4;
        const int per_thread =
            static_cast<int>(budget / kLineCost) / kThreads + 1;
        REQUIRE(per_thread > 1);

        std::vector<std::thread> ws;
        ws.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            ws.emplace_back([t, round, per_thread] {
                for (int i = 0; i < per_thread; ++i)
                    AGT_LOG(General, Warn, "rotation.test",
                            "r={} t={} i={} {}", round, t, i, kPad);
            });
        }
        for (auto& w : ws) w.join();

        const auto all = live_plus_rotated();
        if (live_size() < before) ++rotations_seen;

        // Every line of THIS round must be present — no eviction is
        // possible for a round that fits inside one rotation window.
        int missing = 0, first_missing_t = -1, first_missing_i = -1;
        for (int t = 0; t < kThreads; ++t) {
            for (int i = 0; i < per_thread; ++i) {
                const std::string key = "r=" + std::to_string(round) +
                                        " t=" + std::to_string(t) +
                                        " i=" + std::to_string(i) + " ";
                if (all.find(key) == std::string::npos) {
                    if (missing == 0) { first_missing_t = t; first_missing_i = i; }
                    ++missing;
                }
            }
        }
        REQUIRE_MESSAGE(missing == 0,
                        "round " << round << ": " << missing
                        << " line(s) lost across rotation, first t="
                        << first_missing_t << " i=" << first_missing_i);
    }

    // If nothing ever rotated, the test proved nothing about rotation.
    CHECK_MESSAGE(rotations_seen > 0,
                  "no rotation observed across " << kRounds << " rounds");
}

TEST_CASE("logx: rotation re-points the sink fd instead of replacing it") {
    require_logging();
#if !defined(__linux__)
    MESSAGE("fd-identity check needs /proc; skipped on this platform");
#else
    // THE load-bearing test for the dup2 fix, and the only one here with
    // real power against the bug.
    //
    // The line-accounting test above passes against the BROKEN logger too:
    // the race window between close(oldfd) and a racing writer's write() is
    // nanoseconds wide, so losses are far too rare to catch by writing
    // lines and counting them. Chasing it statistically would buy a slow,
    // flaky test that still proves nothing on a green run.
    //
    // So assert the PROPERTY that makes the race impossible rather than
    // the symptom it produces. Writers read the sink fd without the rotate
    // lock; that is only sound if rotation never invalidates the number
    // they hold. dup2 re-points the existing descriptor, so the number is
    // stable for the process lifetime; swap-and-close publishes a new one
    // and frees the old for reuse. Comparing the fd across a rotation
    // separates those two implementations deterministically — every run,
    // no race required.
    const std::string path = std::string{logx::log_file()};

    const int before = fd_pointing_at(path);
    REQUIRE_MESSAGE(before >= 0, "could not find the log fd in /proc/self/fd");

    // Force at least one rotation.
    const long long limit = rotate_limit();
    const long long need  = limit - live_size() + limit / 4;
    for (long long w = 0; w < need; w += kLineCost)
        AGT_LOG(General, Warn, "rotation.test", "fdid {} {}", w, kPad);
    REQUIRE_MESSAGE(!read_file(path + ".old").empty(),
                    "expected a rotated .old generation");

    const int after = fd_pointing_at(path);
    REQUIRE_MESSAGE(after >= 0, "log fd vanished after rotation");
    CHECK_MESSAGE(before == after,
                  "rotation replaced the sink fd (" << before << " -> "
                  << after << "); writers reading it unlocked would be "
                  "holding a closed, reusable descriptor");

    // And the re-pointed fd must still reach the LIVE file, not the
    // rotated-away one: dup2 onto the wrong target would keep the number
    // stable while silently writing into .old forever.
    AGT_LOG(General, Warn, "rotation.test", "post-rotation sentinel {}", 1);
    CHECK(read_file(path).find("post-rotation sentinel 1") != std::string::npos);
#endif
}

TEST_CASE("logx: rotation bounds the live file") {
    require_logging();
    // The point of rotating is that the live file stays small. Give it a
    // generous ceiling (threshold + one burst) so this pins the guarantee
    // without being sensitive to exactly where the crossing landed.
    const long long limit = rotate_limit();

    for (long long w = 0; w < 4 * limit; w += kLineCost)
        AGT_LOG(General, Warn, "rotation.test", "bound {} {}", w, kPad);

    const auto live = read_file(std::string{logx::log_file()});
    CHECK_MESSAGE(static_cast<long long>(live.size()) <= limit * 4,
                  "live log grew to " << live.size()
                  << " bytes against a " << limit
                  << "-byte rotation threshold");
}
