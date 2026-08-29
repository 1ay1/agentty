// openai_transport_test — verifies the OpenAI-compatible transport's pure
// translation layer without any network:
//
//   1. build_tools     — provider::ToolSpec → OpenAI function schema.
//   2. build_messages  — Thread → OpenAI messages array (assistant tool_calls
//                        + separate role:"tool" results + multimodal images).
//   3. parse_sse_for_test — scripted OpenAI SSE frames → the agentty Msg
//                        sequence the reducer consumes (text deltas, streamed
//                        tool-call assembly, finish_reason→StopReason, usage,
//                        [DONE] terminal).
//
// Run: build the `openai_transport_test` target, execute. Exit 0 = pass.

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/openai/transport.hpp"
#include "agentty/io/http.hpp"
#include "agentty/provider/msg_shared.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include <cstdlib>

using namespace agentty;
namespace oai = agentty::provider::openai;

// ── Msg inspection helpers ──────────────────────────────────────────────────
// Msg is a variant of domain sub-variants; the leaf we want lives one level
// deeper. get_leaf<T> digs it out, returns nullptr if the Msg isn't that leaf.
template <class Leaf>
const Leaf* get_leaf(const Msg& m) {
    const Leaf* found = nullptr;
    std::visit([&](const auto& domain) {
        std::visit([&](const auto& leaf) {
            if constexpr (std::is_same_v<std::decay_t<decltype(leaf)>, Leaf>)
                found = &leaf;
        }, domain);
    }, m);
    return found;
}

template <class Leaf>
int count_leaf(const std::vector<Msg>& msgs) {
    int n = 0;
    for (const auto& m : msgs) if (get_leaf<Leaf>(m)) ++n;
    return n;
}

// Concatenate all StreamTextDelta payloads in order.
static std::string joined_text(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamTextDelta>(m)) s += d->text;
    return s;
}

// Concatenate all StreamToolUseDelta payloads in order.
static std::string joined_tool_args(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m)) s += d->partial_json;
    return s;
}

// ── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("sse anti-buffering headers are the shared SSOT trio") {
    // The three directives that keep gateways from buffering/compressing an
    // event stream. Pinned so a future edit can't silently drop or change one
    // for a single wire — every streaming transport routes through this helper.
    auto trio = agentty::http::sse_no_buffer_headers();
    CHECK(trio.size() == 3);
    auto has = [&](const char* n, const char* v) {
        for (const auto& h : trio) if (h.name == n) return h.value == v;
        return false;
    };
    CHECK(has("cache-control", "no-cache, no-transform"));
    CHECK(has("pragma", "no-cache"));
    CHECK(has("accept-encoding", "identity"));

    // append_sse_no_buffer splices the same trio onto an existing list.
    agentty::http::Headers h;
    h.push_back({"content-type", "application/json"});
    agentty::http::append_sse_no_buffer(h);
    CHECK(h.size() == 4);
}

TEST_CASE("test_build_tools") {
    std::vector<provider::ToolSpec> tools;
    tools.push_back({"read", "Read a file",
                     nlohmann::json{{"type", "object"},
                                    {"properties", {{"path", {{"type", "string"}}}}}},
                     false});
    auto j = agentty::provider::wire::openai_chat_tools(tools);
    CHECK(j.is_array());
    CHECK(j.size() == 1);
    CHECK(j[0]["type"] == "function");
    CHECK(j[0]["function"]["name"] == "read");
    CHECK(j[0]["function"]["description"] == "Read a file");
    CHECK(j[0]["function"]["parameters"]["type"] == "object");

    // Null schema is guarded to a valid empty-object schema (SSOT guard shared
    // with the Ollama + Responses encoders) — strict backends 400 on a bare null.
    std::vector<provider::ToolSpec> noschema;
    noschema.push_back({"ping", "no args", nlohmann::json(nullptr), false});
    auto n = agentty::provider::wire::openai_chat_tools(noschema);
    CHECK(n[0]["function"]["parameters"]["type"] == "object");
    CHECK(n[0]["function"]["parameters"]["properties"].is_object());
}

TEST_CASE("test_build_messages_basic") {
    Thread t;
    Message u;
    u.role = Role::User;
    u.text = "hello";
    t.messages.push_back(u);

    auto arr = oai::build_messages(t);
    CHECK(arr.is_array());
    CHECK(arr.size() == 1);
    CHECK(arr[0]["role"] == "user");
    CHECK(arr[0]["content"] == "hello");
}

TEST_CASE("test_build_messages_tool_roundtrip") {
    // Assistant message with a completed tool call → one assistant message
    // carrying tool_calls + one separate role:"tool" result message.
    Thread t;

    Message a;
    a.role = Role::Assistant;
    a.text = "let me check";
    ToolUse tc;
    tc.id   = ToolCallId{"call_abc"};
    tc.name = ToolName{"read"};
    tc.args = nlohmann::json{{"path", "foo.txt"}};
    tc.status = ToolUse::Done{{}, {}, "file contents here"};
    a.tool_calls.push_back(tc);
    t.messages.push_back(a);

    auto arr = oai::build_messages(t);
    CHECK(arr.size() == 2);

    // [0] assistant with tool_calls.
    CHECK(arr[0]["role"] == "assistant");
    CHECK(arr[0]["content"] == "let me check");
    CHECK(arr[0]["tool_calls"].is_array());
    CHECK(arr[0]["tool_calls"][0]["id"] == "call_abc");
    CHECK(arr[0]["tool_calls"][0]["type"] == "function");
    CHECK(arr[0]["tool_calls"][0]["function"]["name"] == "read");
    // arguments must be a STRING (serialized JSON).
    CHECK(arr[0]["tool_calls"][0]["function"]["arguments"].is_string());
    CHECK(arr[0]["tool_calls"][0]["function"]["arguments"]
          == std::string{"{\"path\":\"foo.txt\"}"});

    // [1] tool result.
    CHECK(arr[1]["role"] == "tool");
    CHECK(arr[1]["tool_call_id"] == "call_abc");
    CHECK(arr[1]["content"] == "file contents here");
}

// Age-tiered tool-result clearing (shared wire::cap_tool_result_aged): on a
// long tool-burst thread the newest results keep the full budget and stale
// ones fade to a tight head+tail so a big dump stops replaying every turn.
static std::string oai_tool_content(const nlohmann::json& arr, const char* id) {
    for (const auto& msg : arr)
        if (msg.value("role", "") == "tool"
            && msg.value("tool_call_id", "") == id)
            return msg.value("content", "");
    return {};
}

