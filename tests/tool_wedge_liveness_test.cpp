// tool_wedge_liveness_test — the hung-syscall wedge net measures liveness
// from the LAST PROGRESS event, not from launch.
//
// Regression target: the wedge net in update/meta.cpp (Tick) failed ANY
// running tool 330s after it STARTED, discarding its work — even a healthy
// long-runner that was actively emitting progress. A `task` subagent doing an
// exhaustive multi-minute audit (stepping through tool after tool, emitting a
// progress snapshot each step) would get force-failed at the flat cap and its
// whole report thrown away. Observed: parallel explorers, two guillotined at
// exactly 330s while a third that happened to finish sooner survived.
//
// The fix: Running carries last_progress_at, stamped on every ToolExecProgress
// snapshot; the wedge measures now - max(started_at, last_progress_at). A tool
// making progress keeps resetting the cap; a tool truly hung on a blocking
// syscall emits nothing, so the net trips exactly as before.
//
// This drives the REAL meta_update reducer with a Tick and asserts:
//   A. started long ago BUT progress just now  → tool stays Running.
//   B. started long ago AND silent since launch → tool is Failed (wedged).
//   C. progress that is ITSELF older than the cap → still wedged (the stale
//      progress doesn't rescue a since-gone-silent tool).

#include <chrono>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

using namespace agentty;
using std::chrono::steady_clock;
using std::chrono::seconds;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (ok) std::printf("ok:   %s\n", what);
    else  { std::printf("FAIL: %s\n", what); ++g_fails; }
}

// A model parked in ExecutingTool with one Running tool whose launch and
// last-progress times we control. `progress_ago` < 0 means "no progress yet".
Model model_with_running_tool(seconds started_ago, seconds progress_ago) {
    Model m;
    m.s.phase = phase::ExecutingTool{phase::Active{}};

    Message asst;
    asst.role = Role::Assistant;
    ToolUse tc;
    tc.id   = ToolCallId{"tc-1"};
    tc.name = ToolName{"task"};
    tc.args = nlohmann::json{{"prompt", "exhaustive audit"}};

    auto now = steady_clock::now();
    ToolUse::Running run;
    run.started_at = now - started_ago;
    if (progress_ago.count() >= 0) {
        run.progress_text     = "step 12: reading spec/rcp-1.0.md";
        run.last_progress_at  = now - progress_ago;
    }
    tc.status = run;
    asst.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(asst));
    return m;
}

bool tool_running(const Model& m) {
    for (const auto& msg : m.d.current.messages)
        for (const auto& tc : msg.tool_calls)
            if (tc.id == ToolCallId{"tc-1"}) return tc.is_running();
    return false;
}
bool tool_failed(const Model& m) {
    for (const auto& msg : m.d.current.messages)
        for (const auto& tc : msg.tool_calls)
            if (tc.id == ToolCallId{"tc-1"})
                return std::holds_alternative<ToolUse::Failed>(tc.status);
    return false;
}

Model tick(Model m) {
    auto [next, _] = app::detail::meta_update(std::move(m), msg::MetaMsg{Tick{}});
    return std::move(next);
}

} // namespace

int main() {
    // The wedge path calls kick_pending_tools, which reaches deps(). Install a
    // no-op Deps so the reducer runs without a real Provider/Store.
    app::install_deps(app::Deps{
        .stream        = [](provider::Request, provider::EventSink) {},
        .save_thread   = [](const Thread&) {},
        .load_threads  = [] { return std::vector<Thread>{}; },
        .load_thread   = [](const ThreadId&) { return std::optional<Thread>{}; },
        .load_settings = [] { return store::Settings{}; },
        .save_settings = [](const store::Settings&) {},
        .new_thread_id = [] { return ThreadId{"t-test"}; },
        .title_from    = [](std::string_view) { return std::string{"t"}; },
    });

    // ── A. Long-running but ACTIVELY progressing → NOT wedged. ──────────
    // Launched 20 minutes ago, but a progress snapshot landed 1s ago. This is
    // the exact case that used to be wrongly killed.
    {
        Model m = model_with_running_tool(seconds{1200}, seconds{1});
        m = tick(std::move(m));
        check(tool_running(m), "A: healthy long-runner with fresh progress stays Running");
        check(!tool_failed(m), "A: it is NOT force-failed by the wedge net");
    }

    // ── B. Long-running and SILENT since launch → wedged. ───────────────
    // Launched 400s ago, never emitted progress. Genuine hung-syscall shape.
    {
        Model m = model_with_running_tool(seconds{400}, seconds{-1});
        m = tick(std::move(m));
        check(tool_failed(m), "B: a tool silent since launch past the cap is failed");
        check(!tool_running(m), "B: it no longer shows as Running");
    }

    // ── C. Progress exists but is itself STALE past the cap → wedged. ───
    // Started 700s ago; last sign of life 400s ago (> 330s). The stale
    // progress must NOT keep a since-dead tool alive.
    {
        Model m = model_with_running_tool(seconds{700}, seconds{400});
        m = tick(std::move(m));
        check(tool_failed(m), "C: a tool whose last progress is older than the cap is failed");
    }

    // ── D. Just under the cap with no progress → still Running. ─────────
    // Launched 300s ago (< 330s), no progress. The net must not trip early.
    {
        Model m = model_with_running_tool(seconds{300}, seconds{-1});
        m = tick(std::move(m));
        check(tool_running(m), "D: below the cap, a silent tool is left alone");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
