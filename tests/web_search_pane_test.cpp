// Web Search pane: the Settings → Web Search form drives the persisted
// web_search policy, and a committed edit reaches the tool layer as an
// effect rather than a reducer call into process-global state.
//
// The failure this pins is the pane being alive but disconnected: rows that
// draw, a door that opens a shell, edits that persist to the Model but leave
// the tool layer reading the seed forever. Smart Mode's pane answers that
// with apply_smart, which both persists AND publishes; this pane's commit()
// must do the same two things, and the tests below count on both.

#include "agtest.hpp"
#include "agtest_fx.hpp"

#include "agentty/io/persistence.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/panel/nav.hpp"
#include "agentty/runtime/panel/settings/items.hpp"
#include "agentty/runtime/panel/web_search_form.hpp"
#include "agentty/runtime/settings_registry.hpp"

#include "agentty/auth/keystore.hpp"
#include "agentty/tool/web_search_secret.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace agentty;
namespace pn = agentty::ui::panel;
namespace reg  = agentty::settings::registry;
namespace se   = agentty::settings;
namespace wtf  = agentty::web_search_form;
namespace fx   = agtest::fx;

namespace {

Model opened(Model m = Model{}) {
    auto [mm, _] = app::update(std::move(m), Msg{OpenWebSearch{}});
    return std::move(mm);
}

const pn::WebSearch& pane_of(const Model& m) {
    const auto* o = m.ui.panel.get<pn::WebSearch>();
    REQUIRE(o != nullptr);
    return *o;
}

Msg key(form::keys::Intent i) { return Msg{WebSearchKey{form::keys::Action{i, 0}}}; }

Model send(Model m, Msg msg) {
    auto [mm, _] = app::update(std::move(m), std::move(msg));
    (void)_;
    return std::move(mm);
}

// Drive the pane to `row_id` (down-arrow until focused), then apply `key`.
void focus_row(Model& m, std::string_view row_id) {
    auto* o = m.ui.panel.get<pn::WebSearch>();
    for (int guard = 0; guard < 32; ++guard) {
        const auto* row = o->form.focused();
        if (row && row->id == row_id) return;
        auto [mm, _] = app::update(std::move(m), key(form::keys::Intent::MoveNext));
        m = std::move(mm);
        o = m.ui.panel.get<pn::WebSearch>();
        REQUIRE(o != nullptr);
    }
    CHECK(false);   // row_id is not a search row
}

// Open the pane with the switch already in `mode` ("auto" | "on" | "off"):
// the count rows are only editable in on, so their tests start there.
Model opened_in(const char* mode) {
    Model m;
    m.d.persisted.web_search.mode_text = mode;
    return opened(std::move(m));
}

} // namespace

TEST_CASE("web search pane: the General list has a door that opens the pane") {
    Model m;
    const auto items = se::items_for(m, se::Category::General);
    const auto* door = static_cast<const se::Item*>(nullptr);
    for (const auto& i : items)
        if (i.action == se::Action::OpenWebSearch) door = &i;
    REQUIRE(door != nullptr);
    CHECK(door->primary == "Web Search");
    CHECK(se::opens_pane(door->action), "a door row wears the \xe2\x86\x92 affordance");
    CHECK(!door->secondary.empty(),
          "the row says what the policy does, so it reads as a state, not a bare name");
    // ...and who answers first, by the name the pane's menu uses.
    CHECK(door->secondary.find("Brave Free") != std::string::npos);

    // Open by action: the settings list descends into the pane.
    auto [m2, c2] = app::update(Model{}, Msg{OpenWebSearch{}});
    // Two headers, from the registry groups ("Search", "Services"); every
    // web_search.* registry row; the dials of the free defaults under their
    // slots; and NO key row, because none of the three needs a key. The
    // pane OPENS on the switch, not on a header.
    //
    // Rows are named rather than counted: an exact count is a test that fails
    // for being right the first time a row is added (see the note in
    // smart_tuning_settings_test).
    const auto& form = pane_of(m2).form;
    int headers = 0;
    for (const auto& f : form.fields) if (f.is_header()) ++headers;
    CHECK(headers == 2);
    for (const auto& d : reg::kSettings)
        if (d.owner() == reg::Owner::WebSearch)
            CHECK(form.find(d.id) != nullptr, std::string{d.id});
    for (const char* id : {"web_search.dial.brave-free.freshness",
                           "web_search.dial.ddg-free.region",
                           "web_search.dial.ddg-free.safe",
                           "web_search.dial.tavily-free.topic",
                           "web_search.dial.tavily-free.time_range"})
        CHECK(form.find(id) != nullptr, id);
    for (const auto& f : form.fields)
        CHECK(f.id.rfind("web_search.key.", 0) != 0, f.id);
    CHECK(form.fields.front().is_header());
    const auto* first = form.focused();
    REQUIRE(first != nullptr);
    CHECK(first->id == "web_search.mode");
    CHECK(!first->locked);
}

