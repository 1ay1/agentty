// provider_conformance_test — ONE contract, asserted against EVERY dialect.
//
// ── Why this file exists ─────────────────────────────────────────────────
//
// Four of the bugs that reached users were the same bug wearing different
// clothes, and each was found by hand, in one dialect, after someone hit it:
//
//   • Responses dropped `function_call_arguments.done` → Copilot tool calls
//     dispatched with `{}` ("[invalid args] pattern required").
//   • Chat blindly concatenated `delta.tool_calls[].arguments`, so a
//     coalescing proxy produced `{"a":1}{"a":1}` — same symptom, different
//     dialect, found only because we went looking after fixing the first.
//
// Per-dialect tests could not have caught that, because each encoded the ONE
// server framing its author had seen. The fix is to state the contract once
// and instantiate it for every backend, so a new provider inherits every
// invariant instead of relying on whoever adds it to remember.
//
// Each dialect below supplies only a `tool_call(name, args, style)` function
// that frames one tool call in its own wire language. Everything asserted is
// dialect-independent — that is the point.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/ollama/transport.hpp"
#include "agentty/provider/openai/transport.hpp"

using namespace agentty;
using json = nlohmann::json;

namespace {

template <class Leaf>
const Leaf* leaf(const Msg& m) {
    const Leaf* found = nullptr;
    std::visit([&](const auto& domain) {
        std::visit([&](const auto& l) {
            if constexpr (std::is_same_v<std::decay_t<decltype(l)>, Leaf>)
                found = &l;
        }, domain);
    }, m);
    return found;
}

// How a server may frame the arguments. Every arm is spec-legal where the
// dialect supports it; a dialect that cannot express one skips it.
enum class Style { Fragments, Snapshot, Both };

const char* name_of(Style s) {
    switch (s) {
        case Style::Fragments: return "Fragments";
        case Style::Snapshot:  return "Snapshot";
        case Style::Both:      return "Both";
    }
    return "?";
}

// The decoded outcome of one tool call, in dialect-independent terms.
struct Decoded {
    std::string name;
    std::string args;
    int         starts = 0;
    int         ends   = 0;
};

Decoded decode(const std::vector<Msg>& msgs) {
    Decoded d;
    for (const auto& m : msgs) {
        if (auto* s = leaf<StreamToolUseStart>(m)) { ++d.starts; d.name = s->name.value; }
        if (auto* x = leaf<StreamToolUseDelta>(m)) d.args += x->partial_json;
        if (auto* e = leaf<StreamToolUseEnd>(m))   ++d.ends;
    }
    return d;
}

// ── The dialects ─────────────────────────────────────────────────────────

struct Responses {
    static constexpr const char* id = "openai-responses";
    static bool supports(Style) { return true; }
    static Decoded run(const std::string& name, const std::string& args, Style st) {
        const std::string esc = json(args).dump();
        std::vector<std::string> sse{
            R"({"type":"response.output_item.added","item":{"type":"function_call",)"
            R"("id":"fc_1","call_id":"call_1","name":")" + name + R"("}})"};
        if (st != Style::Snapshot) {
            const auto mid = args.size() / 2;
            for (const auto& part : {args.substr(0, mid), args.substr(mid)})
                sse.push_back(
                    R"({"type":"response.function_call_arguments.delta",)"
                    R"("item_id":"fc_1","delta":)" + json(part).dump() + "}");
        }
        if (st != Style::Fragments)
            sse.push_back(R"({"type":"response.function_call_arguments.done",)"
                          R"("item_id":"fc_1","arguments":)" + esc + "}");
        sse.push_back(R"({"type":"response.output_item.done","item":)"
                      R"({"type":"function_call","id":"fc_1"}})");
        sse.push_back(R"({"type":"response.completed","response":{"usage":{}}})");
        return decode(provider::chatgpt::parse_sse_for_test(sse));
    }
};

struct Chat {
    static constexpr const char* id = "openai-chat";
    // Chat has no separate "done" event carrying arguments; a coalescing
    // proxy instead REPEATS the full value in each chunk. That is this
    // dialect's snapshot form.
    static bool supports(Style) { return true; }
    static Decoded run(const std::string& name, const std::string& args, Style st) {
        const std::string esc = json(args).dump();
        std::vector<std::string> chunks;
        auto tc = [&](const std::string& head, const std::string& a) {
            chunks.push_back(R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
                             + head + R"("function":{)" + a + "}}]}}]}");
        };
        if (st == Style::Snapshot) {
            tc(R"("id":"call_1",)", R"("name":")" + name + R"(","arguments":)" + esc);
            tc("", R"("arguments":)" + esc);          // repeat: adds nothing
        } else {
            const auto mid = args.size() / 2;
            tc(R"("id":"call_1",)", R"("name":")" + name + R"(","arguments":)"
                                    + json(args.substr(0, mid)).dump());
            tc("", R"("arguments":)" + json(args.substr(mid)).dump());
            if (st == Style::Both) tc("", R"("arguments":)" + esc);
        }
        chunks.push_back(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})");
        std::string bytes;
        for (const auto& c : chunks) bytes += "data: " + c + "\n\n";
        bytes += "data: [DONE]\n\n";
        return decode(provider::openai::parse_sse_for_test(bytes, {name}));
    }
};

