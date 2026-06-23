// ollama_transport_test — verifies the dedicated Ollama /api/chat transport's
// pure translation layer with NO network:
//
//   1. build_messages    — Thread messages → native messages array
//                          (assistant tool_calls with OBJECT args + id,
//                          separate role:"tool" results with tool_name,
//                          user images as base64 array).
//   2. parse_ndjson_for_test — scripted NDJSON frames → the agentty Msg
//                          sequence the reducer consumes (text deltas,
//                          STRUCTURED tool_calls → Start/Delta/End, usage,
//                          done_reason→StopReason). NO content-salvage:
//                          a bare JSON object in content stays prose.
//   3. system_prompt     — contains the recall reminder + env block.
//
// Run: build the `ollama_transport_test` target, execute. Exit 0 = pass.

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agentty/provider/ollama/transport.hpp"

using namespace agentty;
namespace oll = agentty::provider::ollama;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

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

static std::string joined_text(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamTextDelta>(m)) s += d->text;
    return s;
}

// ── 1. build_messages ────────────────────────────────────────────────────────
static void test_build_messages_text() {
    std::vector<Message> msgs;
    Message u; u.role = Role::User; u.text = "hello"; msgs.push_back(u);
    Message a; a.role = Role::Assistant; a.text = "hi there"; msgs.push_back(a);

    auto arr = oll::build_messages(msgs);
    CHECK(arr.is_array());
    CHECK(arr.size() == 2);
    CHECK(arr[0]["role"] == "user");
    CHECK(arr[0]["content"] == "hello");
    CHECK(arr[1]["role"] == "assistant");
    CHECK(arr[1]["content"] == "hi there");
}

static void test_build_messages_tool_calls() {
    // Assistant message with a terminal tool call → assistant msg carries
    // tool_calls[{id, function:{name, arguments:OBJECT}}] AND a separate
    // role:"tool" message with tool_name + content follows it.
    std::vector<Message> msgs;
    Message a;
    a.role = Role::Assistant;
    a.text = "";
    ToolUse tc;
    tc.id   = ToolCallId{"call_1"};
    tc.name = ToolName{"read"};
    tc.args = nlohmann::json{{"path", "/etc/hostname"}};
    tc.status = ToolUse::Done{{}, {}, "myhost"};
    a.tool_calls.push_back(std::move(tc));
    msgs.push_back(std::move(a));

    auto arr = oll::build_messages(msgs);
    CHECK(arr.size() == 2);
    // assistant message
    CHECK(arr[0]["role"] == "assistant");
    CHECK(arr[0].contains("tool_calls"));
    CHECK(arr[0]["tool_calls"].size() == 1);
    CHECK(arr[0]["tool_calls"][0]["id"] == "call_1");
    CHECK(arr[0]["tool_calls"][0]["function"]["name"] == "read");
    // arguments must be a JSON OBJECT, not a serialized string
    CHECK(arr[0]["tool_calls"][0]["function"]["arguments"].is_object());
    CHECK(arr[0]["tool_calls"][0]["function"]["arguments"]["path"]
          == "/etc/hostname");
    // tool result message
    CHECK(arr[1]["role"] == "tool");
    CHECK(arr[1]["tool_name"] == "read");
    CHECK(arr[1]["content"] == "myhost");
}

static void test_build_messages_images() {
    std::vector<Message> msgs;
    Message u; u.role = Role::User; u.text = "what is this?";
    ImageContent img; img.media_type = "image/png"; img.bytes = "abc";
    u.images.push_back(std::move(img));
    msgs.push_back(std::move(u));

    auto arr = oll::build_messages(msgs);
    CHECK(arr.size() == 1);
    CHECK(arr[0].contains("images"));
    CHECK(arr[0]["images"].is_array());
    CHECK(arr[0]["images"].size() == 1);
    CHECK(arr[0]["images"][0].is_string());
}

