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

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agtest.hpp"

#include "agentty/provider/ollama/transport.hpp"

using namespace agentty;
namespace oll = agentty::provider::ollama;


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

// Concatenated reasoning text (the unified StreamThinkingDelta event every
// provider emits). Used to assert Ollama surfaces native + inline reasoning.
static std::string joined_thinking(const std::vector<Msg>& msgs) {
    std::string s;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamThinkingDelta>(m)) s += d->text;
    return s;
}

static std::string joined_tool_args(const std::vector<Msg>& msgs);

// ── 1. build_messages ────────────────────────────────────────────────────────
TEST_CASE("build messages text") {
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

TEST_CASE("build messages tool calls") {
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

// Age-tiered tool-result clearing (shared wire::cap_tool_result_aged). Local
// models have the SMALLEST context windows, so a stale 60 KiB dump replaying
// in full every turn hurts most here. Exercised on BOTH the native role:"tool"
// path and the JSON-protocol "TOOL RESULT (name):" user-turn path.
TEST_CASE("build messages age tiering") {
    std::string big = "HEAD_SENTINEL_AAAA\n";
    big.append(60 * 1024, 'x');
    big += "\nTAIL_SENTINEL_ZZZZ";

    const int n = 20;
    auto make = [&]() {
        std::vector<Message> msgs;
        Message u; u.role = Role::User; u.text = "start"; msgs.push_back(u);
        for (int i = 0; i < n; ++i) {
            Message a; a.role = Role::Assistant; a.text = "c";
            ToolUse tc;
            tc.id   = ToolCallId{"call_" + std::to_string(i)};
            tc.name = ToolName{"grep"};
            tc.status = ToolUse::Done{{}, {}, big};
            a.tool_calls.push_back(std::move(tc));
            msgs.push_back(std::move(a));
        }
        return msgs;
    };

    // Native path: results are role:"tool" messages, in emit order (oldest
    // first). The FIRST tool message is the oldest (faded); the LAST is newest.
    {
        auto arr = oll::build_messages(make(), /*json_protocol=*/false);
        std::string oldest, newest;
        for (const auto& msg : arr)
            if (msg.value("role", "") == "tool") {
                if (oldest.empty()) oldest = msg.value("content", "");
                newest = msg.value("content", "");
            }
        CHECK(newest.size() > 40 * 1024);
        CHECK(oldest.size() < 6 * 1024);
        CHECK(oldest.find("bytes elided") != std::string::npos);
        CHECK(oldest.find("HEAD_SENTINEL_AAAA") != std::string::npos);
        CHECK(oldest.find("TAIL_SENTINEL_ZZZZ") != std::string::npos);
    }

    // JSON-protocol path: results are role:"user" "TOOL RESULT (name):" turns.
    {
        auto arr = oll::build_messages(make(), /*json_protocol=*/true);
        std::string oldest, newest;
        for (const auto& msg : arr)
            if (msg.value("role", "") == "user") {
                auto c = msg.value("content", "");
                if (c.rfind("TOOL RESULT", 0) != 0) continue;
                if (oldest.empty()) oldest = c;
                newest = c;
            }
        CHECK(newest.size() > 40 * 1024);
        CHECK(oldest.size() < 6 * 1024);
        CHECK(oldest.find("bytes elided") != std::string::npos);
    }
}

TEST_CASE("build messages images") {
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
TEST_CASE("ndjson plain text") {
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

// Native reasoning: Ollama's `message.thinking` field (populated when we send
// think:true) must surface as the unified StreamThinkingDelta event — the SAME
// event Anthropic/OpenAI-compat/ChatGPT emit — and must NOT leak into visible
// prose.
TEST_CASE("ndjson native thinking field") {
    const std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"thinking\":\"Let me plan. \","
        "\"content\":\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"thinking\":\"Step two.\","
        "\"content\":\"\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"The answer.\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    CHECK(joined_thinking(msgs) == "Let me plan. Step two.");
    CHECK(joined_text(msgs) == "The answer.");
    // Reasoning never appears as prose.
    CHECK(joined_text(msgs).find("plan") == std::string::npos);
}

// Inline reasoning fallback: models that dump chain-of-thought into
// <think>…</think> in `content` (deepseek-r1, qwen3 without the native field)
// must have it STRIPPED from prose AND surfaced as reasoning.
TEST_CASE("ndjson inline think block surfaced as reasoning") {
    const std::string nd =
        "{\"message\":{\"role\":\"assistant\","
        "\"content\":\"<think>weigh options</think>Final answer.\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    CHECK(joined_text(msgs) == "Final answer.");
    // The reasoning is surfaced (unified event), not discarded.
    CHECK(joined_thinking(msgs).find("weigh options") != std::string::npos);
    // And never leaks into prose.
    CHECK(joined_text(msgs).find("weigh") == std::string::npos);
    CHECK(joined_text(msgs).find("<think>") == std::string::npos);
}

TEST_CASE("ndjson structured tool call") {
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

TEST_CASE("ndjson content salvage to tool") {
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

TEST_CASE("ndjson no tools means no salvage") {
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

TEST_CASE("ndjson footgun tool swallowed") {
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

TEST_CASE("ndjson explicit memory tool salvaged") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"name\\\":\\\"remember\\\",\\\"arguments\\\":"
        "{\\\"text\\\":\\\"hi\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(
        nd, {"remember", "write"}, /*json_protocol=*/false,
        /*allow_memory_salvage=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(joined_tool_args(msgs) == std::string{"{\"text\":\"hi\"}"});
}

TEST_CASE("ndjson rescue tool from prose") {
    // qwen narrates prose AND buries a tool call in a ```sh fenced block. The
    // terminal rescue scan must find it and salvage it into a real call.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"Sure, I'll rename it.\\n\\n```sh\\n"
        "{\\\"name\\\": \\\"bash\\\", \\\"arguments\\\": "
        "{\\\"command\\\":\\\"mv /a /b\\\"}}\\n```\\nDone.\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    bool saw_args = false;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m))
            if (d->partial_json.find("mv /a /b") != std::string::npos) saw_args = true;
    CHECK(saw_args);
}

TEST_CASE("ndjson unknown tool in prose not salvaged") {
    // A fenced block naming a NON-advertised tool (qwen hallucinates `git`)
    // must NOT be salvaged — there's no such tool to call.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"```sh\\n{\\\"name\\\": \\\"git\\\", \\\"arguments\\\": "
        "{\\\"command\\\":\\\"mv /a /b\\\"}}\\n```\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

TEST_CASE("ndjson error frame") {
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
        if (const auto* d = get_leaf<StreamToolUseDelta>(m)) s += d->partial_json;
    return s;
}
static std::string first_tool_name(const std::vector<Msg>& msgs) {
    for (const auto& m : msgs)
        if (const auto* s = get_leaf<StreamToolUseStart>(m)) return s->name.value;
    return {};
}

// Clean single-object {thoughts, tool_name, tool_args} → one tool call.
TEST_CASE("jp clean object") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"thoughts\\\":[\\\"list files\\\"],\\\"tool_name\\\":"
        "\\\"bash\\\",\\\"tool_args\\\":{\\\"command\\\":\\\"ls -la\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "write"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_tool_args(msgs).find("ls -la") != std::string::npos);
    // The reasoning JSON must NOT be emitted as prose.
    CHECK(joined_text(msgs).find("thoughts") == std::string::npos);
}

// Leading narration before the object ("Sure! {...}") still salvages.
TEST_CASE("jp object with leading prose") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"Sure, here goes: {\\\"tool_name\\\":\\\"bash\\\","
        "\\\"tool_args\\\":{\\\"command\\\":\\\"pwd\\\"}} done!\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_tool_args(msgs).find("pwd") != std::string::npos);
}

// `tool`/`args` aliases (loose schema) are accepted.
TEST_CASE("jp tool args aliases") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool\\\":\\\"write\\\",\\\"args\\\":"
        "{\\\"file_path\\\":\\\"/tmp/x\\\",\\\"content\\\":\\\"hi\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"write", "shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "write");
    CHECK(joined_tool_args(msgs).find("/tmp/x") != std::string::npos);
}

// tool_name with a `tool:action` suffix → args.action injected.
TEST_CASE("jp action suffix") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"bash:run\\\",\\\"tool_args\\\":"
        "{\\\"command\\\":\\\"echo hi\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_tool_args(msgs).find("\"action\":\"run\"") != std::string::npos);
}

// Plain chat (no tool) in JSON-protocol mode → prose, no phantom call.
TEST_CASE("jp plain chat no tool") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello! How can I help?\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    CHECK(joined_text(msgs).find("How can I help") != std::string::npos);
}

// A tool_args object that itself contains a `}` inside a quoted string must
// not truncate the balanced-object extraction.
TEST_CASE("jp brace in string value") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"bash\\\",\\\"tool_args\\\":"
        "{\\\"command\\\":\\\"echo '}'\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(joined_tool_args(msgs).find("echo '}'") != std::string::npos);
}

// An unknown tool_name in JSON-protocol mode is NOT salvaged into a call.
TEST_CASE("jp unknown tool not salvaged") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"frobnicate\\\",\\\"tool_args\\\":{}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "write"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

// Grammar `response` pseudo-tool: tool_name=="response" with text in tool_args
// is unwrapped into a plain text delta — no phantom tool call, no raw JSON.
TEST_CASE("jp response pseudo tool") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"thoughts\\\":[\\\"hi\\\"],\\\"tool_name\\\":\\\"response\\\","
        "\\\"tool_args\\\":{\\\"text\\\":\\\"Hello there!\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(joined_text(msgs).find("Hello there!") != std::string::npos);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    // The raw protocol JSON must NOT leak into the prose.
    CHECK(joined_text(msgs).find("tool_name") == std::string::npos);
}

// Grammar-forced tool call: tool_name=="shell" with command in tool_args fires
// a real bash tool-use start/delta/end carrying the args.
TEST_CASE("jp grammar tool call") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"thoughts\\\":[\\\"list files\\\"],\\\"tool_name\\\":\\\"bash\\\","
        "\\\"tool_args\\\":{\\\"command\\\":\\\"ls\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(count_leaf<StreamToolUseEnd>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_tool_args(msgs).find("ls") != std::string::npos);
}

// The `response` text may also be carried under the `response` key in
// tool_args (some models pick that alias instead of `text`).
TEST_CASE("jp response alias key") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"response\\\","
        "\\\"tool_args\\\":{\\\"response\\\":\\\"hey\\\"}}\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(joined_text(msgs).find("hey") != std::string::npos);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

// Progressive `response` streaming: a grammar-forced chat reply object that
// arrives across MANY content frames (the real Ollama wire) must be decoded
// and emitted INCREMENTALLY (one delta per frame as bytes arrive) so maya's
// reveal_fx animates the typewriter — and the full reply text must appear
// EXACTLY ONCE (no duplication at the done/freeze handoff). This is the fix
// for the qwen2.5-coder "Hi Ayush!Hi Ayush!" duplication + frozen-md bug.
TEST_CASE("jp response streamed incrementally") {
    // {"tool_name":"response","tool_args":{"text":"Hello Ayush!"}}
    // delivered char-by-char like Ollama's grammar-constrained stream.
    auto frame = [](const std::string& c) {
        std::string esc;
        for (char ch : c) {
            if (ch == '"') esc += "\\\"";
            else if (ch == '\\') esc += "\\\\";
            else if (ch == '\n') esc += "\\n";
            else if (ch == '\t') esc += "\\t";
            else if (ch == '\r') esc += "\\r";
            else esc += ch;
        }
        return "{\"message\":{\"role\":\"assistant\",\"content\":\""
             + esc + "\"}}\n";
    };
    std::vector<std::string> chunks = {
        "{\n ", " \"tool", "_name", "\": ", " \"", "response", "\",\n",
        " ", " \"tool", "_args", "\": ", " {\n", "   ", " \"", "text",
        "\": ", " \"", "Hello", " Ay", "ush", "!\"\n", " ", " }\n", "}",
    };
    std::string nd;
    for (const auto& c : chunks) nd += frame(c);
    nd += "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
          "\"done\":true,\"done_reason\":\"stop\"}\n";

    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    // Full reply, exactly once — no duplication.
    std::string txt = joined_text(msgs);
    CHECK(txt == "Hello Ayush!");
    // The protocol scaffolding must NOT leak into prose.
    CHECK(txt.find("tool_name") == std::string::npos);
    CHECK(txt.find("tool_args") == std::string::npos);
    CHECK(txt.find('{') == std::string::npos);
    // No phantom tool call.
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
    // Emitted as MULTIPLE deltas (incremental reveal), not one settle dump.
    CHECK(count_leaf<StreamTextDelta>(msgs) >= 2);
}

// Escaped characters inside the streamed reply (\n, \", \\) must be decoded
// correctly even when the escape straddles a frame boundary.
TEST_CASE("jp response streamed escapes") {
    // tool_args.text == 'line1\nsay "hi"'  (a newline + an escaped quote)
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"tool_name\\\":\\\"response\\\",\\\"tool_args\\\":"
        "{\\\"text\\\":\\\"line1\\\\nsay \\\\\\\"hi\\\\\\\"\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    std::string txt = joined_text(msgs);
    CHECK(txt == "line1\nsay \"hi\"");
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

// A REAL tool call streamed char-by-char must NOT be hijacked by the
// progressive-response path — it still fires a tool-use, no text leaks.
TEST_CASE("jp streamed tool call not hijacked") {
    auto frame = [](const std::string& esc) {
        return "{\"message\":{\"role\":\"assistant\",\"content\":\""
             + esc + "\"}}\n";
    };
    std::string nd;
    nd += frame("{\\\"tool_name\\\":\\\"");
    nd += frame("bash\\\",\\\"tool_args\\\":");
    nd += frame("{\\\"command\\\":\\\"ls\\\"}}");
    nd += "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
          "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_tool_args(msgs).find("ls") != std::string::npos);
    CHECK(joined_text(msgs).find("tool_name") == std::string::npos);
}

// A REAL captured qwen2.5-coder:latest grammar-constrained stream (24 frames,
// curl /api/chat with the response pseudo-tool). The reply was
//   { "tool_name": "response", "tool_args": { "text": "Hi Ayush!" } }
// emitted char-by-char, INCLUDING the model's trailing " }" frame that used to
// leak a stray brace onto the reply. This replays the exact bytes off the wire
// so the fix is proven against the model, not just a hand-written fixture.
TEST_CASE("jp live capture qwen") {
    static const char* kFrames[] = {
        "{\"message\":{\"role\":\"assistant\",\"content\":\"{\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"tool\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"_name\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"response\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\",\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"tool\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"_args\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" {\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"text\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\\\":\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" \\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hi\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" Ay\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"ush\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"!\\\"\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" }\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\" }\"}}",
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\","
        "\"prompt_eval_count\":51,\"eval_count\":24}",
    };
    std::string nd;
    for (const char* f : kFrames) { nd += f; nd += '\n'; }

    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    std::string txt = joined_text(msgs);
    CHECK(txt == "Hi Ayush!");                       // exact, ONCE (no dup)
    CHECK(txt.find('{') == std::string::npos);        // no scaffolding leak
    CHECK(txt.find('}') == std::string::npos);        // no trailing-brace leak
    CHECK(txt.find("tool_name") == std::string::npos);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0); // it's a chat reply
    CHECK(count_leaf<StreamTextDelta>(msgs) >= 2);     // streamed incrementally
    CHECK(count_leaf<StreamUsage>(msgs) == 1);
}

// ── 5. build_options (num_ctx / num_predict / sampling) ──────────────────────
TEST_CASE("options unknown window default") {
    // No probed context window → safe agent-sized default 8192, well above
    // Ollama's 2k/4k floor that silently truncates long conversations.
    oll::Request r;
    r.max_tokens = 16384;
    r.context_window = 0;
    auto o = oll::build_options(r);
    CHECK(o["num_ctx"].get<int>() == 8192);
    // num_predict is clamped to half the window so the prompt always fits.
    CHECK(o["num_predict"].get<int>() == 4096);
}

TEST_CASE("options probed window") {
    // A real 32768-window model gets that window (clamped to the 32768 ceiling).
    oll::Request r;
    r.max_tokens = 16384;
    r.context_window = 32768;
    auto o = oll::build_options(r);
    CHECK(o["num_ctx"].get<int>() == 32768);
    CHECK(o["num_predict"].get<int>() == 16384);  // half of 32768, == max_tokens
}

TEST_CASE("options huge window clamped") {
    // A 128k-window model is clamped to the agent ceiling so we don't try to
    // allocate a giant KV cache and OOM the local Ollama server.
    oll::Request r;
    r.max_tokens = 16384;
    r.context_window = 131072;
    auto o = oll::build_options(r);
    CHECK(o["num_ctx"].get<int>() == 32768);
}

TEST_CASE("options small window floored") {
    // A model that reports a tiny window still gets the agent floor.
    oll::Request r;
    r.max_tokens = 16384;
    r.context_window = 2048;
    auto o = oll::build_options(r);
    CHECK(o["num_ctx"].get<int>() == 8192);
}

TEST_CASE("options json protocol sampling") {
    // Weak/json-protocol path gets a low temperature for tool-call reliability.
    oll::Request r;
    r.max_tokens = 16384;
    r.context_window = 8192;
    r.json_protocol = true;
    auto o = oll::build_options(r);
    CHECK(o.contains("temperature"));
    CHECK(o["temperature"].get<double>() < 0.5);
    // Capable (non-json-protocol) models keep their Modelfile defaults.
    oll::Request r2;
    r2.max_tokens = 16384;
    r2.context_window = 8192;
    auto o2 = oll::build_options(r2);
    CHECK(!o2.contains("temperature"));
}

// ── 6. <think> reasoning stripping ───────────────────────────────────────────
TEST_CASE("think block stripped from prose") {
    // A reasoning model inlines <think>…</think> then answers. Only the answer
    // is shown; the reasoning never reaches the user.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"<think>let me reason</think>The answer is 42.\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd);
    CHECK(joined_text(msgs).find("The answer is 42") != std::string::npos);
    CHECK(joined_text(msgs).find("let me reason") == std::string::npos);
    CHECK(joined_text(msgs).find("<think>") == std::string::npos);
}

TEST_CASE("think then tool call salvaged") {
    // The model thinks, then leaks a tool call in content. The <think> block
    // must NOT defeat the hold/salvage path — the tool call still fires.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"<think>I should list files</think>"
        "{\\\"name\\\":\\\"bash\\\",\\\"arguments\\\":"
        "{\\\"command\\\":\\\"ls\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "write"});
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 1);
    CHECK(first_tool_name(msgs) == "shell");
    CHECK(joined_text(msgs).find("should list files") == std::string::npos);
}

// ── 3. system_prompt ─────────────────────────────────────────────────────────
TEST_CASE("system prompt shape") {
    auto p = oll::system_prompt();
    CHECK(p.find("agentty") != std::string::npos);
    CHECK(p.find("CONVERSATION MEMORY") != std::string::npos);
    CHECK(p.find("ENVIRONMENT") != std::string::npos);
    // The verbose Claude prose / big learned-memory dump must NOT be present.
    CHECK(p.find("<file-editing>") == std::string::npos);
    CHECK(p.find("<learned-memory") == std::string::npos);
}

// ── NEW: JSON-protocol history round-trip ────────────────────────────────────
// In JSON-protocol mode build_messages must render the tool call as the
// model's own {tool_name,tool_args} object (assistant role) and the result as
// a plain USER "TOOL RESULT (name):" turn — NOT the native tool_calls[]/
// role:"tool" shape a prose-only model can't read.
TEST_CASE("build messages json protocol roundtrip") {
    std::vector<Message> msgs;
    Message a; a.role = Role::Assistant; a.text = "";
    ToolUse tc;
    tc.id   = ToolCallId{"call_1"};
    tc.name = ToolName{"shell"};
    tc.args = nlohmann::json{{"command", "ls -la"}};
    tc.status = ToolUse::Done{{}, {}, "file1\nfile2"};
    a.tool_calls.push_back(std::move(tc));
    msgs.push_back(std::move(a));

    auto arr = oll::build_messages(msgs, /*json_protocol=*/true);
    // assistant prose-JSON object + user TOOL RESULT turn (no native shapes).
    CHECK(arr.size() == 2);
    CHECK(arr[0]["role"] == "assistant");
    CHECK(!arr[0].contains("tool_calls"));               // NOT native
    {
        auto obj = nlohmann::json::parse(arr[0]["content"].get<std::string>());
        CHECK(obj["tool_name"] == "shell");
        CHECK(obj["tool_args"]["command"] == "ls -la");
    }
    CHECK(arr[1]["role"] == "user");                     // NOT role:"tool"
    CHECK(arr[1]["content"].get<std::string>().find("TOOL RESULT (shell)")
          != std::string::npos);
    CHECK(arr[1]["content"].get<std::string>().find("file1") != std::string::npos);
}

// ── NEW: arg-key repair on the NATIVE structured channel ─────────────────────
// A weak model on the native channel emits the legacy `bash` name (still an
// alias for `shell`) with {"cmd":...} instead of {"command":...}; the parser
// must both accept the alias and remap the key to the canonical `command`.
TEST_CASE("ndjson native arg key repair") {
    const char* nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\","
        "\"tool_calls\":[{\"function\":{\"name\":\"bash\","
        "\"arguments\":{\"cmd\":\"ls\"}}}]}}\n"
        "{\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell", "bash"}, /*json_protocol=*/false);
    std::string args;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m)) args += d->partial_json;
    auto j = nlohmann::json::parse(args);
    CHECK(j.contains("command"));        // remapped
    CHECK(j["command"] == "ls");
    CHECK(!j.contains("cmd"));            // alias erased
}

// ── NEW: arg-key repair on the SALVAGE channel ───────────────────────────────
// A leaked {"tool_name":"read","tool_args":{"file":"x.c"}} must land as
// {"path":"x.c"} so the read tool actually finds its argument.
TEST_CASE("jp salvage arg key repair") {
    const char* nd =
        "{\"message\":{\"role\":\"assistant\","
        "\"content\":\"{\\\"tool_name\\\":\\\"read\\\","
        "\\\"tool_args\\\":{\\\"file\\\":\\\"x.c\\\"}}\"}}\n"
        "{\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"read"}, /*json_protocol=*/true);
    std::string args;
    for (const auto& m : msgs)
        if (const auto* d = get_leaf<StreamToolUseDelta>(m)) args += d->partial_json;
    auto j = nlohmann::json::parse(args);
    CHECK(j.contains("path"));           // file → path
    CHECK(j["path"] == "x.c");
    CHECK(!j.contains("file"));
}

// ── Never-blank-turn safety net (Aider's `if not received_content`) ──────────
// A qwen `response` object whose `text` is empty but whose `thoughts` carry
// the real words must surface the thoughts as prose — never a blank turn.
TEST_CASE("jp response empty text falls back to thoughts") {
    // Single content frame carrying the whole {thoughts,tool_name:response,
    // tool_args:{text:""}} object, then a separate done frame. text is empty
    // so the fallback must surface `thoughts` as prose.
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"{\\\"thoughts\\\":[\\\"You are Ayush\\\"],\\\"tool_name\\\":"
        "\\\"response\\\",\\\"tool_args\\\":{\\\"text\\\":\\\"\\\"}}\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    std::string txt = joined_text(msgs);
    CHECK(txt.find("You are Ayush") != std::string::npos);
    CHECK(txt.find("tool_name") == std::string::npos);
    CHECK(count_leaf<StreamToolUseStart>(msgs) == 0);
}

// A JSON-protocol turn that produced NOTHING usable (model emitted only
// whitespace) must still render a visible turn: Aider's exact
// `(empty response)` placeholder, never a silent void.
TEST_CASE("jp truly empty shows placeholder") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"   \"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"}, /*json_protocol=*/true);
    CHECK(joined_text(msgs).find("(empty response)") != std::string::npos);
    CHECK(count_leaf<StreamFinished>(msgs) == 1);
}

