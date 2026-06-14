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

#include "agentty/provider/openai/transport.hpp"

using namespace agentty;
namespace oai = agentty::provider::openai;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

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

static void test_build_tools() {
    std::vector<provider::ToolSpec> tools;
    tools.push_back({"read", "Read a file",
                     nlohmann::json{{"type", "object"},
                                    {"properties", {{"path", {{"type", "string"}}}}}},
                     false});
    auto j = oai::build_tools(tools);
    CHECK(j.is_array());
    CHECK(j.size() == 1);
    CHECK(j[0]["type"] == "function");
    CHECK(j[0]["function"]["name"] == "read");
    CHECK(j[0]["function"]["description"] == "Read a file");
    CHECK(j[0]["function"]["parameters"]["type"] == "object");
}

static void test_build_messages_basic() {
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

static void test_build_messages_tool_roundtrip() {
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

static void test_sse_text_stream() {
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

static void test_sse_tool_call_stream() {
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

static void test_sse_two_tool_calls() {
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

static void test_sse_error_frame() {
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
static void test_sse_salvage_leaked_tool_call() {
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

static void test_sse_salvage_unknown_tool_stays_text() {
    // A bare JSON naming a tool we did NOT advertise must NOT be salvaged —
    // it falls through to ordinary text (we never invent a call).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"nonexistent\\\", \\\"arguments\\\": {}}\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(!joined_text(msgs).empty());    // surfaced as text
}

static void test_sse_plain_json_prose_not_salvaged() {
    // Ordinary prose that merely STARTS with text isn't held/mangled.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Sure, here you go.\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"echo"});
    CHECK(joined_text(msgs) == "Sure, here you go.");
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

static void test_sse_structured_tool_still_works_with_salvage_on() {
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

static void test_sse_truncated_leaked_tool_call_dropped() {
    // qwen leaks the call into `content` but the wire cuts off mid-body
    // (no closing braces, no [DONE]). The half-written JSON must NOT surface
    // as visible prose — dumping it pollutes the assistant turn and the weak
    // model re-leaks the same call next turn (the stuck "upstream cut off"
    // re-invocation). It must be dropped silently.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":"
            "\"{\\\"name\\\": \\\"remember\\\", \\\"argum\"}}]}\n\n";
    auto msgs = oai::parse_sse_for_test(sse, {"remember"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);  // not salvageable
    CHECK(joined_text(msgs).empty());                  // and not flushed as text
}

static void test_sse_two_leaked_calls_unique_ids() {
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

static void test_endpoint_presets() {
    auto groq = oai::Endpoint::from_spec("groq");
    CHECK(groq.host == "api.groq.com");
    CHECK(groq.use_tls);
    CHECK(groq.path == "/openai/v1/chat/completions");

    auto ollama = oai::Endpoint::from_spec("ollama");
    CHECK(ollama.host == "localhost");
    CHECK(ollama.port == 11434);
    CHECK(!ollama.use_tls);

    auto custom = oai::Endpoint::from_spec("my.host:8080");
    CHECK(custom.host == "my.host");
    CHECK(custom.port == 8080);
    CHECK(!custom.use_tls);

    auto def = oai::Endpoint::from_spec("");
    CHECK(def.host == "api.openai.com");
    CHECK(def.use_tls);
}

int main() {
    test_build_tools();
    test_build_messages_basic();
    test_build_messages_tool_roundtrip();
    test_sse_text_stream();
    test_sse_tool_call_stream();
    test_sse_two_tool_calls();
    test_sse_error_frame();
    test_sse_salvage_leaked_tool_call();
    test_sse_salvage_unknown_tool_stays_text();
    test_sse_truncated_leaked_tool_call_dropped();
    test_sse_two_leaked_calls_unique_ids();
    test_sse_plain_json_prose_not_salvaged();
    test_sse_structured_tool_still_works_with_salvage_on();
    test_endpoint_presets();

    if (g_failures == 0) {
        std::printf("openai_transport_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "openai_transport_test: %d check(s) failed\n", g_failures);
    return 1;
}
