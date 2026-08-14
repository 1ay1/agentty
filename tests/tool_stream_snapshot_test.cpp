// tool_stream_snapshot_test — canonical ID-addressed append/snapshot semantics.

#include <chrono>
#include <cstdio>
#include <string>
#include <utility>

#include "agentty/io/http.hpp"
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
    // `next` is a structured binding: no implicit move-on-return before C++23,
    // and Model's copy ctor is deleted.
    return std::move(next);
}

int main() {
    {
        auto decoded = A::http::test::decode_chunked({
            "4\r\nWi", "ki\r\n5;ext=yes\r\nped", "ia\r\n0\r\n", "\r\n"});
        check(decoded && *decoded == "Wikipedia",
              "incremental chunk decoder accepts valid extensions and splits");
        check(!A::http::test::decode_chunked({"1Z\r\nx\r\n0\r\n\r\n"}),
              "chunk decoder rejects non-hex size suffixes");
        check(!A::http::test::decode_chunked({"1\r\nxXX0\r\n\r\n"}),
              "chunk decoder rejects missing data CRLF");
        check(!A::http::test::decode_chunked({"5\r\nabc"}),
              "chunk decoder rejects truncated streams");
        auto upper = A::http::test::is_chunked({
            {"transfer-encoding", "Chunked"}});
        check(upper && *upper,
              "transfer coding token is parsed case-insensitively");
        check(!A::http::test::is_chunked({
                  {"transfer-encoding", "gzip, chunked"}}),
              "unsupported transfer coding stacks are rejected");
    }

    A::Model m;
    A::Message assistant;
    assistant.role = A::Role::Assistant;
    m.d.current.messages.push_back(std::move(assistant));
    A::phase::Active active;
    active.transient_retries = 3;
    active.last_event_at = std::chrono::steady_clock::now()
                         - std::chrono::seconds{119};
    m.s.phase = A::phase::Streaming{std::move(active)};

    m = apply(std::move(m), A::StreamHeartbeat{.transport_only = true});
    auto* transport_ctx = A::active_ctx(m.s.phase);
    check(transport_ctx && transport_ctx->transport_activity,
          "transport heartbeat records proxy/socket activity");
    check(transport_ctx && transport_ctx->transient_retries == 3,
          "transport heartbeat does not erase retry history");
    check(transport_ctx && std::chrono::steady_clock::now()
              - transport_ctx->last_event_at < std::chrono::seconds{2},
          "transport heartbeat refreshes the stall clock");

    m = apply(std::move(m), A::StreamBufferedWait{});
    auto* buffered_ctx = A::active_ctx(m.s.phase);
    check(buffered_ctx && buffered_ctx->transport_activity,
          "buffered gateway wait is tracked as transport activity");
    check(m.s.status.find("buffering output") != std::string::npos,
          "buffered gateway wait explains burst delivery to the user");

    m = apply(std::move(m), A::StreamHeartbeat{});
    auto* model_ctx = A::active_ctx(m.s.phase);
    check(model_ctx && model_ctx->transient_retries == 0,
          "semantic model heartbeat resets transient retries");

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