// Plain prose that merely STARTED life looking like it could be JSON but is
// not a tool call must STILL reach the user — never a silent blank turn.
TEST_CASE("prose starting with brace not dropped") {
    std::string nd =
        "{\"message\":{\"role\":\"assistant\",\"content\":"
        "\"Here is the answer: 42.\"}}\n"
        "{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
        "\"done\":true,\"done_reason\":\"stop\"}\n";
    auto msgs = oll::parse_ndjson_for_test(nd, {"shell"});
    CHECK(joined_text(msgs).find("42") != std::string::npos);
}

// ── Byte-identity guard ─────────────────────────────────────────────────
// Pin the EXACT build_messages() output over a feature-rich message list so
// the Ollama transport can be refactored with a no-drift guarantee (same
// discipline as the Anthropic/OpenAI golden guards).
static std::uint64_t oll_fnv1a(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

TEST_CASE("wire body golden") {
    auto done = [](const char* id, const char* name, nlohmann::json args,
                   const std::string& out) {
        ToolUse tc; tc.id = ToolCallId{id}; tc.name = ToolName{name};
        tc.args = std::move(args); tc.status = ToolUse::Done{{}, {}, out};
        return tc;
    };
    std::vector<Message> msgs;
    Message u; u.role = Role::User; u.text = "do it"; msgs.push_back(u);
    { Message a; a.role = Role::Assistant; a.text = "reading";
      a.tool_calls.push_back(done("call_r1", "read",
          nlohmann::json{{"path", "src/foo.cpp"}},
          std::string("FIRST_") + std::string(4096, 'a')));
      msgs.push_back(std::move(a)); }
    { Message a; a.role = Role::Assistant; a.text = "grepping";
      a.tool_calls.push_back(done("call_g1", "grep",
          nlohmann::json{{"pattern", "x"}},
          std::string("GHEAD_") + std::string(80 * 1024, 'g')));
      msgs.push_back(std::move(a)); }
    { Message a; a.role = Role::Assistant; a.text = "editing";
      a.tool_calls.push_back(done("call_e1", "edit",
          nlohmann::json{{"path", "src/foo.cpp"}}, "ok"));
      msgs.push_back(std::move(a)); }
    { Message a; a.role = Role::Assistant; a.text = "re-reading";
      a.tool_calls.push_back(done("call_r2", "read",
          nlohmann::json{{"path", "src/foo.cpp"}},
          std::string("SECOND_") + std::string(4096, 'b')));
      msgs.push_back(std::move(a)); }

    const std::string wire = oll::build_messages(msgs, /*json_protocol=*/false).dump();
    CHECK(wire.find("FIRST_") == std::string::npos);      // superseded read collapsed
    CHECK(wire.find("superseded") != std::string::npos);
    CHECK(wire.find("SECOND_") != std::string::npos);     // live read full

    constexpr std::uint64_t kGolden = 0xd4bc14dd8fc53bb8ull;
    const std::uint64_t got = oll_fnv1a(wire);
    if (kGolden == 0)
        std::fprintf(stderr, "OLLAMA wire golden = 0x%016llxull (len=%zu)\n",
                     static_cast<unsigned long long>(got), wire.size());
    else
        CHECK(got == kGolden);
}