TEST_CASE("test_build_messages_age_tiering") {
    std::string big = "HEAD_SENTINEL_AAAA\n";
    big.append(60 * 1024, 'x');
    big += "\nTAIL_SENTINEL_ZZZZ";

    Thread t;
    Message u; u.role = Role::User; u.text = "start"; t.messages.push_back(u);
    const int n = 20;
    for (int i = 0; i < n; ++i) {
        Message a; a.role = Role::Assistant; a.text = "c";
        ToolUse tc;
        tc.id   = ToolCallId{"call_" + std::to_string(i)};
        tc.name = ToolName{"grep"};
        tc.status = ToolUse::Done{{}, {}, big};
        a.tool_calls.push_back(std::move(tc));
        t.messages.push_back(std::move(a));
    }
    auto arr = oai::build_messages(t);

    // Newest (call_19, rank 0) stays full; oldest (call_0, rank 19) is faded.
    std::string newest = oai_tool_content(arr, "call_19");
    std::string oldest = oai_tool_content(arr, "call_0");
    CHECK(newest.size() > 40 * 1024);
    CHECK(oldest.size() < 6 * 1024);
    CHECK(oldest.find("bytes elided") != std::string::npos);
    // Faded result still keeps head+tail (recall, not replay).
    CHECK(oldest.find("HEAD_SENTINEL_AAAA") != std::string::npos);
    CHECK(oldest.find("TAIL_SENTINEL_ZZZZ") != std::string::npos);

    // A short old result ships verbatim (nothing to fade).
    Thread t2;
    Message u2; u2.role = Role::User; u2.text = "start"; t2.messages.push_back(u2);
    for (int i = 0; i < n; ++i) {
        Message a; a.role = Role::Assistant; a.text = "c";
        ToolUse tc;
        tc.id   = ToolCallId{"call_" + std::to_string(i)};
        tc.name = ToolName{"grep"};
        tc.status = ToolUse::Done{{}, {}, "3 matches\n"};
        a.tool_calls.push_back(std::move(tc));
        t2.messages.push_back(std::move(a));
    }
    auto arr2 = oai::build_messages(t2);
    CHECK(oai_tool_content(arr2, "call_0") == "3 matches\n");
}

TEST_CASE("test_sse_text_stream") {
    // A plain text completion: two content deltas, finish_reason stop, [DONE].
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"lo!\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3}}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    CHECK(joined_text(msgs) == "Hello!");
    CHECK(count_leaf<StreamFinished>(msgs) == 1);

    // Usage surfaced.
    bool saw_usage = false;
    for (const auto& m : msgs)
        if (const auto* u = get_leaf<StreamUsage>(m)) {
            CHECK(u->input_tokens == 12);
            CHECK(u->output_tokens == 3);
            saw_usage = true;
        }
    CHECK(saw_usage);

    // finish_reason "stop" → EndTurn.
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::EndTurn);
}

TEST_CASE("test_sse_reasoning_stream") {
    // Reasoning-capable models on the Chat wire stream chain-of-thought in a
    // field PARALLEL to content: DeepSeek uses reasoning_content; some proxies
    // use reasoning. Both must surface as StreamThinkingDelta (the SAME event
    // the Anthropic wire emits) and NEVER leak into the visible text body.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"Let me \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think.\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer: 42\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    // Thinking text captured, in order, via StreamThinkingDelta.
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "Let me think.");

    // Visible text is ONLY the content, not the reasoning.
    CHECK(joined_text(msgs) == "Answer: 42");
    CHECK(count_leaf<StreamFinished>(msgs) == 1);
}

TEST_CASE("test_sse_reasoning_alt_field") {
    // The alternate `reasoning` field name (some OpenRouter passthroughs) is
    // handled identically.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"hmm\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "hmm");
    CHECK(joined_text(msgs) == "ok");
}

// Mistral / Magistral inline reasoning as [THINK]…[/THINK] INSIDE content
// (no reasoning_content field). It must surface as StreamThinkingDelta, the
// tags must be stripped, and the visible answer must NOT lose its first char
// (the leading `[` used to trip the tool-call salvage).
TEST_CASE("test_sse_think_tags_in_content") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"[THINK]let me think[/THINK]\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer: 42\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "let me think");          // reasoning captured
    CHECK(joined_text(msgs) == "Answer: 42"); // tags stripped, first char kept
}

// A THINK tag SPLIT across two SSE deltas ("[TH" | "INK]let me…") must still be
// recognised — the partial tag is carried, not emitted as prose.
TEST_CASE("test_sse_think_tags_split_across_deltas") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"[TH\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"INK]deep[/TH\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"INK]hi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "deep");
    CHECK(joined_text(msgs) == "hi");   // no stray "[TH" / "[/TH" leaked
}

// REASON-BY-DEFAULT (Magistral): the stream begins in reasoning with NO open
// tag — everything up to the first [/THINK] is reasoning, the rest is content.
TEST_CASE("test_sse_reason_by_default_no_open_tag") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"working it out\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" carefully[/THINK]Done.\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    // reason_by_default=true (as for magistral/deepseek-r1).
    auto msgs = oai::parse_sse_for_test(sse, {}, false, /*reason_by_default=*/true);
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "working it out carefully");   // leading text = reasoning
    CHECK(joined_text(msgs) == "Done.");          // after close = content
}

// DeepSeek-R1 / Qwen / QwQ / most local models use <think>…</think> instead of
// Mistral's [THINK]. Same extraction, same guarantees.
TEST_CASE("test_sse_angle_think_tags") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<think>hmm</think>Yes\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "hmm");
    CHECK(joined_text(msgs) == "Yes");
}

// A literal "</think>" appearing in genuine answer prose (no reasoning at the
// start) must NOT retro-hide the answer — the reason-by-default probe only
// fires at the very stream start.
TEST_CASE("test_sse_stray_close_tag_in_prose_is_kept") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"The tag \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"</think> ends reasoning.\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    // First delta commits "The tag " as content → probe disarmed → the later
    // </think> is kept verbatim in the answer, not treated as a reasoning end.
    CHECK(joined_text(msgs).find("The tag") != std::string::npos);
    CHECK(joined_text(msgs).find("ends reasoning") != std::string::npos);
}

TEST_CASE("test_sse_tool_call_stream") {
    // OpenAI tool-call streaming: opening frame carries id+name, subsequent
    // frames carry arguments fragments; finish_reason "tool_calls".
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"id\":\"call_42\",\"type\":\"function\","
            "\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"{\\\"path\\\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"\\\"a.txt\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    // Exactly one tool-use opened + closed.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);

    // Start carries id + name.
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) {
            CHECK(s->id.value == "call_42");
            CHECK(s->name.value == "read");
        }

    // Assembled argument fragments form the full JSON.
    CHECK(joined_tool_args(msgs) == std::string{"{\"path\":\"a.txt\"}"});

    // finish_reason "tool_calls" → ToolUse.
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);

    // Ordering: Start must precede every Delta, and End must follow them.
    int start_idx = -1, end_idx = -1, last_delta_idx = -1;
    for (int i = 0; i < (int)msgs.size(); ++i) {
        if (get_leaf<StreamToolUseStart>(msgs[i])) start_idx = i;
        if (get_leaf<StreamToolUseDelta>(msgs[i])) last_delta_idx = i;
        if (get_leaf<StreamToolUseEnd>(msgs[i]))   end_idx = i;
    }
    CHECK(start_idx >= 0 && last_delta_idx > start_idx && end_idx > last_delta_idx);
}

TEST_CASE("test_sse_two_tool_calls") {
    // Two parallel tool calls (index 0 then index 1). The second index
    // appearing must close the first call before opening the second.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"id\":\"c0\",\"function\":{\"name\":\"glob\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":1,"
            "\"id\":\"c1\",\"function\":{\"name\":\"grep\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    CHECK(count_leaf<StreamToolUseStart>(msgs) == 2);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 2);

    std::vector<std::string> ids;
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) ids.push_back(s->id.value);
    CHECK(ids.size() == 2 && ids[0] == "c0" && ids[1] == "c1");
}

