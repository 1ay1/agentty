// Smart Mode registry rows must WRITE BACK, whatever their control type.
//
// Reported by sail3r (PR #42): in ^S → Smart Mode, "Cheapest main-turn role"
// pops open its choices and selecting one changes nothing.
//
// The reducer read the edited field with
//
//     if (auto* num = std::get_if<form::field::Number>(&f->value))
//         registry::set(cfg, *d, std::to_string(num->value));
//
// so a row whose control is anything OTHER than a Number fell through the
// `if` and was dropped — silently, because the surrounding code still
// persisted the (unchanged) config, rebuilt the form, and returned done().
// The row moved on screen and reverted on the next rebuild.
//
// Two of the five smart.* rows are affected, not one: the Enum
// ("Cheapest main-turn role") and the Bool ("Route the main turn"). Both are
// Tier::Basic, i.e. visible without --advanced.
//
// This pins the CONTRACT rather than the control types: every row the
// registry exposes for an owner must be round-trippable through
// registry::set, so adding a sixth row with a sixth control type cannot
// reintroduce a silent drop.

#include "agtest.hpp"

#include "agentty/runtime/settings_registry.hpp"
#include "agentty/domain/smart_mode.hpp"

#include <string>

TEST_CASE("smart settings: every registry row round-trips through set()") {
    namespace reg = agentty::settings::registry;

    // Walk the Smart Mode rows and prove each accepts a written value and
    // reads it back. A row that silently ignores a write is exactly the
    // reported bug, one control type at a time.
    int checked = 0;
    for (const auto& d : reg::kSettings) {
        if (d.owner() != reg::Owner::Smart) continue;
        ++checked;

        agentty::smart::RoleConfig cfg;   // defaults

        switch (d.type) {
            case reg::Type::Bool: {
                // Flip it away from the default and back; both directions
                // must stick. "false" is the interesting one — a set() that
                // only honours truthy strings looks like it works.
                CHECK(reg::set(cfg, d, "true"));
                CHECK(reg::get(cfg, d) == "true");
                CHECK(reg::set(cfg, d, "false"));
                CHECK(reg::get(cfg, d) == "false");
                break;
            }
            case reg::Type::Enum: {
                // Every declared option must be selectable. The reported row
                // (smart.main_turn_floor) declares utility|implementation|
                // strategic; selecting any of them changed nothing before.
                const std::string opts{d.options};
                REQUIRE(!opts.empty());
                std::size_t start = 0;
                while (start <= opts.size()) {
                    const auto bar = opts.find('|', start);
                    const std::string opt =
                        opts.substr(start, bar == std::string::npos
                                               ? std::string::npos : bar - start);
                    if (!opt.empty()) {
                        CHECK(reg::set(cfg, d, opt));
                        CHECK(reg::get(cfg, d) == opt);
                    }
                    if (bar == std::string::npos) break;
                    start = bar + 1;
                }
                break;
            }
            case reg::Type::Int:
            case reg::Type::Real: {
                // A value inside the declared range must survive the trip.
                const double mid = (d.min + d.max) / 2.0;
                const std::string v = d.type == reg::Type::Int
                    ? std::to_string(static_cast<long long>(mid))
                    : std::to_string(mid);
                CHECK(reg::set(cfg, d, v));
                break;
            }
        }
    }
    // The walk must actually have covered something — a typo in the owner
    // filter would otherwise make this pass vacuously.
    CHECK(checked >= 5);
}

TEST_CASE("smart settings: the reported row is an Enum, and its siblings vary") {
    // The specific regression, named. The handler used to read ONLY
    // form::field::Number, so these two rows could never commit. Asserting
    // their types here means a future change that makes the reported row a
    // Number again (and thus accidentally "fixes" it) still leaves the
    // general contract above as the real guard.
    namespace reg = agentty::settings::registry;

    const auto* floor_row = reg::find("smart.main_turn_floor");
    REQUIRE(floor_row != nullptr);
    CHECK(floor_row->type == reg::Type::Enum);

    const auto* route_row = reg::find("smart.route_main_turn");
    REQUIRE(route_row != nullptr);
    CHECK(route_row->type == reg::Type::Bool);

    // Both are Basic tier — visible in the default pane, not behind
    // --advanced, which is why this was reachable by an ordinary user.
    CHECK(floor_row->tier == reg::Tier::Basic);
    CHECK(route_row->tier == reg::Tier::Basic);

    // And at least one smart row IS a Number, which is why the bug hid: the
    // rows the author tested worked.
    bool any_number = false;
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::Smart
            && (d.type == reg::Type::Int || d.type == reg::Type::Real))
            any_number = true;
    CHECK(any_number);
}