TEST_CASE("web search pane: turning search off persists and publishes") {
    Model m = opened();

    // Defaults: auto, 10 results, ceiling 20.
    CHECK(web_search_cfg::mode(m.d.persisted.web_search) == web_search_cfg::Mode::Auto);

    // Open the mode dropdown on the switch, step to Off, confirm.
    focus_row(m, "web_search.mode");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    m = send(std::move(m), key(form::keys::Intent::MenuNext));
    m = send(std::move(m), key(form::keys::Intent::MenuNext));
    auto [m2, cmd] = app::update(std::move(m), key(form::keys::Intent::MenuCommit));
    CHECK(web_search_cfg::mode(m2.d.persisted.web_search) == web_search_cfg::Mode::Off,
          "the Model holds the new value");

    // Persist effect: a save was RETURNED, for a settings.json with the row.
    const auto* saved = fx::find<agentty::SaveSettings>(cmd);
    REQUIRE(saved != nullptr);
    CHECK(saved->settings.web_search.mode_text == "off");

    // Publish: the reducer itself returns only the save; telling the tool
    // layer is publish_derived's job (update.cpp), which runs after every
    // fold in the app. Apply it here the way the app does and assert the
    // effect names the NEW policy.
    const int saves  = fx::count<agentty::SaveSettings>(cmd);
    CHECK(saves == 1, "persist");
    m2.published_web_search = agentty::web_search_cfg::Config{};   // as if published before
    const Cmd seam = app::publish_derived(m2, Cmd::none());
    const auto* pub = fx::find<agentty::PublishWebSearchPolicy>(seam);
    REQUIRE(pub != nullptr);
    CHECK(web_search_cfg::mode(pub->policy) == web_search_cfg::Mode::Off,
          "the tool layer is told search is off");
    // And not again on a fold that changes nothing.
    CHECK(fx::find<agentty::PublishWebSearchPolicy>(
              app::publish_derived(m2, Cmd::none())) == nullptr);

    // The pane now reflects the disabled state, cursor kept on the row.
    const auto* row = pane_of(m2).form.find("web_search.mode");
    REQUIRE(row != nullptr);
    CHECK(pane_of(m2).form.focused() == row, "cursor stays on the switch");
}

TEST_CASE("web search pane: typed rows commit on leaving the field, not per keystroke") {
    Model m = opened_in("on");
    focus_row(m, "web_search.count");

    // Enter EDIT mode, type a digit, stay in the field: committed edits here
    // would save `1` on the way to `15` and rebuild the form under the caret.
    auto [m1, c1] = app::update(std::move(m), key(form::keys::Intent::Activate));
    const auto* ed = m1.ui.panel.get<pn::WebSearch>();
    CHECK(ed && ed->form.editing(), "in the field");

    auto [m2, c2] = app::update(std::move(m1),
        Msg{WebSearchKey{form::keys::Action{form::keys::Intent::Insert, U'1'}}});
    CHECK(fx::find<agentty::SaveSettings>(c2) == nullptr,
          "mid-edit: nothing saved yet");
    CHECK(m2.d.persisted.web_search.count == 10, "mid-edit: model untouched");

    // Leave the field (Esc/arrow out — one Msg) — THAT is the commit.
    auto [m3, c3] = app::update(std::move(m2), key(form::keys::Intent::MoveNext));
    CHECK(m3.d.persisted.web_search.count == 1, "left the field: committed");
    CHECK(fx::find<agentty::SaveSettings>(c3) != nullptr);
}

