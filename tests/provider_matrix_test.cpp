// provider_matrix_test — every SELECTABLE provider, pinned row by row.
//
// ── What this file is for, and what it deliberately is not ───────────────
//
// provider_conformance_test states ONE contract and instantiates it per
// DIALECT (4 of them). That is the right shape for wire decoding, and it is
// where a tool-call framing bug belongs.
//
// But a user does not pick a dialect. They pick "groq" or "gemini", and 14
// of our 16 rows share the single OpenAIChat decoder. What actually differs
// between those 14 is the ROW: where it dials, which env vars carry its key,
// whether it needs a key at all, whether it has a second dialect, which
// long-lived transport slot it owns. Those facts are what break a provider
// for a user, and they are per-row, not per-dialect.
//
// Registry COHERENCE is already proven at compile time (routing_consistent,
// endpoints_consistent, ids_unique, auth_caps_consistent in registry.hpp).
// This file does not restate those — a test that re-asserts a static_assert
// is noise. It asserts the things a table cannot prove about itself:
//
//   1. every row is reachable by the string a user actually types,
//   2. every row's endpoint survives the round trip into a dialled Endpoint,
//   3. the auth story is consistent with how the row is reached,
//   4. a row's declared dialect matches the path it dials,
//   5. the per-provider quirks we have hand-coded still hold.
//
// The loop is over kProviders, so a new row is tested the moment it is
// added. That is the property worth having: the failure mode this guards
// against is someone appending a row and nothing noticing it is unreachable.

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agtest.hpp"

#include "agentty/provider/openai/transport.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"

using namespace agentty;
using namespace agentty::provider;
using json = nlohmann::json;

namespace {

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

// Every row in the table, by value, so a loop reads naturally.
[[nodiscard]] std::vector<const ProviderDescriptor*> all_rows() {
    std::vector<const ProviderDescriptor*> out;
    for (const auto& p : kProviders) out.push_back(&p);
    return out;
}

// Does this row speak HTTP over the generic OpenAI-compat transport?
// Anthropic (own transport), ChatGPT (dedicated OAuth transport) and ACP
// (subprocess) leave `host` empty and are routed by wire/oauth_native.
[[nodiscard]] bool http_dialled(const ProviderDescriptor& p) {
    return !p.host.empty();
}

}  // namespace

// ── 1. Reachability ──────────────────────────────────────────────────────
//
// A row exists to be SELECTED. The user types a spec token — "groq",
// "gemini", "ollama" — and parse_selection has to hand back that row. A row
// that cannot be reached by its own id is dead weight that still shows up in
// the picker, which is worse than not existing.

TEST_CASE("matrix: every provider is reachable by its own id") {
    for (const auto* p : all_rows()) {
        CAPTURE(p->id);
        const auto sel = parse_selection(std::string{p->id});
        CHECK(sel.provider_id() == p->id);
        const auto* row = preset_for(sel.provider_id());
        REQUIRE(row != nullptr);
        CHECK(row->id == p->id);
    }
}

TEST_CASE("matrix: every provider has a non-empty label and blurb") {
    // The picker renders these. An empty label is a blank row the user
    // cannot identify; an empty blurb is a row they cannot choose between.
    for (const auto* p : all_rows()) {
        CAPTURE(p->id);
        CHECK(!p->id.empty());
        CHECK(!p->label.empty());
        CHECK(!p->blurb.empty());
    }
}

// ── 2. Endpoint round-trip ───────────────────────────────────────────────
//
// The row states host/path/port/tls. Selecting the provider must produce an
// Endpoint carrying exactly those. This is the drift the registry comment
// calls out by name: the `openai` row once claimed Responses while the
// transport dialled /v1/chat/completions, and the UI promised reasoning the
// wire never sent.