// ── 2. parse_ndjson ──────────────────────────────────────────────────────────
static void test_ndjson_plain_text() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Ayush\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\","
        "\"prompt_eval_count\":10,\"eval_count\":3}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    CHECK(joined_text(msgs) == "Hello Ayush");
    CHECK(count_leaf<StreamFinished>(msgs) == 1);
    CHECK(count_leaf<StreamUsage>(msgs) == 1);
    // No tool calls leaked.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

static void test_ndjson_structured_tool_call() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\","
        "\"tool_calls\":[{\"id\":\"call_x\",\"function\":{\"name\":\"read\","
        "\"arguments\":{\"path\":\"/tmp/f\"}}}]},"
        "\"done_reason\":\"stop\",\"done\":true}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    // The arguments object is forwarded as the delta payload.
    bool saw_args = false;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m))
            if (d->partial_json.find("/tmp/f") != std::string::npos) saw_args = true;
    CHECK(saw_args);
    // StopReason should be ToolUse.
    bool tool_stop = false;
    for (const auto& m : msgs)
        if (const auto* f = get_leaf<StreamFinished>(m))
            if (f->stop_reason == StopReason::ToolUse) tool_stop = true;
    CHECK(tool_stop);
}

static void test_ndjson_content_salvage_to_tool() {
    // A weak model leaks a tool call as bare JSON in CONTENT. With the tool
    // advertised, it is salvaged into a STRUCTURED call (not shown as text).
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"name\\\":\\\"write\\\",\\\"arguments\\\":"
        "{\\\"file_path\\\":\\\"/tmp/f\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"write", "read"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    bool saw_args = false;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m))
            if (d->partial_json.find("/tmp/f") != std::string::npos) saw_args = true;
    CHECK(saw_args);
    // Salvaged JSON must NOT also appear as visible prose.
    CHECK(joined_text(msgs).find("\"name\"") == std::string::npos);
}

static void test_ndjson_no_tools_means_no_salvage() {
    // With NO tools advertised, JSON-looking content is plain text — there's
    // nothing to salvage to, so don't swallow legitimate prose.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"name\\\":\\\"read\\\",\\\"arguments\\\":{}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);   // no known_tools
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("\"name\"") != std::string::npos);
}

static void test_ndjson_footgun_tool_swallowed() {
    // A leaked `remember` call (footgun tool) is NEVER auto-run AND never
    // surfaced as garbage text — it is silently swallowed.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"name\\\":\\\"remember\\\",\\\"arguments\\\":"
        "{\\\"text\\\":\\\"hi\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"remember", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);   // not run
    CHECK(joined_text(msgs).find("remember") == std::string::npos);  // not shown
}

static void test_ndjson_rescue_tool_from_prose() {
    // qwen narrates prose AND buries a tool call in a ```sh fenced block. The
    // terminal rescue scan must find it and salvage it into a real call.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"Sure, I'll rename it.\\n\\n```sh\\n"
        "{\\\"name\\\": \\\"bash\\\", \\\"arguments\\\": "
        "{\\\"command\\\":\\\"mv /a /b\\\"}}\\n```\\nDone.\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    bool saw_args = false;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m))
            if (d->partial_json.find("mv /a /b") != std::string::npos) saw_args = true;
    CHECK(saw_args);
}

static void test_ndjson_unknown_tool_in_prose_not_salvaged() {
    // A fenced block naming a NON-advertised tool (qwen hallucinates `git`)
    // must NOT be salvaged — there's no such tool to call.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"```sh\\n{\\\"name\\\": \\\"git\\\", \\\"arguments\\\": "
        "{\\\"command\\\":\\\"mv /a /b\\\"}}\\n```\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

static void test_ndjson_error_frame() {
    std::string nd = "{\"error\":\"model not found\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    bool saw_err = false;
    for (const auto& m : msgs)
        if (const auto* e = get_leaf<StreamError>(m))
            if (e->message.find("model not found") != std::string::npos)
                saw_err = true;
    CHECK(saw_err);
}

// ── 4. JSON-protocol mode (very weak models, agent-zero style) ───────────────
// Concatenate every StreamToolUseDelta payload (the salvaged args JSON).
static std::string joined_tool_args(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m)) s += d->args;
    return s;
}
static std::string first_tool_name(const std::vector<Msg>& msgs) {
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) return s->name.value;
    return {};
}