TEST_CASE("web search pane: the exclusion text normalises at the tool, not the field") {
    Model m = opened();
    focus_row(m, "web_search.exclude_sites");

    // Type a messy list with a URL into the row, then leave.
    auto [m1, _1] = app::update(std::move(m), key(form::keys::Intent::Activate));
    std::string typed = "https://www.Pinterest.com/  foo";
    for (char ch : typed) {
        auto [mm, _] = app::update(std::move(m1),
            Msg{WebSearchKey{form::keys::Action{form::keys::Intent::Insert,
                static_cast<char32_t>(static_cast<unsigned char>(ch))}}});
        m1 = std::move(mm);
    }
    auto [m2, c2] = app::update(std::move(m1), key(form::keys::Intent::MoveNext));

    // The pane shows exactly what was typed; normalising is the tool's job
    // (web_search_cfg), or the pane and the wire would disagree about the value.
    CHECK(m2.d.persisted.web_search.exclude_sites == typed);
    CHECK(fx::find<agentty::SaveSettings>(c2) != nullptr);
    const auto sites =
        web_search_cfg::exclude_list(m2.d.persisted.web_search.exclude_sites);
    CHECK(sites == std::vector<std::string>{"pinterest.com"});

    // Leaving the field committed it: a save was returned. Look up the
    // payload type (SaveSettings), not the effect descriptor alias
    // (save_settings), which never appears inside a Cmd.
    CHECK(fx::find<agentty::SaveSettings>(c2) != nullptr);
}

TEST_CASE("web search pane: Esc closes back out, and close_msg names it") {
    Model m = opened();
    // close_msg for this overlay must be its own Close: a NoOp or a passthrough
    // is the trap the guarantee exists to catch.
    const Msg cm = pn::close_msg(pn::Kind::WebSearch);
    CHECK(std::holds_alternative<msg::WebSearchMsg>(cm),
          "close_msg must name this pane's own close, not pass through it");

    auto [m2, _] = app::update(std::move(m), Msg{CloseWebSearch{}});
    CHECK(m2.ui.panel.get<pn::WebSearch>() == nullptr, "closed");
    (void)_;
}

TEST_CASE("web search pane: auto locks the two count rows, and on unlocks them") {
    Model m = opened();   // defaults are auto
    for (const char* id : {"web_search.count", "web_search.max_count"}) {
        const auto* row = pane_of(m).form.find(id);
        REQUIRE(row != nullptr);
        CHECK(row->locked, "auto: the model sets the count");
    }

    // Switch to On through the dropdown: the count rows unlock.
    focus_row(m, "web_search.mode");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    m = send(std::move(m), key(form::keys::Intent::MenuNext));
    m = send(std::move(m), key(form::keys::Intent::MenuNext));
    m = send(std::move(m), key(form::keys::Intent::MenuPrev));
    m = send(std::move(m), key(form::keys::Intent::MenuCommit));
    CHECK(web_search_cfg::mode(m.d.persisted.web_search) == web_search_cfg::Mode::On);
    for (const char* id : {"web_search.count", "web_search.max_count"}) {
        const auto* row = pane_of(m).form.find(id);
        REQUIRE(row != nullptr);
        CHECK(!row->locked, "on: the counts are the user's to set");
    }
    // Never return is live in both auto and on.
    const auto* ex = pane_of(m).form.find("web_search.exclude_sites");
    REQUIRE(ex != nullptr);
    CHECK(!ex->locked);
}

TEST_CASE("web search pane: auto leaves Never return editable") {
    Model m = opened();   // auto
    const auto* ex = pane_of(m).form.find("web_search.exclude_sites");
    REQUIRE(ex != nullptr);
    CHECK(!ex->locked, "exclusions apply in auto, so the row is live");
}

TEST_CASE("web search pane: off locks every row but the switch") {
    Model m = opened_in("off");
    for (const char* id : {"web_search.count", "web_search.max_count", "web_search.exclude_sites"}) {
        const auto* row = pane_of(m).form.find(id);
        REQUIRE(row != nullptr);
        CHECK(row->locked, "off: nothing is searched, so nothing here applies");
    }
    const auto* sw = pane_of(m).form.find("web_search.mode");
    REQUIRE(sw != nullptr);
    CHECK(!sw->locked, "the way back out must stay open");
}

