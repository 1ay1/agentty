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
#include "agentty/provider/stream_scaffold.hpp"

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

// ── Hostile framing: the same call, sent the way a BROKEN server sends it ──
//
// The contract above uses well-formed wire. Every tool-call bug this project
// has shipped came from the other kind, and a survey of other harnesses says
// it is universal, not our incompetence: openai-python #3377 (fragments
// keyed on arrival order instead of index), opik #8360 (streamed tool calls
// dropped entirely), strands #3950, crewai, azure-ai, theia, llm-gateway
// #136 (empty id on continuations). Same field, same handful of mistakes.
//
// The root cause is a property of the DIALECT, not of any server: Chat
// Completions carries several distinct facts on fields that do not say what
// they are. `arguments` is a fragment or a whole snapshot. `id` is the
// identity or an empty restatement. `index` is the parallel-call key or
// absent. Anthropic names its fragments in the type and Responses separates
// them by event, which is exactly why neither has this bug class.
//
// So the decoder cannot be "correct" against a spec — it has to be correct
// against the wire that real servers emit. These cases pin the shapes that
// have actually broken someone, and they run against EVERY dialect that can
// express them, so a fix in one decoder cannot quietly skip another.
struct Hostile {
    // A continuation chunk restates `id` as empty instead of omitting it.
    static Decoded chat_empty_id(const std::string& name, const std::string& args) {
        const auto mid = args.size() / 2;
        auto tc = [&](const std::string& head, const std::string& a) {
            return R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
                   + head + R"("function":{)" + a + "}}]}}]}";
        };
        std::string bytes;
        bytes += "data: " + tc(R"("id":"call_1",)",
                               R"("name":")" + name + R"(","arguments":")" "\"")
                 + "\n\n";
        bytes += "data: " + tc(R"("id":"",)",
                               R"("arguments":)" + json(args.substr(0, mid)).dump())
                 + "\n\n";
        bytes += "data: " + tc(R"("id":"",)",
                               R"("arguments":)" + json(args.substr(mid)).dump())
                 + "\n\n";
        bytes += R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"
                 "\n\n" "data: [DONE]\n\n";
        return decode(provider::openai::parse_sse_for_test(bytes, {name}));
    }

    // Token-by-token fragmentation fine enough that a fragment equals a
    // PREFIX of the buffer — issue #48. `{"` opening a nested object when
    // the accumulated value already starts `{"`.
    static Decoded chat_prefix_fragments(const std::string& name,
                                         const std::string& args) {
        std::string bytes;
        bytes += R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,)"
                 R"("id":"call_1","function":{"name":")" + name +
                 R"(","arguments":""}}]}}]})" "\n\n";
        // One byte at a time: the most adversarial framing a server can pick,
        // and what llama.cpp approximates.
        for (char c : args) {
            bytes += "data: " +
                std::string{R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
                            R"("function":{"arguments":)"}
                + json(std::string(1, c)).dump() + "}}]}}]}" + "\n\n";
        }
        bytes += R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"
                 "\n\n" "data: [DONE]\n\n";
        return decode(provider::openai::parse_sse_for_test(bytes, {name}));
    }
};

TEST_CASE("conformance: hostile framing round-trips the same arguments") {
    // Whatever the server does to the framing, the tool layer must receive
    // exactly what the model authored.
    for (auto make : {&Hostile::chat_empty_id, &Hostile::chat_prefix_fragments}) {
        const auto d = make("grep", kArgs);
        REQUIRE_NOTHROW((void)json::parse(d.args));
        CHECK(json::parse(d.args) == json::parse(kArgs));
        // Announced once, closed once — the reducer pairs tool_use with
        // tool_result on exactly these, so a dropped End hangs the turn.
        CHECK(d.starts == 1);
        CHECK(d.ends == 1);
        CHECK(d.name == "grep");
    }
}

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
        //
        //    The (void) cast is load-bearing: REQUIRE_NOTHROW evaluates the
        //    expression for its THROW behaviour only, so the nodiscard
        //    return would otherwise warn (-Wunused-result) once per dialect
        //    instantiation of this template.
        REQUIRE_NOTHROW((void)json::parse(d.args));
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

