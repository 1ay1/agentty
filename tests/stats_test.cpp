// stats_test — the derived-statistics projection.
//
// The panel's whole value is that its numbers are TRUE. A stats view that can
// drift from what the router did is worse than no view, because the user
// cannot tell which digits to trust. These tests pin the properties that make
// the numbers trustworthy:
//
//   • shares always sum to 1 over the routed turns (no rounding leak)
//   • the denominator is stated and consistent (routed + unrouted == total)
//   • turns that did not run are not counted at all
//   • Smart-Mode-off turns are their own bucket, never folded into Strategic
//
// Purity is the reason all of this is testable without a Model, a wire or a
// clock: smart_stats() is a function of the transcript and nothing else.

#include "agtest.hpp"

#include "agentty/domain/stats.hpp"

#include <string>
#include <vector>

using agentty::Message;
using agentty::ModelId;
using agentty::Role;
using agentty::smart::ModelRole;
namespace st = agentty::stats;

namespace {

// An assistant turn that RAN: provenance stamped, as launch_stream does.
Message served(const char* model, ModelRole role) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    m.served_role = role;
    return m;
}

// An assistant turn that ran with Smart Mode OFF: it has a model but no role.
Message unrouted(const char* model) {
    Message m;
    m.role = Role::Assistant;
    m.served_model = ModelId{model};
    return m;
}

// A user turn, and the two synthetic assistant messages the transcript also
// holds — the routing card and the in-flight placeholder. Neither ran.
Message user_turn() {
    Message m;
    m.role = Role::User;
    m.text = "do the thing";
    return m;
}
Message placeholder() {
    Message m;
    m.role = Role::Assistant;   // no served_model: never dispatched
    return m;
}

[[nodiscard]] const st::Row* row_named(const std::vector<st::Row>& rows,
                                       std::string_view label) {
    for (const auto& r : rows)
        if (r.label == label) return &r;
    return nullptr;
}

}  // namespace

TEST_CASE("stats: an empty transcript reports empty, not zeroes") {
    const auto s = st::smart_stats({});
    CHECK(s.empty(), "no turns == empty");
    CHECK(s.total_turns == 0);
    CHECK(s.routed_turns == 0);
    CHECK(s.by_role.empty(), "no ladder rows when there is nothing to tally");
    CHECK(s.by_model.empty());
}

TEST_CASE("stats: only turns that RAN are counted") {
    std::vector<Message> msgs = {
        user_turn(),
        placeholder(),                       // never dispatched
        served("glm-5.3", ModelRole::Strategic),
        user_turn(),
        placeholder(),                       // in flight right now
    };
    const auto s = st::smart_stats(msgs);
    CHECK(s.total_turns == 1, "user turns and placeholders are not turns");
    CHECK(s.routed_turns == 1);
    CHECK(s.unrouted_turns == 0);
}

TEST_CASE("stats: the role ladder is complete and ordered") {
    std::vector<Message> msgs = {
        served("big",   ModelRole::Strategic),
        served("mid",   ModelRole::Implementation),
        served("small", ModelRole::Utility),
        served("small", ModelRole::Utility),
    };
    const auto s = st::smart_stats(msgs);

    REQUIRE(s.by_role.size() == 3);
    // Ladder order is structural, not a sort: strongest first, every frame.
    CHECK(s.by_role[0].label == "Strategic");
    CHECK(s.by_role[1].label == "Implementation");
    CHECK(s.by_role[2].label == "Utility");

    CHECK(s.by_role[0].count == 1);
    CHECK(s.by_role[1].count == 1);
    CHECK(s.by_role[2].count == 2);
}

TEST_CASE("stats: a role with no work still gets a row") {
    // The informative statement is "Utility got nothing", which a MISSING row
    // cannot make — it reads as "Utility does not exist".
    std::vector<Message> msgs = {
        served("big", ModelRole::Strategic),
        served("big", ModelRole::Strategic),
    };
    const auto s = st::smart_stats(msgs);
    REQUIRE(s.by_role.size() == 3);
    const auto* util = row_named(s.by_role, "Utility");
    REQUIRE(util != nullptr);
    CHECK(util->count == 0);
    CHECK(util->share == 0.0);
}