TEST_CASE("web search pane: a locked count row ignores keys and saves nothing") {
    Model m = opened();   // auto: count rows locked
    focus_row(m, "web_search.count");
    auto [m1, c1] = app::update(std::move(m), key(form::keys::Intent::AdjustUp));
    CHECK(m1.d.persisted.web_search.count == 10);
    CHECK(fx::find<agentty::SaveSettings>(c1) == nullptr);
    auto [m2, c2] = app::update(std::move(m1), key(form::keys::Intent::Activate));
    CHECK(!pane_of(m2).form.editing(), "a locked row cannot be entered");
    (void)c2;
}

TEST_CASE("web search pane: switching mode keeps the stored counts") {
    // A count set in on must survive a trip through auto: apply_form skips
    // locked rows, so auto must not overwrite them with what it displays.
    Model m;
    m.d.persisted.web_search.mode_text = "on";
    m.d.persisted.web_search.count = 7;
    m = opened(std::move(m));
    focus_row(m, "web_search.mode");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    m = send(std::move(m), key(form::keys::Intent::MenuPrev));    // on → auto
    m = send(std::move(m), key(form::keys::Intent::MenuCommit));
    REQUIRE(web_search_cfg::mode(m.d.persisted.web_search) == web_search_cfg::Mode::Auto);
    CHECK(m.d.persisted.web_search.count == 7);
}

// ── Regressions: editing must survive more than one keystroke ───────────
//
// Reported: "Most results allowed" accepted a single digit, and Backspace /
// Delete did nothing in "Never return". One cause: the key arm rebuilt the
// form from the saved config on EVERY `changed`, including mid-edit ones,
// which reset the row and dropped Editing focus after each keystroke. The
// earlier test above only typed once and only checked that nothing was
// saved, so it could not see it. These type several keys and check the
// FORM, not just the Model.

namespace {

Msg ins(char32_t c) {
    return Msg{WebSearchKey{form::keys::Action{form::keys::Intent::Insert, c}}};
}

std::int64_t number_of(const Model& m, std::string_view id) {
    const auto* f = pane_of(m).form.find(id);
    REQUIRE(f != nullptr);
    const auto* n = std::get_if<form::field::Number>(&f->value);
    REQUIRE(n != nullptr);
    return n->value;
}

std::string text_of(const Model& m, std::string_view id) {
    const auto* f = pane_of(m).form.find(id);
    REQUIRE(f != nullptr);
    const auto* t = std::get_if<form::field::Text>(&f->value);
    REQUIRE(t != nullptr);
    return t->value;
}

} // namespace

TEST_CASE("web search pane: a number row takes more than one digit") {
    Model m = opened_in("on");
    focus_row(m, "web_search.max_count");
    m = send(std::move(m), key(form::keys::Intent::Activate));   // edit
    REQUIRE(pane_of(m).form.editing());

    // Type 3 then 5 on a fresh edit session: the first digit replaces the
    // saved 20 (form layer's "fresh" rule), the second appends.
    m = send(std::move(m), ins(U'3'));
    CHECK(pane_of(m).form.editing(), "still editing after the first digit");
    CHECK(number_of(m, "web_search.max_count") == 3);
    m = send(std::move(m), ins(U'5'));
    CHECK(pane_of(m).form.editing(), "still editing after the second digit");
    CHECK(number_of(m, "web_search.max_count") == 35);
    CHECK(m.d.persisted.web_search.max_count == 20, "nothing saved mid-edit");

    // Backspace on a number drops the last digit and stays in edit.
    m = send(std::move(m), key(form::keys::Intent::Backspace));
    CHECK(number_of(m, "web_search.max_count") == 3);
    m = send(std::move(m), ins(U'8'));
    CHECK(number_of(m, "web_search.max_count") == 38);

    m = send(std::move(m), key(form::keys::Intent::LeaveField));
    CHECK(m.d.persisted.web_search.max_count == 38, "committed on leaving");
}

