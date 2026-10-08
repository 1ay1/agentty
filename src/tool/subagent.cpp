#include "agentty/tool/subagent.hpp"
#include "agentty/util/teardown.hpp"

#include <maya/runtime.hpp>

#include <algorithm>
#include <mutex>
#include <optional>
#include <stop_token>
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

// ── Running-run registry ─────────────────────────────────────────
// See the header for why this exists. It is a maya::stop_group:
// that is the registry this file used to hand-roll (a mutex, a condition
// variable, a vector of {cancelled, done} pairs and a wait that rescanned
// every entry on each wakeup), written once in jaal.
//
// What the jaal type adds beyond deleting code:
//   * ADMISSION CLOSES with shutdown. A run starting mid-teardown used to
//     register after shutdown_running took its snapshot, so it was never
//     asked to stop and never waited for. join() now refuses it.
//   * the cancel is a real std::stop_token, so the stream's cancel can be a
//     stop_callback instead of a thread polling a flag every 20 ms.

struct RunRegistration::State {
    std::optional<maya::stop_group::member> member;
};

namespace {

maya::stop_group& runs() {
    static maya::stop_group g;
    return g;
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
    state_->member = runs().join();
}

// The member leaves on destruction, which wakes a waiting shutdown.
RunRegistration::~RunRegistration() = default;

bool RunRegistration::cancelled() const noexcept {
    // A run refused admission (shutdown already running) reads as cancelled,
    // so it stops at its first check instead of doing work nobody waits for.
    return !state_->member || state_->member->stop_requested();
}

std::stop_token RunRegistration::token() const noexcept {
    if (state_->member) return state_->member->token();
    // Refused: hand back an already-stopped token so callers need no branch.
    std::stop_source dead;
    dead.request_stop();
    return dead.get_token();
}

std::size_t shutdown_running(std::chrono::milliseconds grace) noexcept {
    // Bounded, not a barrier: a wedged syscall must not hold the process
    // open. The runs touch provider objects that main() keeps alive past
    // this call, so the abandoned case is a slow exit, not a dangling write.
    try {
        return runs().stop_and_wait(grace);
    } catch (...) {
        return 0;   // a mutex failure at teardown is not worth a terminate
    }
}

} // namespace agentty::tools::subagent
