// Tests for provider::dialect_for — the single authority that decides whether
// a (provider, model) turn goes out on /chat/completions or /responses.
//
// Why this file is worth its weight: the dialect choice is INVISIBLE to the
// user by design (no picker row, no flag), so a wrong answer never shows up
// as a bad setting — it shows up as a 400 mid-turn, or as a thinking pane
// that spins on a channel the wire will never fill. These cases pin the
// behaviour that has no UI to inspect it.

#include <doctest/doctest.h>

#include "agentty/provider/dialect.hpp"
#include "agentty/provider/openai/responses_site.hpp"
#include "agentty/provider/registry.hpp"

using namespace agentty::provider;

namespace {
// Each case starts from the model-family prior, never from another case's
// learned rejections (the observation store is process-global by design).
struct CleanSlate {
    CleanSlate()  { reset_dialect_observations(); }
    ~CleanSlate() { reset_dialect_observations(); }
};
} // namespace

TEST_CASE("dialect: hosts without a Responses endpoint stay on chat") {
    CleanSlate _;
    // These rows carry no responses_path. Even for a reasoning model the
    // answer must be Chat — they deliver reasoning over chat's
    // `reasoning_content`, and inventing a /responses URL for them 404s.
    CHECK(dialect_for("deepseek", "deepseek-reasoner") == Dialect::Chat);
    CHECK(dialect_for("mistral",  "magistral-medium")  == Dialect::Chat);
    CHECK(dialect_for("groq",     "gpt-5")             == Dialect::Chat);
    // A user's own OpenAI-compatible base URL: unknown capabilities, and a
    // wrong guess costs a failed turn on a host we cannot cheaply probe.
    CHECK(dialect_for("localhost:8080", "gpt-5") == Dialect::Chat);
}

TEST_CASE("dialect: non-OpenAI wires answer structurally") {
    CleanSlate _;
    CHECK(dialect_for("anthropic", "claude-opus-4-5") == Dialect::Native);
    CHECK(dialect_for("ollama",    "qwen3:8b")        == Dialect::Native);
    // A row whose DEFAULT is already Responses has no second dialect to pick
    // between — endpoints_consistent() forbids it carrying both.
    CHECK(dialect_for("chatgpt", "gpt-5-codex") == Dialect::Responses);
}

TEST_CASE("dialect: the GPT-5.4 tool-calling cutoff is a POINT release") {
    CleanSlate _;
    // Per OpenAI's reasoning guide: "Starting with GPT-5.4, Chat Completions
    // does not support tool calling with reasoning_effort values other than
    // none." agentty always sends tools, so 5.4+ is a HARD requirement —
    // while 5.0/5.1 remain legal on chat (they just can't show thinking).
    CHECK(!model_requires_responses("gpt-5"));
    CHECK(!model_requires_responses("gpt-5-mini"));
    CHECK(!model_requires_responses("gpt-5.1"));
    CHECK(model_requires_responses("gpt-5.4"));
    CHECK(model_requires_responses("gpt-5.4-mini"));
    CHECK(model_requires_responses("gpt-5.6-terra"));
    // GPT-6 / Astra drops chat function-calling entirely — no effort caveat.
    CHECK(model_requires_responses("gpt-6-astra"));
    // Copilot's mai-code-* 400s `unsupported_api_for_model` on chat.
    CHECK(model_requires_responses("mai-code-1.1-flash"));
    // Chat-only families must NEVER be dragged onto Responses: they 400
    // there. This is the regression that would break every Claude turn.
    CHECK(!model_requires_responses("claude-haiku-4.5"));
    CHECK(!model_requires_responses("gpt-4o"));
    CHECK(!model_requires_responses("gpt-4.1"));
}

TEST_CASE("dialect: slug shape does not change the family answer") {
    CleanSlate _;
    // Gateways namespace their slugs and casing varies; the family is the
    // part after the last slash. A prefix-only match would miss these and
    // silently route a GPT-6 turn onto a dialect that cannot carry it.
    CHECK(model_requires_responses("openai/gpt-6-astra"));
    CHECK(model_requires_responses("GPT-5.4"));
    CHECK(dialect_for("openrouter", "openai/gpt-5.4") == Dialect::Responses);
    // …but a namespaced chat-only model stays put.
    CHECK(dialect_for("openrouter", "anthropic/claude-haiku-4.5") == Dialect::Chat);
}

TEST_CASE("dialect: openai routes reasoning models, keeps gpt-4o on chat") {
    CleanSlate _;
    CHECK(dialect_for("openai", "gpt-4o")     == Dialect::Chat);
    CHECK(dialect_for("openai", "gpt-4.1")    == Dialect::Chat);
    CHECK(dialect_for("openai", "gpt-5")      == Dialect::Responses);
    CHECK(dialect_for("openai", "o3")         == Dialect::Responses);
    CHECK(dialect_for("openai", "gpt-6-astra")== Dialect::Responses);
    // Provider-level question (picker, before a model is chosen).
    CHECK(dialect_for("openai", "") == Dialect::Responses);
}

TEST_CASE("dialect: copilot keeps its measured per-model split") {
    CleanSlate _;
    // The behaviour Copilot's private table used to own, now answered by the
    // shared predicate. Measured against a live Auto session.
    CHECK(dialect_for("copilot", "gpt-5-mini")        == Dialect::Responses);
    CHECK(dialect_for("copilot", "mai-code-1.1-flash")== Dialect::Responses);
    CHECK(dialect_for("copilot", "claude-haiku-4.5")  == Dialect::Chat);
    CHECK(dialect_for("copilot", "gpt-4.1")           == Dialect::Chat);
}