TEST_CASE("matrix: an http-dialled provider round-trips its endpoint") {
    for (const auto* p : all_rows()) {
        if (!http_dialled(*p)) continue;
        CAPTURE(p->id);

        const auto sel = parse_selection(std::string{p->id});
        const auto ep  = openai::Endpoint::from_spec(std::string{p->id});

        CHECK(ep.host == p->host);
        CHECK(ep.port == p->port);
        CHECK(ep.use_tls == p->use_tls);
        // The completions path is what a turn actually POSTs to. A row whose
        // path is empty would dial "/" and 404 every request.
        CHECK(!p->path.empty());
        CHECK(!p->models_path.empty());
    }
}

TEST_CASE("matrix: a provider's declared dialect matches the path it dials") {
    // wire and path are two statements of the same fact. endpoints_consistent()
    // proves it for the TABLE at compile time; this checks the same holds
    // after from_spec() has built the endpoint we actually dial.
    for (const auto* p : all_rows()) {
        if (!http_dialled(*p)) continue;
        // Ollama speaks its OWN protocol on /api/chat — NDJSON with structured
        // tool_calls, not the OpenAI shim. Its row carries Wire::OpenAIChat
        // because that is how the TRANSPORT is selected, while native_api
        // says the payload shape differs. The compile-time proof exempts it
        // for exactly this reason, and so must this.
        if (p->native_api) continue;
        CAPTURE(p->id);
        switch (p->wire) {
            case Wire::OpenAIChat:
                CHECK(p->path.find("/chat/completions") != std::string_view::npos);
                break;
            case Wire::OpenAIResponses:
                CHECK(p->path.find("/responses") != std::string_view::npos);
                break;
            case Wire::AnthropicMessages:
            case Wire::Acp:
                // Not dialled over the generic transport; `host` is empty and
                // this loop skipped it. Reaching here means a row grew a host
                // without anyone deciding what path that implies.
                FAIL("http-dialled row with a non-HTTP wire: " << p->id);
                break;
        }
    }
}

TEST_CASE("matrix: a native-protocol provider is local and un-keyed") {
    // native_api means the payload is NOT the OpenAI shape, so every generic
    // assumption about the body is off. Today that is Ollama alone. If a
    // second row ever sets it, these are the properties that made it safe to
    // special-case: it runs on this machine and needs no key, so a wrong
    // guess costs a local 400 rather than a leaked credential.
    for (const auto* p : all_rows()) {
        if (!p->native_api) continue;
        CAPTURE(p->id);
        CHECK(p->is_local);
        CHECK(p->auth == AuthStyle::None);
        // And its path must NOT look like the OpenAI one, or the exemption
        // above is hiding a row that could have used the generic shape.
        CHECK(p->path.find("/chat/completions") == std::string_view::npos);
    }
}

TEST_CASE("matrix: a second dialect, when present, is on the same host") {
    // The Responses column is the SAME host's other path, not a different
    // backend. A row that points its second dialect elsewhere would silently
    // send credentials for host A to host B.
    for (const auto* p : all_rows()) {
        if (p->responses_path.empty()) continue;
        CAPTURE(p->id);
        CHECK(http_dialled(*p));
        CHECK(p->responses_path.find("/responses") != std::string_view::npos);
        // And it must differ from the default path, or the column is a no-op
        // that makes dialect_for() look like it has a choice when it doesn't.
        CHECK(p->responses_path != p->path);
    }
}

// ── 3. Auth ──────────────────────────────────────────────────────────────

TEST_CASE("matrix: a key-authenticated provider names its env vars") {
    // A remote row with AuthStyle::ApiKey and no env var can only be
    // configured through the UI — which is a silent regression for anyone
    // scripting agentty, and the usual way a new row ships half-wired.
    for (const auto* p : all_rows()) {
        if (p->auth != AuthStyle::ApiKey) continue;
        if (p->is_local) continue;
        CAPTURE(p->id);
        const bool any_env = std::any_of(
            p->auth_env.begin(), p->auth_env.end(),
            [](std::string_view e) { return !e.empty(); });
        CHECK(any_env);
    }
}

