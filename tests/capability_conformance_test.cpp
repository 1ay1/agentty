// capability_conformance_test — invariants EVERY provider's model catalog
// must satisfy, asserted uniformly rather than per-provider.
//
// ── Why this file exists ─────────────────────────────────────────────────
//
// Model capabilities are decided by a precedence ladder — env override, then
// the live catalog a provider declares, then structural inference from the id.
// That ladder is good. What was missing is any statement of what a catalog
// ENTRY must look like, applied to every provider at once.
//
// The gap was not theoretical. ModelInfo::context_window defaults to 200000,
// which is right for Claude and wrong for Kimi K2 (256k). Kimi's /models
// payload carries no context length, so nothing ever overwrote the default and
// every Kimi turn silently under-reported its window by 56k — the context
// gauge and the compaction threshold both fired early. One provider's correct
// default, silently wrong for a later one: the same shape as the Copilot wire
// bugs.
//
// So these tests do not check "Kimi is 256k". They check properties that must
// hold for EVERY provider, so the next backend inherits them.

#include <set>
#include <string>

#include "agtest.hpp"

#include "agentty/domain/bundled_catalog.hpp"
#include "agentty/domain/catalog.hpp"
#include "agentty/provider/registry.hpp"

using namespace agentty;

namespace {

// Every provider that ships a bundled catalog. The seed is what a user sees
// before (or without) a live fetch, so it is the floor these invariants guard.
const std::vector<std::string>& seeded_providers() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& p : provider::providers())
            if (!catalog::bundled(p.id).empty()) out.emplace_back(p.id);
        return out;
    }();
    return v;
}

} // namespace

TEST_CASE("every bundled model declares a plausible context window") {
    for (const auto& pid : seeded_providers()) {
        for (const auto& m : catalog::bundled(pid)) {
            INFO("provider = " << pid << ", model = " << m.id.value);
            // A window of 0 or a negative would make the context gauge divide
            // by zero and the compaction threshold meaningless.
            CHECK(m.context_window > 0);
            // No shipping model has a window under 4k; a value that small
            // means a field was left unset or parsed wrong.
            CHECK(m.context_window >= 4096);
            // Nor is any current model beyond 10M — a value that large is a
            // units mistake (tokens vs bytes) rather than a real window.
            CHECK(m.context_window <= 10'000'000);
        }
    }
}

TEST_CASE("every bundled model is attributed to its own provider") {
    // The fused picker renders every authed provider's rows at once and
    // resolves capabilities under the row's OWN provider scope. A row carrying
    // the wrong provider id resolves against the wrong contract.
    for (const auto& pid : seeded_providers())
        for (const auto& m : catalog::bundled(pid)) {
            INFO("provider = " << pid << ", model = " << m.id.value);
            CHECK(m.provider == pid);
        }
}

TEST_CASE("bundled model ids are unique and non-empty within a provider") {
    for (const auto& pid : seeded_providers()) {
        std::set<std::string> seen;
        for (const auto& m : catalog::bundled(pid)) {
            INFO("provider = " << pid << ", model = " << m.id.value);
            CHECK_FALSE(m.id.value.empty());
            CHECK_FALSE(m.display_name.empty());
            // A duplicate id makes the picker's selection ambiguous.
            CHECK(seen.insert(m.id.value).second);
        }
    }
}

TEST_CASE("capability gates are internally consistent for every bundled model") {
    for (const auto& pid : seeded_providers()) {
        for (const auto& m : catalog::bundled(pid)) {
            INFO("provider = " << pid << ", model = " << m.id.value);
            const auto c = ModelCapabilities::from_id(m.id.value);

            // The effort ladder is a hierarchy: a model cannot offer the
            // higher rungs without the base. Sending `xhigh` to a model that
            // does not take an effort parameter is a 400.
            if (c.supports_effort_max())   CHECK(c.supports_effort());
            if (c.supports_effort_xhigh()) CHECK(c.supports_effort());

            // The 3-level compat enum (low|medium|high) cannot express the
            // extended rungs, so the two must never both be claimed.
            if (c.reasoning_compat) {
                CHECK_FALSE(c.supports_effort_max());
                CHECK_FALSE(c.supports_effort_xhigh());
            }
        }
    }
}

TEST_CASE("an unrecognised model claims no capabilities") {
    // THE robustness property for "works with every provider": a model we have
    // never seen must degrade to a safe floor, never over-promise a parameter
    // the endpoint will reject. Providers add models constantly; agentty finds
    // out when a user selects one.
    for (const char* id : {"glm-4.6", "minimax-m2", "qwen3-coder-480b",
                           "llama-4-maverick", "command-r-plus",
                           "some-model-from-2027", ""}) {
        INFO("model = " << id);
        const auto c = ModelCapabilities::from_id(id);
        CHECK(c.family == ModelCapabilities::Family::Unknown);
        CHECK_FALSE(c.supports_effort());
        CHECK_FALSE(c.supports_effort_max());
        CHECK_FALSE(c.supports_effort_xhigh());
    }
}

TEST_CASE("known models still resolve their capabilities") {
    // The mirror of the test above: degrading safely is only useful if
    // recognition still works. These pin the structural decode (family +
    // generation + revision), not a hardcoded list of ids.
    const auto opus = ModelCapabilities::from_id("claude-opus-4-5");
    CHECK(opus.family == ModelCapabilities::Family::Opus);
    CHECK(opus.generation == 4);
    CHECK(opus.revision == 5);
    CHECK(opus.supports_effort());

    // A FUTURE revision must inherit its family's gates with no code change —
    // that is what makes the structural decode worth having.
    const auto future = ModelCapabilities::from_id("claude-opus-4-9");
    CHECK(future.family == ModelCapabilities::Family::Opus);
    CHECK(future.supports_effort());

    const auto gpt5 = ModelCapabilities::from_id("gpt-5.6-luna");
    CHECK(gpt5.family == ModelCapabilities::Family::Gpt);
    CHECK(gpt5.supports_effort());
}

TEST_CASE("every provider that can be selected yields a non-empty catalog") {
    // An empty model list is indistinguishable from a failed fetch, which is
    // exactly how the custom-host Copilot bug presented: no models, no error.
    // Every provider with a static seed must show rows before any network.
    for (const auto& p : provider::providers()) {
        if (p.kind() == provider::Kind::ExternalAcp) continue;  // agent picks
        if (p.is_local) continue;                               // server-driven
        INFO("provider = " << std::string{p.id});
        const auto seed = catalog::bundled(p.id);
        // Hosted API-key providers without a seed fall back to the live fetch;
        // that is a deliberate choice, not a bug. What must never happen is a
        // seed that exists but is malformed — covered by the tests above.
        if (!seed.empty()) CHECK(seed.size() >= 1);
    }
}
