#pragma once
// agentty::smart — role-based execution routing (Smart Mode foundation).
//
// See docs/design/smart-mode.md. The idea: a turn (or an internal call) has a
// ROLE — Strategic (plan / hard reasoning), Implementation (write code /
// mechanical edits), Utility (grep / read / commit-msg / retrieval) — and the
// role resolves to a (model, effort) PROFILE at dispatch time, instead of
// hard-coding a model name at each call site.
//
// This header is the PURE resolver (Step 1). It has no I/O, no runtime deps,
// no wire — just the mapping `role -> RoleProfile` given the parent model, the
// user's effort, and the provider's model catalog. It reuses the existing
// catalog primitives (cheapest_capable_model, tier, model_picker_less,
// clamp_effort) so "which model is cheaper/stronger" stays in ONE place.
//
// Zero-config by design: with no user overrides, enabling Smart Mode routes
//   Strategic      -> the parent (flagship) model, at the user's effort
//   Implementation -> the strongest MID-tier model, at one effort step down
//   Utility        -> the cheapest capable model, effort OFF
// A single-model / Opus-only / one-tier account degrades to "everything on the
// parent model" — no change, no regression — exactly like the subagent router.

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/complexity.hpp"
#include "agentty/domain/smart_tuning.hpp"

namespace agentty::smart {

// The three execution roles. Kept minimal on purpose (the design argues 3 is
// the sweet spot: fewer is a false economy, more is unmanageable).
enum class ModelRole : std::uint8_t {
    Strategic,       // planning, architecture, hard reasoning — best model, hard think
    Implementation,  // writing code, mechanical multi-file edits — capable mid model
    Utility,         // grep/read/commit-msg/retrieval/summaries — cheapest capable, no think
};

[[nodiscard]] constexpr std::string_view role_label(ModelRole r) noexcept {
    switch (r) {
        case ModelRole::Strategic:      return "strategic";
        case ModelRole::Implementation: return "impl";
        case ModelRole::Utility:        return "utility";
    }
    return "strategic";
}

// The resolved (model, effort) a role runs on. `model` is a WIRE id (picker
// markers already stripped by the catalog helpers), safe to hand to a request.
struct RoleProfile {
    std::string             model;
    Effort effort = Effort::None;
};

// Per-role user overrides, persisted in settings. An empty `model` means "not
// set — auto-fill from the catalog". Effort::None as an override is
// indistinguishable from "unset"; a role's effort is auto-derived unless the
// whole slot is explicitly configured (see SlotOverride).
struct SlotOverride {
    std::string             model;    // "" = auto
    Effort effort = Effort::None;
    bool                    set   = false;   // true once the user pins this slot
    // The provider this pin was made under. A model id is only meaningful to
    // the endpoint that serves it: `claude-opus-4-5` pinned on Anthropic is
    // not a model Groq or Ollama can stream, and dispatching it there is an
    // instant 400/404. The picker already refuses to SELECT a cross-provider
    // row, but a pin PERSISTS — so a pin made on Anthropic was silently
    // re-applied after the user switched to another provider, on a code path
    // the picker's guard never sees.
    //
    // Recording the provider lets resolve_role scope the pin without
    // validating it against the catalog, which matters: a pin must still be
    // honoured when the catalog is EMPTY or STALE (see subagent_pin_test —
    // the Copilot bug). Provider identity is knowable offline; catalog
    // membership is not.
    //
    // Empty means "unknown provenance" (a settings.json written before this
    // field existed) and is honoured everywhere, preserving old behaviour.
    std::string             provider;
};

struct RoleConfig {
    bool enabled = false;   // Smart Mode master switch (off by default)

    // ONE decision, three slots. There used to be seven more toggles here.
    //
    // Three of them (internal routing, orchestration, subagent routing) are
    // now FOLDED IN: they are what Smart Mode *means*, and no user rationally
    // turned them off. "Send my compaction summaries to the flagship instead
    // of the cheap model" is not a preference worth a row — it is strictly
    // more expensive for no benefit. A toggle earns its place only where a
    // reasonable user would reasonably choose either way; these didn't.
    // Debug/bisect escape hatches live in env vars (smart::tuning::layers()),
    // not in the UI.
    //
    // Four of them (learned routing, outcome feedback, speculative prewarm,
    // plan recall) were self-supervised feedback loops that mutated routing
    // from per-workspace persisted state. They were never measured against
    // the fixed policy, and each carried a correctness surface (two state
    // files, a regret denominator, a decay schedule) plus the cognitive cost
    // of a user wondering what they do. Deleted. Four unmeasured feedback
    // loops is the product hedging, not an opinion.
    SlotOverride strategic;
    SlotOverride implementation;
    SlotOverride utility;

