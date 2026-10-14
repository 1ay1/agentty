// subs_key must cover everything subscribe() reads, and nothing else.
//
// subscribe() asks only for the Tick timer and the login worker. Input goes
// through on_key/on_paste, which read the live model (jaal D43), so input
// state must NOT be in the key: if it were, every keystroke would re-run
// subscribe() for nothing.
#include "agtest.hpp"

#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/appearance.hpp"

using namespace agentty;

TEST_CASE("subs_key: stable when nothing subscribe() reads has changed") {
    Model m;
    CHECK(app::subs_key(m) == app::subs_key(m));

    const auto before = app::subs_key(m);
    m.s.status = "a status line";
    CHECK(before == app::subs_key(m));
}

TEST_CASE("subs_key: input state does not churn the key") {
    Model m;
    const auto before = app::subs_key(m);
    m.ui.composer.text = "hi";
    m.ui.panel.descend(ui::panel::Appearance{});
    CHECK_MESSAGE(before == app::subs_key(m),
                  "typing and opening panels must not re-run subscribe()");
}

TEST_CASE("subs_key: the Tick gate is part of the key") {
    Model m;
    const auto idle = app::subs_key(m);
    m.s.phase = phase::Streaming{phase::Active{}};
    REQUIRE(app::animation_demand(m));
    CHECK_MESSAGE(!(idle == app::subs_key(m)),
                  "starting a turn must arm the Tick timer");
}

TEST_CASE("subs_key: the login worker is part of the key") {
    Model m;
    const auto idle = app::subs_key(m);
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 1,
    };
    CHECK_MESSAGE(!(idle == app::subs_key(m)),
                  "a waiting login must start its worker");
}
