// key_routing_test — a keypress is routed by the CURRENT model, not a stale
// snapshot of it.
//
// ── The gap this fills ──────────────────────────────────────────────────
//
// Every other test in this suite dispatches Msg values straight into
// update(). That skips subscribe(), which is the half of the input path
// that decides WHICH handler a key reaches — and it is the half that can
// go stale.
//
// subscribe() captures its routing facts BY VALUE into the key lambda:
//
//     bool appearance_picking = false;
//     if (const auto* o = m.ui.panel.get<pn::Appearance>())
//         appearance_picking = o->pane.picking;
//     auto key_sub = Sub<Msg>::on_key([=](const KeyEvent& ev) { ... });
//
// That is correct and deliberate — the alternative is a deep copy of every
// pane's rows on the input path. But it means a Sub built from model N
// cannot route a key that model N+1 should own. maya re-fetches the sub
// after every drained message for exactly this reason (app.hpp: "Re-route
// BETWEEN events, not just between reads"), and the contract only holds if
// subscribe() is actually re-run.
//
// The user-visible symptom when it is not: open the theme browser with
// Enter, press ↓, nothing happens. Press ↓ again and it works — because by
// then the sub has been rebuilt. "The input doesn't register until you hit
// it another time."
//
// So this test drives the REAL loop shape: build a sub from the model,
// route a key through it, apply the resulting message, REBUILD the sub, and
// route the next key. If a message changes which surface owns the keyboard,
// the very next key must land on the new surface.
#include "agtest.hpp"

#include <optional>
#include <vector>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/panel/appearance.hpp"
#include "agentty/runtime/app/deps.hpp"

#include <maya/app/app.hpp>

using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {

// Route one key through a sub built from THIS model, exactly as the runtime
// does, and return the message it produced (if any).
std::optional<Msg> route(const Model& m, const maya::KeyEvent& ev) {
    const auto sub = agentty::app::subscribe(m);
    std::vector<Msg> out;
    maya::detail::dispatch_through_sub(sub, maya::Event{ev}, out);
    if (out.empty()) return std::nullopt;
    return std::move(out.front());
}

// Press a key: route it against the current model, then apply whatever it
// produced. This is the loop's per-event step, sub rebuild included.
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
        .stream        = [](provider::Request, provider::EventSink) {},
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
