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

#include <map>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/error_class.hpp"

using namespace agentty;
namespace cc = agentty::provider::chatgpt;
using json = nlohmann::json;


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
TEST_CASE("build body") {
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
    tc.name = ToolName{"shell"};
    tc.args = json{{"command", "ls"}};
    tc.status = ToolUse::Done{.output = "file1\nfile2"};
    a.tool_calls.push_back(tc);
    req.messages.push_back(a);

    provider::ToolSpec ts;
    ts.name         = "shell";
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
    CHECK(body["tools"][0]["name"] == "shell");
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
    CHECK(in[1]["name"] == "shell");
    CHECK(json::parse(in[1]["arguments"].get<std::string>())["command"] == "ls");

    CHECK(in[2]["type"] == "function_call_output");
    CHECK(in[2]["call_id"] == "call_abc");
    CHECK(in[2]["output"] == "file1\nfile2");
}

TEST_CASE("build body with images") {
    provider::Request req;
    req.model = "gpt-5.4";

    Message with_text;
    with_text.role = Role::User;
    with_text.text = "what is this?";
    with_text.images.push_back(ImageContent{
        .media_type = "image/png",
        .bytes = std::string{"\x89PNG", 4},
    });
    req.messages.push_back(std::move(with_text));

    // Image-only messages must not be discarded just because text is empty.
    Message image_only;
    image_only.role = Role::User;
    image_only.images.push_back(ImageContent{
        .media_type = "image/jpeg",
        .bytes = std::string{"\xff\xd8\xff", 3},
    });
    req.messages.push_back(std::move(image_only));

    const json body = cc::build_body_for_test(req);
    const auto& in = body["input"];
    CHECK(in.size() == 2);
    CHECK(in[0]["content"].size() == 2);
    CHECK(in[0]["content"][0]["type"] == "input_text");
    CHECK(in[0]["content"][0]["text"] == "what is this?");
    CHECK(in[0]["content"][1]["type"] == "input_image");
    CHECK(in[0]["content"][1]["image_url"] == "data:image/png;base64,iVBORw==");
    CHECK(in[1]["content"].size() == 1);
    CHECK(in[1]["content"][0]["type"] == "input_image");
    CHECK(in[1]["content"][0]["image_url"] == "data:image/jpeg;base64,/9j/");
}

// ── 2. SSE → Msg mapping ─────────────────────────────────────────────────────
TEST_CASE("sse text and usage") {
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

TEST_CASE("sse tool call") {
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
        if (auto* d = leaf<StreamToolUseDelta>(m)) {
            CHECK(d->id.value == "call_9");
            args += d->partial_json;
        }
        if (auto* e = leaf<StreamToolUseEnd>(m)) {
            CHECK(e->id.value == "call_9");
            end = true;
        }
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

TEST_CASE("sse parallel tool calls are id addressed") {
    std::vector<std::string> sse = {
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"edit"}})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{\"path\":\"a.cpp\","})",
        R"({"type":"response.output_item.added","item":{"type":"function_call","id":"fc_2","call_id":"call_2","name":"shell"}})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_2","delta":"{\"command\":\"true\"}"})",
        R"({"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"\"old_text\":\"x\",\"new_text\":\"y\"}"})",
        R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_2"}})",
        R"({"type":"response.output_item.done","item":{"type":"function_call","id":"fc_1"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };

    std::map<std::string, std::string> args;
    std::map<std::string, int> ends;
    for (const auto& m : cc::parse_sse_for_test(sse)) {
        if (auto* d = leaf<StreamToolUseDelta>(m))
            args[d->id.value] += d->partial_json;
        if (auto* e = leaf<StreamToolUseEnd>(m))
            ++ends[e->id.value];
    }

    CHECK(json::parse(args["call_1"])["new_text"] == "y");
    CHECK(json::parse(args["call_2"])["command"] == "true");
    CHECK(ends["call_1"] == 1);
    CHECK(ends["call_2"] == 1);
}

