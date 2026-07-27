// codex_responses_test — verifies the ChatGPT (Codex) Responses-API transport's
// pure translation layer, no network:
//
//   1. build_body_for_test  — Request → Responses-API JSON (flat function
//                             tools, input[] items, function_call +
//                             function_call_output pairing, reasoning/effort).
//   2. parse_sse_for_test   — scripted Responses SSE `data:` JSON → the agentty
//                             Msg sequence the reducer consumes (text deltas,
//                             reasoning, streamed tool-call assembly,
//                             response.completed → StopReason + usage).
//
// Run: build the `codex_responses_test` target, execute. Exit 0 = pass.

#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/provider/chatgpt/responses.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;
using json = nlohmann::json;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

template <class Leaf>
static const Leaf* leaf(const Msg& m) {
    const Leaf* found = nullptr;
    std::visit([&](const auto& domain) {
        std::visit([&](const auto& l) {
            if constexpr (std::is_same_v<std::decay_t<decltype(l)>, Leaf>)
                found = &l;
        }, domain);
    }, m);
    return found;
}

// ── 1. Request body ─────────────────────────────────────────────────────────
static void test_build_body() {
    provider::Request req;
    req.model         = "gpt-5-codex";
    req.system_prompt = "You are a coding agent.";
    req.effort        = "high";

    Message u; u.role = Role::User; u.text = "list the files";
    req.messages.push_back(u);

    // An assistant turn that called a tool, with a completed result.
    Message a; a.role = Role::Assistant; a.text = "";
    ToolUse tc;
    tc.id   = ToolCallId{"call_abc"};
    tc.name = ToolName{"bash"};
    tc.args = json{{"command", "ls"}};
    tc.status = ToolUse::Done{.output = "file1\nfile2"};
    a.tool_calls.push_back(tc);
    req.messages.push_back(a);

    provider::ToolSpec ts;
    ts.name         = "bash";
    ts.description  = "run a shell command";
    ts.input_schema = json{{"type", "object"},
                           {"properties", {{"command", {{"type", "string"}}}}}};
    req.tools.push_back(ts);

    json body = cc::build_body_for_test(req);

    CHECK(body["model"] == "gpt-5-codex");
    CHECK(body["instructions"] == "You are a coding agent.");
    CHECK(body["stream"] == true);
    CHECK(body["store"] == false);
    CHECK(body["tool_choice"] == "auto");
    CHECK(body["reasoning"]["effort"] == "high");

    // Tools are FLAT (name/description/parameters at top level).
    CHECK(body["tools"].is_array() && body["tools"].size() == 1);
    CHECK(body["tools"][0]["type"] == "function");
    CHECK(body["tools"][0]["name"] == "bash");
    CHECK(body["tools"][0].contains("parameters"));
    CHECK(!body["tools"][0].contains("function"));  // NOT nested

    // input[]: user message, then function_call, then function_call_output.
    const auto& in = body["input"];
    CHECK(in.is_array() && in.size() == 3);
    CHECK(in[0]["type"] == "message");
    CHECK(in[0]["role"] == "user");
    CHECK(in[0]["content"][0]["type"] == "input_text");
    CHECK(in[0]["content"][0]["text"] == "list the files");

    CHECK(in[1]["type"] == "function_call");
    CHECK(in[1]["call_id"] == "call_abc");
    CHECK(in[1]["name"] == "bash");
    CHECK(json::parse(in[1]["arguments"].get<std::string>())["command"] == "ls");

    CHECK(in[2]["type"] == "function_call_output");
    CHECK(in[2]["call_id"] == "call_abc");
    CHECK(in[2]["output"] == "file1\nfile2");
}

// ── 2. SSE → Msg mapping ─────────────────────────────────────────────────────
static void test_sse_text_and_usage() {
    std::vector<std::string> sse = {
        R"({"type":"response.created","response":{}})",
        R"({"type":"response.output_item.added","item":{"type":"message","role":"assistant"}})",
        R"({"type":"response.output_text.delta","delta":"Hello "})",
        R"({"type":"response.output_text.delta","delta":"world"})",
        R"({"type":"response.output_item.done","item":{"type":"message"}})",
        R"({"type":"response.completed","response":{"usage":{"input_tokens":12,"output_tokens":5,"input_tokens_details":{"cached_tokens":8}}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);

    std::string text;
    bool finished = false, saw_usage = false;
    StopReason stop = StopReason::Unspecified;
    for (const auto& m : msgs) {
        if (auto* d = leaf<StreamTextDelta>(m)) text += d->text;
        if (auto* f = leaf<StreamFinished>(m)) { finished = true; stop = f->stop_reason; }
        if (auto* u = leaf<StreamUsage>(m)) {
            saw_usage = true;
            CHECK(u->input_tokens == 12);
            CHECK(u->output_tokens == 5);
            CHECK(u->cache_read_input_tokens == 8);
        }
    }
    CHECK(text == "Hello world");
    CHECK(finished);
    CHECK(stop == StopReason::EndTurn);
    CHECK(saw_usage);
}

static void test_sse_tool_call() {
    std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_9","name":"read"}})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{\"path\":"})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"\"a.txt\"}"})",
        R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_1"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);

    std::string name, id, args;
    bool start = false, end = false, finished = false;
    StopReason stop = StopReason::Unspecified;
    for (const auto& m : msgs) {
        if (auto* s = leaf<StreamToolUseStart>(m)) { start = true; name = s->name.value; id = s->id.value; }
        if (auto* d = leaf<StreamToolUseDelta>(m)) args += d->partial_json;
        if (leaf<StreamToolUseEnd>(m)) end = true;
        if (auto* f = leaf<StreamFinished>(m)) { finished = true; stop = f->stop_reason; }
    }
    CHECK(start);
    CHECK(name == "read");
    CHECK(id == "call_9");                 // correlation id, not the fc_ item id
    CHECK(json::parse(args)["path"] == "a.txt");
    CHECK(end);
    CHECK(finished);
    CHECK(stop == StopReason::ToolUse);    // a function_call ⇒ ToolUse stop
}

static void test_sse_reasoning() {
    std::vector<std::string> sse = {
        R"({"type":"response.reasoning_summary_text.delta","delta":"thinking…"})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);
    std::string think;
    for (const auto& m : msgs)
        if (auto* t = leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "thinking…");
}

static void test_sse_error() {
    std::vector<std::string> sse = {
        R"({"type":"response.failed","response":{"error":{"message":"boom"}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);
    bool saw = false;
    for (const auto& m : msgs)
        if (auto* e = leaf<StreamError>(m)) { saw = true; CHECK(e->message == "boom"); }
    CHECK(saw);
}

int main() {
    test_build_body();
    test_sse_text_and_usage();
    test_sse_tool_call();
    test_sse_reasoning();
    test_sse_error();
    if (g_failures) { std::fprintf(stderr, "\n%d check(s) failed\n", g_failures); return 1; }
    std::puts("codex_responses_test: all checks passed");
    return 0;
}