    // Whether a given layer is active right now. All three are simply the
    // master switch (kept as named accessors so call sites read as intent
    // — "is orchestration on" — rather than a bare bool, and so an env
    // escape hatch has exactly one place to apply).
    [[nodiscard]] bool internal_routing() const noexcept;
    [[nodiscard]] bool orchestration()    const noexcept;
    [[nodiscard]] bool subagent_routing() const noexcept;

    [[nodiscard]] const SlotOverride& slot(ModelRole r) const noexcept {
        switch (r) {
            case ModelRole::Strategic:      return strategic;
            case ModelRole::Implementation: return implementation;
            case ModelRole::Utility:        return utility;
        }
        return strategic;
    }
};

// Layer gates. Each is the master switch minus a developer escape hatch
// (see smart_tuning.hpp). Defined out-of-class so the tuning header's
// helpers are in scope; still header-inline, still trivially inlined.
inline bool RoleConfig::internal_routing() const noexcept {
    return enabled && !tuning::no_internal();
}
inline bool RoleConfig::orchestration() const noexcept {
    return enabled && !tuning::no_orchestrate();
}
inline bool RoleConfig::subagent_routing() const noexcept {
    return enabled && !tuning::no_subagents();
}

namespace detail {

// Strongest MID-tier candidate on this provider (for Implementation): the
// most capable model that is NOT flagship-priced. Reuses model_picker_less
// (tier desc, then newest) restricted to Mid, and refuses non-dispatchable /
// weak / no-tools assets. Empty when the provider has no distinct mid tier.
[[nodiscard]] inline std::string strongest_mid(
        const std::vector<ModelInfo>& candidates) {
    using Tier = ModelCapabilities::Tier;
    const ModelInfo* best = nullptr;
    for (const auto& mi : candidates) {
        const std::string_view id = mi.id.value;
        if (mi.supports_tools.has_value() && !*mi.supports_tools) continue;
        if (!is_dispatchable_model(id)) continue;
        if (ModelCapabilities::tier_for(id) != Tier::Mid) continue;
        if (!best || model_picker_less(mi, *best)) best = &mi;
    }
    return best ? wire_model_id(std::string_view{best->id.value}) : std::string{};
}

// Step `n` places along THE MODEL'S OWN effort ladder: off, then exactly the
// ON levels this model accepts, ascending. THE single stepping primitive —
// every complexity/cascade/step-down adjustment in this header goes through
// it, so "what is one step harder" has one answer per model.
//
// This walks the model's ladder rather than a fixed global one because a
// fixed ladder is wrong twice over:
//
//   1. It omitted Minimal. `minimal` is gpt-5+'s BOTTOM reasoning tier, so a
//      user on gpt-5 at minimal effort had `step(minimal, 0)` fall off the
//      ladder to index 0 == None — Smart Mode silently turned REASONING OFF
//      on every Standard turn, and a Simple turn did the same. The feature
//      whose whole job is scaling reasoning was disabling it.
//   2. It stepped through rungs the model does not have. On a Claude model
//      (no `minimal`) a step down from Low landed on Minimal, which
//      clamp_effort then snapped back UP to Low — so Implementation never
//      actually stepped down from a Low parent.
//
// Walking effort_set_of(caps) fixes both: the ladder IS the capability data,
// so a step can only ever land on a level the model will accept, and the
// result needs no post-hoc clamp. Heterogeneity as data, not a code path.
[[nodiscard]] inline Effort effort_step(
        Effort e, int n, const ModelCapabilities& caps) noexcept {
    Effort ladder[7];
    int nl = 0;
    ladder[nl++] = Effort::None;            // "off" is always a rung
    const std::uint8_t set = effort_set_of(caps);
    for (Effort lv : {Effort::Minimal, Effort::Low, Effort::Medium,
                      Effort::High, Effort::Xhigh, Effort::Max})
        if (set & effort_bit(lv)) ladder[nl++] = lv;

    // Snap a stale/unsupported request onto a REAL rung before stepping, so
    // the step is measured from where the model actually is.
    const Effort start = nearest_effort(e, set);
    int idx = 0;
    for (int i = 0; i < nl; ++i) if (ladder[i] == start) { idx = i; break; }
    idx += n;
    if (idx < 0)   idx = 0;
    if (idx >= nl) idx = nl - 1;
    return ladder[idx];
}

// Effort one step DOWN from `e` on the model's ladder. Used to derive
// Implementation effort from the user's Strategic effort so Impl thinks, but
// a notch less. Already clamped — effort_step can only return a supported
// level.
[[nodiscard]] inline Effort effort_step_down(
        Effort e, const ModelCapabilities& caps) {
    return effort_step(e, -1, caps);
}

} // namespace detail

// Scale a base reasoning effort by the turn's complexity, apply the cascade
// correction `bias` (in effort-steps; the loop's running estimate of whether
// the heuristic is under/over-rating this session), then clamp to what the
// model supports. The research lever "scale effort to query complexity" plus
// the cascade the routing survey prefers over one-shot routing: a Complex turn
// thinks one step HARDER than baseline, Trivial thinks NONE, Simple one step
// down; `bias` nudges the result up/down by the loop's feedback. Bias is
// upward on ambiguity — under-thinking a hard turn costs more than a little
// wasted budget on an easy one.
[[nodiscard]] inline Effort effort_for_complexity(
        Effort base, Complexity c, const ModelCapabilities& caps, int bias = 0) {
    using E = Effort;
    // One stepping primitive, walking THIS model's ladder (see effort_step).
    auto step = [&](E e, int n) { return detail::effort_step(e, n, caps); };
    E scaled;
    switch (c) {
        case Complexity::Trivial:  scaled = E::None;       break;
        case Complexity::Simple:   scaled = step(base, -1); break;
        case Complexity::Standard: scaled = base;          break;
        case Complexity::Complex:  scaled = step(base, +1); break;
        default:                   scaled = base;          break;
    }
    // Trivial stays None regardless of a positive bias — an ack is an ack.
    if (c != Complexity::Trivial && bias != 0) scaled = step(scaled, bias);
    return clamp_effort(scaled, caps);
}

// CONTINUOUS effort scaling. effort_for_complexity uses only the discrete tier,
// so a turn barely into Complex and one that is OVERWHELMINGLY complex both get
// exactly +1 — the classifier's score/margin is thrown away at the last mile.
// This overload keeps the tier step as the backbone (identical output at the
// tier boundary) but adds a fractional refinement from how DEEP the turn sits
// in its band:
//   • deep inside Complex (large margin)  → an extra +1 step (reaches the
//     effective +2 the old path needed the cascade bias to ever hit),
//   • deep inside Simple (large margin)    → an extra -1 step (a clear ack-ish
//     one-liner drops further than a borderline-Simple turn),
//   • Standard and shallow bands            → exactly the tier behaviour.
// Monotone in score, still clamped to the model, Trivial still pins to None.
// `deep` is the margin at which the band is considered saturated.
[[nodiscard]] inline Effort effort_for_score(
        Effort base, const ComplexityScore& cx, const ModelCapabilities& caps,
        int bias = 0) {
    using E = Effort;
    // Same single primitive as effort_for_complexity — identical output at the
    // tier boundary is a property of sharing the stepper, not a coincidence.
    auto step = [&](E e, int n) { return detail::effort_step(e, n, caps); };
    if (cx.tier == Complexity::Trivial) return E::None;

    const int kDeep = tuning::deep_margin();   // saturation margin (env-tunable)
    int extra = 0;
    if (cx.tier == Complexity::Complex && cx.margin >= kDeep) extra = +1;
    else if (cx.tier == Complexity::Simple && cx.margin >= kDeep) extra = -1;

    // Tier backbone (matches effort_for_complexity), then the fractional refine,
    // then the cascade bias, then clamp.
    int tier_step = (cx.tier == Complexity::Complex) ? +1
                  : (cx.tier == Complexity::Simple)  ? -1 : 0;
    E scaled = step(base, tier_step + extra);
    if (bias != 0) scaled = step(scaled, bias);
    return clamp_effort(scaled, caps);
}

// Resolve a role to the (model, effort) it should run on.
//
//   role          the execution role for this call
//   parent_model  the model the user picked for the main turn (WIRE or picker id)
//   parent_effort the user's effort setting on the main turn
//   candidates    the active provider's model catalog
//   cfg           per-role overrides + the enabled flag
//
// When cfg.enabled is false, every role resolves to (parent_model,
// parent_effort) — Smart Mode off is a pure pass-through. When on, a slot the
// user pinned wins; otherwise the zero-config auto-fill applies. The resolver
// NEVER routes a role UP past the parent tier for cost reasons; Strategic is
// the parent (or a user-pinned stronger model).
[[nodiscard]] inline RoleProfile resolve_role(
        ModelRole role,
        std::string_view parent_model,
        Effort parent_effort,
        const std::vector<ModelInfo>& candidates,
        const RoleConfig& cfg,
        std::string_view active_provider = {}) {
    const std::string parent_wire = wire_model_id(parent_model);
    RoleProfile pass{parent_wire,
                     clamp_effort(parent_effort,
                                  resolved_caps(parent_wire))};
    if (!cfg.enabled) return pass;

    // 1. Explicit user override for this slot wins outright — but only under
    //    the provider it was pinned on. A model id is endpoint-scoped, so
    //    replaying an Anthropic pin against Groq dispatches an id that
    //    provider never heard of (an instant 400/404 on every turn, or every
    //    delegation, until the user finds the Smart Mode overlay).
    //
    //    Deliberately NOT a catalog-membership check: a pin must survive an
    //    empty or still-loading catalog (subagent_pin_test / the Copilot
    //    bug). Provider identity is known offline; membership is not. Both an
    //    unknown-provenance pin (pre-upgrade settings) and an unknown active
    //    provider (callers that don't pass one) stay honoured — strictly
    //    fewer surprises than before, never more.
    if (const auto& ov = cfg.slot(role); ov.set && !ov.model.empty()) {
        const bool provider_matches =
            active_provider.empty() || ov.provider.empty()
            || ov.provider == active_provider;
        if (provider_matches) {
            const std::string wire = wire_model_id(std::string_view{ov.model});
            return RoleProfile{wire,
                               clamp_effort(ov.effort,
                                            resolved_caps(wire))};
        }
        // Foreign pin: fall through to the zero-config auto-fill below, which
        // ranks over THIS provider's catalog. The pin is not cleared — switch
        // back and it applies again.
    }

    // 2. Zero-config auto-fill from the catalog.
    switch (role) {
        case ModelRole::Strategic:
            // The parent model, thinking at least as hard as the user set.
            return pass;

        case ModelRole::Implementation: {
            std::string mid = detail::strongest_mid(candidates);
            if (mid.empty()) return pass;   // no distinct mid tier → parent
            const auto caps = resolved_caps(mid);
            return RoleProfile{std::move(mid),
                               detail::effort_step_down(parent_effort, caps)};
        }

        case ModelRole::Utility: {
            // Cheapest capable model, no reasoning budget — grunt work.
            std::string cheap =
                cheapest_capable_model(parent_wire, candidates,
                                       ModelCapabilities::Tier::Cheap);
            return RoleProfile{std::move(cheap), Effort::None};
        }
    }
    return pass;
}

// Subagent-context role resolution. Identical model routing to resolve_role,
// but the returned reasoning effort is HARD-CLAMPED to the parent's: a worker
// must never think harder than the turn that spawned it (invariant: subagent
// effort ≤ parent effort; when the parent's effort is off, every worker's is
// off too). This keeps the invariant in the resolver instead of relying on the
// call site never wiring req.effort — a reviewer/coder role mapped to Strategic
// (which returns the parent model at parent effort) would otherwise inherit
// full parent effort the moment a caller reads .effort.
[[nodiscard]] inline RoleProfile resolve_subagent_role(
        ModelRole role,
        std::string_view parent_model,
        Effort parent_effort,
        const std::vector<ModelInfo>& candidates,
        const RoleConfig& cfg,
        std::string_view active_provider = {}) {
    RoleProfile p = resolve_role(role, parent_model, parent_effort,
                                 candidates, cfg, active_provider);
    // min(resolved, parent) on the ordered Effort scale.
    if (static_cast<int>(p.effort) > static_cast<int>(parent_effort))
        p.effort = parent_effort;
    return p;
}

// Convenience for the INTERNAL utility calls (compaction summary, commit
// messages, HyDE query expansion, fork/thread retrieval). These already
// default to the cheapest capable model even with Smart Mode OFF — so this
// preserves that default and ONLY overrides it when the user has explicitly
// pinned a Utility slot in Smart Mode. That way turning Smart Mode on can
// steer these onto a specific cheap model, but turning it OFF never regresses
// them back up to the flagship. Returns a WIRE model id.
[[nodiscard]] inline std::string utility_model(
        std::string_view parent_model,
        const std::vector<ModelInfo>& candidates,
        const RoleConfig& cfg,
        std::string_view active_provider = {}) {
    if (cfg.internal_routing()) {
        if (const auto& ov = cfg.utility; ov.set && !ov.model.empty()
            && (active_provider.empty() || ov.provider.empty()
                || ov.provider == active_provider))
            return wire_model_id(std::string_view{ov.model});
    }
    return cheapest_capable_model(wire_model_id(parent_model), candidates,
                                  ModelCapabilities::Tier::Cheap);
}

} // namespace agentty::smart
