// Context-window resolution: the ladder, and the 1M models it was hiding.
//
// Reported: "still 1M models don't show up generally". The cause was not the
// gateway probe — it was the rung BELOW it. ModelCapabilities::from_id()
// answers 200k for a known family and 0 for everything else, which is
// correct for Claude (the family it decodes) and wrong for almost everything
// else. So every non-Claude model arrived at the bottom of the ladder as 0
// and left as the 200k default.
//
// Measured against the live models.dev snapshot: 2698 of 7824 models have a
// real window of 1M or more. gemini-2.5-pro (1M), gpt-4.1 (1M),
// qwen3-coder-plus (1M), llama-4-scout (10M) — all understated by 5x or
// worse, all compacting far sooner than they need to, and all reporting a
// number nobody declared.
//
// The ladder, strongest evidence first:
//
//   1. user override    they configured the gateway
//   2. live advertised  the API is TRUTH — only it can 400 on overflow
//   3. models.dev       a real declaration, 7824 models deep
//   4. id inference     Claude/GPT families, [1m] suffix
//   5. 200k default     nothing is known
//
// This pins the ORDER, not just the values: each rung must beat the one
// below and lose to the one above, because the ordering is the design.

#include "agtest.hpp"

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/store/store.hpp"

#include <string>
#include <cstdlib>

namespace {

// A models.dev-shaped declaration, without needing the snapshot on disk.
void declare(const std::string& scope, const std::string& model, int tokens) {
    agentty::set_catalog_context_window(scope + "/" + model, tokens);
}

}  // namespace

