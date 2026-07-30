// tool_stream_snapshot_test — canonical ID-addressed append/snapshot semantics.

#include <cstdio>
#include <string>

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"

namespace A = agentty;
namespace D = agentty::app::detail;

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

static A::Model apply(A::Model m, A::msg::StreamMsg event) {
    auto [next, cmd] = D::stream_update(std::move(m), std::move(event));
    (void)cmd;
    return next;
}

int main() {
    A::Model m;
    A::Message assistant;
    assistant.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(assistant));
    m.s.phase = A::phase::Streaming{A::phase::Active{}};

    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-a"}, A::ToolName{"edit"}});
    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-b"}, A::ToolName{"read"}});

    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-a"}, R"({"path":"a.cpp","old_string":"x"})"});
    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-a"},
        R"({"path":"a.cpp","old_string":"x","new_string":"y"})"});
    m = apply(std::move(m), A::StreamToolUseSnapshot{
        A::ToolCallId{"call-b"}, R"({"path":"b.cpp"})"});

    const auto& calls_before_end = m.d.current.messages.back().tool_calls;
    check(calls_before_end.size() == 2, "both interleaved calls remain distinct");
    check(calls_before_end[0].args_streaming ==
              R"({"path":"a.cpp","old_string":"x","new_string":"y"})",
          "new ACP snapshot replaces rather than concatenates prior JSON");
    check(calls_before_end[1].args_streaming == R"({"path":"b.cpp"})",
          "snapshot is routed by tool call id, not the newest call");

    m = apply(std::move(m), A::StreamToolUseEnd{A::ToolCallId{"call-a"}});
    m = apply(std::move(m), A::StreamObservedToolResult{
        A::ToolCallId{"call-a"}, false, "external edit complete"});
    auto& calls = m.d.current.messages.back().tool_calls;
    check(calls[0].args_streaming.empty(), "ID-addressed end consumes target snapshot");
    check(calls[0].args.value("new_string", "") == "y",
          "final executable arguments equal the latest snapshot");
    check(std::holds_alternative<A::ToolUse::Done>(calls[0].status),
          "delegated ACP result settles the card without host execution");
    check(calls[0].output() == "external edit complete",
          "delegated ACP output is preserved");
    check(calls[1].args_streaming == R"({"path":"b.cpp"})",
          "ending one call does not close an interleaved call");

    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-c"}, A::ToolName{"write"}});
    m = apply(std::move(m), A::StreamToolUseStart{
        A::ToolCallId{"call-d"}, A::ToolName{"bash"}});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-c"}, R"({"path":"c.cpp",")"});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-d"}, R"({"command":"true"})"});
    m = apply(std::move(m), A::StreamToolUseDelta{
        A::ToolCallId{"call-c"}, R"(content":"ok"})"});

    const auto& interleaved = m.d.current.messages.back().tool_calls;
    check(interleaved[2].args_streaming ==
              R"({"path":"c.cpp","content":"ok"})",
          "append deltas are assembled by call id across interleaving");
    check(interleaved[3].args_streaming == R"({"command":"true"})",
          "a sibling delta never appends to the newest call implicitly");

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