TEST_CASE("test_sse_error_frame") {
    std::string sse =
        "data: {\"error\":{\"message\":\"rate limit exceeded\",\"type\":\"rate_limit\"}}\n\n";
    auto msgs = oai::parse_sse_for_test(sse);
    bool saw_err = false;
    for (const auto& m : msgs)
        if (const auto* e = get_leaf<StreamError>(m)) {
            CHECK(e->message == "rate limit exceeded");
            saw_err = true;
        }
    CHECK(saw_err);
}

// ── Leaked-tool-call salvage (weak local models like qwen2.5-coder:7b) ──
// These models emit the call as a bare JSON in `content` with
// finish_reason "stop" instead of the structured tool_calls[] channel.
TEST_CASE("test_sse_salvage_leaked_tool_call") {
    // The exact shape Ollama returns for qwen2.5-coder:7b: one content
    // delta carrying {"name":..,"arguments":{..}} as a string.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"echo\\\", \\\"arguments\\\": "
            "{\\\"text\\\": \\\"hi\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo", "read"});

    // The leaked JSON must become a REAL tool call, not surface as text.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(joined_text(msgs).empty());     // nothing leaked into the text body
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "echo");
    CHECK(joined_tool_args(msgs) == std::string{"{\"text\":\"hi\"}"});
    // Salvaged calls report ToolUse so the reducer kicks the tool loop.
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);
}

// ── <tool_call>-tag-wrapped leak (qwen/Hermes chat-template form) ──────
// The qwen2.5-coder template instructs the model to wrap calls in
// <tool_call>…</tool_call>. When Ollama fails to strip those tags they
// arrive in `content`; salvage must peel the tags and recover the call.
TEST_CASE("test_sse_salvage_tool_call_tags") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"<tool_call>\\n{\\\"name\\\": \\\"echo\\\", "
            "\\\"arguments\\\": {\\\"text\\\": \\\"hi\\\"}}\\n"
            "</tool_call>\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo", "read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(joined_text(msgs).empty());   // tags + JSON consumed, not leaked
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "echo");
    CHECK(joined_tool_args(msgs) == std::string{"{\"text\":\"hi\"}"});
}

// A ```json-fenced leak inside <tool_call> tags (belt-and-suspenders form
// some templates produce) must also salvage cleanly.
TEST_CASE("test_sse_salvage_fenced_tags") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"<tool_call>```json\\n{\\\"name\\\": \\\"echo\\\", "
            "\\\"arguments\\\": {}}\\n```</tool_call>\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(joined_text(msgs).empty());
}

TEST_CASE("test_sse_salvage_unknown_tool_stays_text") {
    // A complete JSON object SHAPED like a tool call ({"name","arguments"})
    // but naming a tool we did NOT advertise is a weak-model mistype (e.g.
    // "read_file" for "read"). It is never salvaged (we never invent a call)
    // AND never surfaced as raw JSON — it's dropped. The empty-turn fallback
    // then fills the turn so the user never sees a blank bubble.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"nonexistent\\\", \\\"arguments\\\": {}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find('{') == std::string::npos);  // raw JSON dropped
    CHECK(joined_text(msgs).find("nonexistent") == std::string::npos);
}

// A non-tool-shaped JSON object (no name+arguments keys) naming nothing in
// particular is genuine prose/data — it must still surface as text.
TEST_CASE("test_sse_plain_object_stays_text") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"answer\\\": 42, \\\"unit\\\": \\\"none\\\"}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(!joined_text(msgs).empty());          // not tool-shaped — kept
    CHECK(joined_text(msgs).find("answer") != std::string::npos);
}

TEST_CASE("test_sse_plain_json_prose_not_salvaged") {
    // Ordinary prose that merely STARTS with text isn't held/mangled.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Sure, here you go.\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(joined_text(msgs) == "Sure, here you go.");
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

TEST_CASE("test_sse_structured_tool_still_works_with_salvage_on") {
    // A REAL structured tool call must be unaffected by the salvage path.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"id\":\"call_1\",\"function\":{\"name\":\"read\","
            "\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "read");
}

TEST_CASE("test_sse_truncated_leaked_tool_call_dropped") {
    // qwen leaks the call into `content` but the wire cuts off mid-body
    // (no closing braces, no [DONE]). The half-written JSON must NOT surface
    // as visible prose — dumping it pollutes the assistant turn and the weak
    // model re-leaks the same call next turn (the stuck "upstream cut off"
    // re-invocation). The raw JSON is dropped; because the turn would
    // otherwise be completely empty (no text, no tool call), ensure_nonempty_turn
    // substitutes a fixed sentinel so the user never sees a blank bubble.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"remember\\\", \\\"argum\"}}]}\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"remember"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);  // not salvageable
    // The truncated JSON itself must not leak as prose.
    CHECK(joined_text(msgs).find("remember") == std::string::npos);
    CHECK(joined_text(msgs).find('{') == std::string::npos);
    // But the turn is non-empty (the empty-turn fallback fired).
    CHECK(!joined_text(msgs).empty());
}

TEST_CASE("test_sse_fence_only_leak_dropped") {
    // qwen answers a greeting with just a ```json fence (the leaked tool-call
    // wrapper) and no JSON body — the bug where "hi" was answered with the
    // literal text "json". The bare wrapper must NOT surface as prose; the
    // empty-turn fallback fills the turn instead.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"```json\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("json") == std::string::npos);
    CHECK(joined_text(msgs).find('`') == std::string::npos);
}

TEST_CASE("test_sse_two_leaked_calls_unique_ids") {
    // Two complete leaked calls in one stream must get DISTINCT synthesised
    // ids, or the reducer keys both onto the same card (duplicate stuck card).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"echo\\\", \\\"arguments\\\": "
            "{\\\"text\\\": \\\"a\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    // One stream only carries one hold, so verify ids are unique across two
    // separate parses sharing no ctx is trivially true; instead assert the id
    // is the seq-0 form so a future second salvage in the same ctx differs.
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->id.value == "call_salvaged_0");
}

