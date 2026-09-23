// tests/openai_request_body_test.cpp — the bytes we actually send.
//
// conformance.hpp records WHICH fields go to which endpoints and why. Until
// the body builder was pulled out of the streaming function there was nothing
// checking that the code agreed with it: the only way to see a request was a
// wire=trace log on a live call, so "do we send prompt_cache_key to a local
// server?" was answered by reading a hot path.
//
// build_request_body() is pure — Request in, json out — so the answer is now
// an assertion. These tests are what turn the tiers from a comment into a
// contract.
//
// The asymmetry that sets every rule below: sending a field an endpoint
// rejects is a HARD 400/422 that kills the turn; omitting one costs an
// optimisation. So the tests care far more about what we DON'T send.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "agentty/provider/openai/transport.hpp"

using json = nlohmann::json;
namespace oa = agentty::provider::openai;

namespace {

// A hosted TLS endpoint (OpenAI, Groq, Yolo-Auto, …).
oa::Endpoint hosted() {
    return oa::Endpoint{"api.example.com", 443, "/v1/chat/completions",
                        "/v1/models", true, "example"};
}

// A local plaintext server (llama.cpp, LM Studio, vLLM on a laptop).
oa::Endpoint local() {
    oa::Endpoint e{"localhost", 8080, "/v1/chat/completions",
                   "/v1/models", false, "llama.cpp"};
    return e;
}

// A bare Ollama daemon speaking its own /api/chat protocol.
oa::Endpoint ollama_native() {
    oa::Endpoint e{"localhost", 11434, "/api/chat", "/api/tags", false, "ollama"};
    e.native_api = true;
    return e;
}

oa::Request base(oa::Endpoint ep) {
    oa::Request r;
    r.model         = "test-model";
    r.system_prompt = "you are a test";
    r.max_tokens    = 4096;
    r.endpoint      = std::move(ep);
    return r;
}

}  // namespace

// ═══ Universal tier: everything gets these ═══════════════════════════════

TEST_CASE("body: the universal fields go to every endpoint") {
    for (auto ep : {hosted(), local()}) {
        const json b = oa::build_request_body(base(ep));

        CHECK(b["model"] == "test-model");
        CHECK(b["stream"] == true);
        CHECK(b["max_tokens"] == 4096);
        // Ask for a final usage frame so the context gauge updates on a
        // streaming turn. Endpoints that don't know it IGNORE it rather than
        // erroring, so it is safe to send broadly (verified on Yolo-Auto).
        CHECK(b["stream_options"]["include_usage"] == true);
        // System prompt leads the conversation.
        REQUIRE(b["messages"].is_array());
        CHECK(b["messages"][0]["role"] == "system");
    }
}

TEST_CASE("body: max_tokens, NOT max_completion_tokens") {
    // THE DELIBERATE DIVERGENCE FROM OPENAI'S SPEC, and the one most likely
    // to be "fixed" by someone reading OpenAI's docs.
    //
    // OpenAI deprecated max_tokens on Chat Completions in favour of
    // max_completion_tokens. But vLLM, llama.cpp, Together and most of the
    // compat family accept ONLY max_tokens — so conforming to the document
    // breaks the majority of the ecosystem named after it. OpenAI still
    // accepts the old name; one spelling that works everywhere beats
    // conformance that works on one host.
    const json b = oa::build_request_body(base(hosted()));
    CHECK(b.contains("max_tokens"));
    CHECK(!b.contains("max_completion_tokens"));
}

TEST_CASE("body: the legacy tool shape stays dead") {
    auto req = base(hosted());
    req.tools.push_back({.name = "read_file",
                         .description = "read a file",
                         .input_schema = json::object()});
    const json b = oa::build_request_body(req);

    CHECK(b.contains("tools"));
    CHECK(b["tool_choice"] == "auto");
    // `functions` was superseded years ago; sending it would opt into the
    // legacy path on endpoints that still honour it, splitting tool handling.
    CHECK(!b.contains("functions"));
    CHECK(!b.contains("function_call"));
}