// ── StreamScaffold: the shared per-turn handler contract ─────────────────
//
// Every transport now builds its StreamHandler from provider::StreamScaffold,
// so these invariants hold for ALL of them by construction. Pinned here so a
// transport that regresses to a hand-rolled handler (or a scaffold edit that
// weakens the contract) fails this suite, not a user in the field.
TEST_CASE("conformance: scaffold caps the error body at exactly 64 KB") {
    provider::StreamScaffold sc;
    sc.dialect = "test";
    sc.sink = [](Msg) {};
    sc.feed = [](std::string_view) { return true; };
    auto h = sc.handler();
    h.on_headers(500, {});
    CHECK(!sc.ok());
    // Feed 3 chunks of 32 KB — only the first two fit under the cap.
    const std::string chunk(32 * 1024, 'x');
    CHECK(h.on_chunk(chunk));   // error path keeps draining
    CHECK(h.on_chunk(chunk));
    CHECK(h.on_chunk(chunk));
    CHECK(sc.error_body.size() == provider::kErrorBodyCap);
}

TEST_CASE("conformance: scaffold routes success chunks to feed, error to body") {
    provider::StreamScaffold sc;
    sc.dialect = "test";
    sc.sink = [](Msg) {};
    std::string fed;
    sc.feed = [&](std::string_view c) { fed += c; return true; };

    auto h = sc.handler();
    h.on_headers(200, {});
    CHECK(sc.ok());
    CHECK(h.on_chunk("data: hello\n\n"));
    CHECK(fed == "data: hello\n\n");
    CHECK(sc.error_body.empty());

    // feed's return value propagates (the Responses codec's deliberate
    // stop-after-terminal read abort).
    sc.feed = [&](std::string_view) { return false; };
    CHECK(!h.on_chunk("more"));
}

TEST_CASE("conformance: scaffold forwards liveness as transport-only events") {
    provider::StreamScaffold sc;
    sc.dialect = "test";
    int heartbeats = 0, buffered = 0;
    sc.sink = [&](Msg m) {
        if (leaf<StreamHeartbeat>(m))    ++heartbeats;
        if (leaf<StreamBufferedWait>(m)) ++buffered;
    };
    sc.feed = [](std::string_view) { return true; };
    auto h = sc.handler();
    // EVERY transport must wire both callbacks — Ollama shipped without
    // on_buffered_wait for months (buffered sends showed as dead air).
    REQUIRE(h.on_activity);
    REQUIRE(h.on_buffered_wait);
    h.on_activity();
    h.on_buffered_wait();
    CHECK(heartbeats == 1);
    CHECK(buffered == 1);
}