TEST_CASE("sse reasoning") {
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

// A completed reasoning output item yields StreamReasoning carrying its opaque
// encrypted_content — the blob the reducer stashes for cross-round replay.
TEST_CASE("sse reasoning encrypted capture") {
    std::vector<std::string> sse = {
        R"({"type":"response.reasoning_summary_text.delta","delta":"planning"})",
        R"({"type":"response.output_item.done","item":{"type":"reasoning","id":"rs_1","encrypted_content":"ENC_BLOB_123"}})",
        R"({"type":"response.completed","response":{"usage":{}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);
    std::string enc;
    for (const auto& m : msgs)
        if (auto* r = leaf<StreamReasoning>(m)) enc = r->encrypted;
    CHECK(enc == "ENC_BLOB_123");
}

// build_body must OPT IN to encrypted reasoning so the backend returns it.
TEST_CASE("build body requests encrypted reasoning") {
    provider::Request req;
    req.model = "gpt-5-codex";
    Message u; u.role = Role::User; u.text = "hi";
    req.messages.push_back(u);
    json body = cc::build_body_for_test(req);
    CHECK(body.contains("include"));
    CHECK(body["include"].is_array());
    bool has = false;
    for (const auto& x : body["include"])
        if (x == "reasoning.encrypted_content") has = true;
    CHECK(has);
}

// An assistant turn carrying reasoning_encrypted replays a `reasoning` item
// AHEAD of its function_call — with encrypted_content and NO server id.
TEST_CASE("build input replays reasoning") {
    provider::Request req;
    req.model = "gpt-5-codex";

    Message u; u.role = Role::User; u.text = "do it";
    req.messages.push_back(u);

    Message a; a.role = Role::Assistant; a.text = "";
    a.reasoning_encrypted = "BLOB_A\nBLOB_B";   // two items in one turn
    ToolUse tc;
    tc.id = ToolCallId{"call_1"}; tc.name = ToolName{"shell"};
    tc.args = json{{"command", "ls"}};
    tc.status = ToolUse::Done{.output = "ok"};
    a.tool_calls.push_back(tc);
    req.messages.push_back(a);

    json body = cc::build_body_for_test(req);
    const auto& in = body["input"];
    // user, reasoning(BLOB_A), reasoning(BLOB_B), function_call, output
    CHECK(in.size() == 5);
    CHECK(in[0]["type"] == "message");
    CHECK(in[1]["type"] == "reasoning");
    CHECK(in[1]["encrypted_content"] == "BLOB_A");
    CHECK(!in[1].contains("id"));                 // no server id under store:false
    CHECK(in[2]["type"] == "reasoning");
    CHECK(in[2]["encrypted_content"] == "BLOB_B");
    CHECK(in[3]["type"] == "function_call");      // reasoning precedes the call
    CHECK(in[4]["type"] == "function_call_output");
}

TEST_CASE("sse error") {
    std::vector<std::string> sse = {
        R"({"type":"response.failed","response":{"error":{"message":"boom"}}})",
    };
    auto msgs = cc::parse_sse_for_test(sse);
    bool saw = false;
    for (const auto& m : msgs)
        if (auto* e = leaf<StreamError>(m)) { saw = true; CHECK(e->message == "boom"); }
    CHECK(saw);
}

// An in-band error that carries a `type`/`code` must surface it in the message
// text so the runtime's classify() can route it to auto-retry (native Codex
// parity with Anthropic/OpenAI). Without the type, "Rate limited" alone would
// be classified Terminal and the turn would fail instead of backing off.
TEST_CASE("sse error type is retryable") {
    {
        std::vector<std::string> sse = {
            R"({"type":"response.failed","response":{"error":)"
            R"({"type":"rate_limit_exceeded","message":"Rate limited"}}})",
        };
        auto msgs = cc::parse_sse_for_test(sse);
        const StreamError* err = nullptr;
        for (const auto& m : msgs)
            if (auto* e = leaf<StreamError>(m)) err = e;
        CHECK(err != nullptr);
        if (err) {
            CHECK(err->message.find("rate_limit_exceeded") != std::string::npos);
            CHECK(provider::classify(err->message) == provider::ErrorClass::RateLimit);
        }
    }
    {
        // Top-level `error` event with a transient server_error type.
        std::vector<std::string> sse = {
            R"({"type":"error","code":"server_error","message":"upstream 503"})",
        };
        auto msgs = cc::parse_sse_for_test(sse);
        const StreamError* err = nullptr;
        for (const auto& m : msgs)
            if (auto* e = leaf<StreamError>(m)) err = e;
        CHECK(err != nullptr);
        if (err) {
            CHECK(err->message.find("server_error") != std::string::npos);
            CHECK(provider::classify(err->message) == provider::ErrorClass::Transient);
        }
    }
}

TEST_CASE("http error details") {
    CHECK(cc::format_http_error_for_test(
        400, R"({"error":{"type":"invalid_request_error","message":"input is too long"}})")
        == "invalid_request_error: input is too long");
    CHECK(cc::format_http_error_for_test(
        400, "event: error\ndata: {\"error\":\"context window exceeded\"}\n\n")
        == "context window exceeded");
    CHECK(cc::format_http_error_for_test(400, "plain edge rejection\n")
        == "Codex backend returned HTTP 400: plain edge rejection");
}

TEST_CASE("session id is stable per conversation") {
    // Caching hinges on the same conversation carrying the SAME session_id
    // every turn (a fresh random id each turn defeats the backend prompt
    // cache). Derivation must be deterministic per key and differ across keys.
    const std::string a1 = cc::session_id_for_test("thread-A");
    const std::string a2 = cc::session_id_for_test("thread-A");
    const std::string b1 = cc::session_id_for_test("thread-B");
    CHECK(!a1.empty());
    CHECK(a1 == a2);          // stable across turns of one conversation
    CHECK(a1 != b1);          // distinct conversations don't collide
    CHECK(a1.size() == 36);   // uuid-v4 shaped (backend accepts it)
    CHECK(a1[14] == '4');     // version nibble
    // No durable identity → non-empty random id (correctness over caching).
    CHECK(!cc::session_id_for_test("").empty());
}

TEST_CASE("stale tool result is budget capped") {
    // A giant tool output from an OLD call must NOT ship verbatim every turn
    // (it would replay on the wire and burn tokens). The newest result keeps
    // the full budget; stale successful ones fade to a tight head+tail via
    // the shared wire::cap_tool_result_aged path.
    provider::Request req;
    req.model = "gpt-5-codex";

    const std::string big(200000, 'x');   // 200 KiB dump

    // Oldest call: the big result, many turns back.
    Message a1; a1.role = Role::Assistant;
    ToolUse old_tc;
    old_tc.id = ToolCallId{"call_old"};
    old_tc.name = ToolName{"grep"};
    old_tc.args = json::object();
    old_tc.status = ToolUse::Done{.output = big};
    a1.tool_calls.push_back(old_tc);
    req.messages.push_back(a1);

    // A run of newer tool calls so the old one ages past the full-result window.
    for (int i = 0; i < 12; ++i) {
        Message a; a.role = Role::Assistant;
        ToolUse tc;
        tc.id = ToolCallId{"call_" + std::to_string(i)};
        tc.name = ToolName{"shell"};
        tc.args = json::object();
        tc.status = ToolUse::Done{.output = "ok"};
        a.tool_calls.push_back(tc);
        req.messages.push_back(a);
    }

    const json body = cc::build_body_for_test(req);
    const auto& in = body["input"];
    // Find the oldest function_call_output (call_old) and assert it was capped.
    std::string old_output;
    for (const auto& item : in) {
        if (item["type"] == "function_call_output"
            && item["call_id"] == "call_old") {
            old_output = item["output"].get<std::string>();
            break;
        }
    }
    CHECK(!old_output.empty());
    CHECK(old_output.size() < big.size());   // faded, not verbatim
    CHECK(old_output.size() < 8192);         // tight head+tail budget
}