TEST_CASE("body: no tools means no tools key at all") {
    // An empty `tools: []` is not the same as absent — some servers reject
    // the empty array, and it costs prompt tokens on others.
    const json b = oa::build_request_body(base(hosted()));
    CHECK(!b.contains("tools"));
    CHECK(!b.contains("tool_choice"));
}

// ═══ Hosted tier: TLS only ═══════════════════════════════════════════════

TEST_CASE("body: prompt_cache_key is hosted-only") {
    // OpenAI auto-caches prefixes >=1024 tokens and a stable key pins a
    // conversation to one cache node. Local servers reject or ignore the
    // field and their KV cache is prefix-automatic anyway: nothing to gain,
    // a 400 to lose.
    auto h = base(hosted());
    h.session_key = "thread-abc";
    CHECK(oa::build_request_body(h)["prompt_cache_key"] == "thread-abc");

    auto l = base(local());
    l.session_key = "thread-abc";
    CHECK(!oa::build_request_body(l).contains("prompt_cache_key"));

    // No session key: no field, even on a hosted endpoint.
    CHECK(!oa::build_request_body(base(hosted())).contains("prompt_cache_key"));
}

// ═══ Probed tier: only with evidence ═════════════════════════════════════

TEST_CASE("body: reasoning_effort is gated, never assumed") {
    // Mistral MAGISTRAL reasons natively and returns 422 for this field, so
    // the catalog excludes it and `effort` arrives EMPTY here. The transport
    // does not re-derive capability — it trusts the gate and sends nothing.
    auto h = base(hosted());
    h.effort = "high";
    CHECK(oa::build_request_body(h)["reasoning_effort"] == "high");

    // Gated off upstream -> absent.
    auto gated = base(hosted());
    gated.effort = "";
    CHECK(!oa::build_request_body(gated).contains("reasoning_effort"));

    // Local servers reject it outright.
    auto l = base(local());
    l.effort = "high";
    CHECK(!oa::build_request_body(l).contains("reasoning_effort"));
}

// ═══ The native dialect is a different wire ══════════════════════════════

TEST_CASE("body: Ollama native speaks its own protocol") {
    // /api/chat is NOT OpenAI-shaped. Leaking OpenAI-only fields here is how
    // a local daemon starts 400ing on fields it never advertised.
    auto req = base(ollama_native());
    req.session_key = "thread-abc";
    req.effort      = "high";
    const json b = oa::build_request_body(req);

    // Output budget rides in options.num_predict — Ollama's default is ~128
    // tokens, which truncates an agent turn to nothing.
    CHECK(b["options"]["num_predict"] == 4096);
    CHECK(!b.contains("max_tokens"));

    // None of the OpenAI-family extras belong on this wire.
    CHECK(!b.contains("stream_options"));
    CHECK(!b.contains("prompt_cache_key"));
    CHECK(!b.contains("reasoning_effort"));

    // But the basics still hold.
    CHECK(b["model"] == "test-model");
    CHECK(b["stream"] == true);
    CHECK(b["messages"][0]["role"] == "system");
}

// ═══ Purity ══════════════════════════════════════════════════════════════

TEST_CASE("body: building is pure and repeatable") {
    // No globals, no I/O, no hidden state. If this ever fails, something in
    // the builder reached outside its argument — which is exactly what made
    // the old inline version untestable.
    auto req = base(hosted());
    req.session_key = "t1";
    req.effort      = "medium";
    CHECK(oa::build_request_body(req) == oa::build_request_body(req));
}

TEST_CASE("body: an empty system prompt adds no empty message") {
    auto req = base(hosted());
    req.system_prompt = "";
    const json b = oa::build_request_body(req);
    REQUIRE(b["messages"].is_array());
    for (const auto& m : b["messages"])
        CHECK(m["role"] != "system");
}
