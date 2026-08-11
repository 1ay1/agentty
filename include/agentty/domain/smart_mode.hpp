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

#include <string>
#include <string_view>
#include <vector>

#include "agentty/domain/catalog.hpp"

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
};

struct RoleConfig {
    bool         enabled = false;   // Smart Mode master switch (off by default)
    SlotOverride strategic;
    SlotOverride implementation;
    SlotOverride utility;

    [[nodiscard]] const SlotOverride& slot(ModelRole r) const noexcept {
        switch (r) {
            case ModelRole::Strategic:      return strategic;
            case ModelRole::Implementation: return implementation;
            case ModelRole::Utility:        return utility;
        }
        return strategic;
    }
};

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

// Effort one step DOWN from `e` within what `caps` supports (High->Medium,
// Medium->Low, Low->None, None stays). Used to derive Implementation effort
// from the user's Strategic effort so Impl thinks, but a notch less.
[[nodiscard]] inline Effort effort_step_down(
        Effort e, const ModelCapabilities& caps) {
    using E = Effort;
    E stepped = e;
    switch (e) {
        case E::Max:   stepped = E::Xhigh; break;
        case E::Xhigh: stepped = E::High;  break;
        case E::High:  stepped = E::Medium; break;
        case E::Medium:stepped = E::Low;   break;
        case E::Low:   stepped = E::None;  break;
        case E::None:  stepped = E::None;  break;
    }
    return clamp_effort(stepped, caps);
}

} // namespace detail

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
        const RoleConfig& cfg) {
    const std::string parent_wire = wire_model_id(parent_model);
    RoleProfile pass{parent_wire,
                     clamp_effort(parent_effort,
                                  ModelCapabilities::from_id(parent_wire))};
    if (!cfg.enabled) return pass;

    // 1. Explicit user override for this slot wins outright.
    if (const auto& ov = cfg.slot(role); ov.set && !ov.model.empty()) {
        const std::string wire = wire_model_id(std::string_view{ov.model});
        return RoleProfile{wire,
                           clamp_effort(ov.effort,
                                        ModelCapabilities::from_id(wire))};
    }

    // 2. Zero-config auto-fill from the catalog.
    switch (role) {
        case ModelRole::Strategic:
            // The parent model, thinking at least as hard as the user set.
            return pass;

        case ModelRole::Implementation: {
            std::string mid = detail::strongest_mid(candidates);
            if (mid.empty()) return pass;   // no distinct mid tier → parent
            const auto caps = ModelCapabilities::from_id(mid);
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
        const RoleConfig& cfg) {
    if (cfg.enabled) {
        if (const auto& ov = cfg.utility; ov.set && !ov.model.empty())
            return wire_model_id(std::string_view{ov.model});
    }
    return cheapest_capable_model(wire_model_id(parent_model), candidates,
                                  ModelCapabilities::Tier::Cheap);
}

} // namespace agentty::smart
