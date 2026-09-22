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

TEST_CASE("context: llama-server reports the window inside meta, not at the top") {
    // Verbatim shape from llama.cpp tools/server/server-context.cpp
    // (get_res_model_info). n_ctx is the SERVED window (slot_n_ctx, i.e.
    // what -c allocated); n_ctx_train is the architectural ceiling.
    //
    // Issue #49: the flat ladder already listed n_ctx/n_ctx_train, but
    // llama.cpp never puts them at the top level of a row — only inside
    // `meta` — so every llama-server model fell through to the 200k
    // default. An 8k model claiming 200k means compaction never fires and
    // the server truncates the prompt instead.
    const auto row = json::parse(R"({
        "id": "qwen2.5-coder-7b",
        "object": "model",
        "owned_by": "llamacpp",
        "meta": {
            "vocab_type": 2, "n_vocab": 152064,
            "n_ctx": 8192, "n_ctx_train": 32768,
            "n_embd": 3584, "n_params": 7615616512,
            "size": 4431389696, "ftype": 15
        }
    })");
    // The SERVED window wins over the train-time ceiling: 8192 is what a
    // request actually has to fit inside.
    CHECK(det::advertised_context_window(row) == 8192);
}

TEST_CASE("context: llama-server falls back to n_ctx_train when n_ctx is absent") {
    const auto row = json::parse(R"({
        "id": "m", "meta": { "n_ctx_train": 32768 }
    })");
    CHECK(det::advertised_context_window(row) == 32768);
}

TEST_CASE("context: a meta block that says nothing is not a window") {
    // `meta` exists on every llama.cpp row, so its mere presence must not
    // be read as an answer — 0 has to stay "nobody said".
    const auto row = json::parse(R"({
        "id": "m", "meta": { "n_vocab": 152064, "ftype": 15 }
    })");
    CHECK(det::advertised_context_window(row) == 0);
}

TEST_CASE("context: LM Studio's loaded instance config is a window") {
    // The per-instance config block from LM Studio's native
    // /api/v1/models. The probe reads each loaded_instances[].config
    // through this same parser, so `context_length` must resolve there.
    const auto cfg = json::parse(R"({
        "context_length": 16384,
        "eval_batch_size": 512,
        "flash_attention": true
    })");
    CHECK(det::advertised_context_window(cfg) == 16384);
}

TEST_CASE("context: an architectural maximum is not the loaded window") {
    // LM Studio's /v1 shim reports max_context_length — what the model
    // ARCHITECTURE supports. A model capable of 128k loaded at 16k still
    // refuses at 16k.
    //
    // Both parse to a number, and that was the trap: the row looked
    // "known", so the endpoint probe (which alone can see the loaded
    // instance) never ran. The parser is right to return the declared
    // value here — it is the only bound available until the model loads —
    // but list_models must treat a smaller probed value as authoritative.
    const auto row = json::parse(R"({
        "id": "qwen/qwen3-coder-30b", "max_context_length": 262144
    })");
    CHECK(det::advertised_context_window(row) == 262144);

    const auto loaded = json::parse(R"({ "context_length": 16384 })");
    CHECK(det::advertised_context_window(loaded) == 16384);
    // The whole point of the probe override: the runtime number is smaller,
    // and smaller is the one a prompt has to respect.
    CHECK(det::advertised_context_window(loaded)
            < det::advertised_context_window(row));
}

TEST_CASE("context: a GGUF arch-prefixed context_length is a window") {
    // How the window is spelled in a model's own metadata, and exactly what
    // Ollama's CLI reads (cmd/cmd.go showInfo). The arch prefix varies per
    // family, so it is matched by suffix.
    const auto row = json::parse(R"({
        "model_info": {
            "general.architecture": "qwen2",
            "qwen2.context_length": 32768,
            "qwen2.embedding_length": 3584
        }
    })");
    CHECK(det::advertised_context_window(row) == 32768);
}

TEST_CASE("context: a FLOAT context_length still counts") {
    // Ollama's ModelInfo is Go's map[string]any, so encoding/json emits
    // every number as float64 — 32768 arrives as 32768.0. Ollama's own
    // tests (cmd/cmd_test.go) use float64 literals for this exact field.
    //
    // An is_number_integer() check silently dropped ALL of these, so every
    // Ollama model fell through to the default despite the daemon having
    // told us the answer.
    const auto row = json::parse(R"({
        "model_info": { "qwen2.context_length": 32768.0 }
    })");
    CHECK(det::advertised_context_window(row) == 32768);

    // The flat spellings take floats too — LiteLLM emits 16385.0.
    const auto flat = json::parse(R"({ "context_length": 16385.0 })");
    CHECK(det::advertised_context_window(flat) == 16385);
}

TEST_CASE("context: embedding_length is not a context window") {
    // Both keys live side by side in model_info and both end in "_length".
    // Matching too loosely would report a 3584-token window for a 32k model.
    const auto row = json::parse(R"({
        "model_info": { "qwen2.embedding_length": 3584 }
    })");
    CHECK(det::advertised_context_window(row) == 0);
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