TEST_CASE("web search pane: Backspace and Delete edit the exclusion text") {
    Model m = opened();
    focus_row(m, "web_search.exclude_sites");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    REQUIRE(pane_of(m).form.editing());

    for (char32_t c : std::u32string{U"abc.com"}) m = send(std::move(m), ins(c));
    CHECK(text_of(m, "web_search.exclude_sites") == "abc.com");

    m = send(std::move(m), key(form::keys::Intent::Backspace));
    CHECK(text_of(m, "web_search.exclude_sites") == "abc.co", "Backspace removes one");
    CHECK(pane_of(m).form.editing(), "and the field stays in edit");

    m = send(std::move(m), key(form::keys::Intent::CaretHome));
    m = send(std::move(m), key(form::keys::Intent::DeleteForward));
    CHECK(text_of(m, "web_search.exclude_sites") == "bc.co", "Delete removes under the caret");

    m = send(std::move(m), key(form::keys::Intent::LeaveField));
    CHECK(m.d.persisted.web_search.exclude_sites == "bc.co");
}

TEST_CASE("web search pane: the exclusion row says what separates domains") {
    const auto* d = reg::find("web_search.exclude_sites");
    REQUIRE(d != nullptr);
    const std::string help{d->help};
    CHECK(help.find("space") != std::string::npos);
    CHECK(help.find("comma") != std::string::npos);
}

// ── Services: keys and slots, through the real reducer ───────────────────
//
// The registry rows above are plain settings. These are the parts that are
// not: a typed API key goes to the key store and NOT to the Model, and a
// slot change rebuilds the rows under it.

namespace {

struct KeyHome {
    std::filesystem::path dir;
    std::string old_home, old_ks;
    bool had_home = false, had_ks = false;
    KeyHome() {
        dir = std::filesystem::temp_directory_path()
            / ("agentty_wt_keys_" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "home");
        if (const char* h = ::getenv("AGENTTY_HOME")) { had_home = true; old_home = h; }
        if (const char* k = ::getenv("AGENTTY_USE_KEYSTORE")) { had_ks = true; old_ks = k; }
        ::setenv("AGENTTY_HOME", (dir / "home").c_str(), 1);
        ::setenv("AGENTTY_USE_KEYSTORE", "0", 1);
        ::unsetenv("AGENTTY_WEB_SEARCH_KEY_EXA_API");
        // keystore::available() is cached per process; if it was armed before
        // this ran, a store() would reach the developer's real keyring.
        real_keyring = agentty::auth::keystore::available();
    }
    bool real_keyring = false;
    ~KeyHome() {
        if (had_home) ::setenv("AGENTTY_HOME", old_home.c_str(), 1);
        else          ::unsetenv("AGENTTY_HOME");
        if (had_ks) ::setenv("AGENTTY_USE_KEYSTORE", old_ks.c_str(), 1);
        else        ::unsetenv("AGENTTY_USE_KEYSTORE");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// update(), then run the tasks it returned (on this thread, as the kernel
// would on a worker) and feed their answers back -- until nothing is left.
// The key store is IO, so the pane hands it to tasks; this is how a test
// sees the whole round trip without a kernel.
Model settle(Model m, Msg msg) {
    std::vector<Msg> queue;
    queue.push_back(std::move(msg));
    for (int round = 0; !queue.empty(); ++round) {
        REQUIRE(round < 32);   // a task that always starts another
        std::vector<Msg> next;
        for (auto& q : queue) {
            auto [mm, cmd] = app::update(std::move(m), std::move(q));
            m = std::move(mm);
            for (auto& r : fx::run_tasks(std::move(cmd))) next.push_back(std::move(r));
        }
        queue = std::move(next);
    }
    return m;
}

Model with_exa_primary() {
    Model m;
    m.d.persisted.web_search.primary = "exa-api";
    return settle(std::move(m), Msg{OpenWebSearch{}});
}

void type_text(Model& m, std::string_view text) {
    for (char c : text) m = send(std::move(m), ins(static_cast<char32_t>(c)));
}

} // namespace

TEST_CASE("web search pane: a typed API key is stored, not kept in the Model") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    Model m = with_exa_primary();
    REQUIRE(pane_of(m).form.find("web_search.key.exa-api") != nullptr);
    CHECK(pane_of(m).form.find("web_search.key.exa-api")->origin == "not set");

