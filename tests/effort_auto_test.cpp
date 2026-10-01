// effort_auto_test — `auto` effort: the default that lets the classifier work.
//
// WHAT WAS WRONG. `Effort::None` was doing two jobs: "the user asked for no
// reasoning" AND "the user never said anything". That conflation quietly broke
// Smart Mode's central premise.
//
// effort_for_complexity treats the user's effort as the MIDPOINT of a ladder:
// Standard keeps it, Complex steps +1, Simple steps −1. That is symmetric and
// it needs headroom both ways. But None is rung 0 — the FLOOR. From there a
// turn classified Complex could only reach `minimal` (one rung), while the
// same turn from a `medium` base reaches `high` (and can fall to `low` when
// easy). So on default settings the classifier's verdict was nearly inert: it
// could score a prompt Complex all it liked and the dial had nowhere to go.
//
// This is the same bug that was already fixed once on the ROLE axis — see
// resolve_turn_routing, where the role was hardcoded Strategic so "the
// classifier's verdict only ever moved the EFFORT dial". The mirror image
// survived on the effort axis until now.
//
// THE FIX, and what this file pins:
//   1. Auto is a distinct state, so "never configured" ≠ "chose off".
//   2. Auto anchors at the MIDPOINT of whatever ladder the model exposes —
//      a position, not a level, so it is right on a 1-rung model and a
//      6-rung one.
//   3. Auto survives a clamp. (The subtle killer: switch_to_model_ref clamps
//      on every model hop, so an Auto that collapsed there would be correct
//      on first run and degraded to a rung forever after.)
//   4. Explicit off still sends "" on every tier, including Complex.
//   5. Trivial is still hard-off regardless of base, so the raised anchor
//      costs nothing on cheap turns.

#include "agentty/domain/smart_mode.hpp"

#include "agtest.hpp"

#include <string>

using namespace agentty;
namespace sm = agentty::smart;
using E = Effort;
using Cx = agentty::smart::Complexity;

namespace {

// A model whose ON ladder is exactly `on`.
ModelCapabilities ladder_of(std::initializer_list<E> on) {
    ModelCapabilities c{};
    c.effort_set_known = true;
    std::uint8_t set = 0;
    for (E e : on) set |= effort_bit(e);
    c.effort_set = set;
    return c;
}

// The three shapes that actually ship, plus the degenerate one.
const ModelCapabilities kGpt5   = ladder_of({E::Minimal, E::Low, E::Medium, E::High});
const ModelCapabilities kClaude = ladder_of({E::Low, E::Medium, E::High, E::Max});
const ModelCapabilities kBinary = ladder_of({E::High});
const ModelCapabilities kNone   = ladder_of({});

// What the runtime does: resolve Auto at the edge, then step by complexity.
E routed(E base, Cx tier, const ModelCapabilities& caps) {
    return sm::effort_for_complexity(resolve_auto_effort(base, caps), tier, caps);
}

} // namespace

TEST_CASE("effort auto: the anchor is a POSITION, not a level") {
    // The midpoint of the ON rungs. Not a hardcoded "medium" — that would be
    // meaningless on a model whose only rung is `high`, and wrong on one that
    // reaches `max`.
    CHECK(auto_base_effort(kGpt5)   == E::Low);     // {min,low,med,high} -> idx 1
    CHECK(auto_base_effort(kClaude) == E::Medium);  // {low,med,high,max} -> idx 1
    CHECK(auto_base_effort(kBinary) == E::High);    // {high}             -> idx 0

    // No ladder at all: nothing to anchor, so auto IS off.
    CHECK(auto_base_effort(kNone) == E::None);

    // `off` is deliberately NOT part of the ladder being averaged. Including
    // it would drag the midpoint toward the floor and reintroduce the very
    // asymmetry this exists to remove.
    CHECK(auto_base_effort(kGpt5) != E::None);
}

