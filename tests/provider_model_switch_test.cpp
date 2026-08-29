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
#include "agentty/domain/bundled_catalog.hpp"

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
    // Hermetic auth: don't depend on real on-disk Anthropic creds (absent on
    // CI). A provider_keys entry makes provider_is_authed("anthropic") true.
    g_settings.provider_keys["anthropic"] = "sk-test";
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
    g_settings.provider_keys["anthropic"] = "sk-test";  // hermetic auth
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

// The fused picker caches its rows (m.d.fused_rows) for cheap per-frame /
// per-keystroke rendering: populated on open, cleared on close/select.
TEST_CASE("fused picker caches rows and clears them on close") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    g_settings.provider_keys["xai"]       = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};

    // Open is INSTANT: only the active provider (seeded from available_models)
    // has models; every other authed provider is empty + Loading and streams
    // in via the async fetch. The cache reflects exactly that on open.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    CHECK(!m1.d.fused_rows.empty());                 // active provider shows now
    bool anthropic_ready = false, xai_pending = false;
    for (const auto& c : m1.d.provider_catalogs) {
        if (c.provider_id == "anthropic")
            anthropic_ready = (c.state == ProviderCatalog::State::Ready
                               && !c.models.empty());
        if (c.provider_id == "xai")
            xai_pending = c.models.empty();          // not seeded synchronously
    }
    CHECK(anthropic_ready);
    CHECK(xai_pending);

    // xai's catalog streams in → rows rebuild to include it.
    FusedCatalogLoaded xai;
    xai.provider_id = "xai";
    xai.models = {mi("grok-4", "xai"), mi("grok-3", "xai")};
    xai.ok = true;
    auto [m1b, c1b] = app::update(std::move(m1), Msg{std::move(xai)});
    bool xai_now = false;
    for (const auto& c : m1b.d.provider_catalogs)
        if (c.provider_id == "xai") xai_now = (c.models.size() == 2);
    CHECK(xai_now);

    // Filtering rebuilds the cache in place (cursor clamped, still open).
    auto [m2, c2] = app::update(std::move(m1b),
                                Msg{FusedPickerFilterInput{U'c'}});
    CHECK(ui::pick::is_open(m2.ui.fused_picker));

    // Close releases the cache.
    auto [m3, c3] = app::update(std::move(m2), Msg{CloseFusedPicker{}});
    CHECK(!ui::pick::is_open(m3.ui.fused_picker));
    CHECK(m3.d.fused_rows.empty());
}

// A digit 1-9 on the UNFILTERED fused list jumps to the Nth row; once a
// query is being typed, digits are search text (so "gpt5" still works).
TEST_CASE("fused picker number quick-select") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    g_settings.provider_keys["xai"]       = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});

    FusedCatalogLoaded xai;
    xai.provider_id = "xai";
    xai.models = {mi("grok-4", "xai"), mi("grok-3", "xai")};
    xai.ok = true;
    auto [m2, c2] = app::update(std::move(m1), Msg{std::move(xai)});
    REQUIRE(m2.d.fused_rows.size() >= 3);

    // '3' on the empty query jumps to the 3rd row (index 2), not into search.
    auto [m3, c3] = app::update(std::move(m2), Msg{FusedPickerFilterInput{U'3'}});
    const auto* c = ui::pick::opened(m3.ui.fused_picker);
    REQUIRE(c != nullptr);
    CHECK(c->index == 2);
    CHECK(c->query.empty());          // digit did NOT enter the query

    // Once a query exists, a digit is search text (jump is disabled).
    auto [m4, c4] = app::update(std::move(m3), Msg{FusedPickerFilterInput{U'g'}});
    auto [m5, c5] = app::update(std::move(m4), Msg{FusedPickerFilterInput{U'3'}});
    const auto* c2p = ui::pick::opened(m5.ui.fused_picker);
    REQUIRE(c2p != nullptr);
    CHECK(c2p->query == "g3");        // digit appended, not a jump
}

// A filtered fused list carries per-row fuzzy-match byte offsets into the
// model NAME, so the view can highlight WHY each row matched (fzf-style).
TEST_CASE("fused rows expose name match positions for highlight") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    m.d.available_models = {mi("claude-sonnet-4-5", "anthropic"),
                            mi("claude-opus-4-5", "anthropic")};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});

    // No query → no highlight offsets.
    for (const auto& r : m1.d.fused_rows)
        CHECK(r.match_positions.empty());

    // Type "son" → the sonnet row gains match offsets into its name.
    Model m2 = std::move(m1);
    for (char ch : std::string{"son"}) {
        auto [n, c] = app::update(std::move(m2),
                                  Msg{FusedPickerFilterInput{static_cast<char32_t>(ch)}});
        m2 = std::move(n);
    }
    bool sonnet_has_hl = false;
    for (const auto& r : m2.d.fused_rows)
        if (r.model.id.value == "claude-sonnet-4-5" && !r.match_positions.empty())
            sonnet_has_hl = true;
    CHECK(sonnet_has_hl);
}

