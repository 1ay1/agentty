// Reducers describe their effects; this is the test shape that proves it.
//
// The jaal design doc's second success criterion:
//
//     "A reducer test is: build a Model, send a Msg, assert on the new Model
//      and expect_effect<save_settings>(1)."
//
// Before persistence became effects, that was impossible: the save happened
// INSIDE the reducer, through the Deps seam, so the only way to check it was
// to install a fake store and inspect it afterwards. The reducer's own return
// value said nothing about what it had written.
//
// Now the Cmd IS the answer. These tests install no deps and touch no disk.
#include "agtest.hpp"
#include "agtest_fx.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"

using namespace agentty;

TEST_CASE("effects: toggling a pref returns exactly one settings save") {
    Model m;

    auto [m2, cmd] = app::update(std::move(m), Msg{ToggleChangesStrip{}});

    // The model changed...
    CHECK(m2.d.show_changes_strip == true);
    // ...and the arm SAID to save, exactly once.
    CHECK(agtest::fx::count<SaveSettings>(cmd) == 1);

    // And what it would write carries the change — the record, not a
    // re-read of the store.
    const auto* e = agtest::fx::find<SaveSettings>(cmd);
    REQUIRE(e != nullptr);
    CHECK(e->settings.show_changes_strip == true);
}

TEST_CASE("effects: a reducer that changes nothing saves nothing") {
    Model m;
    // Moving the cursor in a picker is pure UI state; it must not write.
    auto [m2, cmd] = app::update(std::move(m), Msg{ThreadListMove{+1}});
    CHECK(agtest::fx::count<SaveSettings>(cmd) == 0);
    CHECK(agtest::fx::count<SaveThread>(cmd) == 0);
}

TEST_CASE("effects: save_record describes the record it was handed") {
    Model m;
    m.d.persisted.ui.theme = "probe-theme";
    m.d.persisted.show_reasoning = true;

    const Cmd cmd = app::detail::save_record(m);

    const auto* e = agtest::fx::find<SaveSettings>(cmd);
    REQUIRE(e != nullptr);
    CHECK(e->settings.ui.theme == "probe-theme");
    CHECK(e->settings.show_reasoning == true);
}

// The Quit arm is the one place ORDER matters: jaal's interpreter stops
// dispatching at the first quit (`if (exit_) return`), so a save batched
// after it would be silently dropped — and this is the save that keeps the
// last turn and the active model.
TEST_CASE("effects: Quit saves BEFORE it quits") {
    Model m;
    m.d.current.messages.push_back(Message{.role = Role::User, .text = "hi"});

    auto [m2, cmd] = app::update(std::move(m), Msg{Quit{}});

    bool seen_save = false;
    bool save_came_first = false;
    agtest::fx::for_each(cmd, [&](const auto& e) {
        using U = std::remove_cvref_t<decltype(e)>;
        if constexpr (std::same_as<U, SaveSettings> || std::same_as<U, SaveThread>)
            seen_save = true;
        else if constexpr (std::same_as<U, jaal::payload_t<jaal::fx::quit, Msg>>)
            save_came_first = seen_save;
    });
    CHECK_MESSAGE(seen_save, "Quit must persist the session");
    CHECK_MESSAGE(save_came_first,
                  "the save must be batched BEFORE quit or the interpreter "
                  "drops it");
}
