// concurrency_primitives_test — proves util::run_isolated_detached gives
// background work that is OWNED. Three guarantees, and only the first one
// used to hold:
//   a. a body that throws does NOT reach std::terminate,
//   b. the work is WAITED FOR at shutdown, not detached and forgotten,
//   c. the body is handed a stop_token and asked to stop.
// (b) and (c) arrived with maya::pool. The old hand-rolled version was a
// plain std::thread(...).detach() whose comment claimed a "self-joining
// reaper" that did not exist, so there was nothing to join and nothing to
// cancel. The test below would have failed it.
//
// (It also used to test util::RankedLock, the lock-order tripwire. Nothing
// uses ranked locks any more: every shared lock is a maya::guarded, whose
// captureless body cannot take a second lock at all.)

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>


#include "agentty/util/background.hpp"

namespace {

int failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", (msg)); ++failures; } \
        else         { std::fprintf(stderr, "ok:   %s\n", (msg)); }             \
    } while (0)

// ── Background work that is OWNED ───────────────────────────────────────
// One test, one shutdown: the pool's shutdown is terminal (later posts are
// dropped), so isolation and cancellation are asserted against the same
// drain rather than in two passes.
//
// The spin-and-hope the old version ended with ("even if some haven't run,
// the point is none terminated") is exactly what a detached thread forces on
// a test. Here shutdown() JOINS, so the counter is an assertion and not a
// race: if the work were still detached, these counts would be unreliable
// and `stuck` would be meaningless.
struct Counters {
    std::atomic<int>  ran{0};
    std::atomic<bool> saw_stop{false};
};
using counters_t = maya::co_owned<Counters>;

void test_background_work_is_owned() {
    auto c = counters_t::make();

    // (a) throwing bodies must be isolated, every one of them.
    for (int i = 0; i < 8; ++i) {
        agentty::util::run_isolated_detached("test.throwing_worker",
            [](std::stop_token, counters_t c) {
                c->ran.fetch_add(1);
                throw std::logic_error("boom — must not reach std::terminate");
            }, c);
    }

    // (c) a body must actually be stopped through its token.
    agentty::util::run_isolated_detached("test.cancellable_worker",
        [](std::stop_token st, counters_t c) {
            while (!st.stop_requested())
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            c->saw_stop = true;
        }, c);

    // (b) shutdown REQUESTS STOP and WAITS. Returns how many were still
    // stuck at the grace deadline.
    const std::size_t stuck = agentty::util::background_pool().shutdown();

    CHECK(c->ran.load() == 8,
          "every throwing body ran and was waited for (not detached)");
    CHECK(c->saw_stop.load(),
          "a background body is handed a stop_token and is asked to stop");
    CHECK(stuck == 0, "shutdown drained every job within the grace");
    CHECK(true, "8 throwing workers did not terminate the process");
}

} // namespace

int main() {
    test_background_work_is_owned();

    if (failures == 0) {
        std::fprintf(stderr, "\nALL concurrency-primitive checks passed.\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d check(s) FAILED.\n", failures);
    return 1;
}
