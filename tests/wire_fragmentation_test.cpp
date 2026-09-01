// wire_fragmentation_test — the tool-argument CONFORMANCE MATRIX.
//
// ── Why this test exists ─────────────────────────────────────────────────
//
// A streaming protocol may deliver a tool call's arguments as incremental
// fragments, as one authoritative snapshot, or as both. Every arm is
// spec-legal, and which one you get is a property of the SERVER, not the
// protocol: Codex streams fragments; GitHub Copilot's proxy coalesces and
// sends only the `.done` snapshot.
//
// The pre-existing codec test (codex_responses_test) encoded ONE server's
// choice — deltas — as though it were the protocol. It passed while Copilot
// was completely unusable: every tool call dispatched with `{}`, so `grep`
// failed "pattern required" and `read` failed "path required", while the
// zero-argument tools (repo_map, list, test) appeared to work. That asymmetry
// is what made it look like a flaky model instead of a dropped SSE event.
//
// So this file does not test a transcript. It enumerates the legal
// fragmentation space and asserts the ONE property that must hold across all
// of it:
//
//     however the server chose to frame the arguments,
//     the decoded arguments are identical.
//
// A codec that handles only some arms fails here by construction. Adding a
// new framing a provider is found to use = adding one enumerator.

#include <map>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/wire/streamed.hpp"

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

// ── The legal framing space ──────────────────────────────────────────────
//
// Each style is a way a conforming server may frame one tool call's
// arguments. None is more "correct" than another.
enum class Framing {
    DeltasOnly,      // Codex. Fragments, no terminal snapshot.
    DoneOnly,        // GitHub Copilot. Coalesced: the snapshot is the ONLY
                     // carrier. This is the arm that was silently dropped.
    DeltasAndDone,   // Both. The snapshot must not duplicate the fragments.
    InlineOnAdded,   // Whole args up-front on output_item.added.
    ItemDoneOnly,    // Only the completed item restates them.
};

static std::string name_of(Framing f) {
    switch (f) {
        case Framing::DeltasOnly:    return "DeltasOnly";
        case Framing::DoneOnly:      return "DoneOnly (Copilot)";
        case Framing::DeltasAndDone: return "DeltasAndDone";
        case Framing::InlineOnAdded: return "InlineOnAdded";
        case Framing::ItemDoneOnly:  return "ItemDoneOnly";
    }
    return "?";
}

// Build a spec-legal Responses SSE script delivering `args` under `style`.
static std::vector<std::string> synth(Framing style, const std::string& args) {
    const std::string esc = json(args).dump();  // args as a JSON string literal
    std::vector<std::string> sse;

    std::string added =
        R"({"type":"response.output_item.added","item":{"type":"function_call",)"
        R"("id":"fc_1","call_id":"call_9","name":"grep")";
    if (style == Framing::InlineOnAdded) added += R"(,"arguments":)" + esc;
    added += "}}";
    sse.push_back(added);

    const auto delta = [&](const std::string& chunk) {
        sse.push_back(
            R"({"type":"response.function_call_arguments.delta","item_id":"fc_1",)"
            R"("delta":)" + json(chunk).dump() + "}");
    };

    switch (style) {
        case Framing::DeltasOnly:
        case Framing::DeltasAndDone: {
            const auto mid = args.size() / 2;
            delta(args.substr(0, mid));
            delta(args.substr(mid));
            break;
        }
        case Framing::DoneOnly:
        case Framing::InlineOnAdded:
        case Framing::ItemDoneOnly:
            break;  // no fragments at all
    }

    // The terminal snapshot. Emitted by every server that sends `.done`.
    if (style == Framing::DoneOnly || style == Framing::DeltasAndDone) {
        sse.push_back(
            R"({"type":"response.function_call_arguments.done","item_id":"fc_1",)"
            R"("arguments":)" + esc + "}");
    }

    std::string done =
        R"({"type":"response.output_item.done","item":{"type":"function_call",)"
        R"("id":"fc_1")";
    if (style == Framing::ItemDoneOnly) done += R"(,"arguments":)" + esc;
    done += "}}";
    sse.push_back(done);
    sse.push_back(R"({"type":"response.completed","response":{"usage":{}}})");
    return sse;
}

// Reassemble what the reducer would see: the concatenated arg deltas.
static std::string decode_args(const std::vector<std::string>& sse) {
    std::string out;
    for (const auto& m : cc::parse_sse_for_test(sse))
        if (auto* d = leaf<StreamToolUseDelta>(m)) out += d->partial_json;
    return out;
}