struct AnthropicMessages {
    static constexpr const char* id = "anthropic-messages";
    // Anthropic has exactly ONE carrier: input_json_delta fragments. There is
    // no snapshot event, so the snapshot arms are not expressible — and that
    // is a fact worth pinning rather than papering over.
    static bool supports(Style s) { return s == Style::Fragments; }
    static Decoded run(const std::string& name, const std::string& args, Style) {
        const auto mid = args.size() / 2;
        std::vector<std::pair<std::string, std::string>> ev{
            {"content_block_start",
             R"({"index":0,"content_block":{"type":"tool_use","id":"call_1","name":")"
             + name + R"(","input":{}}})"},
        };
        for (const auto& part : {args.substr(0, mid), args.substr(mid)})
            ev.push_back({"content_block_delta",
                          R"({"index":0,"delta":{"type":"input_json_delta",)"
                          R"("partial_json":)" + json(part).dump() + "}}"});
        ev.push_back({"content_block_stop", R"({"index":0})"});
        ev.push_back({"message_delta", R"({"delta":{"stop_reason":"tool_use"}})"});
        ev.push_back({"message_stop", "{}"});
        return decode(provider::anthropic::parse_sse_for_test(ev));
    }
};

struct OllamaNative {
    static constexpr const char* id = "ollama-native";
    // Ollama emits a tool call ATOMICALLY from a fully-parsed object — there
    // is no incremental form to reconcile. Only the snapshot arm applies.
    static bool supports(Style s) { return s == Style::Snapshot; }
    static Decoded run(const std::string& name, const std::string& args, Style) {
        const std::string nd =
            R"({"message":{"role":"assistant","tool_calls":[{"function":)"
            R"({"name":")" + name + R"(","arguments":)" + args + "}}]},\"done\":false}\n"
            R"({"message":{"role":"assistant","content":""},"done":true})" "\n";
        return decode(provider::ollama::parse_ndjson_for_test(nd, {name}));
    }
};

// The payload every dialect must round-trip: nested object, an escaped quote,
// a slash — the shapes that break naive string handling.
const std::string kArgs =
    R"({"pattern":"foo\"bar","glob":"src/**/*.cpp","opts":{"word":true}})";

template <class D>
void check_contract() {
    for (auto st : {Style::Fragments, Style::Snapshot, Style::Both}) {
        if (!D::supports(st)) continue;
        INFO("dialect = " << std::string{D::id}
             << ", framing = " << std::string{name_of(st)});
        const auto d = D::run("grep", kArgs, st);

        // 1. The arguments the model authored are the arguments the tool
        //    layer receives, regardless of how the server framed them.
        //
        //    Compared as PARSED JSON, not bytes: a dialect may legitimately
        //    re-serialise (Ollama round-trips through json::dump() to repair
        //    wrong argument keys, which normalises key order and whitespace).
        //    What must not change is the VALUE. A double-appended stream
        //    (`{...}{...}`) fails to parse at all, so this still catches the
        //    duplicate-emission bug it was written for.
        REQUIRE_NOTHROW(json::parse(d.args));
        CHECK(json::parse(d.args) == json::parse(kArgs));

        // 3. The call is announced once and closed once — the reducer pairs
        //    tool_use with tool_result on exactly these.
        CHECK(d.starts == 1);
        CHECK(d.ends == 1);
        CHECK(d.name == "grep");
    }
}

} // namespace

TEST_CASE("conformance: openai-responses")   { check_contract<Responses>(); }
TEST_CASE("conformance: openai-chat")        { check_contract<Chat>(); }
TEST_CASE("conformance: anthropic-messages") { check_contract<AnthropicMessages>(); }
TEST_CASE("conformance: ollama-native")      { check_contract<OllamaNative>(); }

// An empty argument object is a LEGITIMATE tool call (repo_map, list, test
// take no arguments) and must survive as `{}` — not as a dropped delta. This
// is the case that made the Copilot bug read like a flaky model: the
// zero-argument tools kept working while every other tool failed.
TEST_CASE("conformance: an empty argument object survives every dialect") {
    const auto empty = [](const std::string& a) {
        return !a.empty() && json::parse(a).is_object() && json::parse(a).empty();
    };
    CHECK(empty(Responses::run("repo_map", "{}", Style::Snapshot).args));
    CHECK(empty(Chat::run("repo_map", "{}", Style::Snapshot).args));
    CHECK(empty(AnthropicMessages::run("repo_map", "{}", Style::Fragments).args));
    CHECK(empty(OllamaNative::run("repo_map", "{}", Style::Snapshot).args));
}
