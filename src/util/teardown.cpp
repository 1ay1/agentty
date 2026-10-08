// teardown.cpp — the process-exit join registry. See teardown.hpp.

#include "agentty/util/teardown.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <maya/runtime.hpp>

#include "agentty/util/logx.hpp"

namespace agentty::util::teardown {

namespace {

struct Action {
    Token                 token;
    std::string           name;
    std::function<void()> fn;
};

// Copy-on-write: register/cancel publish a new list with compare-and-swap.
// Actions are closures over whatever they tear down (often by reference), so
// they are not Sendable values to pass through a lock; a published list of
// them is the honest shape. Registration is rare and the list is short.
struct Registry {
    std::vector<Action> actions;
    Token               next_token = 1;
};

// Function-local static: no static-init-order dependency on any subsystem
// that might register from its own static constructor. Leaked so a late
// registration during static destruction still finds it alive.
maya::published<const Registry>& registry() {
    static auto* r = [] {
        auto* p = new maya::published<const Registry>;
        p->publish(std::make_shared<const Registry>());
        return p;
    }();
    return *r;
}

// Apply `edit` to a copy of the current list and publish it, retrying if
// someone else published first. Returns what `edit` returned.
template <class F>
auto update(F edit) {
    for (;;) {
        auto cur  = registry().current();
        auto next = std::make_shared<Registry>(*cur);
        auto out  = edit(*next);
        if (registry().publish_if(cur, std::move(next))) return out;
    }
}

} // namespace

Token on_shutdown(std::string name, std::function<void()> action) {
    if (!action) return 0;
    return update([&](Registry& r) {
        const Token token = r.next_token++;
        r.actions.push_back(Action{token, name, action});
        return token;
    });
}

void cancel(Token token) noexcept {
    if (token == 0) return;
    try {
        (void)update([token](Registry& r) {
            return std::erase_if(r.actions, [token](const Action& a) { return a.token == token; });
        });
    } catch (...) { /* out of memory copying a short list: leave it */ }
}

void run() noexcept {
    // Swap in an empty list (keeping the token counter), then run: an action
    // may register another action, or reach run() again via some destructor,
    // and neither waits on anything.
    std::vector<Action> actions;
    try {
        actions = update([](Registry& r) { return std::exchange(r.actions, {}); });
    } catch (...) { return; }

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
    return registry().current()->actions.size();
}

} // namespace agentty::util::teardown
