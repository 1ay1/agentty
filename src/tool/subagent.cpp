#include "agentty/tool/subagent.hpp"
#include "agentty/util/teardown.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace agentty::tools::subagent {

namespace {
Config     g_cfg;
std::mutex g_mu;
// Per-thread nesting depth. Each subagent runs synchronously on its own
// (task_isolated) worker thread, so thread_local correctly scopes the
// depth to one chain of nested subagents.
thread_local int g_depth = 0;
} // namespace

void install(Config cfg) {
    cfg.installed = true;
    std::lock_guard lk(g_mu);
    g_cfg = std::move(cfg);
}

Config current() {
    std::lock_guard lk(g_mu);
    return g_cfg;
}

void set_auth(auth::AuthHeader auth) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.auth = std::move(auth);
}

void set_model(std::string model) {
    if (model.empty()) return;
    std::lock_guard lk(g_mu);
    // Only meaningful once a config exists; leave `installed` untouched.
    g_cfg.model = std::move(model);
}

void set_candidates(std::vector<ModelInfo> candidates) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.candidates = std::move(candidates);
}

void set_smart(smart::RoleConfig smart) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.smart = std::move(smart);
}

void set_provider(std::string provider) {
    std::lock_guard lk(g_mu);
    if (!g_cfg.installed) return;
    g_cfg.provider = std::move(provider);
}

int current_depth() noexcept { return g_depth; }
void push_depth() noexcept { ++g_depth; }
void pop_depth() noexcept { if (g_depth > 0) --g_depth; }

namespace {
// Same thread-local discipline as g_depth: a subagent runs synchronously on
// its own worker thread, so parallel subagents each see only their own
// enclosing deadline.
thread_local RunDeadline g_deadline{};
} // namespace

RunDeadline current_deadline() noexcept { return g_deadline; }

DeadlineScope::DeadlineScope(RunDeadline d) noexcept : prev_{g_deadline} {
    g_deadline = d;
}
DeadlineScope::~DeadlineScope() { g_deadline = prev_; }

// ── Running-run registry ───────────────────────────────────────
// See the header for why this exists. Shape notes:
//
//   * State is shared_ptr so shutdown_running() can hold a live reference
//     while the run's own thread is finishing — the registration may be
//     destroyed at any moment by a thread we are not waiting on.
//   * `done` is a latch the run sets on its way out; shutdown waits on the
//     condvar rather than sleeping, so a clean exit costs no fixed delay.
//   * The mutex is never held across the wait for an individual run, only
//     around the registry vector.

struct RunRegistration::State {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};
};

namespace {

struct Registry {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::shared_ptr<RunRegistration::State>> runs;
};

Registry& registry() {
    static Registry r;
    return r;
}

// Register the teardown hook exactly once, from the first run that starts.
// This is the registry's own rule (util/teardown.hpp): the join belongs with
// the code that creates the thread, not in a hand-maintained list in main().
void ensure_teardown_registered() {
    static std::once_flag once;
    std::call_once(once, [] {
        util::teardown::on_shutdown("subagent.running",
                                    [] { (void)shutdown_running(); });
    });
}

} // namespace

RunRegistration::RunRegistration()
    : state_{std::make_shared<State>()} {
    ensure_teardown_registered();
    auto& r = registry();
    std::lock_guard lk(r.mu);
    r.runs.push_back(state_);
}

RunRegistration::~RunRegistration() {
    state_->done.store(true, std::memory_order_release);
    auto& r = registry();
    {
        std::lock_guard lk(r.mu);
        std::erase(r.runs, state_);
    }
    // Wake a shutdown that may be waiting on the last run to leave.
    r.cv.notify_all();
}

bool RunRegistration::cancelled() const noexcept {
    return state_->cancelled.load(std::memory_order_acquire);
}

std::size_t shutdown_running(std::chrono::milliseconds grace) noexcept {
    auto& r = registry();
    std::vector<std::shared_ptr<RunRegistration::State>> live;
    {
        std::lock_guard lk(r.mu);
        live = r.runs;                      // copy: keeps each alive below
    }
    if (live.empty()) return 0;
    for (auto& s : live) s->cancelled.store(true, std::memory_order_release);

    // Wait for them to leave, bounded. A run that is mid-stream notices via
    // the cancel bridge (tens of ms); one wedged in a syscall never will, and
    // holding the process open for it is exactly the hang jaal's detach was
    // designed to avoid.
    const auto deadline = std::chrono::steady_clock::now() + grace;
    std::unique_lock lk(r.mu);
    r.cv.wait_until(lk, deadline, [&] {
        for (const auto& s : live)
            if (!s->done.load(std::memory_order_acquire)) return false;
        return true;
    });
    std::size_t still_running = 0;
    for (const auto& s : live)
        if (!s->done.load(std::memory_order_acquire)) ++still_running;
    return still_running;
}

} // namespace agentty::tools::subagent
