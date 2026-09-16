// Tool advertisement must never be silently dropped.
//
// Reported: tools failing with Copilot models. The gate that decides whether
// tools go on the wire is ONE line in cmd_factory:
//
//     if (mi.id.value == model_id) model_supports_tools = mi.supports_tools;
//     ...
//     tools_disabled_by_capability = has_value() && !value()
//
// Two ways that goes wrong, and both are silent — the turn runs, the model
// simply never sees a tool:
//
//   1. supports_tools == false when the model DOES support tools. Copilot's
//      catalog reads caps["supports"]["tool_calls"], and a row that omits
//      the key, or reports it under a different name, lands here as false.
//
//   2. The id doesn't match. The lookup is an exact string compare against
//      m.d.available_models, so a model the catalog doesn't list (Copilot's
//      Auto rewrites the slug server-side; a provider may return a slug that
//      differs in case or suffix) finds nothing — which is nullopt, i.e.
//      "unknown", i.e. tools ARE sent. That is the safe direction, and this
//      test pins that it stays the safe direction.
//
// The invariant: tools are withheld ONLY on an explicit, positive
// declaration of non-support. Unknown must mean "send them".

#include "agtest.hpp"

#include "agentty/domain/catalog.hpp"
#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/provider/registry.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

// The ONE predicate every call site now uses (agentty::tools_allowed).
[[nodiscard]] bool withholds_tools(std::optional<bool> supports) {
    return !agentty::tools_allowed(supports);
}

// The exact lookup cmd_factory uses to find the active model's capability.
[[nodiscard]] std::optional<bool>
capability_for(const std::vector<agentty::ModelInfo>& models,
               const std::string& model_id) {
    for (const auto& mi : models)
        if (mi.id.value == model_id) return mi.supports_tools;
    return std::nullopt;
}

}  // namespace

TEST_CASE("tools: the member and free predicates agree") {
    // Two spellings, one rule. If these ever diverge, a call site that holds
    // a whole ModelInfo and one that holds only the optional would disagree
    // about the same model — which is the drift the shared predicate exists
    // to prevent.
    for (std::optional<bool> v : {std::optional<bool>{},
                                  std::optional<bool>{true},
                                  std::optional<bool>{false}}) {
        agentty::ModelInfo mi;
        mi.supports_tools = v;
        CHECK(mi.tools_allowed() == agentty::tools_allowed(v));
    }
}

TEST_CASE("tools: unknown capability means SEND, never withhold") {
    // The fail-safe direction. A model missing from the catalog, a slug the
    // server rewrote, a provider that reports no capabilities at all — every
    // one of these resolves to nullopt, and nullopt must never withhold.
    CHECK(!withholds_tools(std::nullopt));
    CHECK(!withholds_tools(true));
    CHECK(withholds_tools(false));   // the ONLY withholding case

    // A model id that isn't in the catalog is unknown, not unsupported.
    std::vector<agentty::ModelInfo> catalog = agentty::catalog::bundled("copilot");
    REQUIRE(!catalog.empty());
    CHECK(!withholds_tools(capability_for(catalog, "some-model-we-never-listed")));
    // Copilot's Auto rewrites the slug server-side, so the id on the wire can
    // differ from anything the catalog lists. That must not disable tools.
    CHECK(!withholds_tools(capability_for(catalog, "copilot-auto")));
}

TEST_CASE("tools: no bundled model claims to lack tool support") {
    // Every provider agentty ships a bundled catalog for is a tool-calling
    // provider — that is why it is in the picker. A bundled row asserting
    // supports_tools=false would silently strip tools for every user on that
    // model before any live catalog arrives, which is exactly the class of
    // failure this file exists to prevent.
    for (const auto& p : agentty::provider::providers()) {
        const auto models = agentty::catalog::bundled(p.id);
        for (const auto& mi : models) {
            const bool bad = withholds_tools(mi.supports_tools);
            if (bad) {
                std::printf("bundled %s/%s declares supports_tools=false\n",
                            std::string{p.id}.c_str(), mi.id.value.c_str());
            }
            CHECK(!bad);
        }
    }
}

TEST_CASE("tools: every OAuth provider's default model can call tools") {
    // The first turn after sign-in runs on default_model(). If that model
    // withholds tools the agent is inert on arrival — it can talk, but it
    // cannot read a file — which reads as "tools are broken with this
    // provider" rather than as a capability mismatch.
    for (const auto& p : agentty::provider::providers()) {
        if (!p.token_in_transport) continue;      // OAuth providers
        const auto models = agentty::catalog::bundled(p.id);
        if (models.empty()) continue;             // catalog is live-only
        const std::string def{p.default_model};
        if (def.empty()) continue;
        CHECK(!withholds_tools(capability_for(models, def)));
    }
}