TEST_CASE("conformance: shared streaming timeout ladder") {
    // The ladder is a contract with the reducer's stall watchdog and the
    // retry classifier; a silent change here shifts user-visible behaviour
    // on every provider at once.
    const auto tos = provider::stream_timeouts();
    CHECK(tos.connect == std::chrono::milliseconds(10'000));
    CHECK(tos.total   == std::chrono::minutes(30));
    CHECK(tos.ping    == std::chrono::milliseconds(15'000));
    CHECK(tos.idle    == std::chrono::milliseconds(90'000));
    // The one legitimate knob: local servers get a longer idle.
    CHECK(provider::stream_timeouts(std::chrono::milliseconds(600'000)).idle
          == std::chrono::milliseconds(600'000));
}

// ── The reasoning toggle, asserted against EVERY dialect ────────────────
//
// This suite existed and still missed a bug: reasoning arrived on Copilot's
// gpt-* models with the toggle OFF, because the Responses codec never read
// Request::show_reasoning. Three transports honoured it, one didn't, and
// nothing cross-checked — the contract only covered tool calls.
//
// That is the general shape: a Request field is a PROMISE to the user, and
// a promise kept by three of four implementations is worse than one kept by
// none, because it looks arbitrary. So the axis gets the same treatment tool
// calls got — state it once, instantiate per dialect.
//
// Two legitimate strategies, and the contract accepts both:
//
//   SUPPRESS AT SOURCE   Anthropic asks the server not to send it
//                        (`thinking.display`), so nothing arrives to filter.
//   FILTER AT DECODE     Chat/Ollama/Responses receive it either way and
//                        drop the text on the way to the reducer.
//
// What both must produce is the same OBSERVABLE: no reasoning text reaches
// the reducer when the user hid it. How a dialect gets there is its own
// business; that it gets there is not.
namespace reasoning {

// Reasoning text that survived to the reducer, and whether ANY liveness
// signal still fired.
//
// Liveness is counted across BOTH carriers on purpose. A dialect may keep
// the turn alive with an empty-text ThinkingDelta (chat, responses) or with
// a StreamHeartbeat (ollama); the reducer resets last_event_at on either, so
// both satisfy the requirement. Asserting one spelling would be pinning a
// mechanism instead of the guarantee — and would have failed a dialect that
// is behaving correctly, which is how a contract loses its authority.
struct Seen {
    std::string text;
    int         liveness = 0;
};

Seen collect(const std::vector<Msg>& msgs) {
    Seen s;
    for (const auto& m : msgs) {
        if (auto* t = leaf<StreamThinkingDelta>(m)) { s.text += t->text; ++s.liveness; }
        if (leaf<StreamHeartbeat>(m))               { ++s.liveness; }
    }
    return s;
}

// Each dialect frames the same turn: a reasoning burst, then an answer.
Seen chat(bool show) {
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"pondering\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    return collect(provider::openai::parse_sse_for_test(
        sse, {}, false, false, show));
}

Seen responses(bool show) {
    const std::vector<std::string> sse{
        R"({"type":"response.reasoning_summary_text.delta","delta":"pondering"})",
        R"({"type":"response.output_text.delta","delta":"answer"})",
        R"({"type":"response.completed","response":{"usage":{}}})"};
    return collect(provider::chatgpt::parse_sse_for_test(sse, show));
}

Seen ollama(bool show) {
    const std::string nd =
        R"({"message":{"role":"assistant","thinking":"pondering"},"done":false})" "\n"
        R"({"message":{"role":"assistant","content":"answer"},"done":true})" "\n";
    return collect(provider::ollama::parse_ndjson_for_test(
        nd, {}, false, false, show));
}

}  // namespace reasoning

TEST_CASE("conformance: hiding reasoning hides it on every dialect") {
    struct Case { const char* id; reasoning::Seen (*run)(bool); };
    for (const Case c : {Case{"openai-chat",      &reasoning::chat},
                         Case{"openai-responses", &reasoning::responses},
                         Case{"ollama-native",    &reasoning::ollama}}) {
        INFO("dialect = " << std::string{c.id});

        // Shown: the text reaches the reducer.
        const auto on = c.run(true);
        CHECK(on.text.find("pondering") != std::string::npos);

        // Hidden: no reasoning TEXT survives…
        const auto off = c.run(false);
        CHECK(off.text.empty());

        // …but SOME liveness signal still fires. Swallowing the event
        // entirely makes a long silent reasoning phase look like a dead
        // stream, and the retry watchdog acts on that.
        CHECK(off.liveness > 0);
    }
}

TEST_CASE("conformance: hiding reasoning never disturbs the answer") {
    // The regression guard. Suppression must touch exactly one channel.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"think\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    std::string text;
    for (const auto& m : provider::openai::parse_sse_for_test(
             sse, {}, false, false, /*show_reasoning=*/false))
        if (auto* t = leaf<StreamTextDelta>(m)) text += t->text;
    CHECK(text == "hello world");
}
