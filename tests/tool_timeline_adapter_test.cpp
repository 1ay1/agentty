// Exhaustive data-level coverage for native tool timeline cards.
// Every lifecycle state must render useful body text rather than a blank card.

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_body_preview.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "maya/widget/tool_body_preview.hpp"

namespace A = agentty;
namespace U = agentty::ui;
using Kind = maya::ToolBodyPreview::Kind;

static int checks = 0;
static int failures = 0;

static void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label.c_str());
    }
}

static A::ToolUse make_tool(std::string name, A::ToolUse::Status status,
                            nlohmann::json args = nlohmann::json::object()) {
    static int sequence = 0;
    A::ToolUse tc;
    tc.id = A::ToolCallId{"card-" + std::to_string(++sequence)};
    tc.name = A::ToolName{std::move(name)};
    tc.args = std::move(args);
    tc.status = std::move(status);
    return tc;
}

static bool visible(const maya::ToolBodyPreview::Config& body) {
    switch (body.kind) {
    case Kind::None:     return false;
    case Kind::EditDiff: return !body.hunks.empty();
    case Kind::TodoList: return !body.todos.empty();
    default:             return !body.text.empty();
    }
}

int main() {
    static constexpr std::array<const char*, 33> native_tools = {
        "read", "edit", "write", "move", "remove", "bash",
        "process_start", "process_poll", "process_stop",
        "grep", "glob", "list_dir", "repo_map", "todo",
        "web_fetch", "web_search", "find_definition", "find_references",
        "diagnostics", "test", "git_status", "git_diff", "git_log",
        "git_show", "git_blame", "git_commit", "remember", "forget",
        "wipe_memory", "task", "skill", "search_docs", "search_code"
    };

    for (const char* name : native_tools) {
        auto expect_visible = [&](A::ToolUse tc, const char* state) {
            const auto body = U::tool_body_preview_config(tc);
            check(visible(body), std::string{name} + " " + state + " has a body");
        };

        expect_visible(make_tool(name, A::ToolUse::Pending{}), "pending");
        expect_visible(make_tool(name, A::ToolUse::Approved{}), "approved");
        expect_visible(make_tool(name, A::ToolUse::Running{}), "silent running");

        auto live = make_tool(name, A::ToolUse::Running{});
        std::get<A::ToolUse::Running>(live.status).progress_text = "live progress";
        const auto live_body = U::tool_body_preview_config(live);
        check(visible(live_body) && live_body.text == "live progress"
                  && live_body.is_streaming,
              std::string{name} + " exposes live progress");

        expect_visible(make_tool(name, A::ToolUse::Done{}), "empty success");
        expect_visible(make_tool(name, A::ToolUse::Done{{}, {}, "result text"}),
                       "success output");

        const auto empty_failure = U::tool_body_preview_config(
            make_tool(name, A::ToolUse::Failed{}));
        check(empty_failure.kind == Kind::Failure && !empty_failure.text.empty(),
              std::string{name} + " explains empty failure");

        const auto failure = U::tool_body_preview_config(
            make_tool(name, A::ToolUse::Failed{{}, {}, "specific error"}));
        check(visible(failure) && failure.text.find("specific error") != std::string::npos,
              std::string{name} + " preserves failure output");

        expect_visible(make_tool(name, A::ToolUse::Rejected{}), "rejected");
        check(!U::tool_display_name(name).empty(),
              std::string{name} + " has a display name");
    }

    // Future native and MCP tools inherit the same fallback without needing
    // to be added to the dispatcher's name list.
    auto external = make_tool("mcp__example__long_operation", A::ToolUse::Running{});
    auto external_body = U::tool_body_preview_config(external);
    check(external_body.kind == Kind::CodeBlock && !external_body.text.empty(),
          "unknown MCP tool gets running fallback");
    check(U::tool_display_name("mcp_search_tools") == "MCP Tool Search"
              && U::tool_timeline_detail(make_tool("mcp_search_tools",
                  A::ToolUse::Running{}, {{"query", "browser automation"}}))
                  == "browser automation",
          "MCP catalog broker has a useful label and query");
    check(U::tool_display_name("mcp_call") == "MCP Call"
              && U::tool_timeline_detail(make_tool("mcp_call",
                  A::ToolUse::Running{}, {{"name", "mcp__browser__click"}}))
                  == "mcp__browser__click",
          "MCP call broker identifies its target");

    auto invisible_live = make_tool("mcp__example__silent", A::ToolUse::Running{});
    std::get<A::ToolUse::Running>(invisible_live.status).progress_text =
        "\x1b[2K\r\n\t";
    const auto invisible_live_body = U::tool_body_preview_config(invisible_live);
    check(invisible_live_body.kind == Kind::CodeBlock
              && invisible_live_body.text.find("Waiting") != std::string::npos,
          "ANSI-only progress gets an explanatory running fallback");

    const auto whitespace_done = U::tool_body_preview_config(make_tool(
        "mcp__example__silent", A::ToolUse::Done{{}, {}, " \n\t"}));
    check(whitespace_done.text.find("Completed successfully") != std::string::npos,
          "whitespace-only success output gets an explanation");

    const auto ansi_failure = U::tool_body_preview_config(make_tool(
        "mcp__example__silent", A::ToolUse::Failed{{}, {}, "\x1b[31m\x1b[0m"}));
    check(ansi_failure.kind == Kind::Failure
              && ansi_failure.text.find("without an error") != std::string::npos,
          "ANSI-only failure output gets an explanation");

    // Structured renderers still win when they have meaningful content.
    auto read = make_tool("read", A::ToolUse::Done{{}, {}, "one\ntwo"},
                          {{"path", "sample.cpp"}});
    check(U::tool_body_preview_config(read).kind == Kind::FileRead,
          "read keeps structured file body");

    auto todo = make_tool("todo", A::ToolUse::Running{},
        {{"todos", {{{"content", "Audit cards"}, {"status", "in_progress"}}}}});
    check(U::tool_body_preview_config(todo).kind == Kind::TodoList,
          "todo keeps structured checklist");

    auto failed_todo = make_tool("todo", A::ToolUse::Failed{{}, {}, "todo failed"},
        {{"todos", {{{"content", "Audit cards"}, {"status", "in_progress"}}}}});
    check(U::tool_body_preview_config(failed_todo).kind == Kind::Failure,
          "failed todo shows error instead of stale checklist");

    // Representative detail mappings prove formerly anonymous tools explain
    // their operation in the event header while the fallback body explains
    // lifecycle state.
    check(U::tool_timeline_detail(make_tool("move", A::ToolUse::Running{},
              {{"source", "a"}, {"destination", "b"}})).find("a") != std::string::npos,
          "move detail includes source");
    check(U::tool_timeline_detail(make_tool("find_references", A::ToolUse::Running{},
              {{"symbol", "Widget"}, {"path", "src"}})).find("Widget") != std::string::npos,
          "references detail includes symbol");
    check(U::tool_timeline_detail(make_tool("search_code", A::ToolUse::Running{},
              {{"query", "retry handling"}})) == "retry handling",
          "semantic search detail includes query");
    check(!U::tool_timeline_detail(make_tool("remove", A::ToolUse::Pending{},
              {{"path", "tmp"}, {"recursive", "not-a-boolean"}})).empty(),
          "malformed remove boolean cannot break rendering");
    check(U::tool_timeline_detail(make_tool("wipe_memory", A::ToolUse::Pending{},
              {{"scope", "project"}, {"confirm", nullptr}})).find("preview")
              != std::string::npos,
          "malformed wipe confirmation cannot break rendering");
    check(!U::tool_timeline_detail(make_tool("search_code", A::ToolUse::Pending{},
              {{"query", nlohmann::json::array({"wrong type"})}})).empty(),
          "malformed string argument cannot break rendering");
    check(!U::tool_timeline_detail(make_tool("git_blame", A::ToolUse::Pending{},
              {{"path", "file.cpp"}, {"start_line", "wrong type"}})).empty(),
          "malformed integer argument cannot break rendering");

    const nlohmann::json malformed_edit_args = {{"edits", {
        {{"old_text", nlohmann::json::array()}, {"new_text", nullptr}}
    }}};
    for (auto tc : {
            make_tool("edit", A::ToolUse::Pending{}, malformed_edit_args),
            make_tool("edit", A::ToolUse::Running{}, malformed_edit_args),
            make_tool("edit", A::ToolUse::Done{{}, {}, "done"}, malformed_edit_args)}) {
        check(visible(U::tool_body_preview_config(tc)),
              "malformed nested edit fields remain renderable");
    }

    const nlohmann::json malformed_todo_args = {{"todos", {
        {{"content", nlohmann::json::array()}, {"status", nullptr}}
    }}};
    auto malformed_todo = make_tool("todo", A::ToolUse::Running{}, malformed_todo_args);
    check(visible(U::tool_body_preview_config(malformed_todo))
              && !U::tool_timeline_detail(malformed_todo).empty(),
          "malformed nested todo fields remain renderable");

    auto huge_read = make_tool("read", A::ToolUse::Done{{}, {}, "line"},
        {{"path", "file.cpp"},
         {"offset", std::numeric_limits<std::uint64_t>::max()}});
    check(U::tool_body_preview_config(huge_read).start_line == 1,
          "out-of-range read offset safely uses default line");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
