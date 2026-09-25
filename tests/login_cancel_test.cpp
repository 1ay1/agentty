// Cancelling a device login must actually stop the worker.
//
// Regression risk from the jaal migration (commit f643ec58, "login: cancel
// through the stop_token, not a shared flag"):
//
//   device_login_async / codex_login_async stopped reading the
//   shared_ptr<atomic_bool> they are handed — the parameter is spelled
//   `/*cancel*/` — and poll `stop.stop_requested()` instead. But a
//   Cmd::task's stop_token is only tripped at KERNEL SHUTDOWN. jaal stops
//   an individual body when the SUBSCRIPTION that started it goes away
//   (kernel.hpp stop_stream / stop_source); a one-shot task has no
//   subscription to drop.
//
//   Meanwhile the reducers (close_login, login_codex_done, login_device_done)
//   still set the shared flag — which nobody reads any more.
//
//   So Esc closes the modal and the worker keeps block-polling the provider
//   for up to its 900-second timeout. Correctness is safe: every message
//   carries attempt_id and the reducers drop mismatches, so a late worker
//   can't complete a newer login. It is a RESOURCE leak (an isolated thread
//   + provider polling per abandoned attempt), not corruption.
//
// This test pins the contract: whatever the mechanism, asking a login to
// cancel must be observable by the worker.
#include "agtest.hpp"

#include "agentty/runtime/login.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"

using namespace agentty;

TEST_CASE("login: Esc requests cancellation through a channel the worker reads") {
    Model m;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot",
        .attempt_id = 7,
        .cancel = cancel,
    };

    auto [m2, _] = app::update(std::move(m), Msg{CloseLogin{}});

    // The modal closes...
    CHECK(std::holds_alternative<ui::login::Closed>(m2.ui.login));

    // ...and the cancellation is actually signalled. If this passes but the
    // worker still runs, the worker is reading a DIFFERENT channel than the
    // one the reducer writes — which is exactly the bug described above.
    CHECK(cancel->load(std::memory_order_acquire) == true);
}

// A late worker for a cancelled attempt must not be able to complete a NEWER
// login. This is the guard that keeps the leak from becoming corruption, so
// it is worth pinning independently of how cancellation is delivered.
TEST_CASE("login: a stale attempt's result is ignored") {
    Model m;
    m.ui.login = ui::login::DeviceWaiting{
        .provider = "copilot",
        .attempt_id = 2,          // the CURRENT attempt
        .cancel = std::make_shared<std::atomic_bool>(false),
    };

    // A worker from attempt 1 reports success.
    auto [m2, _] = app::update(std::move(m),
                               Msg{DeviceLoginDone{
                                   .provider = "copilot",
                                   .provider_label = "GitHub Copilot",
                                   .attempt_id = 1,       // STALE
                                   .error = std::nullopt,
                               }});

    // Still waiting on attempt 2 — the stale success was dropped.
    auto* w = std::get_if<ui::login::DeviceWaiting>(&m2.ui.login);
    REQUIRE(w != nullptr);
    CHECK(w->attempt_id == 2);
}
