// speculative_dispatch_test — the mid-stream read-only tool launch.
//
// StreamToolUseEnd on a READ-ONLY, permission-free tool while the phase is
// Streaming must promote the call Pending→Running and return a run command
// IMMEDIATELY (overlapping its I/O with the rest of the stream), instead of
// waiting for StreamFinished → kick_pending_tools. Write/Exec/Net tools and
// permission-gated calls must stay Pending (the planner owns them).
//
// Also locks the two safety interactions:
//   • ToolExecOutput landing while STILL Streaming must not kick (no
//     continuation launch against a live wire).
//   • kick_pending_tools after StreamFinished adopts a still-Running
//     speculative tool into phase::ExecutingTool (no wedge).

#include "agtest.hpp"

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/provider/selection.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace agentty;

static store::Settings g_settings;
static void install_stub_deps() {
    app::install_deps(app::Deps{
        .stream         = [](auto, auto) {},
        .save_thread    = [](const auto&) {},
        .delete_thread  = [](const auto&) {},
        .load_threads   = [] { return std::vector<Thread>{}; },
        .load_thread    = [](const auto&) -> std::optional<Thread> { return std::nullopt; },
        .load_settings  = [] { return g_settings; },
        .save_settings  = [](const store::Settings& x) { g_settings = x; },
        .new_thread_id  = [] { return ThreadId{}; },
        .title_from     = [](std::string_view t) { return std::string{t}; },
        .auth           = auth::AuthHeader{auth::ApiKeyHeader{std::string{"k"}}},
    });
}

// A Model mid-stream: user turn + assistant placeholder carrying one
// streaming tool call whose args have not yet closed.
static Model streaming_model(const char* tool_name, const char* args_json) {
    Model m;
    m.d.current.id = ThreadId{"spec-test"};
    Message u; u.role = Role::User; u.text = "go";
    m.d.current.messages.push_back(std::move(u));
    Message a; a.role = Role::Assistant;
    ToolUse tc;
    tc.id   = ToolCallId{"tc-1"};
    tc.name = ToolName{tool_name};
    tc.args_streaming = args_json;
    a.tool_calls.push_back(std::move(tc));
    m.d.current.messages.push_back(std::move(a));
    m.s.phase = phase::Streaming{phase::Active{}};
    return m;
}

TEST_CASE("speculative read-only dispatch at StreamToolUseEnd") {
    install_stub_deps();
    provider::select(provider::parse_selection("anthropic"));

    // ── read-only tool (glob): promoted to Running mid-stream ──────────
    {
        auto m = streaming_model("glob", R"({"pattern":"*.cpp"})");
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        const auto& tc = m2.d.current.messages.back().tool_calls.front();
        CHECK(tc.is_running(),
              "read-only tool must go Running at ToolUseEnd (was not)");
        CHECK(m2.s.is_streaming(),
              "phase must STAY Streaming during speculative execution");
        CHECK(!cmd.is_none(),
              "a run command must be returned immediately");
    }

    // ── write tool (edit): must stay Pending for the planner ───────────
    {
        auto m = streaming_model("edit",
            R"({"path":"a.txt","edits":[{"old_text":"a","new_text":"b"}]})");
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        const auto& tc = m2.d.current.messages.back().tool_calls.front();
        CHECK(tc.is_pending(),
              "write tool must NOT be speculatively launched");
    }

    // ── exec tool (bash): must stay Pending ────────────────────────────
    {
        auto m = streaming_model("bash", R"({"command":"ls"})");
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        const auto& tc = m2.d.current.messages.back().tool_calls.front();
        CHECK(tc.is_pending(),
              "exec tool must NOT be speculatively launched");
    }

    // ── pending permission modal: nothing launches ─────────────────────
    {
        auto m = streaming_model("glob", R"({"pattern":"*.h"})");
        m.d.pending_permission = PendingPermission{
            ToolCallId{"other"}, ToolName{"bash"}, "?"};
        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        const auto& tc = m2.d.current.messages.back().tool_calls.front();
        CHECK(tc.is_pending(),
              "speculation must never pre-empt a pending permission");
    }

    // ── ToolExecOutput mid-stream: lands, but no kick ──────────────────
    {
        auto m = streaming_model("glob", R"({"pattern":"*.cpp"})");
        auto [m2, _] = app::update(std::move(m),
                                   Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        CHECK(m2.d.current.messages.back().tool_calls.front().is_running(),
              "precondition: tool Running");
        auto [m3, cmd3] = app::update(std::move(m2),
            Msg{ToolExecOutput{ToolCallId{"tc-1"},
                               std::string{"file list"}}});
        const auto& tc = m3.d.current.messages.back().tool_calls.front();
        CHECK(tc.is_done(), "output must land on the speculative tool");
        CHECK(m3.s.is_streaming(),
              "phase must remain Streaming after mid-stream tool output");
        CHECK(cmd3.is_none(),
              "no kick while the wire is still streaming");
    }

    // ── kick after StreamFinished adopts a still-Running speculative ───
    {
        auto m = streaming_model("glob", R"({"pattern":"*.cpp"})");
        auto [m2, _] = app::update(std::move(m),
                                   Msg{StreamToolUseEnd{ToolCallId{"tc-1"}}});
        // Stream ends while the tool is still Running.
        auto kick = app::cmd::kick_pending_tools(m2);
        CHECK(std::holds_alternative<phase::ExecutingTool>(m2.s.phase),
              "kick must adopt a Running speculative tool into "
              "ExecutingTool so its completion kick isn't suppressed");
    }
}
