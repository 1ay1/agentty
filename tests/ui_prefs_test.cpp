// Appearance settings: the rows exist, Enter changes them, and a change
// both persists and reaches the renderer.
#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/panel/form_keys.hpp"
#include "agentty/domain/ui_theme.hpp"
#include "agentty/runtime/panel/appearance.hpp"

#include <cstdio>
#include <string>

using namespace agentty;
namespace se = agentty::settings;

namespace {

// A settings store that lives in memory, so a test can assert that a row
// press actually WROTE rather than only mutated the model in hand.
store::Settings g_saved;

void install_stub_deps() {
    g_saved = store::Settings{};
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .delete_thread = [](const agentty::ThreadId&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return g_saved; },
        .save_settings = [](const store::Settings& s) { g_saved = s; },
        .new_thread_id = [] { return ThreadId{"t"}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .auth          = {},
    });
}

// Open the Appearance pane and hand back the model holding it.
Model opened() {
    auto [m, _] = app::update(Model{}, Msg{OpenAppearance{}});
    return std::move(m);
}

ui::panel::Appearance& pane(Model& m) {
    auto* o = m.ui.panel.get<ui::panel::Appearance>();
    REQUIRE(o != nullptr);
    return *o;
}

// Put the cursor on the row with `id` and send one key action, exactly as a
// keystroke would. Addressing rows BY ID rather than by index is the same
// discipline the reducer uses — a test that counts rows breaks every time
// the pane grows a section, and for no real reason.
Model press(Model m, std::string_view id, form::keys::Action a) {
    auto& f = pane(m).pane.form;
    int idx = -1;
    for (int i = 0; i < (int)f.fields.size(); ++i)
        if (f.fields[(size_t)i].id == id) { idx = i; break; }
    REQUIRE(idx >= 0);
    f.cursor = idx;
    auto [next, _] = app::update(std::move(m), Msg{AppearanceKey{a}});
    return std::move(next);
}

}  // namespace

TEST_CASE("appearance: the pane opens on a setting, not a header") {
    install_stub_deps();
    Model m = opened();
    const auto& f = pane(m).pane.form;
    REQUIRE(!f.fields.empty());
    // A cursor parked on a section header reads as a broken pane — the
    // first thing you press does nothing.
    CHECK(!f.fields[(size_t)f.cursor].is_header());
}

TEST_CASE("appearance: adjusting in place wraps") {
    install_stub_deps();
    Model m = opened();
    CHECK(m.d.ui.density == ui_prefs::Density::Normal);
    m = press(std::move(m), ui::panel::kApDensity, form::keys::Action{form::keys::Intent::AdjustUp});
    CHECK(m.d.ui.density == ui_prefs::Density::Roomy);
    m = press(std::move(m), ui::panel::kApDensity, form::keys::Action{form::keys::Intent::AdjustUp});
    CHECK(m.d.ui.density == ui_prefs::Density::Compact);
    m = press(std::move(m), ui::panel::kApDensity, form::keys::Action{form::keys::Intent::AdjustUp});
    CHECK(m.d.ui.density == ui_prefs::Density::Normal);   // wrapped
}

TEST_CASE("appearance: a toggle is a toggle") {
    install_stub_deps();
    Model m = opened();
    const bool was = m.d.ui.syntax;
    m = press(std::move(m), ui::panel::kApSyntax, form::keys::Action{form::keys::Intent::Activate});
    CHECK(m.d.ui.syntax == !was);
    m = press(std::move(m), ui::panel::kApSyntax, form::keys::Action{form::keys::Intent::Activate});
    CHECK(m.d.ui.syntax == was);
}

TEST_CASE("appearance: a change is persisted, not just held") {
    install_stub_deps();
    Model m = opened();
    m = press(std::move(m), ui::panel::kApMotion, form::keys::Action{form::keys::Intent::AdjustUp});
    // The pane has no apply step, so "what I see" and "what is saved"
    // must never be two different things — not even for one keystroke.
    CHECK(g_saved.ui.motion == m.d.ui.motion);
    CHECK(m.d.ui.motion != ui_prefs::Motion::Full);
}

TEST_CASE("appearance: the theme row hands off to a browser, not a dropdown") {
    install_stub_deps();
    Model m = opened();
    CHECK(!pane(m).pane.picking);
    m = press(std::move(m), ui::panel::kApTheme, form::keys::Action{form::keys::Intent::Activate});
    CHECK(pane(m).pane.picking);
}

