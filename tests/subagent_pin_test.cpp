// subagent_pin_test — a pinned Smart Mode slot must be honoured by the FIRST
// delegation, not only after a successful model-catalog fetch.
//
// The bug (reported against GitHub Copilot): the user pins Implementation to a
// specific model, Smart Mode is on, and the first `task` subagent nonetheless
// dispatches on a completely different model — one the provider may not even
// serve on that endpoint (a gpt-5.x-codex id that only the Responses API
// accepts), so the delegation 400s.
//
// Root cause: the subagent router keeps its own process-global snapshot of the
// Smart Mode config (tools::subagent::Config::smart), because `task` runs on a
// worker thread with no access to the reducer's Model. main() calls install()
// with a DEFAULT-CONSTRUCTED RoleConfig (enabled=false, no slots) and init()
// rehydrates m.d.smart from settings.json without ever pushing it down. Only
// two paths call set_smart():
//
//   1. persist_settings()  — fires when the user edits Smart Mode this session
//   2. the ModelsLoaded reducer arm — fires only on a SUCCESSFUL fetch
//
// So a user whose pins are already saved from a previous session, and whose
// catalog fetch fails / returns empty / is superseded by a provider switch
// (all three are early-returns before set_smart), delegates with
// enabled=false — i.e. the pin is silently ignored and the tier auto-router
// picks whatever it likes.
//
// This test drives the real seam: install a config the way main() does, then
// assert resolution honours the pin.

#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/domain/smart_mode.hpp"
#include "agentty/tool/subagent.hpp"

using namespace agentty;

namespace {

ModelInfo mi(std::string id, int ctx = 200000) {
    ModelInfo m;
    m.id = ModelId{std::move(id)};
    m.context_window = ctx;
    return m;
}

// The agent types the router resolves for. `Implementation` is the role
// coder/tester/general workers map to — the one in the bug report.
smart::RoleConfig pinned_impl(const std::string& model) {
    smart::RoleConfig cfg;
    cfg.enabled = true;
    cfg.implementation.model = model;
    cfg.implementation.set   = true;
    return cfg;
}

} // namespace

TEST_CASE("subagent honours a pinned Smart Mode slot from the first turn") {
    const std::vector<ModelInfo> candidates = {
        mi("gpt-5.3-codex"), mi("luna-2"), mi("gpt-4o-mini"),
    };

    // ── The resolver itself is correct: a pin wins outright ──────────
    // (Guards the contract the fix depends on — if this ever regresses,
    // pushing the config down would not be enough.)
    {
        const auto cfg = pinned_impl("luna-2");
        const auto p = smart::resolve_role(smart::ModelRole::Implementation,
                                           "gpt-5.3-codex", Effort::None,
                                           candidates, cfg);
        CHECK(p.model == "luna-2",
              "resolve_role: an explicit slot pin beats the parent model");
    }

    // ── THE BUG: main()'s install() leaves `smart` default-constructed ──
    // Reproduces the reported failure without any UI: install exactly the way
    // main() does, then ask the router what a worker would run on.
    {
        tools::subagent::install(tools::subagent::Config{
            .model = "gpt-5.3-codex",
            .installed = true,
        });
        tools::subagent::set_candidates(candidates);

        const auto cfg = tools::subagent::current();
        CHECK(!cfg.smart.enabled,
              "precondition: install() alone leaves Smart Mode off in the "
              "subagent router — this is what the fix must close");

        // With enabled=false the pin cannot be seen: every role passes through
        // to the parent model. That is the observed bug — the worker runs on
        // gpt-5.3-codex (Responses-only on Copilot) instead of the pin.
        const auto p = smart::resolve_role(smart::ModelRole::Implementation,
                                           cfg.model, Effort::None,
                                           cfg.candidates, cfg.smart);
        CHECK(p.model == "gpt-5.3-codex",
              "with a stale RoleConfig the pin is ignored (the bug)");
    }

    // ── THE FIX: startup pushes the rehydrated config down ───────────
    {
        tools::subagent::set_smart(pinned_impl("luna-2"));
        const auto cfg = tools::subagent::current();
        REQUIRE(cfg.smart.enabled);

        const auto p = smart::resolve_role(smart::ModelRole::Implementation,
                                           cfg.model, Effort::None,
                                           cfg.candidates, cfg.smart);
        CHECK(p.model == "luna-2",
              "after set_smart the first delegation honours the pin");
        CHECK(cfg.smart.implementation.set, "the slot survived the push-down");
    }

    // ── The pin must survive a FAILED catalog fetch ──────────────
    // The ModelsLoaded arm returns early on error / empty / provider mismatch,
    // so it must not be the only thing keeping the router in sync. Candidates
    // going empty (a failed fetch) must not resurrect the auto-router.
    {
        tools::subagent::set_candidates({});
        const auto cfg = tools::subagent::current();
        const auto p = smart::resolve_role(smart::ModelRole::Implementation,
                                           cfg.model, Effort::None,
                                           cfg.candidates, cfg.smart);
        CHECK(p.model == "luna-2",
              "an empty/failed catalog must not discard the user's pin");
    }

    // ── A pin is honoured even for a model absent from `candidates` ───
    // The pinned id is the user's explicit instruction, not a ranking input:
    // resolve_role must NOT fall back to the auto-router just because the
    // catalog fetch hasn't landed (or landed without that id). This is the
    // exact shape of the Copilot report — pin present, catalog incomplete.
    {
        tools::subagent::set_smart(pinned_impl("luna-2"));
        tools::subagent::set_candidates({mi("gpt-5.3-codex")});   // pin absent
        const auto cfg = tools::subagent::current();
        const auto p = smart::resolve_role(smart::ModelRole::Implementation,
                                           cfg.model, Effort::None,
                                           cfg.candidates, cfg.smart);
        CHECK(p.model == "luna-2",
              "a pin outranks the candidate list — it is an instruction, "
              "not a hint");
    }

    // ── An UNPINNED slot still auto-routes (the fix isn't a pin-everything) ──
    {
        smart::RoleConfig cfg;
        cfg.enabled = true;              // on, but no slot pinned
        tools::subagent::set_smart(cfg);
        tools::subagent::set_candidates(candidates);
        const auto c = tools::subagent::current();
        const auto p = smart::resolve_role(smart::ModelRole::Utility,
                                           "gpt-5.3-codex", Effort::None,
                                           c.candidates, c.smart);
        CHECK(!p.model.empty(),
              "an unpinned role still resolves through the auto-router");
        CHECK(p.effort == Effort::None,
              "utility workers never carry a reasoning budget");
    }
}
