// proactive_deferred_test — the DEFERRED same-turn grounding reducer path.
//
// The proactive pre-turn RAG "retrieve-then-launch" design: when the
// synchronous hedge in submit_message misses (a large/slow corpus whose
// dense query-embed round-trip can't clear the small budget), submit_message
// leaves the phase in Streaming{ctx} (so the status-bar spinner is live — the
// UI never feels hung) and HOLDS the stream launch behind an isolated
// retrieval task. When that lands it dispatches ProactiveContextReady, whose
// reducer arm (stream.cpp) must:
//
//   1. inject the <retrieved-context> block into THIS turn, right before the
//      trailing empty assistant placeholder (wire order:
//      User(question) → User(context) → Assistant(reply)),
//   2. NOT freeze the live placeholder (only the inserted context msg),
//   3. clear the "retrieving context…" status,
//   4. return a non-none Cmd (the held-back launch_stream), and
//   5. DROP a stale arrival if the user cancelled to Idle meanwhile.
//
// This drives the REAL stream_update reducer with stub deps (no provider I/O;
// the returned launch Cmd is inspected, never executed).

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

using agentty::Model;
using agentty::Message;
using agentty::Role;
using agentty::ThreadId;
using agentty::ModelId;
namespace phase = agentty::phase;
namespace msg   = agentty::msg;
namespace store = agentty::store;
namespace provider = agentty::provider;

static int g_checks = 0;
static int g_fails  = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fails; std::printf("FAIL: %s\n", what); }
    else     { std::printf("ok:   %s\n", what); }
}

// A Model shaped exactly as submit_message leaves it on a hedge MISS: the
// user turn + trailing empty assistant placeholder, phase Streaming, and the
// "retrieving context…" status sticky.
static Model make_deferred_model() {
    Model m;
    m.d.current.id = ThreadId{"deferred"};
    m.d.model_id   = ModelId{"claude-opus-4-5"};

    Message user;
    user.role = Role::User;
    user.text = "how does the retry backoff work in the http client";
    m.d.current.messages.push_back(std::move(user));

    Message placeholder;   // the live assistant tail submit_message pushes
    placeholder.role = Role::Assistant;
    m.d.current.messages.push_back(std::move(placeholder));

    m.s.phase        = phase::Streaming{phase::Active{}};
    m.s.status       = "retrieving context\xE2\x80\xA6";
    m.s.status_until = {};
    return m;
}

int main() {
    // Stub deps — launch_stream (built inside the handler) reads deps() and
    // walks the tool registry; it must not touch the network. The returned
    // Cmd is inspected, never run.
    agentty::app::install_deps(agentty::app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const agentty::Thread&) {},
        .load_threads  = [] { return std::vector<agentty::Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<agentty::Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"stub"}; },
        .title_from    = [](std::string_view) { return std::string{}; },
        .auth          = {},
    });

    const std::string block =
        "<retrieved-context>\n[docs:http/retry.md:12]\nexponential backoff "
        "with full jitter, capped at 30s\n</retrieved-context>";

    // ── 1. Happy path: a real block injects same-turn + launches ────────
    {
        Model m = make_deferred_model();
        const std::size_t before = m.d.current.messages.size();

        auto [m2, cmd] = agentty::app::detail::stream_update(
            std::move(m), msg::StreamMsg{agentty::ProactiveContextReady{block, 0.82}});

        auto& msgs = m2.d.current.messages;
        check(msgs.size() == before + 1,
              "deferred: block inserted as one new message");

        // Order must be User(question) → User(context) → Assistant(placeholder)
        check(msgs.size() >= 3, "deferred: at least 3 messages");
        const auto& ctx = msgs[msgs.size() - 2];
        const auto& tail = msgs.back();
        check(ctx.role == Role::User && ctx.proactive_context,
              "deferred: context msg is a proactive User message");
        check(ctx.text.find("retrieved-context") != std::string::npos,
              "deferred: context msg carries the retrieved block");
        check(ctx.proactive_confidence > 0.81 && ctx.proactive_confidence < 0.83,
              "deferred: retrieval confidence threaded onto the message");
        check(tail.role == Role::Assistant && tail.text.empty(),
              "deferred: trailing placeholder preserved (live tail)");

        // The live placeholder must NOT be frozen (its stream reveal would be
        // sealed). frozen_through counts the frozen prefix; it must stop
        // before the placeholder.
        check(m2.ui.frozen_through < msgs.size(),
              "deferred: placeholder left unfrozen");
        check(m2.ui.frozen_through == msgs.size() - 1,
              "deferred: frozen prefix covers the inserted context msg");

        // Status cleared; a launch Cmd was issued.
        check(m2.s.status.empty(),
              "deferred: 'retrieving context…' status cleared");
        check(!cmd.is_none(),
              "deferred: a stream-launch Cmd is returned");
        // Phase still active (launch_stream needs the live ctx).
        check(!m2.s.is_idle(), "deferred: phase stays active for launch");
    }

    // ── 2. Empty block (retrieval cleared no confidence bar) still ──────
    //    launches the turn — just with no context injected.
    {
        Model m = make_deferred_model();
        const std::size_t before = m.d.current.messages.size();

        auto [m2, cmd] = agentty::app::detail::stream_update(
            std::move(m), msg::StreamMsg{agentty::ProactiveContextReady{std::string{}}});

        check(m2.d.current.messages.size() == before,
              "deferred(empty): no message injected");
        check(m2.s.status.empty(),
              "deferred(empty): status cleared");
        check(!cmd.is_none(),
              "deferred(empty): stream still launches (no grounding, no hang)");
    }

    // ── 3. Stale arrival after cancel: user hit Esc → Idle while the ────
    //    retrieval was in flight. The block must be DROPPED, no launch.
    {
        Model m = make_deferred_model();
        m.s.phase = phase::Idle{};   // user cancelled
        const std::size_t before = m.d.current.messages.size();

        auto [m2, cmd] = agentty::app::detail::stream_update(
            std::move(m), msg::StreamMsg{agentty::ProactiveContextReady{block}});

        check(m2.d.current.messages.size() == before,
              "deferred(stale): no injection after cancel-to-Idle");
        check(cmd.is_none(),
              "deferred(stale): no stream launched after cancel");
        check(m2.s.is_idle(),
              "deferred(stale): phase stays Idle");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails == 0) { std::printf("PASSED\n"); return 0; }
    std::printf("FAILED\n");
    return 1;
}