// ^Tab walks the WHOLE MRU ring (A→B→C→A), progressively older, without
// reordering the ring — not a single A↔B toggle.
TEST_CASE("^Tab cycles the MRU ring without reordering") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-a"};
    m.d.available_models = {mi("claude-a", "anthropic"),
                            mi("claude-b", "anthropic"),
                            mi("claude-c", "anthropic")};
    // MRU ring: active at front, then progressively older.
    m.d.recent_models = {ModelRef{"anthropic", "claude-a"},
                         ModelRef{"anthropic", "claude-b"},
                         ModelRef{"anthropic", "claude-c"}};

    auto step = [](Model mm) {
        auto [n, c] = app::update(std::move(mm), Msg{SwitchToPreviousModel{}});
        return std::move(n);
    };

    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-b");            // A → B
    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-c");            // B → C
    m = step(std::move(m));
    CHECK(m.d.model_id.value == "claude-a");            // C → A (wrap)

    // The ring itself never reordered — that's what makes the walk stable.
    REQUIRE(m.d.recent_models.size() == 3);
    CHECK(m.d.recent_models[0].model_id == "claude-a");
    CHECK(m.d.recent_models[1].model_id == "claude-b");
    CHECK(m.d.recent_models[2].model_id == "claude-c");
}

// The single bundled catalog is THE offline floor for every provider — one
// source, no per-site drift. Sanity-check its shape + that the Anthropic
// flagship (Fable) and its [1m] companion are present.
TEST_CASE("bundled catalog is the single provider floor") {
    using agentty::catalog::bundled;

    const auto anth = bundled("anthropic");
    bool fable = false, fable_1m = false, opus = false;
    for (const auto& mi : anth) {
        if (mi.id.value == "claude-fable-5")     fable = true;
        if (mi.id.value == "claude-fable-5[1m]") fable_1m = true;
        if (mi.id.value == "claude-opus-4-5")    opus = true;
        CHECK(mi.provider == "anthropic");
    }
    CHECK(fable);        // the flagship the fused picker was missing
    CHECK(fable_1m);     // add_1m_variants companion
    CHECK(opus);

    // Other providers resolve through the SAME function (no separate tables).
    CHECK(!bundled("xai").empty());
    CHECK(!bundled("chatgpt").empty());
    CHECK(!bundled("copilot").empty());
    CHECK(!bundled("kimi").empty());
    // Unknown / user-defined catalogs have no floor.
    CHECK(bundled("openrouter").empty());
    CHECK(bundled("some-custom-host").empty());
}

// The active provider's fused catalog MIRRORS available_models on every open,
// so a model that appears in available_models later (e.g. the live /v1/models
// fetch lands a new flagship after the first open seeded the bundled list)
// also shows in the fused picker — it must not freeze on the first snapshot.
TEST_CASE("fused active catalog re-seeds when available_models grows") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.available_models = {mi("claude-opus-4-5", "anthropic"),
                            mi("claude-sonnet-4-5", "anthropic")};

    // First open seeds the fused catalog from the current (Fable-less) list.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    auto [m2, c2] = app::update(std::move(m1), Msg{CloseFusedPicker{}});

    // The live fetch lands a newly-listed flagship into available_models.
    m2.d.available_models.push_back(mi("claude-fable-5", "anthropic"));

    // Re-open: the active catalog must MIRROR the grown available_models.
    auto [m3, c3] = app::update(std::move(m2), Msg{OpenFusedPicker{}});
    bool fable_listed = false;
    for (const auto& cat : m3.d.provider_catalogs)
        if (cat.provider_id == "anthropic")
            for (const auto& mo : cat.models)
                if (mo.id.value == "claude-fable-5") fable_listed = true;
    CHECK(fable_listed);

    bool fable_row = false;
    for (const auto& r : m3.d.fused_rows)
        if (r.model.id.value == "claude-fable-5") fable_row = true;
    CHECK(fable_row);
}

// The provider picker's ^D (Mac-reachable stand-in for forward-Delete)
// signs out of a preset that has a saved key: first ^D arms, second commits
// (clears the key). openrouter is the reported case.
TEST_CASE("provider picker: ^D signs out of a keyed preset (two-press)") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"]  = "sk-a";
    g_settings.provider_keys["openrouter"] = "sk-or";   // keyed preset
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenProviderPicker{}});

    // Point the cursor at the openrouter row deterministically by locating
    // it in the built row list, then set the picker index.
    auto* p = ui::pick::opened(m1.ui.provider_picker);
    REQUIRE(p != nullptr);
    const auto rows = ui::build_provider_rows(
        agentty::provider::saved_custom_hosts(g_settings.provider_keys), "");
    int or_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (const auto* pr = rows[static_cast<std::size_t>(i)].preset();
            pr && std::string{pr->id} == "openrouter") { or_idx = i; break; }
    REQUIRE(or_idx >= 0);
    p->index = or_idx;
    p->query.clear();
    Model m2 = std::move(m1);

    // First ^D arms the sign-out; the key is still present.
    auto [m3, c3] = app::update(std::move(m2), Msg{ProviderPickerDelete{}});
    CHECK(g_settings.provider_keys.count("openrouter") == 1);

    // Second ^D on the same row commits the sign-out.
    auto [m4, c4] = app::update(std::move(m3), Msg{ProviderPickerDelete{}});
    CHECK(g_settings.provider_keys.count("openrouter") == 0);  // signed out
    CHECK(g_settings.provider_keys.count("anthropic") == 1);   // others intact
}