TEST_CASE("test_endpoint_presets") {
    auto groq = oai::Endpoint::from_spec("groq");
    CHECK(groq.host == "api.groq.com");
    CHECK(groq.use_tls);
    CHECK(groq.path == "/openai/v1/chat/completions");

    auto openrouter = oai::Endpoint::from_spec("openrouter");
    CHECK(openrouter.host == "openrouter.ai");
    CHECK(openrouter.use_tls);
    CHECK(openrouter.path == "/api/v1/chat/completions");
    CHECK(openrouter.models_path == "/api/v1/models");
    CHECK(!openrouter.native_api);

    auto together = oai::Endpoint::from_spec("together");
    CHECK(together.host == "api.together.xyz");
    CHECK(together.use_tls);
    CHECK(together.path == "/v1/chat/completions");

    auto cerebras = oai::Endpoint::from_spec("cerebras");
    CHECK(cerebras.host == "api.cerebras.ai");
    CHECK(cerebras.use_tls);
    CHECK(cerebras.path == "/v1/chat/completions");

    auto deepseek = oai::Endpoint::from_spec("deepseek");
    CHECK(deepseek.host == "api.deepseek.com");
    CHECK(deepseek.use_tls);
    // DeepSeek's OpenAI-compatible endpoints live at the ROOT (no /v1 prefix).
    CHECK(deepseek.path == "/chat/completions");
    CHECK(deepseek.models_path == "/models");

    auto xai = oai::Endpoint::from_spec("xai");
    CHECK(xai.host == "api.x.ai");
    CHECK(xai.use_tls);
    CHECK(xai.path == "/v1/chat/completions");
    CHECK(xai.models_path == "/v1/models");

    auto mistral = oai::Endpoint::from_spec("mistral");
    CHECK(mistral.host == "api.mistral.ai");
    CHECK(mistral.use_tls);
    CHECK(mistral.path == "/v1/chat/completions");

    auto gemini = oai::Endpoint::from_spec("gemini");
    CHECK(gemini.host == "generativelanguage.googleapis.com");
    CHECK(gemini.use_tls);
    // Google's OpenAI-compat shim is nested under /v1beta/openai.
    CHECK(gemini.path == "/v1beta/openai/chat/completions");
    CHECK(gemini.models_path == "/v1beta/openai/models");

    auto fireworks = oai::Endpoint::from_spec("fireworks");
    CHECK(fireworks.host == "api.fireworks.ai");
    CHECK(fireworks.use_tls);
    CHECK(fireworks.path == "/inference/v1/chat/completions");
    CHECK(fireworks.models_path == "/inference/v1/models");

    auto ollama = oai::Endpoint::from_spec("ollama");
    CHECK(ollama.host == "localhost");
    CHECK(ollama.port == 11434);
    CHECK(!ollama.use_tls);
    CHECK(ollama.native_api);
    CHECK(ollama.path == "/api/chat");

    auto llama = oai::Endpoint::from_spec("llama.cpp");
    CHECK(llama.host == "localhost");
    CHECK(llama.port == 8080);
    CHECK(!llama.use_tls);
    CHECK(llama.path == "/v1/chat/completions");
    CHECK(!llama.native_api);   // OpenAI dialect, not Ollama native

    auto custom = oai::Endpoint::from_spec("my.host:8080");
    CHECK(custom.host == "my.host");
    CHECK(custom.port == 8080);
    CHECK(!custom.use_tls);
    CHECK(custom.label == "my.host:8080");   // label carries the raw spec

    auto def = oai::Endpoint::from_spec("");
    CHECK(def.host == "api.openai.com");
    CHECK(def.use_tls);

    // ── Full URL specs (http:// or https:// prefix) ──────────────
    // A URL spec lets the user pick a custom path prefix for servers
    // that don't serve on /v1 (e.g. chat.example.org serves on
    // /api/chat/completions). The path in the URL is a BASE PREFIX;
    // agentty appends /chat/completions and /models to it.
    {
        auto url = oai::Endpoint::from_spec("https://chat.example.org/api");
        CHECK(url.host == "chat.example.org");
        CHECK(url.port == 443);
        CHECK(url.use_tls);
        CHECK(url.path == "/api/chat/completions");
        CHECK(url.models_path == "/api/models");
        CHECK(!url.native_api);
        CHECK(url.label == "https://chat.example.org/api");
    }
    {
        auto url = oai::Endpoint::from_spec("http://localhost:8080/custom");
        CHECK(url.host == "localhost");
        CHECK(url.port == 8080);
        CHECK(!url.use_tls);
        CHECK(url.path == "/custom/chat/completions");
        CHECK(url.models_path == "/custom/models");
    }
    {
        // No path after scheme://authority → /v1 DEFAULT (the OpenAI dialect
        // lives under /v1 on every real server — api.openai.com, llama.cpp,
        // vLLM, LM Studio; a bare "/chat/completions" 404s on all of them).
        auto url = oai::Endpoint::from_spec("https://my-gateway.com");
        CHECK(url.host == "my-gateway.com");
        CHECK(url.port == 443);
        CHECK(url.use_tls);
        CHECK(url.path == "/v1/chat/completions");
        CHECK(url.models_path == "/v1/models");
    }
    {
        // Trailing slash on the prefix is stripped before appending.
        auto url = oai::Endpoint::from_spec("https://host:9000/prefix/");
        CHECK(url.host == "host");
        CHECK(url.port == 9000);
        CHECK(url.use_tls);
        CHECK(url.path == "/prefix/chat/completions");
        CHECK(url.models_path == "/prefix/models");
    }
    {
        // http:// with explicit port and no path → /v1 default (see above).
        auto url = oai::Endpoint::from_spec("http://10.0.0.5:5000");
        CHECK(url.host == "10.0.0.5");
        CHECK(url.port == 5000);
        CHECK(!url.use_tls);
        CHECK(url.path == "/v1/chat/completions");
        CHECK(url.models_path == "/v1/models");
    }
    {
        // "#name" fragment: a LOCAL multi-account tag. Two specs that differ
        // only in fragment dial the IDENTICAL endpoint (host/port/path) but
        // keep distinct labels — distinct settings keys → separate API keys
        // and saved models per account on the same server.
        auto a = oai::Endpoint::from_spec("https://ollama.com/v1#work");
        auto b = oai::Endpoint::from_spec("https://ollama.com/v1#personal");
        CHECK(a.host == "ollama.com");
        CHECK(a.port == 443);
        CHECK(a.use_tls);
        CHECK(a.path == "/v1/chat/completions");
        CHECK(a.models_path == "/v1/models");
        CHECK(b.host == a.host);
        CHECK(b.path == a.path);
        CHECK(a.label == "https://ollama.com/v1#work");
        CHECK(b.label == "https://ollama.com/v1#personal");
    }
    {
        // Fragment on a raw host:port spec — stripped BEFORE the port split.
        auto url = oai::Endpoint::from_spec("my-box.lan:8080#lab");
        CHECK(url.host == "my-box.lan");
        CHECK(url.port == 8080);
        CHECK(!url.use_tls);
        CHECK(url.label == "my-box.lan:8080#lab");
    }
    {
        // Invalid port (non-numeric) → falls back to scheme default.
        auto url = oai::Endpoint::from_spec("https://host:abc/path");
        CHECK(url.host == "host");
        CHECK(url.port == 443);
        CHECK(url.use_tls);
        CHECK(url.path == "/path/chat/completions");
    }
    {
        // Out-of-range port → falls back to scheme default.
        auto url = oai::Endpoint::from_spec("http://host:99999/path");
        CHECK(url.host == "host");
        CHECK(url.port == 80);
        CHECK(!url.use_tls);
        CHECK(url.path == "/path/chat/completions");
    }
    {
        // Empty host (e.g. "https:///path") → falls back to default endpoint.
        auto url = oai::Endpoint::from_spec("https:///path");
        CHECK(url.host == "api.openai.com");
        CHECK(url.port == 443);
        CHECK(url.use_tls);
    }
    {
        // Backward compat: bare host:port (no scheme) still uses /v1 default.
        auto noscheme = oai::Endpoint::from_spec("my.host:8080");
        CHECK(noscheme.host == "my.host");
        CHECK(noscheme.port == 8080);
        CHECK(!noscheme.use_tls);
        CHECK(noscheme.path == "/v1/chat/completions");
        CHECK(noscheme.models_path == "/v1/models");
        CHECK(noscheme.label == "my.host:8080");
    }
    {
        // IPv6 literal with a port: the port colon is the one AFTER ']', and
        // the dialed host has the brackets stripped.
        auto url = oai::Endpoint::from_spec("http://[::1]:8080/api");
        CHECK(url.host == "::1");
        CHECK(url.port == 8080);
        CHECK(!url.use_tls);
        CHECK(url.path == "/api/chat/completions");
    }
    {
        // IPv6 literal with NO port: rfind(':') would have split inside the
        // address — the bracket parse keeps the whole address as the host.
        auto url = oai::Endpoint::from_spec("https://[2001:db8::1]/v1");
        CHECK(url.host == "2001:db8::1");
        CHECK(url.port == 443);
        CHECK(url.use_tls);
        CHECK(url.path == "/v1/chat/completions");
    }

    // Registry ↔ from_spec consistency: EVERY OpenAI-family preset id must
    // resolve to a usable endpoint (non-empty host + chat/models paths).
    // Anthropic is skipped — it doesn't go through the OpenAI from_spec.
    // This catches the "added a registry row, forgot the endpoint arm"
    // class of bug: the fallback would treat the id as a raw hostname and
    // silently dial the wrong place.
    for (const auto& p : agentty::provider::providers()) {
        if (p.kind() != agentty::provider::Kind::OpenAI) continue;
        auto ep = oai::Endpoint::from_spec(p.id);
        CHECK(!ep.host.empty());
        CHECK(!ep.path.empty());
        CHECK(!ep.models_path.empty());
        // Hosted (non-local) presets must use TLS on 443; locals must not.
        if (p.is_local) {
            CHECK(!ep.use_tls);
        } else {
            CHECK(ep.use_tls);
            CHECK(ep.port == 443);
        }
        // The label must round-trip to the SAME preset so the model badge
        // and provider readout name it correctly (not fall through to the
        // raw-host label branch).
        CHECK(ep.label == p.id);
    }
}