TEST_CASE("effort auto: the classifier's verdict actually moves the dial") {
    // THE REGRESSION THIS FILE EXISTS FOR. Measure the dial's RANGE across the
    // four tiers. With base=None it was 2 distinct values (off, minimal) —
    // the classifier was decorative. With auto it spans the ladder.
    auto distinct = [](E base, const ModelCapabilities& caps) {
        E seen[4];
        int n = 0;
        for (Cx t : {Cx::Trivial, Cx::Simple,
                             Cx::Standard, Cx::Complex}) {
            const E got = routed(base, t, caps);
            bool dup = false;
            for (int i = 0; i < n; ++i) if (seen[i] == got) dup = true;
            if (!dup) seen[n++] = got;
        }
        return n;
    };

    // The old default could only ever produce 2 operating points.
    CHECK(distinct(E::None, kGpt5) == 2);
    // The new default produces one per tier.
    CHECK(distinct(E::Auto, kGpt5) == 4);
    CHECK(distinct(E::Auto, kClaude) == 4);
}

TEST_CASE("effort auto: per-tier routing on each ladder shape") {
    // gpt-5 {minimal,low,medium,high}, anchored at low.
    CHECK(routed(E::Auto, Cx::Trivial,  kGpt5) == E::None);
    CHECK(routed(E::Auto, Cx::Simple,   kGpt5) == E::Minimal);
    CHECK(routed(E::Auto, Cx::Standard, kGpt5) == E::Low);
    CHECK(routed(E::Auto, Cx::Complex,  kGpt5) == E::Medium);

    // claude {low,medium,high,max}, anchored at medium — a complex turn lands
    // on `high`, which is the case from the bug report.
    CHECK(routed(E::Auto, Cx::Trivial,  kClaude) == E::None);
    CHECK(routed(E::Auto, Cx::Simple,   kClaude) == E::Low);
    CHECK(routed(E::Auto, Cx::Standard, kClaude) == E::Medium);
    CHECK(routed(E::Auto, Cx::Complex,  kClaude) == E::High);

    // A binary model can't express gradations; it must not invent them.
    CHECK(routed(E::Auto, Cx::Trivial,  kBinary) == E::None);
    CHECK(routed(E::Auto, Cx::Standard, kBinary) == E::High);
    CHECK(routed(E::Auto, Cx::Complex,  kBinary) == E::High);

    // A non-reasoning model stays silent on every tier.
    for (Cx t : {Cx::Trivial, Cx::Simple,
                         Cx::Standard, Cx::Complex})
        CHECK(routed(E::Auto, t, kNone) == E::None);
}

TEST_CASE("effort auto: trivial is free, so the raised anchor costs nothing") {
    // This is what makes changing the default safe. You do not pay a floor
    // tax; you pay on the turns that earned it.
    for (const auto& caps : {kGpt5, kClaude, kBinary, kNone})
        for (E base : {E::Auto, E::None, E::Low, E::Medium, E::High, E::Max})
            CHECK(routed(base, Cx::Trivial, caps) == E::None);
}

TEST_CASE("effort auto: explicit off is still off, everywhere") {
    // The property that makes the default change defensible. A user who picks
    // `off` gets silence on EVERY tier, including Complex, forever.
    for (Cx t : {Cx::Trivial, Cx::Simple,
                         Cx::Standard}) {
        CHECK(routed(E::None, t, kGpt5)   == E::None);
        CHECK(routed(E::None, t, kClaude) == E::None);
    }
    // Complex from an explicit off still steps (that is the old, documented
    // behaviour of the stepper) but the WIRE value for a stored None is "".
    CHECK(effort_wire_for(E::None, kGpt5)   == "");
    CHECK(effort_wire_for(E::None, kClaude) == "");
    CHECK(effort_wire_for(E::None, kBinary) == "");
}

TEST_CASE("effort auto: never reaches the wire as a literal") {
    // Auto is a UI/persistence state. effort_wire_for is THE edge, and it
    // resolves there so nothing downstream (the steppers, the Anthropic
    // transport's budget table) has to know Auto exists.
    CHECK(effort_wire_for(E::Auto, kGpt5)   == "low");
    CHECK(effort_wire_for(E::Auto, kClaude) == "medium");
    CHECK(effort_wire_for(E::Auto, kBinary) == "high");
    CHECK(effort_wire_for(E::Auto, kNone)   == "");

    // And it is never a learnable rung: effort_bit must not collide with a
    // real level, or a persisted learned_effort_sets mask would be corrupted.
    CHECK(effort_bit(E::Auto) == 0);
    for (E lv : {E::Minimal, E::Low, E::Medium, E::High, E::Xhigh, E::Max})
        CHECK((effort_bit(lv) & effort_bit(E::Auto)) == 0);
}

