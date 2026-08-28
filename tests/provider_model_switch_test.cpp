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
#include "agentty/runtime/app/update/internal.hpp"  // app::detail::fused_rows_for_model
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/provider_rows.hpp"
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
        .delete_thread  = [](const auto&) {},
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

// ^E in the model picker toggles a per-model reasoning-effort override and
// MUST give feedback: flip the catalog registry, persist to Settings, and set
// a status toast (regression guard for a use-after-move that silently dropped
// the toast). Driven through the REAL reducer via app::update.
TEST_CASE("model picker ^E toggles reasoning override + feedback") {
    namespace pick = agentty::ui::pick;
    install_stub_deps();
    g_settings = store::Settings{};
    agentty::clear_reasoning_overrides();

    // A non-chatgpt provider with a genuinely non-reasoning model (codestral,
    // a code model) highlighted in an open picker — inference does NOT light
    // it up, so ^E force-on has something to prove.
    Model m;
    m.d.available_models = { mi("codestral-latest", "mistral") };
    m.d.model_id = ModelId{"codestral-latest"};
    m.ui.model_picker = pick::OpenAt{0};

    // Baseline: inference says NOT a reasoner, so no override, no effort.
    CHECK(agentty::reasoning_override_for("codestral-latest") == -1);
    CHECK(!agentty::resolved_caps("codestral-latest").supports_effort());

    // 1st ^E: auto -> force ON.
    auto [m1, c1] = app::update(std::move(m), Msg{ModelPickerToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == 1,
          "^E forces the override on");
    CHECK(agentty::resolved_caps("codestral-latest").supports_effort(),
          "effort capability now open for the model");
    CHECK(g_settings.reasoning_effort_overrides.count("codestral-latest") == 1,
          "override persisted to Settings");
    CHECK(g_settings.reasoning_effort_overrides.at("codestral-latest"),
          "persisted value is ON");
    CHECK(!m1.s.status.empty(), "a status toast is set as feedback");

    // 2nd ^E: ON -> force OFF.
    auto [m2, c2] = app::update(std::move(m1), Msg{ModelPickerToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == 0,
          "^E again forces the override off");
    CHECK(!agentty::resolved_caps("codestral-latest").supports_effort(),
          "effort suppressed under force-off");
    CHECK(!m2.s.status.empty(), "force-off also gives feedback");

    // 3rd ^E: OFF -> back to inference (cleared).
    auto [m3, c3] = app::update(std::move(m2), Msg{ModelPickerToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("codestral-latest") == -1,
          "^E a third time clears the override (auto)");
    CHECK(g_settings.reasoning_effort_overrides.count("codestral-latest") == 0,
          "cleared override removed from Settings");
    CHECK(!m3.s.status.empty(), "clear-to-auto also gives feedback");

    agentty::clear_reasoning_overrides();   // keep global state clean
}

// ^E on a FAMILY-GATED model (Claude) is a no-op for the override but still
// gives feedback (a hint), and never writes an override.
TEST_CASE("model picker ^E on family-gated model is a hinted no-op") {
    namespace pick = agentty::ui::pick;
    install_stub_deps();
    g_settings = store::Settings{};
    agentty::clear_reasoning_overrides();

    Model m;
    m.d.available_models = { mi("claude-opus-4-5", "anthropic") };
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.ui.model_picker = pick::OpenAt{0};

    auto [m1, c1] = app::update(std::move(m), Msg{ModelPickerToggleReasoning{}});
    CHECK(agentty::reasoning_override_for("claude-opus-4-5") == -1,
          "family-gated model gets no override");
    CHECK(g_settings.reasoning_effort_overrides.empty(),
          "nothing persisted for a family-gated model");
    CHECK(!m1.s.status.empty(), "still shows a hint toast");
    CHECK(agentty::resolved_caps("claude-opus-4-5").supports_effort(),
          "Claude keeps its own family-gated effort");

    agentty::clear_reasoning_overrides();
}

TEST_CASE("provider filter: fuzzy narrows + ranks") {
    namespace P = agentty::provider;

    // Empty query = every provider, in registry order.
    {
        auto all = P::filter_provider_indices("");
        CHECK(all.size() == P::providers().size());
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            CHECK(all[static_cast<std::size_t>(i)] == i);
    }

    // A specific id floats to the front.
    {
        auto r = P::filter_provider_indices("kimi");
        CHECK(!r.empty());
        const auto ps = P::providers();
        CHECK(ps[static_cast<std::size_t>(r.front())].id == "kimi");
    }
    {
        auto r = P::filter_provider_indices("deepseek");
        CHECK(!r.empty());
        CHECK(P::providers()[static_cast<std::size_t>(r.front())].id == "deepseek");
    }

    // A label word matches even when it's not the id ("grok" -> xai row).
    {
        auto r = P::filter_provider_indices("grok");
        bool found_xai = false;
        for (int i : r) if (P::providers()[static_cast<std::size_t>(i)].id == "xai") found_xai = true;
        CHECK(found_xai);
    }

    // Gibberish matches nothing.
    CHECK(P::filter_provider_indices("zzqzzq").empty());
}

TEST_CASE("provider rows: one ordered list, filter hides non-preset rows") {
    namespace ui = agentty::ui;
    const std::vector<std::string> hosts = {"my-host.example:8443"};

    // Empty query: presets, then the custom host, then the sentinel LAST.
    {
        auto rows = ui::build_provider_rows(hosts, "");
        CHECK(!rows.empty());
        CHECK(rows.back().is_new_custom_host());
        bool saw_host = false, saw_preset = false;
        for (const auto& r : rows) {
            if (r.preset()) saw_preset = true;
            if (const auto* c = r.custom_host()) saw_host = *c == hosts[0];
        }
        CHECK(saw_preset);
        CHECK(saw_host);
    }

    // Filtered: only matching presets + the always-present sentinel; the saved
    // custom host is hidden (it isn't part of the provider text search).
    {
        auto rows = ui::build_provider_rows(hosts, "kimi");
        CHECK(rows.size() >= 2);          // >=1 preset + sentinel
        CHECK(rows.back().is_new_custom_host());
        CHECK(rows.front().preset() != nullptr);
        CHECK(rows.front().preset()->id == "kimi");
        for (const auto& r : rows)
            CHECK(r.custom_host() == nullptr);   // no saved host while filtering
    }

    // No preset matches: still exactly the sentinel, so the escape hatch
    // (open the custom-host modal) is always reachable.
    {
        auto rows = ui::build_provider_rows(hosts, "zzqzzq");
        CHECK(rows.size() == 1);
        CHECK(rows.front().is_new_custom_host());
    }
}

// Fused cross-provider picker, driven through the REAL reducer:
// open seeds catalogs + fires fetches; a same-provider Select changes the
// model in place and records the MRU; a FusedCatalogLoaded merges by id.
TEST_CASE("fused picker open, merge, same-provider switch, MRU") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic"),
                            mi("claude-opus-4", "anthropic")};

    // Open: picker opens, active provider's catalog is seeded from
    // available_models (Ready), other authed providers get Loading + a fetch.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    CHECK(ui::pick::is_open(m1.ui.fused_picker));
    bool anthropic_seeded = false;
    for (const auto& c : m1.d.provider_catalogs)
        if (c.provider_id == "anthropic") {
            anthropic_seeded = (c.state == ProviderCatalog::State::Ready
                                && c.models.size() == 2);
        }
    CHECK(anthropic_seeded);

    // The fused rows include both Anthropic models (active pinned first).
    auto rows = app::detail::fused_rows_for_model(m1);
    CHECK(rows.size() >= 2);
    CHECK(rows[0].active);
    CHECK(rows[0].model.id.value == "claude-sonnet-4-6");

    // Move to the opus row and Select: same-provider model change, no hop.
    // Find opus's index in the fused list.
    int opus_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (rows[static_cast<std::size_t>(i)].model.id.value == "claude-opus-4") {
            opus_idx = i; break;
        }
    REQUIRE(opus_idx >= 0);
    if (auto* c = ui::pick::opened(m1.ui.fused_picker)) c->index = opus_idx;

    auto [m2, c2] = app::update(std::move(m1), Msg{FusedPickerSelect{}});
    CHECK(m2.d.model_id.value == "claude-opus-4");
    CHECK(!ui::pick::is_open(m2.ui.fused_picker));       // picker closed
    // MRU recorded the switch (front = the model just selected).
    REQUIRE(!m2.d.recent_models.empty());
    CHECK(m2.d.recent_models.front().provider_id == "anthropic");
    CHECK(m2.d.recent_models.front().model_id == "claude-opus-4");
    // Persisted to settings.
    CHECK(!g_settings.recent_models.empty());
}

// A FusedCatalogLoaded for a provider merges into provider_catalogs by id and
// flips its state to Ready.
TEST_CASE("fused catalog loaded merges by provider id") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["openai"] = "sk-test";   // openai authed → catalog
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});

    // Simulate openai's catalog resolving.
    FusedCatalogLoaded loaded;
    loaded.provider_id = "openai";
    loaded.models = {mi("gpt-5-codex", "openai"), mi("gpt-4o", "openai")};
    loaded.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(loaded)});

    bool openai_ready = false;
    for (const auto& c : m2.d.provider_catalogs)
        if (c.provider_id == "openai")
            openai_ready = (c.state == ProviderCatalog::State::Ready
                            && c.models.size() == 2);
    CHECK(openai_ready);
}
