// slash_menu_test — the composer's `/` menu and the one slash resolver.
//
// Two layers. The resolver is pure and tested directly. Everything a user
// sees goes through the real key path (on_key -> update), because the bug
// this replaces only showed up there: `/` used to open the action palette
// and the typed name never reached the composer.

#include "agtest.hpp"

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/panel/top.hpp"
#include "agentty/runtime/slash.hpp"
#include "agentty/runtime/view/slash_menu.hpp"
#include "agtest_fx.hpp"

#include <maya/terminal/input.hpp>

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
        .new_thread_id  = [] { return ThreadId{"t-slash"}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = {},
    });
}

maya::KeyEvent sk(maya::SpecialKey k) { return maya::KeyEvent{k, {}, {}}; }
maya::KeyEvent ch(char32_t c) { return maya::KeyEvent{maya::CharKey{c}, {}, {}}; }

Model press(Model m, const maya::KeyEvent& ev) {
    if (auto msg = app::on_key(m, ev)) {
        auto owned = std::move(*msg);
        m = app::update(std::move(m), std::move(owned)).first;
    }
    // Typing can kick a background OAuth refresh, which queues a send until
    // it answers. No refresh runs here, so let the send go through.
    m.s.oauth_refresh_in_flight = false;
    return m;
}
Model type(Model m, std::u32string_view s) {
    for (char32_t c : s) m = press(std::move(m), ch(c));
    return m;
}

// A model with one user command, one skill and one MCP prompt.
Model fresh() {
    install_stub_deps();
    Model m;
    m.d.model_id.value = "test-model";
    tools::commands::Command c;
    c.name = "summarize";
    c.description = "Summarize a file";
    c.argument_hint = "<file>";
    c.body = "Summarize $ARGUMENTS please";
    m.ui.library.commands.push_back(c);
    tools::skills::Skill s;
    s.name = "prose";
    s.description = "house style";
    s.body = "Short sentences.";
    m.ui.library.skills.push_back(s);
    mcp::PromptState p;
    p.name = "gh:issue";
    p.description = "Draft an issue";
    p.hint = "<title>";
    m.ui.plugins.prompts.push_back(p);
    return m;
}

std::vector<std::string> names(const Model& m) {
    const auto cat = slash::catalog_of(m);
    std::vector<std::string> out;
    for (const auto& r : slash::menu_rows(m, cat)) out.push_back(r.entry->name);
    return out;
}

// The text of the last user message the transcript got.
std::optional<std::string> sent(const Model& m) {
    for (auto it = m.d.current.messages.rbegin(); it != m.d.current.messages.rend(); ++it)
        if (it->role == Role::User) return it->text;
    return std::nullopt;
}

}  // namespace

// ── the resolver ────────────────────────────────────────────────────────

TEST_CASE("slash: resolve picks the kind, plain text falls through") {
    const auto cat = slash::make_catalog({{"summarize", "", ""}}, {{"prose", "", ""}},
                                         {{"gh:issue", "", ""}});
    using W = slash::Resolved::What;
    CHECK(slash::resolve("/new", cat).what == W::Action);
    CHECK(slash::resolve("/summarize a.cpp", cat).what == W::Command);
    CHECK(slash::resolve("/summarize a.cpp", cat).args == "a.cpp");
    CHECK(slash::resolve("/prose tighten this", cat).what == W::Skill);
    CHECK(slash::resolve("/gh:issue crash on start", cat).what == W::Prompt);
    CHECK(slash::resolve("/etc/hosts is odd", cat).what == W::Plain);
    CHECK(slash::resolve("/undo", cat).what == W::Plain);   // no guessing
    CHECK(slash::resolve(" /new", cat).what == W::Plain);   // leading space: prose
    CHECK(slash::resolve("/", cat).what == W::Plain);
}

TEST_CASE("slash: a name clash goes to the earlier source") {
    // A user command named like a built-in doesn't replace the built-in.
    const auto cat = slash::make_catalog({{"new", "mine", ""}}, {{"new", "", ""}}, {});
    CHECK(slash::resolve("/new", cat).what == slash::Resolved::What::Action);
    int n = 0;
    for (const auto& e : cat.entries) n += e.name == "new";
    CHECK(n == 1);
}