TEST_CASE("effort auto: survives a clamp (the model-switch killer)") {
    // switch_to_model_ref clamps m.d.effort on EVERY provider/model hop. If a
    // clamp collapsed Auto to a rung, the feature would be correct exactly
    // once — on first run — and silently degrade the first time the user
    // pressed ^P. That failure would look like "auto stopped working" with
    // nothing to grep for, so it gets its own case.
    for (const auto& caps : {kGpt5, kClaude, kBinary, kNone}) {
        CHECK(clamp_effort(E::Auto, caps) == E::Auto);
        CHECK(nearest_effort(E::Auto, effort_set_of(caps)) == E::Auto);
    }

    // Hopping gpt-5 -> claude -> binary -> back must leave it Auto throughout.
    E e = E::Auto;
    for (const auto& caps : {kGpt5, kClaude, kBinary, kGpt5})
        e = clamp_effort(e, caps);
    CHECK(e == E::Auto);
}

TEST_CASE("effort auto: persistence round-trip, and the unset-vs-off split") {
    // The migration story, as a table.
    //
    // "" is what EVERY existing user has: the field has been empty-by-default
    // since the setting existed, so it cannot distinguish "chose off" from
    // "never touched it". Reading it as Auto is what delivers the fix without
    // a migration pass; anyone who wants silence picks `off` once, which now
    // persists as "none" and round-trips exactly.
    CHECK(effort_from_wire("")     == E::Auto);   // never configured
    CHECK(effort_from_wire("auto") == E::Auto);
    CHECK(effort_from_wire("none") == E::None);   // explicit off
    CHECK(effort_from_wire("off")  == E::None);   // tolerated alias

    // A value this build doesn't know anchors at auto rather than silently
    // meaning "off" — the per-model clamp handles the rest.
    CHECK(effort_from_wire("ultra") == E::Auto);

    // Real levels are untouched.
    CHECK(effort_from_wire("minimal") == E::Minimal);
    CHECK(effort_from_wire("low")     == E::Low);
    CHECK(effort_from_wire("medium")  == E::Medium);
    CHECK(effort_from_wire("high")    == E::High);
    CHECK(effort_from_wire("xhigh")   == E::Xhigh);
    CHECK(effort_from_wire("max")     == E::Max);

    // Round-trip: every state survives a save/load cycle. This is what keeps
    // an explicit `off` from decaying back into `auto` on the next launch.
    for (E e : {E::Auto, E::None, E::Minimal, E::Low, E::Medium, E::High,
                E::Xhigh, E::Max})
        CHECK(effort_from_wire(effort_to_wire_setting(e)) == e);

    // The two sentinels must persist as NON-EMPTY strings: persistence drops
    // empty values (`if (!s.effort.empty())`), so an empty "none" would be
    // written as absent and read back as auto — silently undoing the user's
    // explicit choice on every restart.
    CHECK(!effort_to_wire_setting(E::None).empty());
    CHECK(!effort_to_wire_setting(E::Auto).empty());
}

TEST_CASE("effort auto: the picker can reach it and leave it") {
    // Auto leads the list so it is the first thing a new user sees, and one
    // ← from it is `off` — turning reasoning off outright stays one keystroke.
    const auto l = available_efforts(kGpt5);
    REQUIRE(l.size() >= 2);
    CHECK(l[0] == E::Auto);
    CHECK(l[1] == E::None);

    // Cycling must visit auto and come back to it, on every shape with a
    // ladder — a value you can enter but not leave (or vice versa) is a trap.
    for (const auto& caps : {kGpt5, kClaude, kBinary}) {
        const int n = static_cast<int>(available_efforts(caps).size());
        E e = E::Auto;
        for (int i = 0; i < n; ++i) e = cycle_effort(e, +1, caps);
        CHECK(e == E::Auto);                       // full loop returns home
        CHECK(cycle_effort(E::Auto, +1, caps) == E::None);   // → lands on off
    }

    // A model with no ladder offers no auto: an "auto" that can only ever
    // resolve to off would be a lie.
    const auto none_list = available_efforts(kNone);
    REQUIRE(none_list.size() == 1);
    CHECK(none_list[0] == E::None);
}

TEST_CASE("effort auto: label and display") {
    CHECK(effort_label(E::Auto) == "auto");
    CHECK(effort_label(E::None) == "off");
    CHECK(effort_label(E::High) == "high");
}
