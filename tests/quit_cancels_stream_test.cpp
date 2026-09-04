// quit_cancels_stream_test — ^C / Quit must trip the in-flight turn's cancel
// token so the streaming (or tool) worker unblocks immediately. Without it the
// event loop exits but ~BackgroundQueue's join waits for the HTTP worker to
// notice cancellation the slow way (next server byte / idle timeout) — the
// "^C doesn't quit during an animation" hang. Esc/CancelStream already did
// this; this pins that Quit does too.

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agtest.hpp"

#include "agentty/io/http.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace A = agentty;
namespace D = agentty::app::detail;

namespace {
void install_stub_deps() {
    A::app::install_deps(A::app::Deps{
        .stream        = [](A::provider::Request, A::provider::EventSink) {},
        .save_thread   = [](const A::Thread&) {},
        .delete_thread = [](const A::ThreadId&) {},
        .load_threads  = [] { return std::vector<A::Thread>{}; },
        .load_thread   = [](const A::ThreadId&) { return std::optional<A::Thread>{}; },
        .load_settings = [] { return A::store::Settings{}; },
        .save_settings = [](const A::store::Settings&) {},
        .new_thread_id = [] { return A::ThreadId{"t"}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });
}
} // namespace

TEST_CASE("Quit cancels the in-flight stream token") {
    install_stub_deps();

    // A model mid-stream: Streaming phase with an active ctx carrying a live
    // cancel token — the exact state where the spinner is animating.
    A::Model m;
    A::Message assistant;
    assistant.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(assistant));

    A::phase::Active active;
    auto token = std::make_shared<A::http::CancelToken>();
    active.cancel = token;
    m.s.phase = A::phase::Streaming{std::move(active)};

    check(!token->is_cancelled(), "precondition: token starts un-cancelled");

    // Dispatch Quit through the meta reducer.
    auto [next, cmd] = D::meta_update(std::move(m), A::Quit{});
    (void)next; (void)cmd;

    check(token->is_cancelled(),
          "Quit trips the active turn's cancel token so teardown doesn't block");
}

TEST_CASE("Quit with no active stream is safe") {
    install_stub_deps();
    // Idle model: no active_ctx. Quit must not crash on the null cancel path.
    A::Model m;
    auto [next, cmd] = D::meta_update(std::move(m), A::Quit{});
    (void)next; (void)cmd;
    check(true, "Quit is a no-op on the cancel token when nothing is in flight");
}
