// anthropic_sse_golden_test — byte/shape-identity guard for the Anthropic
// SSE event parser (dispatch_event / feed_sse via StreamCtx).
//
// The SSE parser is being split out of transport.cpp into its own module.
// That split must not change which agentty Msgs a given SSE stream produces.
// This test scripts a realistic Anthropic stream — message_start (+usage),
// a text content block with deltas, a tool_use block with input_json deltas,
// a message_delta carrying stop_reason + final usage, and message_stop — then
// pins a canonical rendering of the emitted Msg sequence. Any drift in the
// parse behavior (dropped event, reordered emit, changed field) trips it.
//
// Drives parse_sse_for_test, which runs the SAME feed_sse → dispatch_event
// path the live on_chunk uses.

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/runtime/msg.hpp"

namespace {


using namespace agentty;
using agentty::msg::StreamMsg;

// Canonical one-line rendering of a stream Msg. Msg is a variant-of-variants:
// the stream events are wrapped as Msg{StreamMsg{StreamXxx{...}}}, so we peel
// the outer Msg to its StreamMsg, then visit the concrete event. Deterministic
// — captures the discriminant + wire-relevant fields, nothing timing-dependent.
std::string render_stream(const StreamMsg& sm) {
    return std::visit([](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, StreamStarted>)
            return "Started";
        else if constexpr (std::is_same_v<T, StreamTextDelta>)
            return "TextDelta(" + e.text + ")";
        else if constexpr (std::is_same_v<T, StreamTextBlockClosed>)
            return "TextBlockClosed";
        else if constexpr (std::is_same_v<T, StreamToolUseStart>)
            return "ToolUseStart(" + e.id.value + "," + e.name.value + ")";
        else if constexpr (std::is_same_v<T, StreamToolUseDelta>)
            return "ToolUseDelta(" + e.id.value + "," + e.partial_json + ")";
        else if constexpr (std::is_same_v<T, StreamToolUseEnd>)
            return "ToolUseEnd(" + e.id.value + ")";
        else if constexpr (std::is_same_v<T, StreamThinkingDelta>)
            return "ThinkingDelta(" + e.text + "|" + e.signature + ")";
        else if constexpr (std::is_same_v<T, StreamUsage>)
            return "Usage(in=" + std::to_string(e.input_tokens)
                 + ",out=" + std::to_string(e.output_tokens)
                 + ",cc=" + std::to_string(e.cache_creation_input_tokens)
                 + ",cr=" + std::to_string(e.cache_read_input_tokens) + ")";
        else if constexpr (std::is_same_v<T, StreamFinished>)
            return "Finished(" + std::to_string(static_cast<int>(e.stop_reason)) + ")";
        else if constexpr (std::is_same_v<T, StreamError>)
            return "Error(" + e.message + ")";
        else if constexpr (std::is_same_v<T, StreamHeartbeat>)
            return "Heartbeat";
        else if constexpr (std::is_same_v<T, StreamBufferedWait>)
            return "BufferedWait";
        else
            return "OTHER";
    }, sm);
}

std::string render(const Msg& m) {
    if (const auto* sm = std::get_if<StreamMsg>(&m)) return render_stream(*sm);
    return "NON_STREAM";
}

std::string render_all(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs) { s += render(m); s += "\n"; }
    return s;
}

std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

} // namespace

