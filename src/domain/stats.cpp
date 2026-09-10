// stats.cpp — the derived-statistics computation.
//
// One pass over the transcript, no state. See stats.hpp for why this is
// derived rather than accumulated.

#include "agentty/domain/stats.hpp"

#include <algorithm>
#include <unordered_map>

namespace agentty::stats {

namespace {

// A turn worth counting.
//
// Assistant messages only, and only ones that actually ran: the transcript
// also holds the zero-text routing card (a synthetic Assistant message the
// submit path inserts to render the brain card) and the streaming
// placeholder for a turn still in flight. Counting either would inflate the
// denominator with turns no model served.
//
// The test is `served_model` non-empty rather than "has text": a turn can
// legitimately produce no prose (tool calls only), and those DID run. The
// provenance stamp is written at dispatch, so its presence means exactly
// "the wire served this".
[[nodiscard]] bool is_served_turn(const Message& m) noexcept {
    return m.role == Role::Assistant && !m.served_model.value.empty();
}

}  // namespace

SmartStats smart_stats(const std::vector<Message>& messages) {
    SmartStats s;

    // Role counts indexed by the enum's underlying value, so the ladder
    // order is structural rather than a sort key that could drift from
    // ModelRole's declaration order.
    std::size_t role_counts[3] = {0, 0, 0};
    std::unordered_map<std::string, std::size_t> model_counts;

    for (const auto& m : messages) {
        if (!is_served_turn(m)) continue;
        ++s.total_turns;
        if (!m.served_role) {
            // Smart Mode was off when this turn ran. Its own bucket: folding
            // it into Strategic would overstate the flagship's share, and
            // dropping it would understate the denominator.
            ++s.unrouted_turns;
            continue;
        }
        ++s.routed_turns;
        const auto idx = static_cast<std::size_t>(*m.served_role);
        if (idx < 3) ++role_counts[idx];
        ++model_counts[m.served_model.value];
    }

    if (s.routed_turns == 0) return s;

    const auto denom = static_cast<double>(s.routed_turns);

    // Roles in ladder order (Strategic, Implementation, Utility). Every role
    // is emitted even at zero: a missing row reads as "this role does not
    // exist", when the informative statement is "this role got no work".
    s.by_role.reserve(3);
    for (int i = 0; i < 3; ++i) {
        const auto role = static_cast<smart::ModelRole>(i);
        const auto n = role_counts[i];
        s.by_role.push_back({std::string{smart::role_display_name(role)}, n,
                             static_cast<double>(n) / denom});
    }

    // Models by descending share, ties broken by id so the order is stable
    // across frames (an unordered_map walk is not).
    s.by_model.reserve(model_counts.size());
    for (const auto& [id, n] : model_counts)
        s.by_model.push_back({id, n, static_cast<double>(n) / denom});
    std::sort(s.by_model.begin(), s.by_model.end(),
              [](const Row& a, const Row& b) {
                  if (a.count != b.count) return a.count > b.count;
                  return a.label < b.label;
              });

    // The headline. Computed from the Strategic count rather than by summing
    // the other two, so it stays correct if a role is ever added.
    const auto strategic =
        role_counts[static_cast<std::size_t>(smart::ModelRole::Strategic)];
    s.delegated_share = static_cast<double>(s.routed_turns - strategic) / denom;

    return s;
}

}  // namespace agentty::stats
