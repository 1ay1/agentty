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

#include <filesystem>
#include <fstream>
#include <string>

#include "agentty/provider/openai/transport.hpp"
#include "agentty/domain/session.hpp"
#include "agentty/domain/catalog.hpp"

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

TEST_CASE("context: the OpenAI-compatible universe beyond Claude and GPT") {
    // The ladder above was built from the stacks we had reports about.
    // But "OpenAI-compatible" is most of the ecosystem now, and each
    // vendor picked its own spelling for the same number. A spelling we
    // do not read is not a small miss — it silently drops the model to
    // the 200k default, which is the exact bug this file was opened for,
    // just for a vendor nobody had filed yet.
    //
    // So: one case per major hosted OpenAI-compatible API, using the
    // field each actually returns on /v1/models. These are the providers
    // a user reaches through a gateway or a direct base-url override.

    // Mistral (api.mistral.ai). Its own spelling, matching nothing else.
    CHECK(advertised(json::parse(
        R"({"id":"mistral-large-latest","max_context_length":131072})"))
        == 131'072);

    // Together.ai, Fireworks, DeepSeek, Perplexity — all `context_length`.
    CHECK(advertised(json::parse(
        R"({"id":"meta-llama/Llama-3.3-70B","context_length":131072})"))
        == 131'072);
    CHECK(advertised(json::parse(
        R"({"id":"deepseek-chat","context_length":65536})")) == 65'536);

    // Groq spells it `context_window` on its model rows.
    CHECK(advertised(json::parse(
        R"({"id":"llama-3.3-70b-versatile","context_window":131072})"))
        == 131'072);

    // Cohere's compat layer nests under `context_length` inside a
    // per-model object the same way LiteLLM nests model_info.
    CHECK(advertised(json::parse(
        R"({"id":"command-r-plus","model_info":{"context_window":128000}})"))
        == 128'000);

    // Qwen / DashScope and several Chinese providers report the input
    // half separately — max_input_tokens is the number that bounds the
    // prompt, which is what a context gauge is measuring.
    CHECK(advertised(json::parse(
        R"({"id":"qwen-max","max_input_tokens":30720})")) == 30'720);
}

TEST_CASE("context: an output cap is never mistaken for a context window") {
    // The dangerous near-miss. Many rows carry BOTH a context window and a
    // max-output cap, and the output cap is always much smaller
    // (4k–32k against 128k–1M).
    //
    // Reading the wrong one does not fail loudly: it produces a plausible
    // small number, the gauge reads ~10x the true usage, and
    // auto-compaction fires almost immediately — so the model is handed a
    // summary of a conversation that comfortably fit. The user sees an
    // agent that "forgets" constantly on a model with a huge window.
    //
    // `max_output_tokens` / `max_completion_tokens` / `max_tokens` at the
    // TOP level are output caps and must never be read as the window.
    CHECK(advertised(json::parse(
        R"({"id":"x","max_output_tokens":8192})")) == 0);
    CHECK(advertised(json::parse(
        R"({"id":"x","max_completion_tokens":16384})")) == 0);

    // And when both appear, the WINDOW wins — not whichever came first.
    CHECK(advertised(json::parse(
        R"({"id":"x","max_output_tokens":8192,"context_length":200000})"))
        == 200'000);
    CHECK(advertised(json::parse(
        R"({"id":"x","max_context_length":131072,"max_output_tokens":4096})"))
        == 131'072);
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

// ── A private gateway that declares nothing: the documented last rung ────
//
// Real setup (gitlab.com/r3xxar/fw16-ai-inference): Bifrost on :8090 in
// front of two llama.cpp servers. Its /v1/models rows are bare — id and
// object, nothing else — and the model ids (`igpu/laguna-xs-2.1`) are
// unknown to every public catalog by construction.
//
// So every automatic rung answers 0, and that is CORRECT: inventing a
// window here is how a real 262k model gets clamped to a default nobody
// chose. But 0 has a consequence worth pinning, because it is silent:
// StreamState::compaction_threshold() returns 0 when context_max <= 0, so
// auto-compaction NEVER FIRES and a long thread grows until the server
// rejects it.
//
// AGENTTY_MAX_CONTEXT_TOKENS is the escape hatch (docs/CONTEXT_WINDOW.md).
// These pin both halves: the detection stays honest, and the override is
// what arms compaction again.
TEST_CASE("context: a bare private-gateway row declares nothing") {
    // Exactly what Bifrost serves for a locally-hosted model.
    CHECK(advertised(json::parse(
        R"({"id":"igpu/laguna-xs-2.1","object":"model","owned_by":"bifrost"})"))
        == 0);
    CHECK(advertised(json::parse(
        R"({"id":"dgpu/granite-4.2-3b","object":"model","owned_by":"bifrost"})"))
        == 0);
}

TEST_CASE("context: an unknown window disarms compaction, a known one arms it") {
    // The consequence, stated directly. This is the part a user cannot see:
    // the gauge shows no maximum and nothing warns, so the first symptom is
    // a 400 from the server deep into a long session.
    agentty::StreamState s;

    s.context_max = 0;                       // nobody declared one
    CHECK(s.compaction_threshold() == 0);    // → auto-compaction never fires

    s.context_max = 262'144;                 // AGENTTY_MAX_CONTEXT_TOKENS=262144
    const int thr = s.compaction_threshold();
    CHECK(thr > 0);                          // → armed
    CHECK(thr < s.context_max);              // → leaves room for the reply
}

TEST_CASE("context: id inference only speaks for families it knows") {
    // Rung 4 of resolve_context_window is id inference. It is ABOVE the
    // env escape hatch and the conservative default, so a number it
    // returns silences both — which makes a confident wrong answer here
    // worse than no answer at all.
    //
    // It used to return 200k for any KNOWN FAMILY, and Family::Gpt is a
    // known family. So gpt-5.4 (really 400k) and gpt-4.1 (really 1M) both
    // inferred 200k. On a Copilot session whose catalog had not loaded
    // yet, the gauge read double the true usage and auto-compaction fired
    // at roughly half the thread it should have — silently discarding
    // context the model could still see, with nothing anywhere saying so.
    //
    // The rule: infer only where the id genuinely determines the window.
    using MC = agentty::ModelCapabilities;

    // Claude: 200k is the published base for every generation, and `[1m]`
    // is the one documented widening. That is knowledge, so it speaks.
    CHECK(MC::from_id("claude-sonnet-5").context_window()      == 200'000);
    CHECK(MC::from_id("claude-opus-5").context_window()        == 200'000);
    CHECK(MC::from_id("claude-haiku-4-5").context_window()     == 200'000);
    CHECK(MC::from_id("claude-sonnet-5[1m]").context_window()  == 1'000'000);

    // GPT: the window is NOT inferable from the id. 0 means unknown, which
    // hands the question to models.dev / the env override / the default —
    // rungs that exist precisely for this.
    CHECK(MC::from_id("gpt-5.4").context_window()  == 0);
    CHECK(MC::from_id("gpt-4.1").context_window()  == 0);
    CHECK(MC::from_id("gpt-5-codex").context_window() == 0);

    // And an id from no family we recognise stays 0, as it always did.
    CHECK(MC::from_id("gemini-2.5-pro").context_window()   == 0);
    CHECK(MC::from_id("some-private-model").context_window() == 0);
}

TEST_CASE("context: every path into available_models bakes the window") {
    // bake_context_window() exists so ONE number reaches both the picker's
    // ctx column and the status bar (PR #39). That only holds if every
    // path that fills `available_models` runs it.
    //
    // The refresh path did. The SEED path — init.cpp, the rows you start
    // with — did not: it ran before settings were even loaded, so a user
    // who pinned a context window had their pin ignored on every row until
    // the first provider refresh. The invariant held everywhere except the
    // state you actually boot into, which is the one state nobody thinks
    // to re-check.
    //
    // This is a source scan because the bug is a MISSING CALL SITE. A
    // behavioural test would have to construct the full init path, and
    // would still only cover the two call sites that exist today rather
    // than the third somebody adds next year.
    const std::filesystem::path root{AGENTTY_SRC_ROOT};

    struct Site { const char* file; const char* why; };
    static constexpr Site kSites[] = {
        {"src/runtime/app/init.cpp",
         "the SEEDED rows — the catalog you start with, before any refresh. "
         "Bakes right after load_settings(), because the ladder needs them."},
        {"src/runtime/app/update/models.cpp",
         "the REFRESH rows — rebuilt when a provider lists its models."},
    };

    for (const auto& s : kSites) {
        const auto path = root / s.file;
        INFO("site = " << s.file);
        REQUIRE(std::filesystem::exists(path));

        std::ifstream in(path);
        REQUIRE(in);
        std::string src;
        {
            std::string line;
            while (std::getline(in, line)) {
                // Strip comment lines. Every one of these files EXPLAINS
                // the invariant in prose, and a scan that matches the
                // explanation passes while the call is gone — which is
                // exactly what this test caught about itself.
                const auto first = line.find_first_not_of(" \t");
                if (first != std::string::npos
                    && line.compare(first, 2, "//") == 0) continue;
                src += line;
                src += '\n';
            }
        }

        // A CALL, not a mention: the name followed by an open paren.
        CHECK_MESSAGE(src.find("bake_context_window(") != std::string::npos,
            s.file << " fills available_models without baking the context "
                      "window. " << s.why
                   << "  Unbaked rows carry the raw catalog figure, so the "
                      "picker's ctx column and the status bar disagree about "
                      "the same model, and a user's pinned window is "
                      "silently ignored. Call ui::bake_context_window(row, "
                      "provider_id, settings) on every row.");
    }
}
