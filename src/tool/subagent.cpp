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
// The installed config, behind one guarded value. A whole Config is
// copied out by current() and replaced by install(); the setters change one
// field at a time as the user switches.
maya::guarded<Config>& cfg() {
    static maya::guarded<Config> c;
    return c;
}

} // namespace

void install(Config c) {
    c.installed = true;
    cfg().with([](Config& k, Config v) { k = std::move(v); }, std::move(c));
}

Config current() {
    return cfg().read([](const Config& c) { return c; });
}

void set_auth(Io, auth::AuthHeader auth) {
    cfg().with([](Config& c, auth::AuthHeader a) {
        if (c.installed) c.auth = std::move(a);
    }, std::move(auth));
}

void set_model(Io, std::string model) {
    if (model.empty()) return;
    // Only meaningful once a config exists; leave `installed` untouched.
    cfg().with([](Config& c, std::string m) { c.model = std::move(m); },
               std::move(model));
}

void set_candidates(Io, std::vector<ModelInfo> candidates) {
    cfg().with([](Config& c, std::vector<ModelInfo> v) {
        if (c.installed) c.candidates = std::move(v);
    }, std::move(candidates));
}

void set_smart(Io, smart::RoleConfig smart) {
    cfg().with([](Config& c, smart::RoleConfig v) {
        if (c.installed) c.smart = std::move(v);
    }, std::move(smart));
}

void set_provider(Io, std::string provider) {
    cfg().with([](Config& c, std::string p) {
        if (c.installed) c.provider = std::move(p);
    }, std::move(provider));
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
