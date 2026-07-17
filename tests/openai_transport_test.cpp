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
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include <cstdlib>

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

static void test_build_messages_age_tiering() {
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

// ── <tool_call>-tag-wrapped leak (qwen/Hermes chat-template form) ──────
// The qwen2.5-coder template instructs the model to wrap calls in
// <tool_call>…</tool_call>. When Ollama fails to strip those tags they
// arrive in `content`; salvage must peel the tags and recover the call.
static void test_sse_salvage_tool_call_tags() {
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
static void test_sse_salvage_fenced_tags() {
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

static void test_sse_salvage_unknown_tool_stays_text() {
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
static void test_sse_plain_object_stays_text() {
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

static void test_sse_fence_only_leak_dropped() {
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

    // Registry ↔ from_spec consistency: EVERY OpenAI-family preset id must
    // resolve to a usable endpoint (non-empty host + chat/models paths).
    // Anthropic is skipped — it doesn't go through the OpenAI from_spec.
    // This catches the "added a registry row, forgot the endpoint arm"
    // class of bug: the fallback would treat the id as a raw hostname and
    // silently dial the wrong place.
    for (const auto& p : agentty::provider::providers()) {
        if (p.kind != agentty::provider::Kind::OpenAI) continue;
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

static void test_build_request_headers() {
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
static void test_resolve_auth_per_preset() {
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
        if (p.kind == Kind::Anthropic) {
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
static void test_sse_salvage_streamed_tokens() {
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
static void test_sse_prose_then_tool_call() {
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
static void test_sse_salvage_array_of_calls() {
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
static void test_sse_salvage_two_sequential_calls() {
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
static void test_sse_salvage_function_key() {
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
static void test_sse_salvage_memory_tool_swallowed() {
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

// A leaked `skill` call (the meta-tool weak models hallucinate from the
// catalog block on a greeting — {"name":"skill","arguments":{"name":...}})
// must be SWALLOWED, never executed. Surfacing it spawns a "skill not found"
// card that then loops. The JSON is consumed, not shown as prose.
static void test_sse_salvage_skill_swallowed() {
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
static void test_ndjson_plain_greeting() {
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
static void test_ndjson_structured_tool_call() {
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
static void test_ndjson_leaked_content_salvaged() {
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
static void test_ndjson_prose_not_salvaged() {
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
static void test_sse_markdown_code_fence_not_held() {
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
static void test_ndjson_fenced_tool_call_simple() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"```json\\n{\\n  \\\"name\\\": \\\"read\\\",\\n  \\\"arguments\\\": {\\n    \\\"path\\\": \\\"/tmp/test\\\"\\n  }\\n}\\n```\""
        "},\"done\":true}\n";
    auto msgs = oai::parse_ndjson_for_test(nd, {"read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
}

// Regression: qwen2.5-coder:14b outputs tool calls wrapped in ```json fence
// via streaming tokens. Each token arrives separately.
static void test_ndjson_fenced_tool_call_streaming() {
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

void test_ndjson_empty_object_response() {
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

int main() {
    test_build_tools();
    test_build_messages_basic();
    test_build_messages_tool_roundtrip();
    test_build_messages_age_tiering();
    test_sse_text_stream();
    test_sse_tool_call_stream();
    test_sse_two_tool_calls();
    test_sse_error_frame();
    test_sse_salvage_leaked_tool_call();
    test_sse_salvage_tool_call_tags();
    test_sse_salvage_fenced_tags();
    test_sse_salvage_unknown_tool_stays_text();
    test_sse_plain_object_stays_text();
    test_sse_truncated_leaked_tool_call_dropped();
    test_sse_fence_only_leak_dropped();
    test_sse_two_leaked_calls_unique_ids();
    test_sse_plain_json_prose_not_salvaged();
    test_sse_structured_tool_still_works_with_salvage_on();
    test_endpoint_presets();
    test_build_request_headers();
    test_resolve_auth_per_preset();
    // Incremental salvage tests.
    test_sse_salvage_streamed_tokens();
    test_sse_prose_then_tool_call();
    test_sse_salvage_array_of_calls();
    test_sse_salvage_two_sequential_calls();
    test_sse_salvage_function_key();
    test_sse_salvage_memory_tool_swallowed();
    test_sse_salvage_skill_swallowed();

    // Native Ollama /api/chat NDJSON path.
    test_ndjson_plain_greeting();
    test_ndjson_structured_tool_call();
    test_ndjson_leaked_content_salvaged();
    test_ndjson_prose_not_salvaged();

    // Markdown code fence regression.
    test_sse_markdown_code_fence_not_held();

    // Streaming fence regression (TODO: streaming case needs more work).
    test_ndjson_fenced_tool_call_simple();
    // test_ndjson_fenced_tool_call_streaming();

    test_ndjson_empty_object_response();

    if (g_failures == 0) {
        std::printf("openai_transport_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "openai_transport_test: %d check(s) failed\n", g_failures);
    return 1;
}