// Clean single-object {thoughts, tool_name, tool_args} → one tool call.
static void test_jp_clean_object() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"thoughts\\\":[\\\"list files\\\"],\\\"tool_name\\\":"
        "\\\"bash\\\",\\\"tool_args\\\":{\\\"command\\\":\\\"ls -la\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash", "write"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "bash");
    CHECK(joined_tool_args(msgs).find("ls -la") != std::string::npos);
    // The reasoning JSON must NOT be emitted as prose.
    CHECK(joined_text(msgs).find("thoughts") == std::string::npos);
}

// Leading narration before the object ("Sure! {...}") still salvages.
static void test_jp_object_with_leading_prose() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"Sure, here goes: {\\\"tool_name\\\":\\\"bash\\\","
        "\\\"tool_args\\\":{\\\"command\\\":\\\"pwd\\\"}} done!\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "bash");
    CHECK(joined_tool_args(msgs).find("pwd") != std::string::npos);
}

// `tool`/`args` aliases (loose schema) are accepted.
static void test_jp_tool_args_aliases() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool\\\":\\\"write\\\",\\\"args\\\":"
        "{\\\"file_path\\\":\\\"/tmp/x\\\",\\\"content\\\":\\\"hi\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"write", "bash"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "write");
    CHECK(joined_tool_args(msgs).find("/tmp/x") != std::string::npos);
}

// tool_name with a `tool:action` suffix → args.action injected.
static void test_jp_action_suffix() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"bash:run\\\",\\\"tool_args\\\":"
        "{\\\"command\\\":\\\"echo hi\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "bash");
    CHECK(joined_tool_args(msgs).find("\"action\":\"run\"") != std::string::npos);
}

// Plain chat (no tool) in JSON-protocol mode → prose, no phantom call.
static void test_jp_plain_chat_no_tool() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello! How can I help?\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("How can I help") != std::string::npos);
}

// A tool_args object that itself contains a `}` inside a quoted string must
// not truncate the balanced-object extraction.
static void test_jp_brace_in_string_value() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"bash\\\",\\\"tool_args\\\":"
        "{\\\"command\\\":\\\"echo '}'\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(joined_tool_args(msgs).find("echo '}'") != std::string::npos);
}

// An unknown tool_name in JSON-protocol mode is NOT salvaged into a call.
static void test_jp_unknown_tool_not_salvaged() {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"frobnicate\\\",\\\"tool_args\\\":{}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"bash", "write"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

// ── 3. system_prompt ─────────────────────────────────────────────────────────
static void test_system_prompt_shape() {
    auto p = oll::system_prompt();
    CHECK(p.find("agentty") != std::string::npos);
    CHECK(p.find("CONVERSATION MEMORY") != std::string::npos);
    CHECK(p.find("ENVIRONMENT") != std::string::npos);
    // The verbose Claude prose / big learned-memory dump must NOT be present.
    CHECK(p.find("<file-editing>") == std::string::npos);
    CHECK(p.find("<learned-memory") == std::string::npos);
}

int main() {
    test_build_messages_text();
    test_build_messages_tool_calls();
    test_build_messages_images();
    test_ndjson_plain_text();
    test_ndjson_structured_tool_call();
    test_ndjson_content_salvage_to_tool();
    test_ndjson_no_tools_means_no_salvage();
    test_ndjson_footgun_tool_swallowed();
    test_ndjson_rescue_tool_from_prose();
    test_ndjson_unknown_tool_in_prose_not_salvaged();
    test_ndjson_error_frame();
    test_jp_clean_object();
    test_jp_object_with_leading_prose();
    test_jp_tool_args_aliases();
    test_jp_action_suffix();
    test_jp_plain_chat_no_tool();
    test_jp_brace_in_string_value();
    test_jp_unknown_tool_not_salvaged();
    test_system_prompt_shape();

    if (g_failures == 0) {
        std::printf("ollama_transport_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "ollama_transport_test: %d check(s) failed\n", g_failures);
    return 1;
}
