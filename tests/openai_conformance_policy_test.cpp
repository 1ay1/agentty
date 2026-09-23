// tests/openai_conformance_policy_test.cpp — what we send, and to whom.
//
// "Make it 100% conform to the OpenAI spec" is the obvious goal and it is the
// wrong one. OpenAI DEPRECATED `max_tokens` on Chat Completions in favour of
// `max_completion_tokens`; vLLM, llama.cpp, Together and most of the compat
// family accept ONLY `max_tokens`. Conforming to the document breaks the
// majority of the ecosystem named after it.
//
// So the target is not "match the spec", it is "be correct for the endpoint
// we are actually talking to" — a per-endpoint decision. conformance.hpp
// records those decisions with their evidence; this pins the properties that
// make the table trustworthy, and asserts the two places where agentty
// deliberately diverges from OpenAI's document.

#include <doctest/doctest.h>

#include <string_view>

#include "agentty/provider/openai/conformance.hpp"

namespace cf = agentty::provider::openai::conformance;

TEST_CASE("conformance: every field rule carries its evidence") {
    // A tier assignment without a reason is a guess that will be cargo-culted
    // by the next person. The rationale IS the artifact — it is where "we
    // tried that and it 422'd on Mistral" survives.
    for (const auto& r : cf::kRequestFields) {
        CHECK(!r.field.empty());
        CHECK(!r.rationale.empty());
        // Long enough to actually say something, not "needed".
        CHECK(r.rationale.size() > 20);
    }
    for (const auto& r : cf::kOmittedFields) {
        CHECK(!r.field.empty());
        CHECK(!r.rationale.empty());
        CHECK(r.rationale.size() > 20);
    }
}

TEST_CASE("conformance: a field is sent or omitted, never both") {
    // The two tables are a partition. A field in both means the policy
    // contradicts itself and the body builder is the tiebreaker — which is
    // exactly the implicit-decision problem this header exists to remove.
    for (const auto& sent : cf::kRequestFields)
        for (const auto& omitted : cf::kOmittedFields)
            CHECK(sent.field != omitted.field);
}

TEST_CASE("conformance: max_tokens is the DELIBERATE divergence") {
    // THE LOAD-BEARING DECISION. If someone "fixes" this to conform to
    // OpenAI's current document, every vLLM/llama.cpp/Together endpoint
    // starts 400ing. This test is the tripwire.
    bool sends_max_tokens = false, omits_max_completion_tokens = false;

    for (const auto& r : cf::kRequestFields)
        if (r.field == "max_tokens") {
            sends_max_tokens = true;
            CHECK(r.tier == cf::Tier::Universal);
        }
    for (const auto& r : cf::kOmittedFields)
        if (r.field == "max_completion_tokens") omits_max_completion_tokens = true;

    CHECK(sends_max_tokens);
    CHECK(omits_max_completion_tokens);
}

TEST_CASE("conformance: endpoint-specific fields are not Universal") {
    // Sending a field an endpoint rejects is a HARD 400/422 that kills the
    // turn; omitting one costs an optimisation. The asymmetry sets the
    // default, so anything known to be rejected somewhere must NOT be
    // Universal.
    for (const auto& r : cf::kRequestFields) {
        if (r.field == "prompt_cache_key") {
            // OpenAI-only routing hint. Local servers reject it and their KV
            // cache is prefix-automatic anyway.
            CHECK(r.tier == cf::Tier::Hosted);
        }
        if (r.field == "reasoning_effort") {
            // Mistral MAGISTRAL reasons natively and returns 422 for this.
            // Evidence gates it, not a capability table.
            CHECK(r.tier == cf::Tier::Probed);
        }
    }
}

TEST_CASE("conformance: the legacy tool shape stays dead") {
    // `functions` was superseded by `tools` years ago. Sending it would opt
    // into the legacy path on endpoints that still honour it, splitting tool
    // handling in two.
    bool omits_functions = false;
    for (const auto& r : cf::kOmittedFields)
        if (r.field == "functions") omits_functions = true;
    CHECK(omits_functions);

    bool sends_tools = false;
    for (const auto& r : cf::kRequestFields)
        if (r.field == "tools") {
            sends_tools = true;
            CHECK(r.tier == cf::Tier::Universal);
        }
    CHECK(sends_tools);
}