TEST_CASE("dialect: a rejection from the wire overrides the prior") {
    CleanSlate _;
    // THE anti-rot mechanism. Every table here is a guess about model names,
    // and names move. A stale guess must self-correct from live evidence
    // rather than needing a release.
    REQUIRE(dialect_for("openai", "gpt-5") == Dialect::Responses);
    note_dialect_rejected("openai", "gpt-5", Dialect::Responses);
    CHECK(dialect_for("openai", "gpt-5") == Dialect::Chat);

    // Even a REQUIRED model must yield to a host that says no: a turn that
    // fails on chat with the model's own error beats one we know will 404.
    REQUIRE(dialect_for("openai", "gpt-6-astra") == Dialect::Responses);
    note_dialect_rejected("openai", "gpt-6-astra", Dialect::Responses);
    CHECK(dialect_for("openai", "gpt-6-astra") == Dialect::Chat);

    // Correction is scoped to ONE (provider, model): a single bad model must
    // not disable the dialect for its neighbours, or one 404 on a deprecated
    // slug would silently downgrade every reasoning turn on the account.
    CHECK(dialect_for("openai", "o3") == Dialect::Responses);
    CHECK(dialect_for("copilot", "gpt-5-mini") == Dialect::Responses);

    // …and the reverse direction promotes.
    note_dialect_rejected("openrouter", "some-new-reasoner", Dialect::Chat);
    CHECK(dialect_for("openrouter", "some-new-reasoner") == Dialect::Responses);
}

TEST_CASE("dialect: the UI predicate and the URL agree") {
    CleanSlate _;
    // The load-bearing invariant of this whole refactor. These were two
    // separate guesses once, and they drifted: the `openai` row claimed
    // Responses while dialling chat, so the thinking pane promised output
    // the wire never sent. Assert they are now ONE answer.
    const char* pairs[][2] = {
        {"openai",   "gpt-4o"},
        {"openai",   "gpt-5"},
        {"openai",   "gpt-5.4"},
        {"copilot",  "claude-haiku-4.5"},
        {"copilot",  "gpt-5-mini"},
        {"deepseek", "deepseek-reasoner"},
    };
    for (const auto& p : pairs) {
        const bool ui_says   = wire_streams_reasoning_text(p[0], p[1]);
        const bool on_resp   = dialect_for(p[0], p[1]) == Dialect::Responses;
        const bool summaries = model_emits_reasoning_summaries(p[1]);
        // If we route to Responses for a summary-emitting model, the UI must
        // offer ^R; if we route there for a model with no summaries, it must
        // NOT. Either way the UI cannot claim more than the wire carries.
        if (on_resp) CHECK(ui_says == summaries);
    }

    // A host demoted at runtime must also stop advertising thinking — the
    // UI following the URL is the entire point of the shared authority.
    REQUIRE(wire_streams_reasoning_text("openai", "gpt-5"));
    note_dialect_rejected("openai", "gpt-5", Dialect::Responses);
    CHECK(!wire_streams_reasoning_text("openai", "gpt-5"));
}

TEST_CASE("dialect: the Responses endpoint resolves from the row") {
    CleanSlate _;
    openai::ResponsesEndpoint ep;
    // Rows that advertise the second dialect hand back a complete destination.
    REQUIRE(openai::responses_endpoint_for("openai", ep));
    CHECK(ep.host == std::string{"api.openai.com"});
    CHECK(ep.path == std::string{"/v1/responses"});
    CHECK(ep.use_tls);

    REQUIRE(openai::responses_endpoint_for("openrouter", ep));
    CHECK(ep.path == std::string{"/api/v1/responses"});

    // Rows without the column must REFUSE rather than invent a URL — the
    // caller's contract is "false means stay on chat", and a fabricated
    // /responses on a host that has none is a guaranteed 404 mid-turn.
    CHECK(!openai::responses_endpoint_for("deepseek", ep));
    CHECK(!openai::responses_endpoint_for("groq", ep));
    // ChatGPT is Responses-native but rides its OWN OAuth transport and
    // carries no endpoint columns; it must not be reachable through here.
    CHECK(!openai::responses_endpoint_for("chatgpt", ep));
    // A custom host spec (from_spec puts the raw URL in `label`) matches no
    // row, so it stays on chat — we cannot know it speaks Responses.
    CHECK(!openai::responses_endpoint_for("https://gw.example.com/api", ep));
}

TEST_CASE("dialect: routing survives an endpoint that isn't there") {
    CleanSlate _;
    // The end-to-end anti-rot story, at the level the transport fork sees it:
    // a reasoning model routes to Responses, the host 404s, and the NEXT turn
    // must go out on chat by itself. Without this the user would sit in a
    // failure loop with no setting to change — the dialect has no UI.
    REQUIRE(dialect_for("openai", "gpt-5") == Dialect::Responses);
    note_dialect_rejected("openai", "gpt-5", Dialect::Responses);
    CHECK(dialect_for("openai", "gpt-5") == Dialect::Chat);
    // And the UI stops advertising thinking for it in the same instant, so
    // the pane never waits on a channel this turn will not carry.
    CHECK(!wire_streams_reasoning_text("openai", "gpt-5"));
}

TEST_CASE("dialect: every row advertising Responses is well-formed") {
    CleanSlate _;
    // endpoints_consistent() already static_asserts this at compile time;
    // restate it as a runtime case so a failure names the offending row
    // instead of collapsing into one opaque static_assert message.
    for (const auto& p : kProviders) {
        if (p.responses_path.empty()) continue;
        CAPTURE(p.id);
        CHECK(p.responses_path != p.path);
        CHECK(!p.host.empty());
        CHECK(p.wire != Wire::OpenAIResponses);   // would be two answers
        const auto tail = p.responses_path.substr(p.responses_path.size() - 10);
        CHECK(tail == std::string_view{"/responses"});
    }
}
