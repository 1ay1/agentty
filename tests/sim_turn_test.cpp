// The turn state machine, under jaal::sim.
//
// agentty's phase is a variant — Idle, Streaming, AwaitingPermission,
// ExecutingTool — and the reducers move between them from a dozen places:
// a stream finishing, a tool asking for permission, Esc, a retry, a queued
// message firing on the way out of a turn. The transitions are covered
// one-by-one by hand-written tests, which is exactly the coverage shape
// that misses INTERLEAVINGS: a StreamError arriving while a permission
// prompt is up, a Tick landing between a tool result and the next request.
//
// sim<P> drives the real program with simulated time and a seeded, random
// task-completion order, and re-checks every invariant after every single
// message. `explore` then runs the same scenario over many seeds. What it
// buys over a hand-written test is the orders nobody thought to write down.
//
// The invariants here are deliberately about STRUCTURE, not behaviour —
// things that must hold after any message whatsoever, which is what makes
// them safe to assert against a random interleaving.
#include "agtest.hpp"

#include <jaal/host/sim.hpp>
#include <maya/device/events.hpp>

#include "agentty/runtime/app/program.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/io/persistence.hpp"
#include "agentty/io/blob_gc.hpp"
#include "agentty/util/modelsdev.hpp"

using namespace agentty;
using P = app::AgenttyApp;

// agentty's subscribe() routes maya's input events, and the kernel checks a
// program's routers against the host's event_type — so the sim has to name
// them even though it never produces one (it drives with messages).
using Ev = std::variant<maya::KeyEvent, maya::MouseEvent, maya::PasteEvent,
                        maya::FocusEvent, maya::ResizeEvent>;
using Sim = jaal::sim<P, Ev>;

namespace {

// A store that touches nothing. The sim is about the state machine, not
// about persistence — and a test that writes the real settings file would
// be a test that changes its own machine.
struct NullStore {
    std::vector<Thread> load_threads() { return {}; }
    std::optional<Thread> load_thread(const ThreadId&) { return std::nullopt; }
    void save_thread(const Thread&) {}
    void delete_thread(const ThreadId&) {}
    store::Settings load_settings() { return {}; }
    void save_settings(const store::Settings&) {}
    ThreadId new_id() { return ThreadId{"sim"}; }
    std::string title_from(std::string_view t) { return std::string{t}; }
};

struct NullProvider {
    provider::StreamResult stream(provider::Request, provider::EventSink) {
        return provider::StreamResult::failed("sim");
    }
};

NullProvider g_provider;
NullStore    g_store;

void install() { app::install(g_provider, g_store, auth::AuthHeader{}); }

// init() starts owned background threads (the blob GC, the models.dev
// refresh) that main() joins on its way out. A test binary has no such
// shutdown, and ~std::thread on a still-joinable handle calls
// std::terminate — the suite passed and then aborted with 134. Each sim
// TEST_CASE that runs init() joins them the way main() does.
struct JoinsBackgroundThreads {
    ~JoinsBackgroundThreads() {
        modelsdev::join_background_refresh();
        blobs::join_background_gc();
    }
};

// Every invariant that must hold after ANY message.
void arm_invariants(Sim& s) {
    // 1. The turn's context lives INSIDE the phase, so "active" and "has a
    //    context" are the same statement. This is the property that replaced
    //    the stale-turn guards: if they could drift apart, a late message
    //    from a dead turn would have something to corrupt again.
    s.check("active iff a turn context exists", [](const P::Model& m) {
        const bool has_ctx = agentty::active_ctx(m.s.phase) != nullptr;
        return m.s.active() == has_ctx;
    });

    // 2. Idle is exclusive with every working phase. A variant gives this
    //    structurally; the check is here so a future phase added as a BOOL
    //    beside it (the usual regression) fails immediately.
    s.check("idle excludes streaming and permission", [](const P::Model& m) {
        if (!m.s.is_idle()) return true;
        return !m.s.is_streaming() && !m.s.is_awaiting_permission();
    });

    // 3. A permission prompt implies a turn to return to. Losing this is how
    //    an approval ends up with nothing to resume.
    s.check("a permission prompt belongs to a turn", [](const P::Model& m) {
        if (!m.s.is_awaiting_permission()) return true;
        return agentty::active_ctx(m.s.phase) != nullptr;
    });

    // 4. The composer's cursor is always inside its text. Every editing arm
    //    maintains it independently, which is precisely why it is worth
    //    asserting globally.
    s.check("composer cursor is in range", [](const P::Model& m) {
        const int n = static_cast<int>(m.ui.composer.text.size());
        return m.ui.composer.cursor >= 0 && m.ui.composer.cursor <= n;
    });

    // 5. The frozen prefix can never claim more messages than exist. It is
    //    an index into the transcript, advanced from several places.
    s.check("frozen prefix fits the transcript", [](const P::Model& m) {
        return m.ui.frozen_through <= m.d.current.messages.size();
    });
}

}  // namespace

