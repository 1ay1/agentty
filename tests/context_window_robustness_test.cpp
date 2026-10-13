// tests/context_window_robustness_test.cpp — a window a gateway lied about.
//
// The context window drives the gauge AND auto-compaction, so a wrong number
// is not a cosmetic bug: a NEGATIVE window disables compaction entirely
// (every `used < window` test is false) and a window of 1 compacts on every
// turn. Both read to a user as "agentty is broken" rather than "this gateway
// reported nonsense".
//
// There is no spec for this field (OpenAI's /v1/models carries no window at
// all — see conformance.hpp), so every value arrives from a third party with
// no contract. The extraction has to survive all of it.
//
// MEASURED FAILURES of the previous narrowing implementation, which took
// `v.get<int>()` / `static_cast<int>(uint64)` / `std::stoi` directly:
//
//   18446744073709551615  ->        -1     uint64 max from a buggy proxy
//   4000000000            -> -294967296    a real number, just > INT_MAX
//   1e18                  -> -2147483648   float narrowing, UB
//   "1e6"                 ->         1     stoi stops at 'e'
//   "128000abc"           ->    128000     partial parse of a junk string
//
// GitHub Copilot's runtime keeps max_context_window_tokens / max_prompt_tokens
// / max_output_tokens as separate validated limits for the same reason: one
// unlabelled, unvalidated number is the thing you cannot debug later.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "agentty/provider/openai/transport.hpp"

using json = nlohmann::json;
namespace det = agentty::provider::openai;

TEST_CASE("window: hostile numeric shapes yield 0, never garbage") {
    // Every one of these produced a wrong-but-plausible int before.
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":18446744073709551615})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":4000000000})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":1e18})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":-1})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":0})")) == 0);

    // 0 means "nothing advertised", which the caller turns into a default.
    // That is the ONLY safe failure: a default is visibly a default, whereas
    // -294967296 is silently catastrophic.
}

TEST_CASE("window: a partial string parse is a misread, not a lenient read") {
    // Some proxies stringify numeric metadata, so strings must be read — but
    // only when the WHOLE string is the number.
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":"131072"})")) == 131072);

    // "1e6" used to become 1 — a window of one token, compacting every turn.
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":"1e6"})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":"128000abc"})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":"not a number"})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":""})")) == 0);
}

TEST_CASE("window: implausible magnitudes are rejected at both ends") {
    // Below 1k is an OUTPUT cap or an artifact, not an agent context window.
    // Reading one as the window is the failure mode the ladder's comment
    // calls out: "a plausible wrong number rather than a visible failure".
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":512})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":1})")) == 0);

    // The band's edges themselves are accepted.
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":1024})")) == 1024);

    // Above 100M is not a 2026 deployment (largest shipped ~10M).
    CHECK(det::advertised_context_window(
              json::parse(R"({"context_length":999000000})")) == 0);
}

TEST_CASE("window: the real shapes still work") {
    // CAPTURED LIVE from https://yolo-auto.com/v1/models, 2026-09-23. vLLM
    // carries both keys; verified honoured with a 125,040-token prompt.
    CHECK(det::advertised_context_window(json::parse(
              R"({"id":"qwen3.8-flash","context_length":131072,
                  "max_model_len":131072})")) == 131072);

    // OpenRouter nests the DEPLOYMENT's window, which wins over any flat key
    // because it is what that specific route will actually accept.
    CHECK(det::advertised_context_window(json::parse(
              R"({"top_provider":{"context_length":200000},
                  "context_length":128000})")) == 200000);

    // LiteLLM's model_info block.
    CHECK(det::advertised_context_window(json::parse(
              R"({"model_info":{"max_input_tokens":1000000}})")) == 1000000);

    // llama.cpp's per-model meta: n_ctx is the SERVED window (what -c was
    // set to), n_ctx_train the train-time max. Served wins — it is the one
    // that bounds this request. This is issue #49: without it an 8k local
    // model claimed 200k and compaction never fired.
    CHECK(det::advertised_context_window(json::parse(
              R"({"meta":{"n_ctx":8192,"n_ctx_train":32768}})")) == 8192);

    // GGUF arch-prefixed keys, which is how a model's own metadata spells it.
    CHECK(det::advertised_context_window(json::parse(
              R"({"qwen2.context_length":32768})")) == 32768);

    // Stringified, as several proxies emit.
    CHECK(det::advertised_context_window(json::parse(
              R"({"max_model_len":"65536"})")) == 65536);
}

TEST_CASE("window: OUTPUT caps are never read as the window") {
    // max_tokens / max_output_tokens / max_completion_tokens are an order of
    // magnitude smaller. Reading one as the window makes the agent "forget"
    // constantly on a model with a huge window — and it looks plausible, so
    // nobody suspects the number.
    CHECK(det::advertised_context_window(
              json::parse(R"({"max_tokens":4096})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"max_output_tokens":8192})")) == 0);
    CHECK(det::advertised_context_window(
              json::parse(R"({"max_completion_tokens":16384})")) == 0);

    // ...but model_info.max_tokens IS a window on LiteLLM, which is why the
    // ladder reads it there and only there.
    CHECK(det::advertised_context_window(json::parse(
              R"({"model_info":{"max_tokens":128000}})")) == 128000);
}

TEST_CASE("window: a row that says nothing returns 0") {
    // OpenAI's own /v1/models shape. Absence must be representable so the
    // caller can apply a default knowingly rather than inventing a number.
    CHECK(det::advertised_context_window(json::parse(
              R"({"id":"gpt-4o","object":"model","owned_by":"openai"})")) == 0);
    CHECK(det::advertised_context_window(json::parse(R"({})")) == 0);
    CHECK(det::advertised_context_window(json::parse(R"([])")) == 0);
    CHECK(det::advertised_context_window(json::parse(R"(null)")) == 0);
}