TEST_CASE("slash: menu shows only while typing the leading name") {
    CHECK(slash::menu_query("/", 1).has_value());
    CHECK(slash::menu_query("/re", 3)->query == "re");
    CHECK(slash::menu_query("/re", 2)->query == "re");           // caret inside the name
    CHECK_FALSE(slash::menu_query("/re x", 5).has_value());      // past the name
    CHECK_FALSE(slash::menu_query("/etc/hosts", 10).has_value()); // a path
    CHECK_FALSE(slash::menu_query("hi /re", 6).has_value());     // not leading
    CHECK_FALSE(slash::menu_query("x\n/re", 5).has_value());     // not line one
    CHECK_FALSE(slash::menu_query("/re", 0).has_value());        // caret before the '/'
}

TEST_CASE("slash: filter ranks exact, then prefix, then fuzzy") {
    const auto cat = slash::make_catalog({{"rebase-all", "", ""}, {"fork-and-rename", "", ""}}, {}, {});
    const auto rows = slash::filter(cat, "re");
    REQUIRE(rows.size() >= 3);
    // Every name that starts with "re" comes before one that only contains it.
    bool seen_fuzzy = false;
    for (const auto& r : rows) {
        const bool prefix = r.entry->name.starts_with("re");
        if (!prefix) seen_fuzzy = true;
        else CHECK_FALSE(seen_fuzzy);
    }
    CHECK(slash::filter(cat, "rename").front().entry->name == "rename");   // exact first
}

TEST_CASE("slash: complete replaces the name and keeps what follows") {
    slash::Entry e; e.name = "summarize";
    auto c = slash::complete("/su", e);
    CHECK(c.text == "/summarize ");
    CHECK(c.cursor == 11);
    c = slash::complete("/su x.cpp", e);
    CHECK(c.text == "/summarize x.cpp");
    CHECK(c.cursor == 11);
}

// ── through the keys ────────────────────────────────────────────────────

TEST_CASE("slash menu: typed text stays in the composer") {
    auto m = type(fresh(), U"/su");
    CHECK(m.ui.composer.text == "/su");
    CHECK(ui::panel::top(m) == ui::panel::Kind::None);   // no palette took over
    CHECK(names(m).front() == "summarize");
    CHECK(ui::slash_menu_open(m));
}

TEST_CASE("slash menu: Tab completes, Enter on the full name sends the template") {
    auto m = type(fresh(), U"/su");
    m = press(std::move(m), sk(maya::SpecialKey::Tab));
    CHECK(m.ui.composer.text == "/summarize ");
    CHECK_FALSE(ui::slash_menu_open(m));   // past the name now
    m = type(std::move(m), U"a.cpp");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(sent(m) == "Summarize a.cpp please");
}

TEST_CASE("slash menu: Enter on a partial name completes instead of guessing") {
    auto m = type(fresh(), U"/su");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(m.ui.composer.text == "/summarize ");
    CHECK_FALSE(sent(m).has_value());   // nothing sent
}

TEST_CASE("slash menu: arrows pick a row, Tab takes it") {
    auto m = type(fresh(), U"/");
    const auto all = names(m);
    REQUIRE(all.size() > 2);
    m = press(std::move(m), sk(maya::SpecialKey::Down));
    m = press(std::move(m), sk(maya::SpecialKey::Down));
    CHECK(m.ui.composer.slash.index == 2);
    m = press(std::move(m), sk(maya::SpecialKey::Tab));
    CHECK(m.ui.composer.text == "/" + all[2] + " ");
    // Up wraps from the top, same as every list.
    m = type(fresh(), U"/");
    m = press(std::move(m), sk(maya::SpecialKey::Up));
    CHECK(m.ui.composer.slash.index == static_cast<int>(all.size()) - 1);
}

TEST_CASE("slash menu: Esc hides it and keeps the text; editing the name brings it back") {
    auto m = type(fresh(), U"/su");
    m = press(std::move(m), sk(maya::SpecialKey::Escape));
    CHECK(m.ui.composer.text == "/su");
    CHECK_FALSE(ui::slash_menu_open(m));
    m = type(std::move(m), U"m");
    CHECK(ui::slash_menu_open(m));
}

TEST_CASE("slash menu: a path or unknown name is sent as typed") {
    auto m = type(fresh(), U"/etc/hosts");
    CHECK_FALSE(ui::slash_menu_open(m));
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(sent(m) == "/etc/hosts");

    m = type(fresh(), U"/undo");
    CHECK_FALSE(ui::slash_menu_open(m));   // no match: menu closes itself
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(sent(m) == "/undo");
}

