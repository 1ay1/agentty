// settings_rows_input_test — every editable row on every settings pane takes
// input through the real key path (on_key -> update), not just the reducer.
//
// For each row: an in-place kind (toggle, choice, slider, number) must change
// on → or ←; a text kind must enter an edit on a typed letter, keep the edit
// across several keys, and leave it on Enter. A row that ignores all of that
// is a dead field.

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/form.hpp"
#include "agentty/runtime/panel/rag.hpp"
#include "agentty/runtime/panel/settings/categories.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/panel/top.hpp"

#include <maya/terminal/input.hpp>

#include <functional>
#include <string>
#include <vector>

namespace pn = agentty::ui::panel;
using namespace agentty;

namespace {

store::Settings g_settings;

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](const maya::unlocked&, auto, auto) {},
        .save_thread    = [](const Thread&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& s) { g_settings = s; },
        .new_thread_id  = [] { return ThreadId{"t-rows"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

maya::KeyEvent sk(maya::SpecialKey k) { return maya::KeyEvent{k, {}, {}}; }
maya::KeyEvent ch(char32_t c) { return maya::KeyEvent{maya::CharKey{c}, {}, {}}; }

Model press(Model m, const maya::KeyEvent& ev) {
    if (auto msg = app::on_key(m, ev))
        m = app::update(std::move(m), std::move(*msg)).first;
    return m;
}

using FormOf = std::function<form::Form*(Model&)>;

std::string show(const form::FieldValue& v) {
    return std::visit([](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, form::field::Toggle>) return x.on ? "on" : "off";
        else if constexpr (std::is_same_v<T, form::field::Choice>) return std::to_string(x.index);
        else if constexpr (std::is_same_v<T, form::field::Number>) return std::to_string(x.value);
        else if constexpr (std::is_same_v<T, form::field::Slider>) return std::to_string(x.value);
        else if constexpr (requires { x.value; }) return std::string{x.value};
        else return "";
    }, v);
}

// Visit every row in turn and exercise each one.
void exercise(const std::string pane, Msg open, const FormOf& form_of,
              const std::vector<std::string>& skip = {}, Model start = {}) {
    install_stub_deps();
    Model m = app::update(std::move(start), std::move(open)).first;
    form::Form* f = form_of(m);
    REQUIRE_MESSAGE(f != nullptr, pane);
    const auto kind = pn::top(m);
    int checked = 0;

    for (int at = 0; at < 200; ++at) {
        f = form_of(m);
        REQUIRE_MESSAGE(f != nullptr, pane << ": pane closed by a key");
        if (at >= static_cast<int>(f->fields.size())) break;
        if (f->fields[static_cast<std::size_t>(at)].is_header()) continue;
        f->cursor = at;
        const auto* row = f->focused();
        if (!row) break;
        const std::string id = row->id;

        const bool skipped = std::find(skip.begin(), skip.end(), id) != skip.end();
        if (row->is_pick() && !skipped) {
            // A pick hands off on Enter: something else must take the
            // keyboard, and Esc must come back to this pane.
            m = press(std::move(m), sk(maya::SpecialKey::Enter));
            const bool moved = pn::top(m) != kind || [&] {
                auto* g = form_of(m);
                return g && g->choosing();
            }();
            bool sub_open = false;
            if (auto* a = m.ui.panel.get<pn::Appearance>()) sub_open = a->pane.picking;
            CHECK_MESSAGE((moved || sub_open), pane << " / " << id << ": Enter on a pick did nothing");
            for (int i = 0; i < 3 && pn::top(m) != kind; ++i)
                m = press(std::move(m), sk(maya::SpecialKey::Escape));
            if (auto* a = m.ui.panel.get<pn::Appearance>(); a && a->pane.picking)
                m = press(std::move(m), sk(maya::SpecialKey::Escape));
            REQUIRE_MESSAGE(pn::top(m) == kind, pane << " / " << id << ": Esc did not come back");
            ++checked;
            continue;
        }
        if (!row->editable())
            MESSAGE(pane << " / " << id << ": not editable ("
                    << (row->locked ? "locked: " + row->locked_reason
                        : row->is_pick() ? std::string{"pick"}
                        : row->is_action() ? std::string{"action"} : std::string{"?"}) << ")");
        if (row->editable() && !skipped) {
            const std::string before = show(row->value);
            if (row->is_text_like()) {
                // Type-to-edit, two more keys, then Enter leaves the edit.
                m = press(std::move(m), ch(U'7'));
                m = press(std::move(m), ch(U'x'));
                m = press(std::move(m), sk(maya::SpecialKey::Backspace));
                f = form_of(m);
                REQUIRE(f != nullptr);
                CHECK_MESSAGE(f->editing(), pane << " / " << id << ": typing did not start an edit");
                m = press(std::move(m), sk(maya::SpecialKey::Enter));
                f = form_of(m);
                REQUIRE(f != nullptr);
                CHECK_MESSAGE(!f->editing(), pane << " / " << id << ": Enter did not end the edit");
                ++checked;
            } else {
                m = press(std::move(m), sk(maya::SpecialKey::Right));
                f = form_of(m);
                REQUIRE(f != nullptr);
                std::string after = f->find(id) ? show(f->find(id)->value) : before;
                if (after == before) {   // already at max: try the other way
                    m = press(std::move(m), sk(maya::SpecialKey::Left));
                    f = form_of(m);
                    REQUIRE(f != nullptr);
                    after = f->find(id) ? show(f->find(id)->value) : before;
                }
                CHECK_MESSAGE(after != before, pane << " / " << id << ": arrows did nothing");
                ++checked;
            }
        }
        // Still on this pane, cursor still where we left it?
        REQUIRE_MESSAGE(pn::top(m) == kind, pane << " / " << id << ": a key left the pane");
    }
    CHECK_MESSAGE(checked > 0, pane << ": no editable rows exercised");
    MESSAGE(pane << ": " << checked << " rows exercised");
}

}  // namespace

