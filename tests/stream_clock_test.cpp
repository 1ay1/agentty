// stream_clock_test.cpp — the per-stream clock that ttft is measured from.
//
// sail3r's report on PR #65: stats are precise with one model on a paid
// provider, and "balloon out of proportion" on OpenRouter's free tier, where
// automatic model selection and retries are constant. He suspected the PR. It
// was a second, older bug the PR happened to make visible.
//
// finalize_turn seals ttft = first_delta_at - a->started. `started` was only
// (re)set by the StreamStarted event, and only two transport families emit
// that: Anthropic (message_start) and the Responses codec. The OpenAI-chat
// transport -- OpenRouter, Groq, Mistral, llama.cpp, LM Studio -- and Ollama
// emit none. On those, `started` held the value submit stamped for the WHOLE
// turn, so ttft absorbed:
//   * every retry's backoff sleep
//   * every prior sub-turn's generation and every tool's runtime, because the
//     post-tool continuation never reset the clock either
//
// The fix stamps the clock in launch_stream, the one point every stream launch
// passes through. These tests drive the real reducer with NO StreamStarted, the
// way an OpenAI-chat transport behaves, and assert ttft measures one stream.
#include <chrono>
#include <string>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/store/store.hpp"
#include "agtest.hpp"

using namespace agentty;
namespace detail = agentty::app::detail;
namespace cmd = agentty::app::cmd;
using Clock = std::chrono::steady_clock;

namespace {

void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .delete_thread = [](const ThreadId&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-clock"}; },
    });
}

// Every fold here runs at an explicit time, so ttft comes out exact. T0 is
// when the stream under test launches.
constexpr Clock::time_point T0 = app::kTestEpoch + std::chrono::hours{1};
using std::chrono::milliseconds;

// A model mid-turn: user message, an assistant placeholder, Streaming phase
// with a ctx whose clock was started `ago` before T0 -- standing in for a
// long earlier stretch of the same turn (a prior sub-turn, a tool, a retry
// backoff) that the next stream must NOT inherit.
Model streaming_since(Clock::duration ago) {
    Model m;
    m.now = T0;
    m.d.current.id = ThreadId{"t-clock"};
    Message u;
    u.role = Role::User;
    u.text = "go";
    m.d.current.messages.push_back(std::move(u));
    Message a;
    a.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(a));

    phase::Active ctx;
    ctx.started       = T0 - ago;
    ctx.last_event_at = ctx.started;
    m.s.phase = phase::Streaming{std::move(ctx)};
    return m;
}

Model step(Model m, msg::StreamMsg sm, Clock::time_point at) {
    return app::update(std::move(m), Msg{std::move(sm)}, at).first;
}

// Seal the turn the way the real stream does: one delta at `at`, then finish.
const Message::Telemetry& seal(Model& m, Clock::time_point at) {
    m = step(std::move(m), StreamTextDelta{"hello"}, at);
    m = step(std::move(m), StreamFinished{StopReason::EndTurn}, at);
    REQUIRE(m.d.current.messages.back().telemetry.has_value());
    return *m.d.current.messages.back().telemetry;
}

}  // namespace

TEST_CASE("stream clock: a relaunch restarts ttft, with no StreamStarted") {
    install_stub_deps();

    // The turn has been running for 90 s before this stream launches --
    // retries, a prior sub-turn, a slow tool. An OpenAI-chat transport then
    // sends deltas WITHOUT a StreamStarted event.
    Model m = streaming_since(std::chrono::seconds{90});
    (void)cmd::launch_stream(m);   // the relaunch: retry or post-tool, at T0

    const auto& t = seal(m, T0 + milliseconds{20});
    // ttft is this stream's wait, not the 90 s the turn had already spent.
    CHECK_MESSAGE(t.ttft_ms == 20,
                  "ttft must measure THIS stream, not the whole turn so far: "
                  "an OpenAI-chat transport never emits StreamStarted, so "
                  "launch_stream is the only thing that can restart the clock");
}

TEST_CASE("stream clock: without a relaunch the stale origin is what leaks") {
    install_stub_deps();

    // The negative control. Same model, but deliver deltas straight onto the
    // 90-s-old clock, as happened on every OpenAI-chat sub-turn before the
    // fix. If this ever stops reporting ~90 s, the clock is being restarted
    // somewhere new and the test above needs re-reading.
    Model m = streaming_since(std::chrono::seconds{90});
    const auto& t = seal(m, T0);
    CHECK(t.ttft_ms == 90'000);
}

TEST_CASE("stream clock: StreamStarted still moves the origin forward") {
    install_stub_deps();

    // Anthropic emits StreamStarted on message_start. Both resets compose:
    // launch_stream stamps the request, StreamStarted re-stamps when the
    // provider answers. ttft then excludes connect time, as it always did.
    Model m = streaming_since(std::chrono::seconds{90});
    (void)cmd::launch_stream(m);
    m = step(std::move(m), StreamStarted{}, T0 + milliseconds{30});

    const auto& t = seal(m, T0 + milliseconds{35});
    CHECK(t.ttft_ms == 5);   // the 30 ms before StreamStarted is excluded
}