    focus_row(m, "web_search.key.exa-api");
    m = send(std::move(m), key(form::keys::Intent::Activate));      // start editing
    REQUIRE(pane_of(m).form.editing());
    type_text(m, "exa-SECRET-0123456789");
    // Mid-edit the row holds the text, and the Model holds nothing.
    CHECK(wtf::key_row_service(pane_of(m).form.focused()->id) == "exa-api");
    // Leaving the field: the reducer empties the row and returns a task that
    // stores the key. It does no IO itself.
    {
        auto [m2, cmd] = app::update(std::move(m), key(form::keys::Intent::LeaveField));
        CHECK(!agentty::tools::web_search_secret::has("exa-api"),
              "the reducer stored nothing itself");
        CHECK(std::get<form::field::Secret>(
                  pane_of(m2).form.find("web_search.key.exa-api")->value).value.empty(),
              "the typed key left the row at once");
        m = std::move(m2);
        for (auto& r : fx::run_tasks(std::move(cmd))) m = settle(std::move(m), std::move(r));
    }

    CHECK(agentty::tools::web_search_secret::load("exa-api") == "exa-SECRET-0123456789");
    // The row was emptied and now says where the key lives, never what it is.
    const auto* row = pane_of(m).form.find("web_search.key.exa-api");
    REQUIRE(row != nullptr);
    CHECK(std::get<form::field::Secret>(row->value).value.empty());
    CHECK(row->origin == "stored");
    // The Model (and so settings.json) never saw it.
    CHECK(m.d.persisted.web_search.primary == "exa-api");
    CHECK(pane_of(m).form.find("web_search.keyclear.exa-api") != nullptr);
}

TEST_CASE("web search pane: opening reads key origins on a worker, not in the reducer") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    REQUIRE(agentty::tools::web_search_secret::store("exa-api", "exa-STORED-0123456789"));
    Model m;
    m.d.persisted.web_search.primary = "exa-api";
    auto [m1, cmd] = app::update(std::move(m), Msg{OpenWebSearch{}});
    // Before the read answers the pane does not claim either way.
    const auto* row = pane_of(m1).form.find("web_search.key.exa-api");
    REQUIRE(row != nullptr);
    CHECK(row->origin != "stored");
    CHECK(row->origin != "not set");
    // The read is a task; its answer fills the origin in.
    auto replies = fx::run_tasks(std::move(cmd));
    REQUIRE(replies.size() == 1);
    Model m2 = std::move(m1);
    for (auto& r : replies) m2 = settle(std::move(m2), std::move(r));
    CHECK(pane_of(m2).form.find("web_search.key.exa-api")->origin == "stored");
    CHECK(pane_of(m2).form.find("web_search.keyclear.exa-api") != nullptr);
}

TEST_CASE("web search pane: a stale key read does not undo a newer change") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    Model m;
    m.d.persisted.web_search.primary = "exa-api";
    auto [opened_m, open_cmd] = app::update(std::move(m), Msg{OpenWebSearch{}});
    Model m1 = std::move(opened_m);
    // The opening read finishes, but its answer has not been folded yet...
    auto stale = fx::run_tasks(std::move(open_cmd));
    // ...when the pane starts a store, which bumps its generation.
    m1.ui.panel.get<pn::WebSearch>()->keys_gen += 1;
    // So the old answer is dropped rather than shown over the newer state.
    for (auto& r : stale) {
        auto [mm, cmd] = app::update(std::move(m1), std::move(r));
        m1 = std::move(mm);
        CHECK(fx::run_tasks(std::move(cmd)).empty());
    }
    CHECK(!pane_of(m1).keys_read);
}

