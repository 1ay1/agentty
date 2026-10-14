// key_routing_test: a keypress is routed by the CURRENT model.
//
// Most tests dispatch Msg values straight into update(), which skips the
// half of the input path that decides WHICH handler a key reaches. These
// drive it: route a key through AgenttyApp::on (what jaal calls, with the
// live model), apply the result, route the next key.
//
// Routing used to be a router closed over a snapshot built by subscribe(),
// and a message that changed which surface owned the keyboard left the
// next key routed against the old one ("the input doesn't register until
// you hit it again"). jaal D43 removed routers, so this can't go stale any
// more; these tests keep it that way.
#include "agtest.hpp"

#include <optional>
#include <vector>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/app/deps.hpp"

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

// Route one key the way the kernel does: the program's on() for KeyEvent,
// called with the model as it is now.
std::optional<Msg> route(const Model& m, const maya::KeyEvent& ev) {
    return app::AgenttyApp::on(m, ev);
}

// Press a key: route it against the current model, then apply whatever it
// produced.
[[nodiscard]] bool press(Model& m, const maya::KeyEvent& ev) {
    auto msg = route(m, ev);
    if (!msg) return false;
    m = app::update(std::move(m), std::move(*msg)).first;
    return true;
}

// Escape routes through a handler that reaches app::deps() (it persists the
// restored theme), so the stub has to be installed even though this test
// only cares about routing.
void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream        = [](const maya::unlocked&, provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{}; },
        .title_from    = [](std::string_view t) { return std::string{t}; },
        .write_file    = [](const std::string&, const std::string&) {},
        .auth          = {},
    });
}

maya::KeyEvent special(maya::SpecialKey k) {
    maya::KeyEvent ev;
    ev.key = k;
    return ev;
}

} // namespace

TEST_CASE("routing: the first arrow after Enter reaches the theme browser") {
    Model m;
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;

    // Walk to the Theme row the way a user does, so the form's focus is
    // real rather than assumed.
    const auto* pane = m.ui.panel.get<pn::Appearance>();
    REQUIRE(pane != nullptr);

    // Open the browser with Enter. This is the panel-CHANGING key: after it
    // the Appearance pane owns the keyboard in a different mode.
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
    const auto* opened = m.ui.panel.get<pn::Appearance>();
    REQUIRE(opened != nullptr);
    CHECK(opened->pane.picking, "Enter did not open the browser");

    const int idx_before    = opened->pane.picker.picker.index();
    const int cursor_before = opened->pane.form.cursor;

    // THE ASSERTION: one ↓, routed through a sub built from the model as it
    // is NOW. It must move the browser's highlight. If subscribe() were
    // routing against the pre-Enter snapshot this would move the form
    // cursor underneath instead, and the user would see nothing happen.
    CHECK(press(m, special(maya::SpecialKey::Down)),
          "the arrow produced no message at all");

    const auto* after = m.ui.panel.get<pn::Appearance>();
    REQUIRE(after != nullptr);
    CHECK(after->pane.picker.picker.index() != idx_before,
          "the FIRST arrow after Enter did not move the browser — this is "
          "the 'input doesn't register until you hit it again' bug");
    CHECK(after->pane.form.cursor == cursor_before,
          "the arrow leaked into the form behind the browser");
}

TEST_CASE("routing: Escape leaves the browser and the next key hits the form") {
    install_stub_deps();
    Model m;
    m = app::update(std::move(m), Msg{OpenAppearance{}}).first;
    m = app::update(std::move(m), Msg{AppearancePickTheme{}}).first;
    REQUIRE(m.ui.panel.get<pn::Appearance>() != nullptr);
    CHECK(m.ui.panel.get<pn::Appearance>()->pane.picking);

    // Esc closes the browser — the same transition in reverse. The next key
    // must reach the FORM, not a browser that is no longer up.
    CHECK(press(m, special(maya::SpecialKey::Escape)),
          "Escape produced no message");
    const auto* closed = m.ui.panel.get<pn::Appearance>();
    REQUIRE(closed != nullptr);
    CHECK(!closed->pane.picking, "Escape did not close the browser");

    const int idx_before    = closed->pane.picker.picker.index();
    const int cursor_before = closed->pane.form.cursor;

    CHECK(press(m, special(maya::SpecialKey::Down)),
          "the arrow after Escape produced no message");

    const auto* after = m.ui.panel.get<pn::Appearance>();
    REQUIRE(after != nullptr);
    CHECK(after->pane.form.cursor != cursor_before,
          "the first arrow after closing the browser did not move the form");
    CHECK(after->pane.picker.picker.index() == idx_before,
          "the arrow still reached the closed browser");
}

// ^A/^X once shipped with their arming gate captured by the router and
// missing from SubsKey, so they were dead for a frame after edits landed and
// stale-live after the queue cleared. Routing reads the live model now.
TEST_CASE("routing: the changes-strip chords follow the live model") {
    install_stub_deps();
    Model m;
    m.d.show_changes_strip = true;
    const auto ctrl_x = [] {
        maya::KeyEvent k{};
        k.key = maya::CharKey{U'x'};
        k.mods.ctrl = true;
        return k;
    }();
    const auto is_reject = [](const std::optional<Msg>& msg) {
        const auto* dr = msg ? std::get_if<msg::DiffReviewMsg>(&*msg) : nullptr;
        return dr && std::holds_alternative<RejectAllChanges>(*dr);
    };

    CHECK_FALSE(is_reject(route(m, ctrl_x)));   // nothing queued: not armed

    FileChange fc;
    fc.path = "src/main.cpp";
    m.d.pending_changes.push_back(fc);
    CHECK_MESSAGE(is_reject(route(m, ctrl_x)),
                  "queued changes arm ^X on the very next key");

    m.d.show_changes_strip = false;
    CHECK_MESSAGE(!is_reject(route(m, ctrl_x)),
                  "hiding the strip disarms ^X on the very next key");
}
