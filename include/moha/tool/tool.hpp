#pragma once
// moha::tool — Tool concept + DynamicDispatch adapter.
//
// The Tool concept describes the static interface every tool module
// exposes: a typed (Args, Result, Effects) bundle plus identity
// (name, description, schema). The runtime dispatcher uses the
// untyped registry edge (ToolDef) to look up + invoke; individual
// tools live behind `util::adapt<Args>(parse, run)` which lifts a
// typed pair into the JSON-typed signature ToolDef holds.

#include <concepts>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "moha/runtime/model.hpp"
#include "moha/tool/effects.hpp"
#include "moha/tool/policy.hpp"
#include "moha/tool/registry.hpp"

namespace moha::tool {

using tools::ToolOutput;
using tools::ToolError;
using tools::ExecResult;
using tools::EffectSet;

// A Tool is a static-type bundle of identity + schema + effects +
// behavior. The `Args` and `Result` types are exposed as nested
// typedefs so the surface is fully typed at compile time; only the
// dispatcher boundary speaks JSON.
template <class T>
concept Tool = requires {
    typename T::Args;
    typename T::Result;
    { T::name() }         -> std::convertible_to<std::string_view>;
    { T::description() }  -> std::convertible_to<std::string_view>;
    { T::input_schema() } -> std::convertible_to<nlohmann::json>;
    { T::effects() }      -> std::convertible_to<EffectSet>;
} && requires(const nlohmann::json& args) {
    { T::execute(args) }  -> std::convertible_to<ExecResult>;
};

struct DynamicDispatch {
    [[nodiscard]] static const tools::ToolDef* find(std::string_view name) noexcept {
        return tools::find(name);
    }

    [[nodiscard]] static ExecResult execute(std::string_view name,
                                            const nlohmann::json& args) noexcept {
        const auto* td = tools::find(name);
        if (!td) return std::unexpected(ToolError::not_found("unknown tool: " + std::string{name}));
        // Avoid copying `args` on the hot path: tools receive an empty object
        // only when the model emitted a non-object (rare). Use a process-
        // lifetime empty json so the reference stays valid either way.
        static const nlohmann::json kEmpty = nlohmann::json::object();
        const nlohmann::json& safe_args = args.is_object() ? args : kEmpty;
        try {
            return td->execute(safe_args);
        } catch (const std::exception& e) {
            return std::unexpected(ToolError::unknown(std::string{"tool crashed: "} + e.what()));
        }
    }

    // Single source of truth for whether a tool gates on the user.
    // Reads the tool's declared effects, asks the policy. Unknown
    // tools default to "needs permission" (fail closed).
    [[nodiscard]] static bool needs_permission(std::string_view name,
                                               Profile profile) noexcept {
        const auto* td = tools::find(name);
        if (!td) return true;
        return tools::policy::permission(td->effects, profile)
               == tools::policy::Decision::Prompt;
    }

    // Friendly reason text for the permission card. Reads the tool's
    // effects + the active profile to explain why permission is needed.
    [[nodiscard]] static std::string_view permission_reason(
        std::string_view name, Profile profile) noexcept {
        const auto* td = tools::find(name);
        if (!td) return "unknown tool";
        return tools::policy::reason(td->effects, profile);
    }
};

} // namespace moha::tool