TEST_CASE("web search pane: key origins that arrive mid-edit are shown when the edit ends") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    REQUIRE(agentty::tools::web_search_secret::store("exa-api", "exa-STORED-0123456789"));
    Model m;
    m.d.persisted.web_search.primary = "exa-api";
    auto [m1, open_cmd] = app::update(std::move(m), Msg{OpenWebSearch{}});
    Model mm = std::move(m1);
    // Start editing a text row before the read answers...
    focus_row(mm, "web_search.exclude_sites");
    mm = send(std::move(mm), key(form::keys::Intent::Activate));
    REQUIRE(pane_of(mm).form.editing());
    for (auto& r : fx::run_tasks(std::move(open_cmd))) mm = settle(std::move(mm), std::move(r));
    // ...the edit is not disturbed...
    CHECK(pane_of(mm).form.editing());
    // ...and leaving it unchanged (Esc: no commit) still shows the origins.
    mm = send(std::move(mm), key(form::keys::Intent::LeaveField));
    CHECK(!pane_of(mm).form.editing());
    CHECK(pane_of(mm).form.find("web_search.key.exa-api")->origin == "stored");
}

TEST_CASE("web search pane: a key that does not look like one is refused, not stored") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    Model m = with_exa_primary();
    focus_row(m, "web_search.key.exa-api");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    type_text(m, "nope");
    m = settle(std::move(m), key(form::keys::Intent::LeaveField));
    CHECK(!agentty::tools::web_search_secret::has("exa-api"));
    CHECK(std::get<form::field::Secret>(
              pane_of(m).form.find("web_search.key.exa-api")->value).value.empty());
    CHECK(pane_of(m).form.find("web_search.key.exa-api")->origin == "not set");
}

TEST_CASE("web search pane: Remove key forgets a stored key") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    REQUIRE(agentty::tools::web_search_secret::store("exa-api", "exa-STORED-0123456789"));
    Model m = with_exa_primary();
    REQUIRE(pane_of(m).form.find("web_search.keyclear.exa-api") != nullptr);
    focus_row(m, "web_search.keyclear.exa-api");
    m = settle(std::move(m), key(form::keys::Intent::Activate));
    CHECK(!agentty::tools::web_search_secret::has("exa-api"));
    CHECK(pane_of(m).form.find("web_search.keyclear.exa-api") == nullptr);
    CHECK(pane_of(m).form.find("web_search.key.exa-api")->origin == "not set");
    // The Remove row is gone, so the cursor lands on the key row it served.
    REQUIRE(pane_of(m).form.focused() != nullptr);
    CHECK(pane_of(m).form.focused()->id == "web_search.key.exa-api");
}

TEST_CASE("web search pane: picking a keyed service for a slot brings its key row") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    Model m = opened();
    CHECK(pane_of(m).form.find("web_search.key.exa-api") == nullptr);
    // Primary dropdown: open, step to a keyed service, commit.
    focus_row(m, "web_search.primary");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    for (int i = 0; i < 7; ++i) m = send(std::move(m), key(form::keys::Intent::MenuNext));
    m = send(std::move(m), key(form::keys::Intent::MenuCommit));
    CHECK(m.d.persisted.web_search.primary == "exa-api");
    CHECK(pane_of(m).form.find("web_search.key.exa-api") != nullptr);
    CHECK(pane_of(m).form.find("web_search.dial.exa-api.type") != nullptr);
    // The old service's rows are gone from the pane (its dials stay on record).
    CHECK(pane_of(m).form.find("web_search.dial.brave-free.freshness") == nullptr);
    // The rebuild added and removed rows under Primary; the cursor stays ON
    // Primary rather than on whatever row slid into its old position.
    REQUIRE(pane_of(m).form.focused() != nullptr);
    CHECK(pane_of(m).form.focused()->id == "web_search.primary");
}

TEST_CASE("web search pane: choosing a dial value persists it") {
    KeyHome home;
    if (home.real_keyring) { MESSAGE("skipped: OS keystore armed for this process"); return; }
    Model m = opened();
    focus_row(m, "web_search.dial.brave-free.freshness");
    m = send(std::move(m), key(form::keys::Intent::Activate));
    m = send(std::move(m), key(form::keys::Intent::MenuNext));      // Any time -> Past day
    m = send(std::move(m), key(form::keys::Intent::MenuNext));      // -> Past week
    auto [m2, cmd] = app::update(std::move(m), key(form::keys::Intent::MenuCommit));
    CHECK(m2.d.persisted.web_search.dials.at("brave-free.freshness") == "pw");
    const auto* saved = fx::find<agentty::SaveSettings>(cmd);
    REQUIRE(saved != nullptr);
    CHECK(saved->settings.web_search.dials.at("brave-free.freshness") == "pw");
}
