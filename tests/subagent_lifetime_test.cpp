// subagent_lifetime_test — the provider seam must survive an ABANDONED worker.
//
// THE BUG THIS PINS. The long-lived providers (Anthropic, ChatGPT, Copilot,
// Kimi) used to be stack objects in main(), and the stream seam captured them
// by reference. The justification was "main outlives maya::run", which is
// true for every thread the runtime waits for.
//
// Subagents are exactly the threads it does not wait for. A `task` runs on a
// Cmd::task_isolated thread, and jaal DETACHES those — its kernel pool header
// says isolated threads are "asked to stop but never waited for". Shutdown
// now asks (run_tool reads its stop_token) and waits, but the wait is
// BOUNDED on purpose: a thread wedged in a syscall must not hold the process
// open. So abandonment is a designed outcome, and with a by-reference capture
// an abandoned worker called into freed stack memory while main unwound.
//
// It only faulted when a subagent happened to be mid-stream at quit — which
// is precisely when a user gives up on a slow one and presses escape. That
// is why it read as "subagents crash sometimes".
//
// The fix is ownership, not timing: the seam holds shared_ptrs, so the last
// user out destroys the provider and an abandoned worker keeps it alive for
// as long as it is still touching it. No grace period to tune, nothing to
// race.
//
// Run under ASan (this is a `sanitizer`-labelled lane) so the BEFORE shape
// would be a hard stack-use-after-scope rather than a probabilistic crash.
// Verified: the by-reference version of this same harness aborts here with
// "AddressSanitizer: stack-use-after-scope ... WRITE of size 4 thread T1".

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

namespace {

int g_checks = 0;
int g_fails  = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fails; std::printf("FAIL: %s\n", what); }
    else       std::printf("ok:   %s\n", what);
}

// Stands in for a provider: has state the stream both reads and writes, and
// a destructor that poisons it so a use-after-free is detectable even
// without a sanitizer.
struct FakeProvider {
    static constexpr int kCanary = 0x5A5A5A;
    int canary = kCanary;
    std::atomic<long long> calls{0};

    bool stream() {
        calls.fetch_add(1, std::memory_order_relaxed);
        return canary == kCanary;
    }
    ~FakeProvider() { canary = 0; }
};

}  // namespace

int main() {
    // The seam, shaped like main()'s stream_fn: a std::function that outlives
    // the scope which constructed the providers, because the subagent config
    // holds a copy of it process-wide.
    std::function<bool()> seam;

    std::atomic<bool> worker_started{false};
    std::atomic<bool> worker_finished{false};
    std::atomic<bool> saw_live_provider{true};
    std::atomic<long long> post_scope_calls{0};
    std::atomic<bool> scope_exited{false};

    {
        auto provider = std::make_shared<FakeProvider>();
        // BY VALUE. This single character is the fix: `[provider]` rather
        // than `[&provider]`.
        seam = [provider] { return provider->stream(); };

        // A DETACHED worker — the shape jaal gives an isolated task. Nothing
        // joins it; it simply keeps calling the seam.
        std::thread([&, seam] {
            worker_started.store(true);
            const auto until = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(400);
            while (std::chrono::steady_clock::now() < until) {
                if (!seam()) saw_live_provider.store(false);
                if (scope_exited.load(std::memory_order_acquire))
                    post_scope_calls.fetch_add(1, std::memory_order_relaxed);
            }
            worker_finished.store(true);
        }).detach();

        while (!worker_started.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        // main() unwinds here, with the worker still inside the seam.
    }
    scope_exited.store(true, std::memory_order_release);

    // Drop the local handle too, so ONLY the abandoned worker's captured
    // copy keeps the provider alive. This is the state after main returns.
    seam = nullptr;

    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until && !worker_finished.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    check(worker_finished.load(),
          "an abandoned worker runs to completion after its creating scope "
          "exits");
    check(post_scope_calls.load() > 0,
          "it really did call into the provider AFTER the scope exited "
          "(otherwise this proves nothing)");
    check(saw_live_provider.load(),
          "every one of those calls saw a LIVE provider, not a poisoned one");

    std::printf("\n%d checks, %d failures (%lld post-scope calls)\n",
                g_checks, g_fails, post_scope_calls.load());
    return g_fails == 0 ? 0 : 1;
}