TEST_CASE("appearance: moving in the browser previews the theme live") {
    install_stub_deps();
    Model m = opened();
    CHECK(m.d.ui.theme.empty());              // native by default
    m = press(std::move(m), ui::panel::kApTheme, form::keys::Action{form::keys::Intent::Activate});

    // Moving APPLIES — the list is its own preview, so you judge a scheme
    // on the real UI rather than on its name.
    {
        auto [n, _] = app::update(std::move(m), Msg{AppearanceThemeMove{+1}});
        m = std::move(n);
    }
    CHECK(!m.d.ui.theme.empty());

    // Esc is a true cancel: it puts back what we opened on, in the model
    // AND on disk, so a browse you abandoned leaves nothing behind.
    {
        auto [n, _] = app::update(std::move(m), Msg{AppearanceThemeCancel{}});
        m = std::move(n);
    }
    CHECK(m.d.ui.theme.empty());
    CHECK(g_saved.ui.theme.empty());
    CHECK(!pane(m).pane.picking);
}

TEST_CASE("appearance: Enter in the browser keeps what you are looking at") {
    install_stub_deps();
    Model m = opened();
    m = press(std::move(m), ui::panel::kApTheme, form::keys::Action{form::keys::Intent::Activate});
    {
        auto [n, _] = app::update(std::move(m), Msg{AppearanceThemeMove{+1}});
        m = std::move(n);
    }
    const std::string previewing = m.d.ui.theme;
    {
        auto [n, _] = app::update(std::move(m), Msg{AppearanceThemeCommit{}});
        m = std::move(n);
    }
    CHECK(m.d.ui.theme == previewing);
    CHECK(g_saved.ui.theme == previewing);
    CHECK(!pane(m).pane.picking);
}

TEST_CASE("appearance: typing filters and previews the top match") {
    install_stub_deps();
    Model m = opened();
    m = press(std::move(m), ui::panel::kApTheme, form::keys::Action{form::keys::Intent::Activate});
    for (const char* c : {"d", "r", "a"}) {
        auto [n, _] = app::update(std::move(m), Msg{AppearanceThemeQuery{c}});
        m = std::move(n);
    }
    CHECK(pane(m).pane.picker.query == "dra");
    // Narrowing PREVIEWS too — "dra" shows you Dracula without a second
    // keystroke to move onto it.
    CHECK(!m.d.ui.theme.empty());
}

TEST_CASE("appearance: resolution never leaves the user unable to read") {
    // A scheme needs 256 colors to be worth painting; below that native —
    // the user's own palette — is strictly better than a quantised
    // approximation, so resolve() must fall back rather than obey.
    ui_prefs::Prefs p;
    p.theme = "Dracula";
    p.tier  = ui_prefs::ColorTier::TrueColor;
    CHECK(ui_prefs::resolve(p, true).theme != &maya::theme::native);
    p.tier  = ui_prefs::ColorTier::Ansi16;
    CHECK(ui_prefs::resolve(p, true).theme == &maya::theme::native);
    // An unknown name is a config typo, not a reason to refuse to start.
    p.theme = "Nope";
    p.tier  = ui_prefs::ColorTier::TrueColor;
    CHECK(ui_prefs::resolve(p, true).theme == &maya::theme::native);
}


TEST_CASE("appearance form: rows, groups and provenance") {
    install_stub_deps();
    ui_prefs::Prefs p;
    const auto f = ui::panel::build_appearance_form(p, /*tty=*/true);

    // Every knob has a row, and the pane is grouped rather than a flat wall.
    CHECK(f.fields.size() >= 11);
    int headers = 0;
    for (const auto& r : f.fields)
        if (std::holds_alternative<form::field::Header>(r.value)) ++headers;
    CHECK(headers >= 4);

    // The theme row leads and is a Pick — 57 schemes is a searchable set,
    // not a dropdown.
    bool found_pick = false;
    for (const auto& r : f.fields)
        if (r.id == ui::panel::kApTheme)
            found_pick = std::holds_alternative<form::field::Pick>(r.value);
    CHECK(found_pick);
}

TEST_CASE("appearance: theme search is a fuzzy subsequence") {
    using ui::panel::matching_themes;
    // Empty query is a browsable catalogue, native first.
    const auto all = matching_themes("");
    CHECK(all.size() > 20);
    CHECK(all.front().empty());              // native

    // Subsequence, not substring: the point of a fuzzy picker.
    const auto gvd = matching_themes("gvd");
    bool has_gruvbox_dark = false;
    for (const auto& n : gvd) if (n == "Gruvbox Dark") has_gruvbox_dark = true;
    CHECK(has_gruvbox_dark);

    // Case-insensitive.
    CHECK(!matching_themes("DRACULA").empty());
    // A query that means a scheme does not drag native along.
    const auto dr = matching_themes("dracula");
    CHECK(!dr.empty());
    CHECK(!dr.front().empty());
}

