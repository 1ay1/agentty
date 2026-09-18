// teardown.cpp — the process-exit join registry. See teardown.hpp.

#include "agentty/util/teardown.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "agentty/util/logx.hpp"

namespace agentty::util::teardown {

namespace {

struct Action {
    Token                 token;
    std::string           name;
    std::function<void()> fn;
};

struct Registry {
    std::mutex          mu;
    std::vector<Action> actions;
    Token               next_token = 1;
};

// Function-local static: no static-init-order dependency on any subsystem
// that might register from its own static constructor.
Registry& registry() {
    static Registry r;
    return r;
}

} // namespace

Token on_shutdown(std::string name, std::function<void()> action) {
    if (!action) return 0;
    auto& r = registry();
    std::lock_guard lk(r.mu);
    const Token token = r.next_token++;
    r.actions.push_back(Action{token, std::move(name), std::move(action)});
    return token;
}

void cancel(Token token) noexcept {
    if (token == 0) return;
    auto& r = registry();
    std::lock_guard lk(r.mu);
    std::erase_if(r.actions, [token](const Action& a) { return a.token == token; });
}

void run() noexcept {
    // Move the list out under the lock, then run WITHOUT the lock: an action
    // is allowed to register another action (or to call run() reentrantly via
    // some destructor) without deadlocking.
    std::vector<Action> actions;
    {
        auto& r = registry();
        std::lock_guard lk(r.mu);
        actions = std::move(r.actions);
        r.actions.clear();
    }

    // LIFO — a subsystem registered later may depend on an earlier one, so it
    // is torn down first. Same discipline as static destruction.
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
        try {
            if (it->fn) it->fn();
        } catch (const std::exception& e) {
            AGT_LOG(General, Warn, "teardown",
                    "{} threw during shutdown: {}", it->name, e.what());
        } catch (...) {
            AGT_LOG(General, Warn, "teardown",
                    "{} threw a non-std exception during shutdown", it->name);
        }
    }
}

std::size_t pending() noexcept {
    auto& r = registry();
    std::lock_guard lk(r.mu);
    return r.actions.size();
}

} // namespace agentty::util::teardown