TEST_CASE("anthropic sse golden") {
    namespace ap = agentty::provider::anthropic;

    // A realistic stream: usage-bearing start, a text block, a tool_use block,
    // a ping heartbeat, then delta(stop_reason)+usage and stop.
    const std::vector<std::pair<std::string, std::string>> events = {
        {"message_start",
         R"({"type":"message_start","message":{"usage":{"input_tokens":100,"output_tokens":1,"cache_creation_input_tokens":40,"cache_read_input_tokens":60}}})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":0,"content_block":{"type":"text"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello "}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"world"}})"},
        {"content_block_stop",
         R"({"type":"content_block_stop","index":0})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_01","name":"read"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"a.cpp\"}"}})"},
        {"content_block_stop",
         R"({"type":"content_block_stop","index":1})"},
        {"ping", R"({"type":"ping"})"},
        {"message_delta",
         R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":12}})"},
        {"message_stop", R"({"type":"message_stop"})"},
    };

    const std::vector<Msg> msgs = ap::parse_sse_for_test(events);
    const std::string rendered = render_all(msgs);

    // Feature-level assertions documenting the expected sequence.
    check(rendered.find("Started\n") != std::string::npos,
          "message_start emits Started");
    check(rendered.find("Usage(in=100,out=1,cc=40,cr=60)") != std::string::npos,
          "message_start usage is surfaced");
    check(rendered.find("TextDelta(Hello )") != std::string::npos
       && rendered.find("TextDelta(world)") != std::string::npos,
          "text deltas surface in order");
    check(rendered.find("TextBlockClosed\n") != std::string::npos,
          "text content_block_stop emits TextBlockClosed");
    check(rendered.find("ToolUseStart(toolu_01,read)") != std::string::npos,
          "tool_use content_block_start emits ToolUseStart");
    check(rendered.find("ToolUseDelta(toolu_01,{\"path\":)") != std::string::npos,
          "tool_use input_json deltas surface with the block's id");
    check(rendered.find("ToolUseEnd(toolu_01)") != std::string::npos,
          "tool_use content_block_stop emits ToolUseEnd");
    check(rendered.find("Heartbeat\n") != std::string::npos,
          "ping emits a (non-transport) heartbeat");
    check(rendered.find("Finished(") != std::string::npos,
          "message_stop emits Finished with the captured stop_reason");

    // Byte-identity of the whole rendered sequence.
    const std::uint64_t kGoldenHash = 0x2cc3af5474193951ull;
    const std::uint64_t got = fnv1a(rendered);
    if (kGoldenHash == 0) {
        std::fprintf(stderr, "SSE GOLDEN hash = 0x%016llxull\n---\n%s---\n",
                     static_cast<unsigned long long>(got), rendered.c_str());
    } else {
        check(got == kGoldenHash, "SSE Msg sequence is byte-identical to golden");
        if (got != kGoldenHash)
            std::fprintf(stderr, "  expected 0x%016llx got 0x%016llx\n---\n%s---\n",
                         static_cast<unsigned long long>(kGoldenHash),
                         static_cast<unsigned long long>(got), rendered.c_str());
    }
}

TEST_CASE("anthropic parallel tool blocks stay separate") {
    namespace ap = agentty::provider::anthropic;

    // Two tool_use blocks open at once, their input_json deltas
    // INTERLEAVED. Anthropic puts an `index` on every content_block frame,
    // and that index is the block's identity for its whole life: start,
    // every delta, stop.
    //
    // The decoder used to keep ONE `current_tool_id` and route every delta
    // to whichever block opened last — the `latest_tool_item` shape from
    // docs/TOOL_CALL_ATTRIBUTION.md, which has shipped as a bug in
    // openai-python, opik, strands, zed, and twice here. On this stream it
    // appends block 0's arguments to block 1's call, producing JSON that
    // still parses and a tool that runs with another call's path.
    //
    // Claude emits blocks one at a time today, so the old code was safe by
    // luck rather than by construction. This removes the luck.
    const std::vector<std::pair<std::string, std::string>> events = {
        {"message_start",
         R"({"type":"message_start","message":{"usage":{"input_tokens":1}}})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"toolu_A","name":"read"}})"},
        {"content_block_start",
         R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_B","name":"grep"}})"},
        // Interleaved, and deliberately not in open order.
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"pattern\":"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"path\":"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"b\"}"}})"},
        {"content_block_delta",
         R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"\"a.txt\"}"}})"},
        {"content_block_stop", R"({"type":"content_block_stop","index":0})"},
        {"content_block_stop", R"({"type":"content_block_stop","index":1})"},
        {"message_stop", R"({"type":"message_stop"})"},
    };

    const std::string rendered = render_all(ap::parse_sse_for_test(events));

    // Each call receives its OWN bytes. Before the fix both accumulated
    // onto toolu_B — the block that opened last — and toolu_A got none.
    check(rendered.find(R"(ToolUseDelta(toolu_A,{"path":))") != std::string::npos,
          "block 0's first delta lands on block 0's call");
    check(rendered.find(R"(ToolUseDelta(toolu_A,"a.txt"}))") != std::string::npos,
          "block 0's second delta lands on block 0's call");
    check(rendered.find(R"(ToolUseDelta(toolu_B,{"pattern":))") != std::string::npos,
          "block 1's first delta lands on block 1's call");
    check(rendered.find(R"(ToolUseDelta(toolu_B,"b"}))") != std::string::npos,
          "block 1's second delta lands on block 1's call");

    // Crucially: NO crossover. block 1's payload must never appear under
    // block 0's id — that is the corruption this whole seam exists to
    // prevent, and it is invisible downstream because the result parses.
    check(rendered.find(R"(ToolUseDelta(toolu_A,{"pattern":))") == std::string::npos,
          "block 1's bytes never land on block 0's call");
    check(rendered.find(R"(ToolUseDelta(toolu_B,{"path":))") == std::string::npos,
          "block 0's bytes never land on block 1's call");

    // And each closes exactly once. The stop names its own index, so it
    // cannot end the wrong call or leave the right one open forever.
    check(rendered.find("ToolUseEnd(toolu_A)") != std::string::npos,
          "block 0 ends");
    check(rendered.find("ToolUseEnd(toolu_B)") != std::string::npos,
          "block 1 ends");
}