// ── Bundled model seed: hosted API-key providers show models before a key ──
// With an EMPTY auth header, openai::list_models short-circuits to empty for a
// TLS endpoint (no network). list_models_for must then fall back to the
// per-provider bundled seed so a freshly selected hosted provider shows models
// in the picker before any key is set. The live fetch supersedes it later.
TEST_CASE("bundled model seed when no key / fetch empty") {
    namespace P = agentty::provider;
    const auth::AuthHeader none{};

    for (const char* id : {"xai", "mistral", "gemini", "fireworks",
                           "deepseek", "groq", "cerebras", "together"}) {
        auto models = P::list_models_for(P::parse_selection(id), none);
        CHECK(!models.empty());
        // The seed stamps the provider label so the picker groups it right.
        if (!models.empty())
            CHECK(models.front().provider == std::string{id});
    }

    // A couple of concrete slugs land where expected (newest first).
    {
        auto xai = P::list_models_for(P::parse_selection("xai"), none);
        CHECK(!xai.empty());
        if (!xai.empty()) CHECK(xai.front().id.value == "grok-4.6");
    }
    {
        auto gem = P::list_models_for(P::parse_selection("gemini"), none);
        CHECK(!gem.empty());
        if (!gem.empty()) CHECK(gem.front().id.value == "gemini-2.5-pro");
    }

    // Providers WITHOUT a seed (openrouter, custom hosts, locals) legitimately
    // stay empty with no key rather than showing guessed models.
    {
        auto orr = P::list_models_for(P::parse_selection("openrouter"), none);
        CHECK(orr.empty());
    }
}

// ── provider_display_name: URL-form labels collapse to host[:port] ──
// A custom OpenAI-compatible host entered as "https://chat.example.org/api"
// has Endpoint::label == the full URL (see Endpoint::from_spec, transport.cpp).
// The badge/toast should read "chat.example.org", not the long URL. Default
// port for the scheme is omitted; a non-default port is kept ("host:port").
TEST_CASE("test_provider_display_name_url_label") {
    namespace P = agentty::provider;
    using oai::Endpoint;

    {
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("https://chat.example.org/api");
        // Sanity: from_spec built the URL-form endpoint as designed.
        CHECK(s.openai_endpoint.host == "chat.example.org");
        CHECK(s.openai_endpoint.port == 443);
        CHECK(s.openai_endpoint.use_tls);
        CHECK(s.openai_endpoint.path == "/api/chat/completions");
        // label stays the full URL (preset lookup keys on it); display collapses.
        CHECK(s.openai_endpoint.label == "https://chat.example.org/api");
        CHECK(P::provider_display_name(s) == "chat.example.org");
    }
    {
        // Non-default https port: keep host:port.
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("https://host:9000/prefix/");
        CHECK(P::provider_display_name(s) == "host:9000");
    }
    {
        // http:// with non-default port: keep host:port (80 IS default for http,
        // so http://host:80 → "host", not "host:80").
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("http://10.0.0.5:5000");
        CHECK(P::provider_display_name(s) == "10.0.0.5:5000");
    }
    {
        // http:// with default port 80: collapse to bare host.
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("http://10.0.0.5:80");
        CHECK(P::provider_display_name(s) == "10.0.0.5");
    }
    {
        // Bare host[:port] (no scheme) label is unchanged: passes through.
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("my.host:8080");
        CHECK(s.openai_endpoint.label == "my.host:8080");
        CHECK(P::provider_display_name(s) == "my.host:8080");
    }
    {
        // IPv6 literal collapses to the bracketed authority (a valid form),
        // keeping the port after ']'. Without bracket-awareness the label
        // would be split inside the address.
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("http://[::1]:8080/api");
        CHECK(P::provider_display_name(s) == "[::1]:8080");
    }
    {
        // Portless IPv6: default port omitted, brackets kept.
        P::Selection s;
        s.kind = P::Kind::OpenAI;
        s.openai_endpoint = Endpoint::from_spec("https://[2001:db8::1]/v1");
        CHECK(P::provider_display_name(s) == "[2001:db8::1]");
    }
}

