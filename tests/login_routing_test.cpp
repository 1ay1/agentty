// OAuth login routing — every provider must open ITS OWN sign-in panel.
//
// Reported: "all oauth flow open openai panel". Picking GitHub Copilot (or
// Kimi) from the provider picker launched the ChatGPT/Codex browser flow,
// so the user was asked to sign in to OpenAI to authenticate GitHub.
//
// Root cause: two flags that read like synonyms and are not.
//
//   oauth_native  — "authenticates by sign-in and rides its own long-lived
//                   transport". TRUE for chatgpt, copilot AND kimi; that is
//                   exactly how credentials.cpp reads it.
//   device_login  — "uses the shared device-code launcher". copilot, kimi.
//
// Both login.cpp and providers.cpp tested `oauth_native` FIRST and treated a
// hit as "this is the Codex flow" — which is only true for the one provider
// that is not also device_login. The registry comment even claimed "today
// only ChatGPT sets this", which stopped being true when Copilot and Kimi
// were added, and both call sites trusted the comment over the rows.
//
// The fix is ProviderDescriptor::codex_login(), so the relationship between
// the flags lives on the row instead of being re-derived (and mis-ordered)
// by each caller.

#include "agtest.hpp"

#include "agentty/provider/registry.hpp"

#include <string>
#include <string_view>

TEST_CASE("login routing: only ChatGPT uses the Codex flow") {
    namespace pr = agentty::provider;

    // The three OAuth providers, and what each must do.
    const auto* chatgpt = pr::preset_for("chatgpt");
    const auto* copilot = pr::preset_for("copilot");
    const auto* kimi    = pr::preset_for("kimi");
    REQUIRE(chatgpt != nullptr);
    REQUIRE(copilot != nullptr);
    REQUIRE(kimi    != nullptr);

    // All three are oauth_native — this is the fact that made the naive
    // test wrong, so assert it explicitly rather than leaving it implied.
    CHECK(chatgpt->oauth_native);
    CHECK(copilot->oauth_native);
    CHECK(kimi->oauth_native);

    // …but only ChatGPT uses the bespoke Codex flow.
    CHECK(chatgpt->codex_login());
    CHECK(!copilot->codex_login());
    CHECK(!kimi->codex_login());

    // The device-flow providers take the shared launcher instead.
    CHECK(!chatgpt->device_login);
    CHECK(copilot->device_login);
    CHECK(kimi->device_login);
}

TEST_CASE("login routing: the two flags cannot both claim a provider") {
    // The invariant that makes the bug unrepresentable, checked over the
    // WHOLE registry rather than the three rows above — a fourth OAuth
    // provider added later gets this for free.
    //
    // codex_login() and device_login are mutually exclusive by construction;
    // what this pins is that every OAuth-native row resolves to exactly one
    // launcher, so none can fall through to the wrong panel (or to none).
    namespace pr = agentty::provider;

    int native = 0, codex = 0;
    for (const auto& p : pr::providers()) {
        if (!p.oauth_native) continue;
        ++native;
        const bool takes_codex  = p.codex_login();
        const bool takes_device = p.device_login;
        if (takes_codex) ++codex;
        // Exactly one launcher, never both, never neither.
        CHECK(takes_codex != takes_device);
    }
    CHECK(native >= 3);   // chatgpt, copilot, kimi at least
    // Exactly one provider owns the Codex flow. If a second ever does, the
    // flow is no longer "the ChatGPT one" and this routing needs rethinking
    // rather than another row.
    CHECK(codex == 1);
}