TEST_CASE("tool arguments survive every legal SSE framing") {
    // A realistic payload: nested object, an escaped quote, a slash.
    const std::string args =
        R"({"pattern":"foo\"bar","glob":"src/**/*.cpp","opts":{"word":true}})";

    for (auto style : {Framing::DeltasOnly, Framing::DoneOnly,
                       Framing::DeltasAndDone, Framing::InlineOnAdded,
                       Framing::ItemDoneOnly}) {
        INFO("framing = " << name_of(style));
        const auto decoded = decode_args(synth(style, args));

        // The property: framing is invisible to the decoded result.
        CHECK(decoded == args);

        // And it must be PARSEABLE — a half-merged or double-appended stream
        // can equal neither, so this catches duplicate emission too.
        CHECK(json::parse(decoded)["pattern"] == "foo\"bar");
        CHECK(json::parse(decoded)["opts"]["word"] == true);
    }
}

TEST_CASE("a tool call emits exactly one end regardless of framing") {
    const std::string args = R"({"pattern":"x"})";
    for (auto style : {Framing::DeltasOnly, Framing::DoneOnly,
                       Framing::DeltasAndDone, Framing::InlineOnAdded}) {
        INFO("framing = " << name_of(style));
        std::map<std::string, int> ends;
        for (const auto& m : cc::parse_sse_for_test(synth(style, args)))
            if (auto* e = leaf<StreamToolUseEnd>(m)) ++ends[e->id.value];
        CHECK(ends["call_9"] == 1);
    }
}

// ── unseen()'s algebra ───────────────────────────────────────────────────

TEST_CASE("unseen: a snapshot after fragments adds nothing") {
    std::string have;
    CHECK(provider::wire::unseen(have, "{\"a\":", false) == "{\"a\":");
    CHECK(provider::wire::unseen(have, "1}", false) == "1}");
    CHECK(provider::wire::unseen(have, "{\"a\":1}", true).empty());
    CHECK(have == "{\"a\":1}");
}

TEST_CASE("unseen: a snapshot with no fragments yields all of it, once") {
    std::string have;
    CHECK(provider::wire::unseen(have, "{\"a\":1}", true) == "{\"a\":1}");
    CHECK(provider::wire::unseen(have, "{\"a\":1}", true).empty());
}

TEST_CASE("unseen: a snapshot extending fragments yields only the tail") {
    std::string have;
    CHECK(provider::wire::unseen(have, "{\"a\":", false) == "{\"a\":");
    CHECK(provider::wire::unseen(have, "{\"a\":1}", true) == "1}");
    CHECK(have == "{\"a\":1}");
}

TEST_CASE("unseen: a shorter snapshot never retracts emitted bytes") {
    std::string have;
    CHECK(provider::wire::unseen(have, "{\"a\":1}", false) == "{\"a\":1}");
    CHECK(provider::wire::unseen(have, "{\"a\":", true).empty());
    CHECK(have == "{\"a\":1}");
}

// ── The chat-completions dialect ─────────────────────────────────────────
//
// The same duality exists on /chat/completions: `delta.tool_calls[].function
// .arguments` normally carries FRAGMENTS, but a coalescing proxy may repeat
// the COMPLETE arguments in successive chunks. Blind concatenation turns that
// into `{"a":1}{"a":1}` — unparseable, and it fails as "invalid args" exactly
// like the Responses bug did.
#include "agentty/provider/openai/transport.hpp"

static std::string decode_chat_args(const std::vector<std::string>& chunks) {
    std::string bytes;
    for (const auto& c : chunks) bytes += "data: " + c + "\n\n";
    bytes += "data: [DONE]\n\n";
    std::string out;
    for (const auto& m : provider::openai::parse_sse_for_test(bytes, {"grep"}))
        if (auto* d = leaf<StreamToolUseDelta>(m)) out += d->partial_json;
    return out;
}

TEST_CASE("chat: coalesced tool arguments are not concatenated") {
    const std::string args = R"({"pattern":"x"})";
    const std::string esc  = json(args).dump();
    // A proxy that repeats the full arguments in every chunk.
    std::vector<std::string> sse = {
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
        R"("function":{"name":"grep","arguments":)" + esc + "}}]}}]}",
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
        R"("function":{"arguments":)" + esc + "}}]}}]}",
        R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})",
    };
    CHECK(decode_chat_args(sse) == args);
}

TEST_CASE("chat: genuine fragments still assemble") {
    std::vector<std::string> sse = {
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1",)"
        R"("function":{"name":"grep","arguments":"{\"pattern\":"}}]}}]})",
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
        R"("function":{"arguments":"\"x\"}"}}]}}]})",
        R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})",
    };
    CHECK(decode_chat_args(sse) == R"({"pattern":"x"})");
}