// ── TUI custom-host submit: the spec string must round-trip unchanged ──
// login_submit's CustomHostInput arm used to strip the URL scheme and trim
// the path, which broke https://host/api → /v1/chat/completions. After the
// fix the raw spec flows into parse_selection → from_spec. This pins the
// contract for the specs the TUI must accept: every URL form a CLI user
// can pass via --provider, plus bare host[:port].
TEST_CASE("test_tui_custom_host_specs") {
    namespace P = agentty::provider;

    // The exact spec the user reported broken.
    {
        auto sel = P::parse_selection("https://chat.example.org/api");
        CHECK(sel.kind == P::Kind::OpenAI);
        CHECK(sel.openai_endpoint.host == "chat.example.org");
        CHECK(sel.openai_endpoint.port == 443);
        CHECK(sel.openai_endpoint.use_tls);
        CHECK(sel.openai_endpoint.path == "/api/chat/completions");
        CHECK(sel.openai_endpoint.models_path == "/api/models");
    }
    // http:// with explicit port and path prefix.
    {
        auto sel = P::parse_selection("http://localhost:8080/custom");
        CHECK(sel.kind == P::Kind::OpenAI);
        CHECK(sel.openai_endpoint.host == "localhost");
        CHECK(sel.openai_endpoint.port == 8080);
        CHECK(!sel.openai_endpoint.use_tls);
        CHECK(sel.openai_endpoint.path == "/custom/chat/completions");
        CHECK(sel.openai_endpoint.models_path == "/custom/models");
    }
    // https:// with no path → /v1 default (OpenAI dialect lives under /v1
    // on every real server; a bare /chat/completions 404s everywhere).
    {
        auto sel = P::parse_selection("https://my-gateway.com");
        CHECK(sel.kind == P::Kind::OpenAI);
        CHECK(sel.openai_endpoint.host == "my-gateway.com");
        CHECK(sel.openai_endpoint.path == "/v1/chat/completions");
        CHECK(sel.openai_endpoint.models_path == "/v1/models");
    }
    // Bare host[:port] (legacy TUI behaviour) → /v1 default, unchanged.
    {
        auto sel = P::parse_selection("localhost:8080");
        CHECK(sel.kind == P::Kind::OpenAI);
        CHECK(sel.openai_endpoint.host == "localhost");
        CHECK(sel.openai_endpoint.port == 8080);
        CHECK(!sel.openai_endpoint.use_tls);
        CHECK(sel.openai_endpoint.path == "/v1/chat/completions");
        CHECK(sel.openai_endpoint.models_path == "/v1/models");
    }
}

// ── Request headers / --auth-header ─────────────────────────────────────────
// build_request_headers is the single place the OpenAI-family auth header is
// emitted (stream, model listing, Ollama probe). Verify the default Bearer
// arms, the custom-name override, and the empty-key case.
static void find_header(const agentty::http::Headers& hs, std::string_view name,
                        const agentty::http::Header** out) {
    *out = nullptr;
    for (const auto& h : hs)
        if (h.name == name) { *out = &h; return; }
}

TEST_CASE("test_build_request_headers") {
    oai::Endpoint def;   // no auth_header_name → Bearer

    // Default: both auth arms emit `authorization: Bearer <key>`.
    const agentty::http::Header* h = nullptr;
    auto hs = oai::build_request_headers(oai::AuthHeader{oai::ApiKeyHeader{"k1"}}, def);
    find_header(hs, "authorization", &h);
    CHECK(h && h->value == "Bearer k1");

    hs = oai::build_request_headers(oai::AuthHeader{oai::BearerHeader{"k2"}}, def);
    find_header(hs, "authorization", &h);
    CHECK(h && h->value == "Bearer k2");

    // Custom name: key goes out RAW under the (lowercased) name, and no
    // authorization header is sent.
    oai::Endpoint custom;
    custom.auth_header_name = "X-API-Key";
    hs = oai::build_request_headers(oai::AuthHeader{oai::ApiKeyHeader{"k3"}}, custom);
    find_header(hs, "x-api-key", &h);
    CHECK(h && h->value == "k3");
    find_header(hs, "authorization", &h);
    CHECK(h == nullptr);

    // Empty key: no auth header under either scheme (local backends).
    hs = oai::build_request_headers(oai::AuthHeader{oai::ApiKeyHeader{""}}, custom);
    find_header(hs, "x-api-key", &h);
    CHECK(h == nullptr);
    find_header(hs, "authorization", &h);
    CHECK(h == nullptr);

    // parse_selection stamps the session override onto the endpoint (and an
    // empty override leaves it clear).
    agentty::provider::set_custom_auth_header("Api-Key");
    auto sel = agentty::provider::parse_selection("my.host:9000");
    CHECK(sel.openai_endpoint.auth_header_name == "Api-Key");
    agentty::provider::set_custom_auth_header("");
    sel = agentty::provider::parse_selection("my.host:9000");
    CHECK(sel.openai_endpoint.auth_header_name.empty());
}

// ── Per-preset auth resolution ──────────────────────────────────────────────
// resolve_auth_for is the single mapping every provider switch goes through
// (startup AND the picker). Verify each preset kind lands on the right auth:
//   • Anthropic → the login creds, verbatim.
//   • local (ollama/llama.cpp) → an EMPTY key (no auth), never the env chain.
//   • hosted OpenAI-family → a bearer key, with precedence
//     cli_key > saved_key > env chain.
TEST_CASE("test_resolve_auth_per_preset") {
    using namespace agentty::provider;
    // A distinctive Anthropic cred so we can prove it round-trips untouched.
    auth::AuthHeader anthropic{auth::BearerHeader{"anthropic-oauth-token"}};

    // Clear any ambient keys so the env-chain branch is deterministic.
    ::unsetenv("OPENAI_API_KEY");
    ::unsetenv("GROQ_API_KEY");
    ::unsetenv("OPENROUTER_API_KEY");
    ::unsetenv("TOGETHER_API_KEY");
    ::unsetenv("CEREBRAS_API_KEY");

    for (const auto& p : providers()) {
        auto a = resolve_auth_for(p.id, anthropic, /*cli_key=*/{}, /*saved_key=*/{});
        if (p.kind() == Kind::Anthropic) {
            // Echoes the login creds unchanged.
            CHECK(!auth::is_empty(a));
            auto* b = std::get_if<auth::BearerHeader>(&a);
            CHECK(b && b->token == "anthropic-oauth-token");
        } else if (p.auth == AuthStyle::None) {
            // Local backend: empty key, and it must NOT have grabbed the
            // Anthropic token by accident.
            CHECK(auth::is_empty(a));
        } else {
            // Hosted OpenAI-family with no env/saved/cli key → empty bearer.
            CHECK(auth::is_empty(a));
        }
    }

    // Saved key (the in-app paste path) is honoured for a hosted provider.
    {
        auto a = resolve_auth_for("openrouter", anthropic,
                                  /*cli_key=*/{}, /*saved_key=*/"sk-saved");
        CHECK(!auth::is_empty(a));
        auto* k = std::get_if<auth::ApiKeyHeader>(&a);
        CHECK(k && k->value == "sk-saved");
    }
    // cli_key wins over saved_key.
    {
        auto a = resolve_auth_for("groq", anthropic,
                                  /*cli_key=*/"sk-cli", /*saved_key=*/"sk-saved");
        auto* k = std::get_if<auth::ApiKeyHeader>(&a);
        CHECK(k && k->value == "sk-cli");
    }
    // A local backend ignores a saved key (stays keyless).
    {
        auto a = resolve_auth_for("ollama", anthropic,
                                  /*cli_key=*/{}, /*saved_key=*/"sk-ignored");
        CHECK(auth::is_empty(a));
    }
}

// ── Incremental salvage tests ──────────────────────────────────────────────────

// Streamed JSON tokens (like real Ollama does) should salvage correctly.
TEST_CASE("test_sse_salvage_streamed_tokens") {
    // Simulate how Ollama sends JSON one token at a time.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"{\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\\"name\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\\":\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" \\\"read\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\\",\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" \\\"arguments\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\\":\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" {}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(joined_text(msgs).empty());
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "read");
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);
}

// Prose BEFORE a JSON object: once prose is detected, salvage is disabled.
// This prevents false positives on code like "int main() {" or similar.
// If a model emits prose followed by a tool call, the JSON is shown as text.
TEST_CASE("test_sse_prose_then_tool_call") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Let me check.\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"{\\\"name\\\": \\\"read\\\", \\\"arguments\\\": {}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read"});
    // Salvage is disabled after prose, so no tool call is emitted.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    // Both prose and JSON are flushed as text.
    auto text = joined_text(msgs);
    CHECK(text.find("Let me check") != std::string::npos);
    CHECK(text.find("read") != std::string::npos);
}