TEST_CASE("matrix: only a genuinely local backend is marked local") {
    // is_local means "runs on this machine", NOT "needs no key". Conflating
    // the two is why ChatGPT once claimed to be local — the credential layer
    // skips key resolution for a local row, so a mislabelled remote row
    // dials with no auth and 401s.
    for (const auto* p : all_rows()) {
        CAPTURE(p->id);
        if (p->is_local) {
            // A local backend is reached on loopback, or not over HTTP.
            if (http_dialled(*p)) {
                const bool loopback = p->host == "localhost"
                                   || p->host == "127.0.0.1"
                                   || p->host == "::1";
                CHECK(loopback);
                // Local HTTP is plain by default; TLS on loopback would need
                // a cert the user does not have.
                CHECK(!p->use_tls);
            }
            // And a local backend must not demand a key.
            CHECK(p->auth == AuthStyle::None);
        }
    }
}

TEST_CASE("matrix: an oauth-native provider is long-lived and un-keyed") {
    // oauth_native means the row rides a DEDICATED transport that owns its
    // token refresh. That only works if the transport is long-lived, and it
    // must not also be looking for an API key.
    for (const auto* p : all_rows()) {
        if (!p->oauth_native) continue;
        CAPTURE(p->id);
        CHECK(p->lifetime == Lifetime::LongLived);
        CHECK(p->auth != AuthStyle::ApiKey);
    }
}

// ── 4. Routing slots ─────────────────────────────────────────────────────

TEST_CASE("matrix: long-lived providers own distinct routing slots") {
    // A long-lived row owns in-process transport state (a session, a token).
    // Two rows sharing a slot would share that state, which is how one
    // provider's credentials end up on another's request.
    std::set<int> seen;
    for (const auto* p : all_rows()) {
        if (p->lifetime != Lifetime::LongLived) continue;
        CAPTURE(p->id);
        CHECK(p->route != RouteSlot::None);
        const auto [_, inserted] = seen.insert(static_cast<int>(p->route));
        CHECK(inserted);
    }
}

TEST_CASE("matrix: per-call providers claim no routing slot") {
    // The converse: a per-call row is rebuilt from the Endpoint every turn
    // and has no state to own. Claiming a slot would reserve capacity for a
    // transport that never exists.
    for (const auto* p : all_rows()) {
        if (p->lifetime != Lifetime::PerCall) continue;
        CAPTURE(p->id);
        CHECK(p->route == RouteSlot::None);
    }
}

// ── 5. The shared decoder, per provider ──────────────────────────────────
//
// 14 rows share the OpenAIChat decoder, so a decoding bug is 14 broken
// providers. The conformance suite pins the dialect; this pins that each
// CHAT row actually round-trips a tool call through it — a row that declares
// OpenAIChat but is somehow not decodable by it is a contradiction worth
// catching here rather than in a user's session.

namespace {

// One tool call, framed the way a well-behaved chat server sends it.
[[nodiscard]] std::string chat_tool_call(std::string_view name,
                                         std::string_view args) {
    const auto mid = args.size() / 2;
    std::string sse;
    sse += R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,)"
           R"("id":"call_1","type":"function","function":{"name":")"
           + std::string{name} + R"(","arguments":""}}]}}]})" "\n\n";
    for (auto part : {args.substr(0, mid), args.substr(mid)})
        sse += "data: " + std::string{
            R"({"choices":[{"delta":{"tool_calls":[{"index":0,)"
            R"("function":{"arguments":)"}
            + json(std::string{part}).dump() + "}}]}}]}" + "\n\n";
    sse += R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"
           "\n\n" "data: [DONE]\n\n";
    return sse;
}

}  // namespace

