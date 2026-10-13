#include "agentty/tool/subagent.hpp"
#include "agentty/util/teardown.hpp"

#include "agentty/util/sendable.hpp"   // maya::guarded + the opt-ins Live needs

#include <algorithm>
#include <mutex>   // std::once_flag
#include <optional>
#include <stop_token>
#include <vector>

namespace agentty::tools::subagent {

namespace {
// The installed config. A whole Config is copied out by current() and
// replaced by install(); the setters change one field at a time as the user
// switches. Copy-on-write in a published slot, not a lock: set_auth is
// called from the effect host's auth swap and current() from every subagent
// start, neither of which should need a capability for a value that is
// replaced, never mutated in place. Each setter is one compare-and-swap.
// (Config holds callables, so it can't be a jaal::snapshot -- those need a
// provably-immutable T; a published<const Config> holds an immutable copy.)
maya::published<const Config>& cfg() {
    static maya::published<const Config> c;
    return c;
}

// Read-modify-write: f(Config&) on a private copy, swapped in only if no one
// else swapped first; retried otherwise. f must have no side effects.
template <class F>
void edit(F f) {
    for (;;) {
        auto cur  = cfg().current();
        auto next = std::make_shared<Config>(cur ? *cur : Config{});
        f(*next);
        if (cfg().publish_if(cur, std::shared_ptr<const Config>(std::move(next)))) return;
    }
}

} // namespace

void install(Config c) {
    c.installed = true;
    cfg().publish(std::make_shared<const Config>(std::move(c)));
}

Config current() {
    auto c = cfg().current();
    return c ? *c : Config{};
}

void set_auth(Io, auth::AuthHeader auth) {
    edit([&](Config& c) { if (c.installed) c.auth = auth; });
}

void set_model(Io, std::string model) {
    if (model.empty()) return;
    // Only meaningful once a config exists; leave `installed` untouched.
    edit([&](Config& c) { c.model = model; });
}

void set_candidates(Io, std::vector<ModelInfo> candidates) {
    edit([&](Config& c) { if (c.installed) c.candidates = candidates; });
}

void set_smart(Io, smart::RoleConfig smart) {
    edit([&](Config& c) { if (c.installed) c.smart = smart; });
}

void set_provider(Io, std::string provider) {
    edit([&](Config& c) { if (c.installed) c.provider = provider; });
}



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
        util::teardown::on_shutdown("subagent.running", [](const maya::unlocked&) {
            (void)shutdown_running();
        });
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
