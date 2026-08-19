// provider_model_switch_test — the provider/model switching state machine.
//
// Regression locks for the field complaint "model switching is not working —
// the model changes but the provider stays the same":
//
//   1. STALE-FETCH GATE. fetch_models() stamps the provider it fetched FOR;
//      the ModelsLoaded reducer drops a payload whose provider_id doesn't
//      match the provider active at DELIVERY time. Without the gate, two
//      quick provider switches interleave their slow catalog fetches and
//      provider A's late catalog is installed under provider B — the picker
//      then offers models the active backend cannot stream (pick one and the
//      request 400s, or silently streams the wrong backend's model).
//
//   2. ACCEPTED FETCH INSTALLS THE CATALOG + auto-corrects a model id that
//      the new provider doesn't offer (first-available fallback).
//
// Driven through the REAL app::update reducer, no mocks of the reducer path.

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/selection.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace agentty;

// The ModelsLoaded reducer touches deps().load_settings() (favorites) and
// deps().save_settings() (persisting an auto-corrected model). Stub them
// with an in-memory Settings.
static store::Settings g_settings;
static void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{}}},
    });
}

static ModelInfo mi(const char* id, const char* prov) {
    ModelInfo m;
    m.id = ModelId{id};
    m.display_name = id;
    m.provider = prov;
    m.supports_tools = true;
    return m;
}

TEST_CASE("provider model switch") {
    install_stub_deps();
    // Make the active provider deterministic: the Anthropic path, whose
    // canonical id ("anthropic") is what active_provider_id() reports.
    {
        provider::Selection sel;
        sel.kind = provider::Kind::Anthropic;
        provider::select(sel);
    }

    // ── 1: a ModelsLoaded stamped for a DIFFERENT provider is dropped ──
    {
        Model m;
        m.s.models_loading = true;
        m.d.model_id = ModelId{"claude-opus-4-5"};

        ModelsLoaded stale;
        stale.models      = { mi("gpt-4o", "openai"), mi("o4-mini", "openai") };
        stale.provider_id = "openai";   // fetched for openai; anthropic active

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(stale)});
        CHECK(m2.d.available_models.empty(),
              "stale catalog must NOT be installed");
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "active model untouched by a stale catalog");
        CHECK(m2.s.models_loading,
              "loading stays armed — the newer fetch is still in flight");
    }

    // ── 2: a ModelsLoaded stamped for the ACTIVE provider installs ──
    {
        Model m;
        m.s.models_loading = true;
        m.d.model_id = ModelId{"some-stale-model"};

        ModelsLoaded fresh;
        fresh.models      = { mi("claude-opus-4-5", "anthropic"),
                              mi("claude-sonnet-4-5", "anthropic") };
        fresh.provider_id = "anthropic";

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(fresh)});
        CHECK(m2.d.available_models.size() == 2, "fresh catalog installed");
        CHECK(!m2.s.models_loading, "loading cleared on accepted fetch");
        // The stale active model isn't in the new catalog: auto-correct to
        // the first available so the next prompt can't 400.
        CHECK(m2.d.model_id.value == "claude-opus-4-5",
              "model auto-corrected to a catalog member");
    }

    // ── 3: legacy/synthetic dispatch (empty provider_id) still accepted ──
    {
        Model m;
        m.s.models_loading = true;

        ModelsLoaded legacy;
        legacy.models = { mi("claude-opus-4-5", "anthropic") };
        // provider_id left empty

        auto [m2, cmd] = app::update(std::move(m), Msg{std::move(legacy)});
        CHECK(m2.d.available_models.size() == 1,
              "unstamped payload accepted (back-compat)");
        CHECK(!m2.s.models_loading, "loading cleared");
    }
}