TEST_CASE("settings rows: every plugin add-form row takes input") {
    // The add form opens from the Plugins list with `a`.
    install_stub_deps();
    Model m = app::update(Model{}, Msg{OpenSettingsList{settings::Category::Plugins}}).first;
    m = press(std::move(m), ch(U'a'));
    REQUIRE(m.ui.panel.get<pn::PluginEdit>() != nullptr);
    exercise("Plugin add", Msg{SettingsListMove{0}},
             [](Model& mm) -> form::Form* {
                 auto* o = mm.ui.panel.get<pn::PluginEdit>();
                 return o ? &o->form : nullptr;
             }, {}, std::move(m));
}

TEST_CASE("settings rows: list keys move, open and come back") {
    install_stub_deps();
    for (auto c : {settings::Category::General, settings::Category::Plugins,
                   settings::Category::Commands, settings::Category::Agents,
                   settings::Category::Hooks}) {
        Model m = app::update(Model{}, Msg{OpenSettingsList{c}}).first;
        const auto* o = m.ui.panel.get<pn::SettingsList>();
        REQUIRE(o != nullptr);
        const int n = static_cast<int>(settings::items_for(m, c).size());
        const int start = o->index;
        m = press(std::move(m), sk(maya::SpecialKey::Down));
        o = m.ui.panel.get<pn::SettingsList>();
        REQUIRE(o != nullptr);
        if (n > 1) CHECK_MESSAGE(o->index != start, settings::label(c) << ": Down did nothing");
        m = press(std::move(m), sk(maya::SpecialKey::Escape));
        CHECK_MESSAGE(m.ui.panel.get<pn::SettingsList>() == nullptr,
                      settings::label(c) << ": Esc did not close");
    }
}

TEST_CASE("settings rows: a paste lands in the open pane, never the composer") {
    // Appearance and Sandbox were missing from the paste router, and RAG,
    // Smart Mode and Plugin only took a paste mid-edit; the rest fell
    // through to the composer hidden behind the pane.
    install_stub_deps();
    const auto composer_after = [](Msg open, Model start = {}) {
        Model m = app::update(std::move(start), std::move(open)).first;
        const std::string before = m.ui.composer.text;
        auto msg = app::on_paste(m, maya::PasteEvent{"pasted"});
        m = app::update(std::move(m), std::move(msg)).first;
        return m.ui.composer.text == before;
    };
    Model sb;
    sb.d.persisted.sandbox.configured = true;
    CHECK_MESSAGE(composer_after(Msg{OpenAppearance{}}), "Appearance");
    CHECK_MESSAGE(composer_after(Msg{OpenSandbox{}}, std::move(sb)), "Sandbox");
    CHECK_MESSAGE(composer_after(Msg{OpenSmartMode{}}), "Smart Mode");
    CHECK_MESSAGE(composer_after(Msg{OpenRag{}}), "RAG");
    CHECK_MESSAGE(composer_after(Msg{OpenWebSearch{}}), "Web Search");
}

TEST_CASE("settings rows: every Appearance row takes input") {
    // The theme row hands off to a browser (Enter), not arrows.
    exercise("Appearance", Msg{OpenAppearance{}},
             [](Model& m) -> form::Form* {
                 auto* o = m.ui.panel.get<pn::Appearance>();
                 return o ? &o->pane.form : nullptr;
             });
}

TEST_CASE("settings rows: every Sandbox row takes input") {
    // Under bwrap most rows are locked on purpose. Pick claybin the way a
    // user does, then every row it unlocks must take input.
    Model m;
    m.d.persisted.sandbox.backend = sandbox_cfg::LinuxBackend::Claybin;
    m.d.persisted.sandbox.configured = true;
    exercise("Sandbox", Msg{OpenSandbox{}},
             [](Model& m) -> form::Form* {
                 auto* o = m.ui.panel.get<pn::Sandbox>();
                 return o ? &o->pane.form : nullptr;
             }, {}, std::move(m));
}

TEST_CASE("settings rows: every Smart Mode row takes input") {
    exercise("Smart Mode", Msg{OpenSmartMode{}},
             [](Model& m) -> form::Form* {
                 auto* o = m.ui.panel.get<pn::SmartMode>();
                 return o ? &o->form : nullptr;
             });
}

TEST_CASE("settings rows: every RAG row takes input") {
    exercise("RAG", Msg{OpenRag{}},
             [](Model& m) -> form::Form* {
                 auto* o = m.ui.panel.get<pn::Rag>();
                 return o ? &o->embed.form : nullptr;
             });
}

TEST_CASE("settings rows: every Web Search row takes input") {
    exercise("Web Search", Msg{OpenWebSearch{}},
             [](Model& m) -> form::Form* {
                 auto* o = m.ui.panel.get<pn::WebSearch>();
                 return o ? &o->form : nullptr;
             });
}