TEST_CASE("sim: the turn machine holds under a scripted interleaving") {
    install();
    JoinsBackgroundThreads joiner;   // see its comment

    jaal::sim_options opt;
    opt.max_steps = 2'000;
    jaal::sim<P, Ev> s(0xA9E77C0Dull, opt);
    arm_invariants(s);

    // A plausible session: type, submit, a tool asks, the user answers,
    // Esc lands somewhere in the middle. The POINT is that the sim decides
    // when each task result arrives relative to these.
    s.at(std::chrono::milliseconds{0},   Msg{msg::ComposerMsg{ComposerCharInput{U'h'}}});
    s.at(std::chrono::milliseconds{1},   Msg{msg::ComposerMsg{ComposerCharInput{U'i'}}});
    s.at(std::chrono::milliseconds{2},   Msg{msg::ComposerMsg{ComposerSubmit{}}});
    s.at(std::chrono::milliseconds{20},  Msg{msg::StreamMsg{StreamStarted{}}});
    s.at(std::chrono::milliseconds{30},  Msg{msg::StreamMsg{StreamTextDelta{"one"}}});
    s.at(std::chrono::milliseconds{40},  Msg{msg::StreamMsg{CancelStream{}}});
    s.at(std::chrono::milliseconds{50},  Msg{msg::StreamMsg{StreamTextDelta{"late"}}});
    s.at(std::chrono::milliseconds{60},  Msg{msg::StreamMsg{StreamFinished{}}});

    auto r = s.run();
    CHECK_MESSAGE(r.ok(), r.describe());
}

// The same scenario over many seeds. Each seed reorders the task results
// and breaks ties differently, so this is the part that covers the
// interleavings nobody wrote down.
TEST_CASE("sim: the turn machine holds across 200 seeds") {
    install();
    JoinsBackgroundThreads joiner;   // see its comment

    jaal::sim_options opt;
    opt.max_steps = 2'000;
    // A late result is the interesting case, so let latency vary widely.
    opt.task_latency_max = std::chrono::milliseconds{40};

    auto out = jaal::explore<P, Ev>(1, 200, opt, [](Sim& s) {
        arm_invariants(s);
        s.at(std::chrono::milliseconds{0},  Msg{msg::ComposerMsg{ComposerCharInput{U'x'}}});
        s.at(std::chrono::milliseconds{1},  Msg{msg::ComposerMsg{ComposerSubmit{}}});
        s.at(std::chrono::milliseconds{10}, Msg{msg::StreamMsg{StreamStarted{}}});
        s.at(std::chrono::milliseconds{15}, Msg{msg::StreamMsg{StreamTextDelta{"a"}}});
        s.at(std::chrono::milliseconds{25}, Msg{msg::StreamMsg{CancelStream{}}});
        s.at(std::chrono::milliseconds{30}, Msg{msg::StreamMsg{StreamFinished{}}});
    });

    const std::string why =
        out.failure ? out.failure->describe() : std::string{"ok"};
    CHECK_MESSAGE(out.ok(), why);
}

// Fault injection: a task that crashes or never returns must not leave the
// machine wedged. This is the path a flaky provider actually takes.
TEST_CASE("sim: a lost or crashing task leaves the machine consistent") {
    install();
    JoinsBackgroundThreads joiner;   // see its comment

    jaal::sim_options opt;
    opt.max_steps  = 2'000;
    opt.task_crash = 0.15;   // the body throws
    opt.task_lose  = 0.15;   // the body never finishes

    auto out = jaal::explore<P, Ev>(1, 100, opt, [](Sim& s) {
        arm_invariants(s);
        s.at(std::chrono::milliseconds{0}, Msg{msg::ComposerMsg{ComposerCharInput{U'q'}}});
        s.at(std::chrono::milliseconds{1}, Msg{msg::ComposerMsg{ComposerSubmit{}}});
        s.at(std::chrono::milliseconds{9}, Msg{msg::StreamMsg{CancelStream{}}});
    });

    const std::string why =
        out.failure ? out.failure->describe() : std::string{"ok"};
    CHECK_MESSAGE(out.ok(), why);
}