// Array of tool calls: [{...}, {...}] should emit multiple tools.
TEST_CASE("test_sse_salvage_array_of_calls") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"[{\\\"name\\\": \\\"read\\\", \\\"arguments\\\": {}}, "
            "{\\\"name\\\": \\\"write\\\", \\\"arguments\\\": {}}]\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 2);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 2);
    CHECK(joined_text(msgs).empty());
    // Verify distinct IDs.
    std::vector<std::string> ids;
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            ids.push_back(s->id.value);
    CHECK(ids.size() == 2 && ids[0] != ids[1]);
}

// Multiple separate JSON objects in sequence (two calls, not an array).
TEST_CASE("test_sse_salvage_two_sequential_calls") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"read\\\", \\\"arguments\\\": {}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"write\\\", \\\"arguments\\\": {}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 2);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 2);
}

// Some models use "function" instead of "name" for the tool name key.
TEST_CASE("test_sse_salvage_function_key") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"function\\\": \\\"echo\\\", \\\"arguments\\\": "
            "{\\\"text\\\": \\\"Hi there!\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo", "read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(joined_text(msgs).empty());
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "echo");
    CHECK(joined_tool_args(msgs) == std::string{"{\"text\":\"Hi there!\"}"});
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);
}

// A leaked memory tool (remember/forget/wipe_memory) must be SWALLOWED at the
// transport: no card is ever born (we never auto-run memory tools on the
// model's own initiative, and a flash-then-delete card is bad UX). The JSON is
// consumed (not surfaced as prose) and the turn finishes without a tool call.
TEST_CASE("test_sse_salvage_memory_tool_swallowed") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"remember\\\", \\\"arguments\\\": "
            "{\\\"text\\\": \\\"a fact\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"remember", "read"});
    // No tool card, and the JSON never surfaces as prose.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("remember") == std::string::npos);
    CHECK(joined_text(msgs).find('{') == std::string::npos);
}

