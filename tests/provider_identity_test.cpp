// provider_identity_test — a provider's IDENTITY must survive an endpoint
// override, and every registry row must have a reachable model catalog.
//
// ── Why this test exists ─────────────────────────────────────────────────
//
// `Selection` carries two orthogonal facts: WHICH provider this is, and WHERE
// to dial it. They used to be collapsed — identity was re-derived on every
// query by comparing `openai_endpoint.label` against a literal:
//
//     bool is_copilot() const { return is_oauth_native() && label == "copilot"; }
//
// but the custom-host flow OVERWRITES that label with the user's base URL. So
// configuring a custom provider against a Copilot endpoint silently destroyed
// its identity: `is_copilot()` went false, list_models_for fell past every arm
// to the generic /v1/models fetch, and Copilot serves its catalog at `/models`
// — the picker came up empty with no error, because "fell off the end of an
// if-chain" is indistinguishable from "this account has no models".
//
// The fix resolves the registry row ONCE and reads identity off it. These
// tests pin that property: mutate the endpoint however you like, the row (and
// therefore the catalog mechanism) stays put.

#include <string>

#include "agtest.hpp"

#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"

using namespace agentty;
using namespace agentty::provider;

TEST_CASE("provider identity survives an endpoint override") {
    auto sel = parse_selection("copilot");
    CHECK(sel.provider_id() == "copilot");
    CHECK(sel.is_copilot());

    // The user points the same provider at their own base URL. This is what
    // the custom-host modal does: host, port and LABEL are all replaced.
    sel.openai_endpoint.host  = "copilot.internal.corp";
    sel.openai_endpoint.label = "copilot.internal.corp";

    // Identity is a property of the ROW, not of the display string — so the
    // catalog still routes to Copilot's own fetcher rather than degrading to
    // the generic /v1/models GET that returns nothing.
    CHECK(sel.provider_id() == "copilot");
    CHECK(sel.is_copilot());
}

TEST_CASE("identity is by registry id, never by display label") {
    // A genuinely unknown custom host has no registry identity at all, and
    // must not be mistaken for one of the OAuth-native backends.
    Selection unknown;
    unknown.kind = Kind::OpenAI;
    unknown.openai_endpoint.label = "not-a-provider.example";
    CHECK(unknown.provider_id().empty());
    CHECK_FALSE(unknown.is_copilot());
    CHECK_FALSE(unknown.is_kimi());
    CHECK_FALSE(unknown.is_chatgpt());
    CHECK_FALSE(unknown.is_oauth_native());
}

TEST_CASE("parse_selection carries the row for every registry provider") {
    // The property that makes identity survive: every id the registry knows
    // round-trips through parse_selection back to itself.
    for (const auto& p : providers()) {
        INFO("provider = " << std::string{p.id});
        const auto sel = parse_selection(p.id);
        CHECK(sel.provider_id() == p.id);
    }
}

TEST_CASE("the three oauth-native backends are identified by id") {
    for (const char* id : {"copilot", "chatgpt", "kimi"}) {
        INFO("provider = " << id);
        const auto sel = parse_selection(id);
        CHECK(sel.provider_id() == id);
        CHECK(sel.is_oauth_native());
    }
}

TEST_CASE("an Anthropic selection carries its row") {
    // Anthropic never populates openai_endpoint, so its label keeps the struct
    // default — "openai". Deriving identity from that label would resolve the
    // WRONG row; carrying it cannot.
    const auto sel = parse_selection("anthropic");
    CHECK(sel.kind == Kind::Anthropic);
    CHECK(sel.provider_id() == "anthropic");
    CHECK_FALSE(sel.is_oauth_native());
}

TEST_CASE("a custom host naming a known provider adopts its row") {
    // The Discord report: "custom provider with gh copilot enabled does not
    // provide any model". A custom-host spec has no registry row, so it used
    // to get the GENERIC OpenAI defaults — /v1/models instead of Copilot's
    // /models — and the picker came up empty with no error.
    for (const char* spec : {"api.githubcopilot.com",
                             "api.githubcopilot.com:443",
                             "https://api.githubcopilot.com"}) {
        INFO("spec = " << spec);
        const auto sel = parse_selection(spec);
        CHECK(sel.provider_id() == "copilot");
        CHECK(sel.is_copilot());
        CHECK(sel.is_oauth_native());
        // The endpoint columns come from the row, so the catalog fetch hits
        // Copilot's /models rather than the generic /v1/models.
        CHECK(sel.openai_endpoint.models_path == "/models");
    }
}

TEST_CASE("a genuinely custom host keeps the endpoint the user typed") {
    // Adoption must NOT capture unrelated hosts: a real custom endpoint keeps
    // its own paths and gains no false identity.
    const auto sel = parse_selection("llm.internal.corp:8000");
    CHECK(sel.provider_id().empty());
    CHECK_FALSE(sel.is_oauth_native());
    CHECK(sel.openai_endpoint.host == "llm.internal.corp");
    CHECK(sel.openai_endpoint.port == 8000);
    CHECK(sel.openai_endpoint.models_path == "/v1/models");
}

// ── Smart Mode / model pinning ───────────────────────────────────────────
#include "agentty/provider/copilot/provider.hpp"

// Reported: "smart mode model pinning is not honored — I pinned Luna xhigh for
// implementation, but it picked GPT 5.3 codex and failed because that model is
// supported only by responses endpoint".
//
// The Auto resolver used to fall back to `selected_model` (then to any
// chat-capable model) whenever the pinned slug wasn't in available_models. So
// a pin silently became a DIFFERENT model — wrong cost, wrong capabilities,
// and a 400 when the substitute was Responses-only but the chat path ran.
TEST_CASE("copilot: a pinned model is never silently substituted") {
    const std::vector<std::string> avail{"gpt-5.3-codex", "gpt-4o"};

    // The pin IS available → honoured exactly.
    CHECK(copilot::pick_auto_model_for_test(avail, "gpt-5.3-codex", "gpt-4o")
          == "gpt-4o");

    // The pin is NOT available → empty, so the caller reports it. Crucially
    // NOT "gpt-5.3-codex", which is what the old fallback produced.
    CHECK(copilot::pick_auto_model_for_test(avail, "gpt-5.3-codex",
                                            "gpt-5.6-luna").empty());
}

TEST_CASE("copilot: Auto still delegates the choice") {
    const std::vector<std::string> avail{"mai-code-1.1-flash", "gpt-4o"};
    // Auto means "you choose": the server's own pick wins when it is
    // chat-capable...
    CHECK(copilot::pick_auto_model_for_test(avail, "gpt-4o", "copilot-auto")
          == "gpt-4o");
    // ...and when choosing on the user's behalf we still PREFER a chat-capable
    // model over a Responses-only one (mai-code-*), so an Auto turn takes the
    // path with the broadest support. A user who explicitly pins mai-code-*
    // gets it via the concrete-request branch above.
    CHECK(copilot::pick_auto_model_for_test(avail, "mai-code-1.1-flash",
                                            "copilot-auto") == "gpt-4o");
    CHECK(copilot::pick_auto_model_for_test(avail, "", "copilot-auto") == "gpt-4o");
    // Nothing on offer at all.
    CHECK(copilot::pick_auto_model_for_test({}, "", "copilot-auto").empty());
}
