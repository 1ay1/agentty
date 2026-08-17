// chatgpt_bundled_models_test — guards the ChatGPT (Codex) provider's model
// discovery against two regressions that broke the picker:
//
//   1. The retired `gpt-5.1-codex` slug. The server withdrew it and now rejects
//      it on the first turn ("model is not supported when using Codex with a
//      ChatGPT account"). It must NEVER appear in list_models() nor be returned
//      by default_model() — not from the live catalog, not from the offline
//      bundled fallback.
//   2. Hidden/internal models (e.g. `codex-auto-review`, visibility="hide")
//      must never surface in the user-facing catalog.
//
// The test adapts to the environment:
//   • unsigned  → exercises the OFFLINE bundled fallback (must be `gpt-5`).
//   • signed in → exercises the LIVE catalog (must be non-empty, real slugs).
// Either way the retired slug must be absent and a model must be selectable.
//
// Run: build the `chatgpt_bundled_models_test` target, execute. Exit 0 = pass.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/provider.hpp"
#include "agentty/provider/chatgpt/responses.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;

TEST_CASE("chatgpt bundled model catalog") {
    const bool signed_in = cc::responses_available();

    auto models = cc::list_models();
    CHECK(!models.empty());   // a model must ALWAYS be selectable

    // The retired, server-rejected slug must never surface — this is the exact
    // failure the user hit (picker defaulting to `gpt-5.1-codex` → first turn
    // 400s). Also assert the internal auto-review model is filtered out.
    for (const auto& m : models) {
        CHECK(m.id.value != "gpt-5.1-codex");
        CHECK(m.id.value != "codex-auto-review");
    }

    // The default (catalog index 0) must be a live, selectable slug.
    const std::string def = cc::default_model();
    CHECK(!def.empty());
    CHECK(def != "gpt-5.1-codex");

    if (signed_in) {
        // Live catalog path: the fetched list must contain real slugs and the
        // default must be one of them (never the bundled placeholder blindly).
        bool default_in_list = false;
        for (const auto& m : models)
            if (m.id.value == def) { default_in_list = true; break; }
        CHECK(default_in_list);
    } else {
        // Offline bundled fallback: must be the stable, always-accepted `gpt-5`.
        CHECK(def == "gpt-5");
    }

    (void)signed_in;
}