TEST_CASE("slash menu: a built-in runs and leaves no message") {
    auto m = type(fresh(), U"/threads");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(ui::panel::top(m) == ui::panel::Kind::ThreadList);
    CHECK(m.ui.composer.text.empty());
    CHECK_FALSE(sent(m).has_value());
}

TEST_CASE("slash menu: an action given arguments it doesn't take keeps the text") {
    auto m = type(fresh(), U"/threads now");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(m.ui.composer.text == "/threads now");
    CHECK(ui::panel::top(m) == ui::panel::Kind::None);
}

TEST_CASE("slash menu: /rename <title> renames the open thread") {
    auto m = fresh();
    m.d.current.id = ThreadId{"t-r"};
    Message u; u.role = Role::User; u.text = "hello";
    m.d.current.messages.push_back(u);
    m.d.threads.push_back(m.d.current);
    m = type(std::move(m), U"/rename Login bug");
    agtest::fx::Store fx;
    {
        auto msg = app::on_key(m, sk(maya::SpecialKey::Enter));
        REQUIRE(msg);
        auto [next, cmd] = app::update(std::move(m), std::move(*msg));
        m = std::move(next);
        agtest::fx::run(cmd, fx);
    }
    CHECK(m.d.current.title == "Login bug");
    CHECK(m.d.current.renamed_at > 0);
    CHECK(m.d.threads[0].title == "Login bug");
    CHECK(m.ui.composer.text.empty());
}

TEST_CASE("slash menu: a skill loads its instructions") {
    auto m = type(fresh(), U"/prose tighten this");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    const auto s = sent(m);
    REQUIRE(s);
    CHECK(s->find("Short sentences.") != std::string::npos);
    CHECK(s->find("tighten this") != std::string::npos);
}

TEST_CASE("slash menu: lists every kind, and a mid-prose slash never opens it") {
    auto m = type(fresh(), U"/");
    const auto all = names(m);
    auto has = [&](const char* n) { return std::ranges::find(all, n) != all.end(); };
    CHECK(has("new"));
    CHECK(has("summarize"));
    CHECK(has("prose"));
    CHECK(has("gh:issue"));

    m = type(fresh(), U"see a/b");
    CHECK_FALSE(ui::slash_menu_open(m));
    CHECK(m.ui.composer.text == "see a/b");
}

TEST_CASE("slash menu: paste then Enter expands the command") {
    auto m = fresh();
    {
        auto msg = app::on_paste(m, maya::PasteEvent{"/summarize b.cpp"});
        m = app::update(std::move(m), std::move(msg)).first;
        // A paste that could be a file path is checked for an image on a
        // worker first; it isn't one, so the reply hands the text back.
        if (m.ui.composer.text.empty())
            m = app::update(std::move(m),
                Msg{ImagePathSniffed{"/summarize b.cpp", {}, {}, {}}}).first;
        m.s.oauth_refresh_in_flight = false;
    }
    CHECK(m.ui.composer.text == "/summarize b.cpp");
    m = press(std::move(m), sk(maya::SpecialKey::Enter));
    CHECK(sent(m) == "Summarize b.cpp please");
}

TEST_CASE("slash menu: a space ends the name, backspace reopens it, a new query resets") {
    auto m = type(fresh(), U"/summarize ");
    CHECK_FALSE(ui::slash_menu_open(m));
    CHECK(m.ui.composer.text == "/summarize ");
    m = press(std::move(m), sk(maya::SpecialKey::Backspace));
    CHECK(ui::slash_menu_open(m));
    CHECK(names(m).front() == "summarize");
    m = press(std::move(m), sk(maya::SpecialKey::Down));
    m = press(std::move(m), sk(maya::SpecialKey::Backspace));
    CHECK(m.ui.composer.slash.index == 0);
}

TEST_CASE("slash menu: ^K palette still opens, and no longer on /") {
    auto m = fresh();
    maya::KeyEvent k{maya::CharKey{U'k'}, {}, {}};
    k.mods.ctrl = true;
    m = press(std::move(m), k);
    CHECK(ui::panel::top(m) == ui::panel::Kind::Palette);
}