// The same leaked-content shape is a legitimate tool call when the latest user
// explicitly asked to remember something. Weak/local models often lack a
// working structured tool channel; swallowing this path made remember fail
// 100% of the time for them.
TEST_CASE("test_sse_salvage_memory_tool_explicit_request") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"remember\\\", \\\"arguments\\\": "
            "{\\\"text\\\": \\\"a fact\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"remember", "read"},
                                        /*allow_memory_salvage=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(joined_tool_args(msgs) == std::string{"{\"text\":\"a fact\"}"});
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m))
            CHECK(s->name.value == "remember");
}

// A leaked `skill` call (the meta-tool weak models hallucinate from the
// catalog block on a greeting — {"name":"skill","arguments":{"name":...}})
// must be SWALLOWED, never executed. Surfacing it spawns a "skill not found"
// card that then loops. The JSON is consumed, not shown as prose.
TEST_CASE("test_sse_salvage_skill_swallowed") {
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"skill\\\", \\\"arguments\\\": "
            "{\\\"name\\\": \\\"greeting\\\"}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"skill", "read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("skill") == std::string::npos);
    CHECK(joined_text(msgs).find('{') == std::string::npos);
}

// ── Native Ollama /api/chat (NDJSON) path ────────────────────────────────────

// A clean greeting: content streams as plain text, no tool calls.
TEST_CASE("test_ndjson_plain_greeting") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello! \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"How can I help?\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
            "\"done\":true,\"done_reason\":\"stop\","
            "\"prompt_eval_count\":10,\"eval_count\":5}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read", "remember"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs) == std::string{"Hello! How can I help?"});
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::EndTurn);
}

// Structured native tool_calls (function.arguments as an object) become a
// real tool call with a call_native_ id.
TEST_CASE("test_ndjson_structured_tool_call") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\","
            "\"tool_calls\":[{\"function\":{\"name\":\"read\","
            "\"arguments\":{\"path\":\"/etc/hostname\"}}}]}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
            "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) {
            CHECK(s->name.value == "read");
            CHECK(std::string_view{s->id.value}.starts_with("call_native_"));
        }
    CHECK(joined_tool_args(msgs) == std::string{"{\"path\":\"/etc/hostname\"}"});
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);
}

// THE regression: qwen2.5-coder leaks a bare {"name":..} tool call into
// native message.content (its output doesn't match Ollama's <tool_call>
// template wrapper, so the server leaves it in content). It must be SALVAGED
// into a real tool call so the tool actually runs — NOT shown as raw JSON.
TEST_CASE("test_ndjson_leaked_content_salvaged") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
            "\"{\\\"name\\\": \\\"read\\\", \\\"arguments\\\": "
            "{\\\"path\\\": \\\"/etc/hostname\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
            "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(joined_text(msgs).empty());          // never shown as raw JSON
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) {
            CHECK(s->name.value == "read");
            CHECK(std::string_view{s->id.value}.starts_with("call_salvaged_"));
        }
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            CHECK(f->stop_reason == StopReason::ToolUse);
}

// Plain prose that merely mentions JSON is NOT salvaged.
TEST_CASE("test_ndjson_prose_not_salvaged") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
            "\"Sure, here is what I think about your question.\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
            "\"done\":true}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read", "remember"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(!joined_text(msgs).empty());
}

// Markdown code fences with a language tag (```cpp, ```python) must stream
// immediately, NOT be held as potential tool-call JSON. This was a bug:
// the model emitting a code block with {} inside would freeze the stream.
TEST_CASE("test_sse_markdown_code_fence_not_held") {
    // A ```cpp code block with braces inside.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"```cpp\\n\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"int main() {\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\n  return 0;\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"\\n}\\n```\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"read"});
    // Should NOT be held/salvaged as a tool call.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    // All text should be flushed immediately as prose.
    auto text = joined_text(msgs);
    CHECK(text.find("```cpp") != std::string::npos);
    CHECK(text.find("int main()") != std::string::npos);
    CHECK(text.find("return 0") != std::string::npos);
}

// Regression: qwen2.5-coder:14b outputs tool calls wrapped in ```json fence.
// SIMPLE TEST: all content in one chunk.
TEST_CASE("test_ndjson_fenced_tool_call_simple") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"```json\\n{\\n  \\\"name\\\": \\\"read\\\",\\n  \\\"arguments\\\": {\\n    \\\"path\\\": \\\"/tmp/test\\\"\\n  }\\n}\\n```\""
        "},\"done\":true}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
}

// Regression: qwen2.5-coder:14b outputs tool calls wrapped in ```json fence
// via streaming tokens. Each token arrives separately.
[[maybe_unused]] static void test_ndjson_fenced_tool_call_streaming() {
    // Simulate the actual streaming from qwen2.5-coder:14b
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"```\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"json\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"{\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"name\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"read\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\",\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"arguments\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" {\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"   \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"path\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"/tmp/test\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\" }\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\n\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"```\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read"});
    // Debug: print all messages
    std::printf("\n=== test_ndjson_fenced_tool_call_streaming ===\n");
    std::printf("Total msgs: %zu\n", msgs.size());
    for (const auto& m : msgs) {
        if (auto* sm = std::get_if<msg::StreamMsg>(&m)) {
            std::visit([](const auto& inner) {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, StreamToolUseStart>) {
                    std::printf("  ToolUseStart: %s\n", inner.name.value.c_str());
                } else if constexpr (std::is_same_v<T, StreamTextDelta>) {
                    std::printf("  TextDelta: '%s'\n", inner.text.c_str());
                } else if constexpr (std::is_same_v<T, StreamFinished>) {
                    std::printf("  Finished\n");
                } else if constexpr (std::is_same_v<T, StreamToolUseDelta>) {
                    std::printf("  ToolUseDelta\n");
                } else if constexpr (std::is_same_v<T, StreamToolUseEnd>) {
                    std::printf("  ToolUseEnd\n");
                }
            }, *sm);
        }
    }
    // Should be salvaged as a tool call, NOT shown as raw JSON.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    // No raw JSON text should be visible.
    auto text = joined_text(msgs);
    CHECK(text.find("read") == std::string::npos);
    CHECK(text.find('{') == std::string::npos);
}

TEST_CASE("test_ndjson_empty_object_response") {
    // qwen2.5-coder:14b outputs {} when tools are passed
    auto msgs = oai::parse_ndjson_for_test(
        R"({"message":{"role":"assistant","content":"{}"},"done":false}
{"message":{"role":"assistant","content":""},"done":true,"done_reason":"stop"}
)", {"read", "write"});
    
    // Should flush {} as text, not show "unparseable"
    bool found_braces = false;
    bool found_unparseable = false;
    for (auto& m : msgs) {
        if (auto* td = get_leaf<StreamTextDelta>(m)) {
            if (td->text == "{}") found_braces = true;
            if (td->text.find("unparseable") != std::string::npos) found_unparseable = true;
        }
    }
    CHECK(found_braces);
    CHECK(!found_unparseable);
}

// ── Byte-identity guards ────────────────────────────────────────────────
// Pin the EXACT output of build_messages() and the exact Msg sequence of
// parse_sse_for_test() over feature-rich inputs, so the OpenAI transport can
// be refactored (e.g. split into modules) with a proven no-drift guarantee —
// same discipline as the Anthropic wire_golden / anthropic_sse_golden tests.
static std::uint64_t fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

TEST_CASE("test_wire_body_golden") {
    namespace oai = agentty::provider::openai;
    using agentty::Message; using agentty::Role; using agentty::Thread;
    using agentty::ThreadId; using agentty::ToolCallId; using agentty::ToolName;
    using agentty::ToolUse;

    auto done = [](const char* id, const char* name, nlohmann::json args,
                   const std::string& out) {
        ToolUse tc; tc.id = ToolCallId{id}; tc.name = ToolName{name};
        tc.args = std::move(args); tc.status = ToolUse::Done{{}, {}, out};
        return tc;
    };

    std::vector<Message> msgs;
    Message u; u.role = Role::User; u.text = "do the thing"; msgs.push_back(u);
    // read foo.cpp (superseded by the later edit)
    { Message a; a.role = Role::Assistant; a.text = "reading";
      a.tool_calls.push_back(done("call_r1", "read",
          nlohmann::json{{"path", "src/foo.cpp"}},
          std::string("FIRST_") + std::string(4096, 'a')));
      msgs.push_back(std::move(a)); }
    // big grep (faded once old enough)
    { Message a; a.role = Role::Assistant; a.text = "grepping";
      a.tool_calls.push_back(done("call_g1", "grep",
          nlohmann::json{{"pattern", "x"}},
          std::string("GHEAD_") + std::string(80 * 1024, 'g')));
      msgs.push_back(std::move(a)); }
    // edit foo.cpp (supersedes call_r1)
    { Message a; a.role = Role::Assistant; a.text = "editing";
      a.tool_calls.push_back(done("call_e1", "edit",
          nlohmann::json{{"path", "src/foo.cpp"}}, "ok"));
      msgs.push_back(std::move(a)); }
    // newest: re-read foo.cpp (live copy)
    { Message a; a.role = Role::Assistant; a.text = "re-reading";
      a.tool_calls.push_back(done("call_r2", "read",
          nlohmann::json{{"path", "src/foo.cpp"}},
          std::string("SECOND_") + std::string(4096, 'b')));
      msgs.push_back(std::move(a)); }

    Thread t{ThreadId{"g"}, "", std::move(msgs), {}, {}};
    const std::string wire = oai::build_messages(t).dump();

    CHECK(wire.find("FIRST_") == std::string::npos);      // superseded read collapsed
    CHECK(wire.find("superseded") != std::string::npos);  // pointer text present
    CHECK(wire.find("SECOND_") != std::string::npos);     // live read full

    constexpr std::uint64_t kGolden = 0x60773f1adaf86e13ull;
    const std::uint64_t got = fnv1a(wire);
    if (kGolden == 0)
        std::fprintf(stderr, "OPENAI wire golden = 0x%016llxull (len=%zu)\n",
                     static_cast<unsigned long long>(got), wire.size());
    else
        CHECK(got == kGolden);
}

TEST_CASE("test_sse_named_error_event_llamacpp") {
    // llama.cpp streams failures as a NAMED SSE event under HTTP 200:
    //   event: error
    //   data: {"code":500,"message":"...","type":"server_error"}
    // The old parser ignored event names entirely and only recognised the
    // {"error":...} wrapper — the frame was silently dropped, the stream
    // "closed clean", ensure_nonempty_turn fabricated "(empty response)",
    // and the retry machinery looped: the reported custom-host dead loop.
    std::string sse =
        "event: error\n"
        "data: {\"code\":500,\"message\":\"failed to apply chat template\","
        "\"type\":\"server_error\"}\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    // Exactly one StreamError, carrying the message AND the numeric code as
    // http_status so the reducer classifies via the typed path (500 →
    // Transient with a real budget — not string-sniffed).
    int errors = 0;
    for (const auto& m : msgs)
        if (const auto* e = get_leaf<StreamError>(m)) {
            ++errors;
            CHECK(e->message == "failed to apply chat template");
            CHECK(e->http_status == 500);
        }
    CHECK(errors == 1);
    // No fabricated "(empty response)" text and no StreamFinished — the
    // error is the terminal event.
    CHECK(joined_text(msgs).empty());
    CHECK(count_leaf<StreamFinished>(msgs) == 0);
}

TEST_CASE("test_sse_bare_error_object_llamacpp") {
    // Same failure, but as an UNNAMED data frame with the bare error shape
    // ({"code":..,"message":..} — no {"error":...} wrapper). Some llama.cpp
    // versions emit this; it must surface as StreamError too, not be
    // mistaken for an empty delta.
    std::string sse =
        "data: {\"code\":400,\"message\":\"unknown model alias\","
        "\"type\":\"invalid_request_error\"}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse);

    int errors = 0;
    for (const auto& m : msgs)
        if (const auto* e = get_leaf<StreamError>(m)) {
            ++errors;
            CHECK(e->message == "unknown model alias");
            CHECK(e->http_status == 400);   // → typed Terminal, never retried
        }
    CHECK(errors == 1);
    CHECK(joined_text(msgs).empty());
}