TEST_CASE("stats: shares sum to one over the routed turns") {
    std::vector<Message> msgs = {
        served("a", ModelRole::Strategic),
        served("b", ModelRole::Implementation),
        served("c", ModelRole::Utility),
        served("c", ModelRole::Utility),
        served("c", ModelRole::Utility),
    };
    const auto s = st::smart_stats(msgs);

    double role_sum = 0.0;
    for (const auto& r : s.by_role) role_sum += r.share;
    CHECK(role_sum > 0.999 && role_sum < 1.001, "role shares sum to 1");

    double model_sum = 0.0;
    for (const auto& r : s.by_model) model_sum += r.share;
    CHECK(model_sum > 0.999 && model_sum < 1.001, "model shares sum to 1");
}

TEST_CASE("stats: Smart-Mode-off turns are their own bucket") {
    // Folding these into Strategic would OVERSTATE the flagship's share;
    // dropping them would understate the denominator. Both are lies a stats
    // panel can tell while every individual number looks fine.
    std::vector<Message> msgs = {
        served("big", ModelRole::Strategic),
        unrouted("big"),
        unrouted("big"),
    };
    const auto s = st::smart_stats(msgs);

    CHECK(s.total_turns == 3);
    CHECK(s.routed_turns == 1);
    CHECK(s.unrouted_turns == 2);
    CHECK(s.routed_turns + s.unrouted_turns == s.total_turns,
          "the denominator is consistent");

    // Percentages are OF the routed turns, so the unrouted ones must not
    // dilute them.
    const auto* strat = row_named(s.by_role, "Strategic");
    REQUIRE(strat != nullptr);
    CHECK(strat->count == 1);
    CHECK(strat->share > 0.999, "1 of 1 routed turns == 100%");
}

TEST_CASE("stats: delegated share is the headline number") {
    // This is the figure that was structurally 0 before the main turn was
    // routed by complexity — "is Smart Mode working?" in one number.
    {
        std::vector<Message> all_strategic = {
            served("big", ModelRole::Strategic),
            served("big", ModelRole::Strategic),
        };
        const auto s = st::smart_stats(all_strategic);
        CHECK(s.delegated_share == 0.0, "nothing delegated == 0%");
    }
    {
        std::vector<Message> mixed = {
            served("big",   ModelRole::Strategic),
            served("small", ModelRole::Utility),
            served("mid",   ModelRole::Implementation),
            served("small", ModelRole::Utility),
        };
        const auto s = st::smart_stats(mixed);
        CHECK(s.delegated_share > 0.749 && s.delegated_share < 0.751,
              "3 of 4 routed turns ran below Strategic");
    }
}

TEST_CASE("stats: models are ordered by share, ties stable") {
    std::vector<Message> msgs = {
        served("zeta",  ModelRole::Utility),
        served("alpha", ModelRole::Utility),
        served("beta",  ModelRole::Implementation),
        served("beta",  ModelRole::Implementation),
    };
    const auto s = st::smart_stats(msgs);
    REQUIRE(s.by_model.size() == 3);
    CHECK(s.by_model[0].label == "beta", "most turns first");
    // An unordered_map walk is not stable; ties break on id so the panel does
    // not reshuffle between frames.
    CHECK(s.by_model[1].label == "alpha");
    CHECK(s.by_model[2].label == "zeta");
}

TEST_CASE("stats: tabs are total and wrap") {
    CHECK(st::tab_title(st::Tab::Smart) == "Smart Mode");
    CHECK(!st::tab_subtitle(st::Tab::Smart).empty(),
          "every tab explains itself");
    // Stepping is total: with one tab every step is a no-op, and adding a
    // tab joins the cycle with no key-handling change.
    CHECK(st::tab_step(st::Tab::Smart, +1) == st::Tab::Smart);
    CHECK(st::tab_step(st::Tab::Smart, -1) == st::Tab::Smart);
}