// Enter on an ACCOUNT-CAPABLE provider that is already active opens its
// accounts drill-down (Esc from there closes the whole picker — handled by
// login_back → close_login for AccountList). Non-account providers switch.
TEST_CASE("provider picker: Enter opens accounts on active OAuth provider") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-a";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-5"};
    auto [m1, c1] = app::update(std::move(m), Msg{OpenProviderPicker{}});

    // Land the cursor on the (active) anthropic row.
    auto* p = ui::pick::opened(m1.ui.provider_picker);
    REQUIRE(p != nullptr);
    const auto rows = ui::build_provider_rows(
        agentty::provider::saved_custom_hosts(g_settings.provider_keys), "");
    int a_idx = -1;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
        if (const auto* pr = rows[static_cast<std::size_t>(i)].preset();
            pr && std::string{pr->id} == "anthropic") { a_idx = i; break; }
    REQUIRE(a_idx >= 0);
    p->index = a_idx;

    // Enter opens the accounts list (and closes the provider picker).
    auto [m2, c2] = app::update(std::move(m1), Msg{ProviderPickerSelect{}});
    CHECK(std::holds_alternative<ui::login::AccountList>(m2.ui.login));
    CHECK(!ui::pick::opened(m2.ui.provider_picker));  // picker closed
}

// The fused picker tunes reasoning effort (←/→) on the highlighted model,
// ported from the old model picker so the fused surface is complete.
TEST_CASE("fused picker cycles reasoning effort") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    // An effort-capable model (Claude Opus supports the reasoning ladder).
    m.d.model_id = ModelId{"claude-opus-4-5"};
    m.d.effort = Effort::None;
    m.d.available_models = {mi("claude-opus-4-5", "anthropic")};

    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    // Cursor on the active (only) model row.
    if (auto* cur = ui::pick::opened(m1.ui.fused_picker)) cur->index = 0;
    const Effort before = m1.d.effort;
    auto [m2, c2] = app::update(std::move(m1), Msg{FusedPickerCycleEffort{+1}});
    // ←/→ mutates the GLOBAL m.d.effort LIVE — identical to the classic model
    // picker, so the two surfaces share one state and can't disagree.
    CHECK(m2.d.effort != before);
    CHECK(m2.ui.effort_dirty);
    const Effort after = m2.d.effort;

    // The change is already global; select just persists + switches.
    auto [m3, c3] = app::update(std::move(m2), Msg{FusedPickerSelect{}});
    CHECK(m3.d.effort == after);
    CHECK(!ui::pick::opened(m3.ui.fused_picker));  // picker closed on select

    // Closing flushes a dirty effort edit (parity with the classic picker).
    auto [m4, c4] = app::update(std::move(m3), Msg{OpenFusedPicker{}});
    if (auto* cur = ui::pick::opened(m4.ui.fused_picker)) cur->index = 0;
    auto [m5, c5] = app::update(std::move(m4), Msg{FusedPickerCycleEffort{+1}});
    auto [m6, c6] = app::update(std::move(m5), Msg{CloseFusedPicker{}});
    CHECK(!m6.ui.effort_dirty);            // persisted on close
}

// ^/ toggles between the fused (all-providers) picker and the classic
// single-provider picker: opening one tears the other down cleanly.
TEST_CASE("fused and classic model pickers toggle, not stack") {
    using namespace agentty::msg;
    install_stub_deps();
    g_settings = store::Settings{};
    g_settings.provider_keys["anthropic"] = "sk-test";
    provider::select(provider::parse_selection("anthropic"));

    Model m;
    m.d.model_id = ModelId{"claude-sonnet-4-6"};
    m.d.available_models = {mi("claude-sonnet-4-6", "anthropic")};

    // ^/ once: fused open.
    auto [m1, c1] = app::update(std::move(m), Msg{OpenFusedPicker{}});
    CHECK(ui::pick::is_open(m1.ui.fused_picker));
    CHECK(!ui::pick::is_open(m1.ui.model_picker));

    // ^/ twice: classic opens, fused closes (+ its cache released).
    auto [m2, c2] = app::update(std::move(m1), Msg{OpenModelPicker{}});
    CHECK(ui::pick::is_open(m2.ui.model_picker));
    CHECK(!ui::pick::is_open(m2.ui.fused_picker));
    CHECK(m2.d.fused_rows.empty());

    // ^/ again: back to fused, classic closes.
    auto [m3, c3] = app::update(std::move(m2), Msg{OpenFusedPicker{}});
    CHECK(ui::pick::is_open(m3.ui.fused_picker));
    CHECK(!ui::pick::is_open(m3.ui.model_picker));
}