TEST_CASE("context ladder: models.dev rescues the non-Claude models") {
    agentty::store::Settings s;

    // Before any declaration: an unknown family has nothing to go on, so the
    // conservative default stands. This is the reported bug's starting state.
    CHECK(agentty::ui::resolve_context_window("google", "ctxtest-unknown-1", 0, s)
          == agentty::ui::kDefaultContextWindow);

    // With a declaration, the real window is used.
    declare("google", "ctxtest-wide-1", 1'048'576);
    CHECK(agentty::ui::resolve_context_window("google", "ctxtest-wide-1", 0, s)
          == 1'048'576);

    // Claude keeps its id-inferred window — the new rung must not disturb
    // what already worked.
    CHECK(agentty::ui::resolve_context_window("anthropic", "claude-sonnet-4-5", 0, s)
          == 200'000);
    // …including the explicit [1m] variant.
    CHECK(agentty::ui::resolve_context_window("anthropic", "claude-sonnet-4-5[1m]", 0, s)
          == 1'000'000);
}

TEST_CASE("context ladder: the API outranks every static source") {
    agentty::store::Settings s;
    declare("acme", "ctxtest-wide-2", 1'000'000);

    // models.dev says 1M; the gateway says 32k. The gateway wins — it is the
    // only party that knows how IT serves this model, and the only one that
    // can reject the request for overflowing. A third-party catalog
    // describing the model in general cannot overrule the host describing
    // this deployment.
    CHECK(agentty::ui::resolve_context_window("acme", "ctxtest-wide-2",
                                              /*advertised=*/32'768, s) == 32'768);

    // And with nothing advertised, the declaration is used.
    CHECK(agentty::ui::resolve_context_window("acme", "ctxtest-wide-2", 0, s)
          == 1'000'000);
}

TEST_CASE("context ladder: the user override outranks even the API") {
    agentty::store::Settings s;
    declare("acme", "ctxtest-wide-3", 1'000'000);
    s.context_overrides[agentty::ui::context_override_key("acme", "ctxtest-wide-3")]
        = 512'000;

    // The user configured the gateway; nothing inferred beats that. Not the
    // declaration, and not even the advertisement.
    CHECK(agentty::ui::resolve_context_window("acme", "ctxtest-wide-3", 0, s)
          == 512'000);
    CHECK(agentty::ui::resolve_context_window("acme", "ctxtest-wide-3",
                                              /*advertised=*/32'768, s) == 512'000);
}

TEST_CASE("context ladder: hosts disagreeing about a bare id poisons it") {
    // The same bare model id is genuinely served at different sizes by
    // different hosts — measured: qwen3-coder-plus is 256k on one provider
    // and 1M on another. A shared key that kept the first writer's number
    // would bleed one host's figure onto another's, so disagreement must
    // read as "no declaration" and fall through to inference instead.
    agentty::merge_catalog_context_window("ctxtest-shared-1", 256'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-shared-1") == 256'000);

    agentty::merge_catalog_context_window("ctxtest-shared-1", 1'000'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-shared-1") == 0);

    // Poison is permanent for the session: a later agreeing write does not
    // resurrect a key two sources already contradicted each other on.
    agentty::merge_catalog_context_window("ctxtest-shared-1", 256'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-shared-1") == 0);

    // A SCOPED record is unaffected — that is the whole point of scoping.
    declare("acme", "ctxtest-shared-1", 777'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-shared-1", "acme") == 777'000);
}

TEST_CASE("context ladder: a vendor-prefixed id meets its bare form") {
    // The bare namespace is keyed by the TAIL so "elsewhere/model" and
    // "model" collide and can poison each other. Writing the prefixed key
    // instead would mean they never meet: disagreement could never be
    // detected, and the reader — which looks up the tail — would miss every
    // prefixed write. That asymmetry exists in a sibling registry; this
    // pins that this one does not copy it.
    agentty::merge_catalog_context_window("vendorx/ctxtest-tail-1", 128'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-tail-1") == 128'000);

    agentty::merge_catalog_context_window("ctxtest-tail-1", 999'000);
    CHECK(agentty::catalog_context_window_for("ctxtest-tail-1") == 0);
}

TEST_CASE("context ladder: a non-declaration is never recorded as one") {
    // 0 and negatives are "nothing was declared", not tiny windows. Storing
    // them would be indistinguishable from a real declaration of zero and
    // would short-circuit the rungs below.
    agentty::set_catalog_context_window("acme/ctxtest-zero-1", 0);
    CHECK(agentty::catalog_context_window_for("ctxtest-zero-1", "acme") == 0);
    agentty::merge_catalog_context_window("ctxtest-zero-2", -5);
    CHECK(agentty::catalog_context_window_for("ctxtest-zero-2") == 0);

    agentty::store::Settings s;
    CHECK(agentty::ui::resolve_context_window("acme", "ctxtest-zero-1", 0, s)
          == agentty::ui::kDefaultContextWindow);
}

TEST_CASE("context ladder: the picker column and the status bar agree") {
    // The two surfaces resolve the window by DIFFERENT routes, and that is
    // the bug sail3r's PR #39 identified:
    //
    //   status bar  -> ui::resolve_context_window(), i.e. the ladder
    //   picker ctx  -> ModelInfo::context_window, baked when the catalog
    //                  loaded
    //
    // If the bake does not run the ladder, a models.dev declaration is
    // invisible in the one place a user looks to check it: the column says
    // "auto" while the status bar says 1M, and they are describing the same
    // model. His framing — "the picker's ctx column and the status bar agree
    // BY CONSTRUCTION" — is the right requirement, so it is asserted here
    // rather than left to whichever call site remembers.
    //
    // bake_context_window() is that construction: one function, used by the
    // catalog loaders, that gives a row the same number the ladder would.
    agentty::store::Settings s;
    agentty::set_catalog_context_window("ctxprov/ctxtest-baked-1", 1'000'000);

    // A gateway that advertised nothing.
    agentty::ModelInfo row;
    row.id = agentty::ModelId{"ctxtest-baked-1"};
    row.provider = "ctxprov";
    row.context_window = 0;

    agentty::ui::bake_context_window(row, "ctxprov", s);
    const int ladder = agentty::ui::resolve_context_window(
        "ctxprov", "ctxtest-baked-1", 0, s);

    CHECK(row.context_window == ladder);
    CHECK(row.context_window == 1'000'000);   // not "auto", not 200k

    // An ADVERTISED window still wins, and both surfaces still agree.
    agentty::ModelInfo adv;
    adv.id = agentty::ModelId{"ctxtest-baked-1"};
    adv.provider = "ctxprov";
    adv.context_window = 32'768;              // the gateway spoke
    agentty::ui::bake_context_window(adv, "ctxprov", s);
    CHECK(adv.context_window == 32'768);
    CHECK(adv.context_window == agentty::ui::resolve_context_window(
              "ctxprov", "ctxtest-baked-1", 32'768, s));

    // And a per-model override outranks both, on both surfaces.
    s.context_overrides[agentty::ui::context_override_key(
        "ctxprov", "ctxtest-baked-1")] = 512'000;
    agentty::ModelInfo ovr;
    ovr.id = agentty::ModelId{"ctxtest-baked-1"};
    ovr.provider = "ctxprov";
    ovr.context_window = 32'768;
    agentty::ui::bake_context_window(ovr, "ctxprov", s);
    CHECK(ovr.context_window == 512'000);
}

TEST_CASE("context ladder: the env floor catches what no catalog can know") {
    // The unknowable case: a private gateway serving an id nobody published.
    // Discovery covers ~95% of shipped providers, but a model at
    // 10.0.0.5:4000 called `internal-v2` is unreachable by construction, so
    // one manual rung has to exist. Zed requires max_tokens in settings.json
    // for OpenAI-compatible endpoints; Claude Code has
    // CLAUDE_CODE_MAX_CONTEXT_TOKENS. This mirrors the latter's spelling.
    struct ScopedEnv {
        const char* k;
        ScopedEnv(const char* key, const char* v) : k(key) {
            if (v) ::setenv(k, v, 1); else ::unsetenv(k);
        }
        ~ScopedEnv() { ::unsetenv(k); }
    };
    agentty::store::Settings s;
    const char* kEnv = "AGENTTY_MAX_CONTEXT_TOKENS";

    {   // Unset: the conservative default stands.
        ScopedEnv e{kEnv, nullptr};
        CHECK(agentty::ui::resolve_context_window("priv", "ctxtest-env-1", 0, s)
              == agentty::ui::kDefaultContextWindow);
    }
    {   // Set: it is used, because nothing better spoke.
        ScopedEnv e{kEnv, "1000000"};
        CHECK(agentty::ui::resolve_context_window("priv", "ctxtest-env-1", 0, s)
              == 1'000'000);
    }
    {   // LAST rung, not first. It is GLOBAL, so a session reaching several
        // models would otherwise stamp one number on all of them — anything
        // that actually knows this model must win.
        ScopedEnv e{kEnv, "1000000"};
        CHECK(agentty::ui::resolve_context_window("priv", "ctxtest-env-1",
                                                  /*advertised=*/32'768, s)
              == 32'768);
        agentty::set_catalog_context_window("priv/ctxtest-env-2", 262'144);
        CHECK(agentty::ui::resolve_context_window("priv", "ctxtest-env-2", 0, s)
              == 262'144);
        CHECK(agentty::ui::resolve_context_window("anthropic",
                                                  "claude-sonnet-4-5", 0, s)
              == 200'000);
    }
    {   // Garbage must not become a tiny window. "1M" parses as 1 under a
        // naive atoi, and a 1-token context would compact on every turn —
        // far worse than the default it replaced.
        for (const char* bad : {"1M", "abc", "", "0", "-5", "12x", "1e6"}) {
            ScopedEnv e{kEnv, bad};
            CHECK(agentty::ui::resolve_context_window("priv", "ctxtest-env-3", 0, s)
                  == agentty::ui::kDefaultContextWindow);
        }
    }
}
