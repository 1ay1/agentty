// Context-window resolution for gateways — the "my 1M models show 200k" bug.
//
// Reported by a LiteLLM user: models behind the proxy support ~1M, agentty
// showed 200k, and they asked for a setting to raise it. There already IS
// one (^W in the model picker cycles to 1M/2M and persists), so the report
// is really two failures:
//
//   1. agentty didn't KNOW the window — the gateway's /v1/models rows carry
//      no size, so every model fell to kDefaultContextWindow.
//   2. the UI didn't SAY it didn't know — the ctx column was blank, which
//      reads as "not applicable" rather than "guessing", so ^W was never
//      found and 200k looked like a cap.
//
// This pins the parsing contract (1) shape by shape. The parse ladder is the
// single place a gateway's answer is recovered, so every dialect that
// carries a window must be recovered by it, and every shape that does NOT
// must resolve to 0 — "unknown" — rather than to a number nobody declared.
// Conflating those two is the whole bug: 0 is what makes the probe fire, the
// column say "auto", and the override matter.

#include "agtest.hpp"

#include "agentty/provider/openai/transport.hpp"

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

namespace {

// The REAL parser, not a copy. An earlier draft mirrored the ladder here so
// the test would not need an export — but a mirrored contract is not a
// contract: deleting the float branch in the transport left this file green
// while LiteLLM's 16385.0 silently became "unknown" in the product. The test
// has to call what ships.
int advertised(const json& m) {
    return agentty::provider::openai::advertised_context_window(m);
}

}  // namespace

TEST_CASE("context: every gateway dialect that declares a window is recovered") {
    // One case per real serving stack. Each is the shape that stack actually
    // emits on /v1/models, so a regression here is a regression against a
    // deployment somebody runs.

    // OpenRouter: per-DEPLOYMENT window, which outranks the model's generic
    // one — the same name behind two providers can be served at two sizes.
    CHECK(advertised(json::parse(
        R"({"id":"x","context_length":200000,
             "top_provider":{"context_length":1000000}})")) == 1'000'000);

    // vLLM: max_model_len is the runtime ceiling (--max-model-len).
    CHECK(advertised(json::parse(R"({"id":"x","max_model_len":262144})")) == 262'144);

    // LiteLLM: flat max_input_tokens, resolved from its own cost map.
    CHECK(advertised(json::parse(R"({"id":"x","max_input_tokens":1048576})")) == 1'048'576);

    // …and LiteLLM emits it as a FLOAT (observed: 16385.0). A strict integer
    // read drops the window silently and the model falls back to the default,
    // which is the failure mode this whole file exists for.
    CHECK(advertised(json::parse(R"({"id":"x","max_input_tokens":1048576.0})")) == 1'048'576);

    // Nested under model_info (LiteLLM /v1/model/info rows, some proxies).
    CHECK(advertised(json::parse(
        R"({"id":"x","model_info":{"max_input_tokens":200000}})")) == 200'000);

    // llama.cpp: n_ctx is what the server was STARTED with; n_ctx_train is
    // the architectural ceiling and is only a fallback.
    CHECK(advertised(json::parse(R"({"id":"x","n_ctx":32768})")) == 32'768);
    CHECK(advertised(json::parse(R"({"id":"x","n_ctx_train":131072})")) == 131'072);
    CHECK(advertised(json::parse(
        R"({"id":"x","n_ctx":8192,"n_ctx_train":131072})")) == 8'192);

    // Proxies that stringify numeric metadata still count as declarations.
    CHECK(advertised(json::parse(R"({"id":"x","context_length":"131072"})")) == 131'072);
}

TEST_CASE("context: a silent row is UNKNOWN (0), never a guess") {
    // The other half of the contract, and the more important one. These
    // shapes declare nothing, and must resolve to 0 so the caller can tell
    // "the gateway said 200k" from "nobody knows". Returning a default here
    // is exactly what made 200k look like a cap: the probe never fires, the
    // picker column shows a confident number, and ^W looks pointless.
    for (const char* body : {
            R"({"id":"x","object":"model"})",                    // bare OpenAI
            R"({"id":"qwen3:8b","object":"model","owned_by":"library"})", // ollama /v1
            R"({"id":"qwen/qwen3-coder-30b","object":"model"})", // LM Studio
            R"({"id":"x","object":"model","owned_by":"openai"})" // stock LiteLLM
        }) {
        CHECK(advertised(json::parse(body)) == 0);
    }

    // Garbage must not become a window either — a malformed declaration is
    // still "unknown", not a number.
    CHECK(advertised(json::parse(R"({"id":"x","context_length":null})")) == 0);
    CHECK(advertised(json::parse(R"({"id":"x","context_length":"many"})")) == 0);
    CHECK(advertised(json::parse(R"({"id":"x","model_info":"not-an-object"})")) == 0);
    CHECK(advertised(json::parse(R"({"id":"x","top_provider":42})")) == 0);
    // Zero and negative are declarations of nothing, not tiny windows.
    CHECK(advertised(json::parse(R"({"id":"x","max_model_len":0})")) == 0);
}

TEST_CASE("context: the deployment's own figure outranks the generic one") {
    // Precedence within the ladder. A gateway reports both when it proxies a
    // model whose upstream window differs from what this deployment serves —
    // and only the gateway knows which applies to the request we are about
    // to send, so the specific one wins.
    CHECK(advertised(json::parse(
        R"({"id":"x","context_length":8192,"max_model_len":262144})")) == 262'144);
    CHECK(advertised(json::parse(
        R"({"id":"x","max_input_tokens":8192,
             "top_provider":{"context_length":1000000}})")) == 1'000'000);
}
