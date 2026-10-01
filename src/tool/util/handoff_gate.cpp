// The trust-handoff gate. See the header for why this sits on the tool call
// rather than inside the sandbox.

#include "agentty/tool/util/handoff_gate.hpp"

#include <jaal/kernel/guarded.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "agentty/tool/util/sandbox.hpp"
#include "agentty/util/logx.hpp"

namespace agentty::tools::util::handoff {

namespace {

// Same storage shape and the same reasoning as the broker's blocked feed: see
// sandbox_broker.cpp. `jaal::guarded` because access is only possible through
// `with()`, so "forgot the lock" is unrepresentable rather than a review item;
// function-local static because the first handoff can arrive from a tool worker
// during startup and a file-scope global would race its own constructor.
jaal::guarded<std::vector<sandbox_cfg::TrustHandoff>>& feed() {
    static jaal::guarded<std::vector<sandbox_cfg::TrustHandoff>> f;
    return f;
}

// Bounded for the same reason the broker's feed is: a loop writing to a trusted
// path would otherwise grow this without limit, which is a denial of service the
// gate inflicts on its own host. Smaller than the broker's 64 because a handoff
// is a far rarer and far louder event -- if there are 32 distinct ones, the
// count stopped being the information a while ago.
constexpr std::size_t kMaxHandoffs = 32;

[[nodiscard]] std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void remember(const sandbox_cfg::TrustHandoff& h) {
    feed().with([](std::vector<sandbox_cfg::TrustHandoff>& all,
                   sandbox_cfg::TrustHandoff incoming) {
        // Collapse a repeat of the same path. Unlike the broker's feed this
        // checks the WHOLE list rather than just the back: a handoff is
        // identified by its path, and the same file rewritten twice in a turn
        // is one fact about one file, not a sequence worth preserving.
        for (auto& existing : all) {
            if (existing.write.path == incoming.write.path) {
                existing.write.at_ms = incoming.write.at_ms;
                existing.write.command = std::move(incoming.write.command);
                return;
            }
        }
        all.push_back(std::move(incoming));
        // Drop the oldest when full, keeping the newest -- flushing an early
        // handoff out of view costs an attacker 32 distinct ones, each of which
        // is itself recorded and (by default) refused.
        if (all.size() > kMaxHandoffs) all.erase(all.begin());
    }, h);
}

}  // namespace

Verdict check_with(std::string_view path, std::string_view tool,
                   sandbox_cfg::HandoffPolicy policy) {
    Verdict v;

    sandbox_cfg::TrustKind kind{};
    if (!sandbox_cfg::is_host_trusted(path, &kind)) {
        // The common case, and it must stay cheap: this runs on every write the
        // agent makes. `is_host_trusted` is a table walk over path shapes with
        // no I/O, so an ordinary path costs a few string compares.
        return v;
    }

    v.is_handoff = true;
    v.kind = kind;

    // Record BEFORE branching on the policy. A handoff that was allowed under
    // `Allow` is still the single most interesting thing that happened to the
    // workspace this turn, and a feed that only lists refusals would go silent
    // exactly when the user has turned the gate off -- which is when they most
    // need to see it.
    sandbox_cfg::TrustHandoff h;
    h.write.path = std::string{path};
    h.write.command = std::string{tool};
    h.write.at_ms = now_ms();
    h.kind = kind;
    remember(h);

    const auto why = sandbox_cfg::explain(kind);

    // Logged at every outcome, allow included, for the same reason the broker
    // logs allows: the trail is what makes the boundary teachable. The path is
    // behind logx::body() because it is workspace content.
    AGT_LOG(Tool, Info, "sandbox.handoff", "{} {} ({}) policy={}",
            policy == sandbox_cfg::HandoffPolicy::Refuse ? "REFUSED" : "allowed",
            logx::body(std::string{path}), why,
            sandbox_cfg::to_string(policy));

    switch (policy) {
        case sandbox_cfg::HandoffPolicy::Allow:
            return v;

        case sandbox_cfg::HandoffPolicy::Warn:
            // Proceeds, but says so. The reason rides along so the caller can
            // surface it without re-deriving the explanation.
            v.reason = std::string{"wrote "} + std::string{why};
            return v;

        case sandbox_cfg::HandoffPolicy::Refuse:
            break;
    }

    v.allowed = false;
    // Phrased for the MODEL as much as the user: a refusal that only says "no"
    // gets retried, and a retry loop on a security gate is worse than either
    // outcome. Naming the mechanism and the alternative ends the loop in one
    // turn.
    v.reason =
        "refused: " + std::string{why} + ". The sandbox confines this process, "
        "but a file the host later executes escapes it by being run outside. "
        "Write the content somewhere inert and ask the user to wire it up, or "
        "change Trust handoff in the Sandbox pane.";
    return v;
}

Verdict check(std::string_view path, std::string_view tool) {
    // Reads the SEALED config, so the policy cannot change under a running
    // turn -- the same guarantee every other wall here has.
    return check_with(path, tool, sandbox::config_snapshot()->handoff);
}

std::vector<sandbox_cfg::TrustHandoff> handoff_feed() {
    return feed().with(
        [](std::vector<sandbox_cfg::TrustHandoff>& all) { return all; });
}

void clear_handoff_feed() {
    feed().with([](std::vector<sandbox_cfg::TrustHandoff>& all) { all.clear(); });
}

}  // namespace agentty::tools::util::handoff