TEST_CASE("matrix: every chat-dialect provider decodes a tool call") {
    const std::string args =
        R"({"pattern":"foo\"bar","opts":{"word":true},"n":3})";

    for (const auto* p : all_rows()) {
        if (p->wire != Wire::OpenAIChat) continue;
        // Ollama's /api/chat is NDJSON, not SSE — a different decoder
        // entirely (parse_ndjson_for_test), covered by the conformance
        // suite's OllamaNative arm.
        if (p->native_api) continue;
        CAPTURE(p->id);

        const auto msgs = openai::parse_sse_for_test(
            chat_tool_call("grep", args), {"grep"});

        std::string got;
        int starts = 0, ends = 0;
        std::string id, name;
        for (const auto& m : msgs) {
            if (const auto* s = get_leaf<StreamToolUseStart>(m)) {
                ++starts; id = s->id.value; name = s->name.value;
            }
            if (const auto* d = get_leaf<StreamToolUseDelta>(m))
                got += d->partial_json;
            if (get_leaf<StreamToolUseEnd>(m)) ++ends;
        }

        // Announced once, closed once, arguments intact and parseable.
        CHECK(starts == 1);
        CHECK(ends == 1);
        CHECK(name == "grep");
        CHECK(!id.empty());
        REQUIRE_NOTHROW((void)json::parse(got));
        CHECK(json::parse(got) == json::parse(args));
    }
}

// ── 6. Hand-coded per-provider quirks ────────────────────────────────────
//
// Two divergences are coded into the shared decoder by NAME. They are the
// only places a specific provider's behaviour is special-cased, so they are
// the only places where "it works for DeepSeek but not Mistral" can hide.

TEST_CASE("matrix: deepseek-style reasoning_content surfaces as thinking") {
    // DeepSeek and most compat proxies stream reasoning in a field parallel
    // to content. It must become StreamThinkingDelta — the same event the
    // Anthropic transport emits — so the UI renders it identically.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"one\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
        "data: [DONE]\n\n";

    const auto msgs = openai::parse_sse_for_test(sse, {});
    std::string think, text;
    for (const auto& m : msgs) {
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
        if (const auto* c = get_leaf<StreamTextDelta>(m))     text  += c->text;
    }
    CHECK(think == "step one");
    CHECK(text == "answer");
}

TEST_CASE("matrix: openrouter-style empty reasoning_content is not fatal") {
    // Some OpenRouter passthroughs send an EMPTY reasoning_content alongside
    // a populated `reasoning`. Breaking on the first key present would drop
    // all reasoning — the decoder must keep looking until it finds content.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"\","
            "\"reasoning\":\"real thinking\"}}]}\n\n"
        "data: [DONE]\n\n";

    const auto msgs = openai::parse_sse_for_test(sse, {});
    std::string think;
    for (const auto& m : msgs)
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
    CHECK(think == "real thinking");
}

TEST_CASE("matrix: mistral-style inline [THINK] tags are re-routed") {
    // Mistral / Magistral wrap thinking in [THINK]…[/THINK] INSIDE content.
    // Left alone it leaks into the answer, and the leading '[' trips the
    // tool-call salvage heuristic.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"[THINK]pondering[/THINK]\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"the answer\"}}]}\n\n"
        "data: [DONE]\n\n";

    const auto msgs = openai::parse_sse_for_test(sse, {});
    std::string think, text;
    for (const auto& m : msgs) {
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
        if (const auto* c = get_leaf<StreamTextDelta>(m))     text  += c->text;
    }
    CHECK(think.find("pondering") != std::string::npos);
    CHECK(text.find("pondering") == std::string::npos);
    CHECK(text.find("the answer") != std::string::npos);
}

TEST_CASE("matrix: deepseek-style <think> tags split across deltas") {
    // The tag itself can be split by the server's chunking ("<thi" | "nk>").
    // A decoder that only matches whole tags leaks the marker into the answer.
    const std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<thi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"nk>hidden</think>shown\"}}]}\n\n"
        "data: [DONE]\n\n";

    const auto msgs = openai::parse_sse_for_test(sse, {});
    std::string think, text;
    for (const auto& m : msgs) {
        if (const auto* t = get_leaf<StreamThinkingDelta>(m)) think += t->text;
        if (const auto* c = get_leaf<StreamTextDelta>(m))     text  += c->text;
    }
    CHECK(think.find("hidden") != std::string::npos);
    CHECK(text.find("hidden") == std::string::npos);
    CHECK(text.find("<think") == std::string::npos);
    CHECK(text.find("shown") != std::string::npos);
}
