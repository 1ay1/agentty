// context_window_test — how a model's context window is resolved.
//
// The window drives the ctx-% gauge, the auto-compaction trigger and the
// compaction slice ceiling, and the two failure modes are NOT symmetric:
// guessing too high lets the prefix run past what the endpoint accepts and
// the turn dies on the wire, guessing too low only compacts earlier than it
// had to. So the resolution order matters, and it is pinned here.
//
// The bug these were written for: a user running 1M-token models behind
// LiteLLM saw agentty cap them at 200k with no way to change it. TWO causes,
// both covered below — the /v1/models parser ignored every field a gateway
// could advertise a window in, and there was no user override at all.

#include "agtest.hpp"

#include "agentty/domain/catalog.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/store/store.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using agentty::store::Settings;
namespace ui = agentty::ui;

// The parser under test lives in the transport TU; it is re-declared here
// rather than exported, because it is an implementation detail of
// list_models and the test is the only other caller.
namespace agentty::provider::openai::detail {
[[nodiscard]] int advertised_context_window(const nlohmann::json& m);
}
namespace det = agentty::provider::openai::detail;

TEST_CASE("context: a bare OpenAI /v1/models row advertises nothing") {
    // OpenAI's own schema is {id, object, created, owned_by} — no window.
    // 0 means "nobody said", which must stay distinguishable from "200k".
    const auto row = json::parse(R"({
        "id": "gpt-4o", "object": "model",
        "created": 1234567890, "owned_by": "openai"
    })");
    CHECK(det::advertised_context_window(row) == 0);
}

TEST_CASE("context: LiteLLM model_info is read") {
    // LiteLLM passes a config's `model_info:` block straight through.
    const auto row = json::parse(R"({
        "id": "my-1m-model",
        "model_info": {"max_input_tokens": 1000000, "max_output_tokens": 8192}
    })");
    CHECK(det::advertised_context_window(row) == 1000000);

    // max_tokens is the older spelling and a valid fallback.
    const auto older = json::parse(R"({
        "id": "x", "model_info": {"max_tokens": 128000}
    })");
    CHECK(det::advertised_context_window(older) == 128000);
}

TEST_CASE("context: OpenRouter's per-deployment window wins") {
    // A row carries the model's catalog window AND the window of the
    // endpoint that will actually serve it. They disagree routinely (a
    // provider may serve a 1M model at 128k), and the SERVING one is the
    // one that will reject an over-long prompt.
    const auto row = json::parse(R"({
        "id": "anthropic/claude-3.5-sonnet",
        "context_length": 1000000,
        "top_provider": {"context_length": 128000}
    })");
    CHECK(det::advertised_context_window(row) == 128000,
          "the serving endpoint's window outranks the catalog figure");
}

TEST_CASE("context: vLLM max_model_len is read") {
    const auto row = json::parse(R"({"id": "m", "max_model_len": 32768})");
    CHECK(det::advertised_context_window(row) == 32768);
}

TEST_CASE("context: a stringified window still parses") {
    // Some proxies stringify numeric metadata. Dropping those would silently
    // clamp the model, which is the whole failure this guards.
    const auto row = json::parse(R"({"id": "m", "context_length": "262144"})");
    CHECK(det::advertised_context_window(row) == 262144);
}

TEST_CASE("context: garbage never throws or yields a bogus window") {
    for (const char* src : {R"({"id":"m","context_length":null})",
                            R"({"id":"m","context_length":"not a number"})",
                            R"({"id":"m","model_info":"not an object"})",
                            R"({"id":"m","context_length":-5})",
                            R"({"id":"m"})"}) {
        const auto row = json::parse(src);
        CHECK(det::advertised_context_window(row) == 0, src);
    }
}

TEST_CASE("context: resolution order is override > advertised > id") {
    Settings s;
    const char* prov = "litellm";
    const char* model = "my-model";

    // 3. Nothing known anywhere → the conservative default, never a guess.
    CHECK(ui::resolve_context_window(prov, model, 0, s)
              == ui::kDefaultContextWindow);

    // 2. What the gateway advertised beats an id guess.
    CHECK(ui::resolve_context_window(prov, model, 1'000'000, s) == 1'000'000);

    // A KNOWN id still loses to a live figure from the endpoint that will
    // serve the request — the same name behind two gateways can differ.
    CHECK(ui::resolve_context_window(prov, "claude-sonnet-4-5", 400'000, s)
              == 400'000,
          "advertised outranks id inference");

    // 1. The user's override beats everything. They configured the gateway.
    s.context_overrides[ui::context_override_key(prov, model)] = 2'000'000;
    CHECK(ui::resolve_context_window(prov, model, 1'000'000, s) == 2'000'000,
          "an override a heuristic can overrule is not a setting");
}

TEST_CASE("context: overrides are per provider AND model") {
    // The same model id behind two gateways can be served with two different
    // windows, so a bare model id is not a sufficient key.
    Settings s;
    s.context_overrides[ui::context_override_key("litellm", "shared")] = 1'000'000;

    CHECK(ui::resolve_context_window("litellm", "shared", 0, s) == 1'000'000);
    CHECK(ui::resolve_context_window("other", "shared", 0, s)
              == ui::kDefaultContextWindow,
          "an override on one provider must not leak to another");
}

TEST_CASE("context: a known family still resolves without a catalog") {
    Settings s;
    // Claude ids carry their window in the id; no advertisement needed.
    CHECK(ui::resolve_context_window("anthropic", "claude-opus-4-5", 0, s)
              == 200'000);
    // The [1m] suffix forces the wide window.
    CHECK(ui::resolve_context_window("anthropic", "claude-sonnet-4-5[1m]", 0, s)
              == 1'000'000);
}

TEST_CASE("context: labels read the way the picker prints them") {
    CHECK(ui::context_window_label(0)         == "auto");
    CHECK(ui::context_window_label(128'000)   == "128k");
    CHECK(ui::context_window_label(200'000)   == "200k");
    CHECK(ui::context_window_label(1'000'000) == "1M");
    CHECK(ui::context_window_label(1'048'576) == "1M+");
}

TEST_CASE("context: the override ladder starts at auto and covers the range") {
    // "auto" must be rung 0 so one full lap of ^W always returns to
    // "let agentty decide" — the control clears itself.
    REQUIRE(std::size(ui::kContextLadder) > 1);
    CHECK(ui::kContextLadder[0] == 0, "auto is the first rung");
    // Strictly ascending, so stepping is monotonic in both directions.
    for (std::size_t i = 1; i < std::size(ui::kContextLadder); ++i)
        CHECK(ui::kContextLadder[i] > ui::kContextLadder[i - 1],
              "the ladder ascends");
    // Covers what the reporter actually needed.
    bool has_1m = false;
    for (int w : ui::kContextLadder) if (w == 1'000'000) has_1m = true;
    CHECK(has_1m, "1M is reachable — the window this feature was asked for");
}
