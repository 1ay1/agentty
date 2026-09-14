// Appearance settings: the rows exist, Enter changes them, and a change
// both persists and reaches the renderer.
#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/domain/ui_theme.hpp"

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

// Press Enter on the row whose primary label is `name`.
Model activate(Model m, const char* name) {
    auto rows = se::items_for(m, se::Category::UI);
    int idx = -1;
    for (int i = 0; i < (int)rows.size(); ++i)
        if (rows[(size_t)i].primary == name) { idx = i; break; }
    REQUIRE(idx >= 0);
    if (auto* o = m.ui.panel.get<ui::panel::SettingsList>()) o->index = idx;
    auto [next, _] = app::update(std::move(m), Msg{SettingsListActivate{}});
    return std::move(next);
}

Model opened() {
    auto [m, _] = app::update(Model{},
                              Msg{OpenSettingsList{se::Category::UI}});
    return std::move(m);
}

}  // namespace

TEST_CASE("appearance: every knob has a row") {
    install_stub_deps();
    const Model m = opened();
    const auto rows = se::items_for(m, se::Category::UI);
    CHECK(rows.size() >= 10);
    // Each row must DO something — an appearance screen of read-only text
    // would be a status page wearing a settings screen's clothes.
    for (const auto& r : rows) CHECK(r.action != se::Action::None);
}

TEST_CASE("appearance: Enter cycles, and the cycle wraps") {
    install_stub_deps();
    Model m = opened();
    CHECK(m.d.ui.density == ui_prefs::Density::Normal);
    m = activate(std::move(m), "Density");
    CHECK(m.d.ui.density == ui_prefs::Density::Roomy);
    m = activate(std::move(m), "Density");
    CHECK(m.d.ui.density == ui_prefs::Density::Compact);
    m = activate(std::move(m), "Density");
    CHECK(m.d.ui.density == ui_prefs::Density::Normal);   // wrapped
}

TEST_CASE("appearance: a toggle is a toggle") {
    install_stub_deps();
    Model m = opened();
    const bool was = m.d.ui.syntax;
    m = activate(std::move(m), "Syntax highlighting");
    CHECK(m.d.ui.syntax == !was);
    m = activate(std::move(m), "Syntax highlighting");
    CHECK(m.d.ui.syntax == was);
}

TEST_CASE("appearance: the theme cycle passes through native") {
    install_stub_deps();
    Model m = opened();
    CHECK(m.d.ui.theme.empty());              // native by default
    m = activate(std::move(m), "Theme");
    CHECK(!m.d.ui.theme.empty());             // moved onto a scheme
    // Walking the whole ring returns to native, so there is always a way
    // back to "just my terminal" without knowing a name.
    const int n = (int)std::size(maya::theme::schemes);
    for (int i = 0; i < n; ++i) m = activate(std::move(m), "Theme");
    CHECK(m.d.ui.theme.empty());
}

TEST_CASE("appearance: a change is persisted, not just held") {
    install_stub_deps();
    Model m = opened();
    m = activate(std::move(m), "Motion");
    CHECK(g_saved.ui.motion == m.d.ui.motion);
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

