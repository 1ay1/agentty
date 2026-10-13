// Cancelling a login must actually stop the worker.
//
// History, because the shape of this bug is easy to reintroduce:
//
//   Originally the worker was a Cmd::task holding a shared_ptr<atomic_bool>,
//   and Esc tripped the flag. The jaal migration (f643ec58) switched the
//   body to the task's stop_token and stopped reading the flag — on the
//   reasoning that jaal trips the token "on shutdown AND when the
//   subscription that started the work goes away". That second half is only
//   true of a SOURCE. A one-shot Cmd::task has no subscription to drop, so
//   its token fires at kernel shutdown and nowhere else. Meanwhile the
//   reducers went on setting a flag nobody read, and Esc left the worker
//   block-polling the provider for the rest of its 900 s budget.
//
// The fix is the shape jaal wants: the worker is a keyed Sub::stream, and
// HOLDING THE WAITING STATE is what runs it. So "close the modal" and
// "cancel the worker" stop being two things that can drift apart — they are
// the same act, and these tests pin exactly that.
#include "agtest.hpp"

#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/model.hpp"

using namespace agentty;

namespace {

// How many leaf subscriptions this model asks for.
[[nodiscard]] int leaves(const Sub& s) {
    int n = 0;
    s.for_each([&](const auto&) { ++n; });
    return n;
}

} // namespace

TEST_CASE("login: the worker runs only while the modal is waiting") {
    Model m;
    const int idle = leaves(app::subscribe(m));

    // Entering the waiting state asks for one more source: the poll loop.
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 1,
    };
    CHECK_MESSAGE(leaves(app::subscribe(m)) == idle + 1,
                  "a waiting device login must subscribe its worker");

    // Esc drops it — which is what fires the body's stop_token.
    auto [m2, _] = app::update(std::move(m), Msg{CloseLogin{}});
    CHECK(std::holds_alternative<ui::login::Closed>(m2.ui.login));
    CHECK_MESSAGE(leaves(app::subscribe(m2)) == idle,
                  "closing the modal must drop the worker's subscription — "
                  "otherwise it keeps polling the provider for 900 s");
}

TEST_CASE("login: ChatGPT waiting subscribes its own worker") {
    Model m;
    const int idle = leaves(app::subscribe(m));

    m.ui.login = ui::login::ChatGptWaiting{.attempt_id = 1};
    CHECK(leaves(app::subscribe(m)) == idle + 1);

    auto [m2, _] = app::update(std::move(m), Msg{CloseLogin{}});
    CHECK(leaves(app::subscribe(m2)) == idle);
}

// The key is what gives the source its identity. A new attempt must produce
// a DIFFERENT key, or jaal would keep the old worker running and the new
// attempt would never start.
TEST_CASE("login: each attempt is a distinct subscription") {
    Model m;
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 1,
    };
    const auto first = app::subs_key(m);

    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 2,
    };
    CHECK_MESSAGE(!(first == app::subs_key(m)),
                  "a newer attempt must replace the running worker");
}

// A login can be open while a turn animates. subscribe() returns early on
// that path to add the Tick timer, and the login source has to survive it —
// dropping it there would silently cancel a sign-in mid-flight.
TEST_CASE("login: the worker survives an animating turn") {
    Model m;
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 1,
    };
    const int quiet = leaves(app::subscribe(m));

    m.s.phase = phase::Streaming{phase::Active{}};
    REQUIRE(app::animation_demand(m));          // the early-return path
    CHECK_MESSAGE(leaves(app::subscribe(m)) == quiet + 1,   // +1 = the Tick
                  "the login worker must still be subscribed while streaming");
}

// The guard that keeps a late worker from corrupting a newer login. Worth
// pinning independently of how cancellation is delivered.
TEST_CASE("login: a stale attempt's result is ignored") {
    Model m;
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot", .provider_label = "GitHub Copilot",
        .attempt_id = 2,                        // the CURRENT attempt
    };

    auto [m2, _] = app::update(std::move(m),
                               Msg{DeviceLoginDone{
                                   .provider = "copilot",
                                   .provider_label = "GitHub Copilot",
                                   .attempt_id = 1,       // STALE
                                   .error = std::nullopt,
                               }});

    auto* w = std::get_if<ui::login::DeviceWaiting>(&m2.ui.login);
    REQUIRE(w != nullptr);
    CHECK(w->attempt_id == 2);
}
