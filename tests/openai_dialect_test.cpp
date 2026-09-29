// tests/openai_dialect_test.cpp — the corpus the ecosystem doesn't have.
//
// "OpenAI-compatible" has no specification (see lens.hpp). There is nothing to
// conform to, so conformance cannot be asserted — only OBSERVED. This file is
// the substitute: real chunks captured off real endpoints, replayed against
// the lens tables in dialect.hpp.
//
// Every fixture below was taken from an actual response. Where a provider was
// probed live for this suite, the date is recorded. When a provider changes
// its spelling, a fixture here starts failing — which is the closest thing to
// a spec violation this ecosystem can produce.

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "agentty/provider/openai/dialect.hpp"
#include "agentty/provider/openai/lens.hpp"

using json = nlohmann::json;
namespace oa = agentty::provider::openai;
namespace dx = agentty::provider::openai::dialect;

// ═══ the algebra ═════════════════════════════════════════════════════════
// Lens is an Alternative over Maybe. These pin the laws, because every
// deviation table below is a closed expression in them — if `|` is not
// associative or `fail` is not its identity, a table means something other
// than what it reads as.

TEST_CASE("lens: Alternative laws") {
    const json j = json::parse(R"({"b":"from-b","c":"from-c"})");

    const auto a = oa::key<std::string>("a");   // never observes
    const auto b = oa::key<std::string>("b");
    const auto c = oa::key<std::string>("c");

    // left identity / right identity: fail is the unit of |
    CHECK((oa::fail<std::string>() | b)(j) == b(j));
    CHECK((b | oa::fail<std::string>())(j) == b(j));

    // associativity: the shape of the parens cannot change the answer
    CHECK(((a | b) | c)(j) == (a | (b | c))(j));

    // choice takes the FIRST that observes, in written order
    CHECK((b | c)(j).value() == "from-b");
    CHECK((c | b)(j).value() == "from-c");

    // a lens that observes nothing is nullopt, never a throw
    CHECK(!a(j).has_value());
    CHECK(!a(json::parse("[]")).has_value());     // wrong root type
    CHECK(!a(json::parse("null")).has_value());
}

TEST_CASE("lens: a wrong-typed member falls through, it does not throw") {
    // A gateway that sends `reasoning: null` beside a populated
    // `reasoning_content` must fall through. Throwing here would abort a live
    // stream over a field that is not even required to exist.
    const json j = json::parse(R"({"reasoning":null,"reasoning_content":"think"})");
    CHECK(dx::reasoning_delta()(j).value() == "think");

    const json wrong = json::parse(R"({"content":42})");
    CHECK(!dx::content_delta()(wrong).has_value());   // number, not string
}

// ═══ reasoning: the field with three spellings ═══════════════════════════

TEST_CASE("dialect: reasoning — DeepSeek / vLLM spelling") {
    // reasoning_content is what DeepSeek introduced and vLLM copied.
    const json j = json::parse(R"({"reasoning_content":"step one"})");
    CHECK(dx::reasoning_delta()(j).value() == "step one");
}

TEST_CASE("dialect: reasoning — bare `reasoning` spelling") {
    // OpenAI's Responses API, some OpenRouter passthroughs, and Yolo-Auto.
    // CAPTURED LIVE 2026-09-23 from https://yolo-auto.com/v1 (qwen3.8-flash,
    // system_fingerprint vllm-0.29.1rc1): it streams BARE `reasoning`.
    // A client that only checks reasoning_content renders nothing at all.
    const json j = json::parse(
        R"({"reasoning":"The user wants me to count from 1"})");
    CHECK(dx::reasoning_delta()(j).value() ==
          "The user wants me to count from 1");
}

TEST_CASE("dialect: reasoning — empty first key must fall through") {
    // THE REGRESSION THIS FILE EXISTS FOR.
    //
    // Some OpenRouter passthroughs normalise by emitting BOTH keys, with
    // reasoning_content EMPTY and reasoning populated. Breaking on the first
    // key that merely EXISTS drops every reasoning token, silently — no
    // error, no warning, just a model that appears to think in silence.
    const json j = json::parse(
        R"({"reasoning_content":"","reasoning":"actual thinking"})");
    CHECK(dx::reasoning_delta()(j).value() == "actual thinking");

    // ...and the symmetric case still prefers the more specific key.
    const json k = json::parse(
        R"({"reasoning_content":"specific","reasoning":"generic"})");
    CHECK(dx::reasoning_delta()(k).value() == "specific");
}

TEST_CASE("dialect: reasoning — absent is not an error") {
    const json j = json::parse(R"({"content":"just prose"})");
    CHECK(!dx::reasoning_delta()(j).has_value());
}

// ═══ content: string vs structured parts ═════════════════════════════════

TEST_CASE("dialect: content — the ordinary string form") {
    const json j = json::parse(R"({"content":"hello"})");
    CHECK(dx::content_delta()(j).value() == "hello");
}

TEST_CASE("dialect: content — Mistral's structured parts") {
    // Probed on mistral-small-latest with reasoning_effort=high. Before this
    // shape was handled the whole array was silently DROPPED: reasoning
    // invisible, prose missing, and no liveness heartbeat during a long
    // reasoning pass — which starved the stall watchdog and read as a hang.
    const json part_text = json::parse(R"({"type":"text","text":"answer"})");
    CHECK(dx::content_part_text()(part_text).value() == "answer");

    const json part_think = json::parse(
        R"({"type":"thinking","thinking":[{"type":"text","text":"a"},
                                          {"type":"text","text":"b"}]})");
    const auto segs = dx::content_part_thinking_nested()(part_think);
    REQUIRE(segs.has_value());
    CHECK(segs->size() == 2);
    CHECK((*segs)[0] == "a");
    CHECK((*segs)[1] == "b");

    // the flat variant of the same part
    const json flat = json::parse(R"({"type":"thinking","text":"flat"})");
    CHECK(dx::content_part_thinking_flat()(flat).value() == "flat");

    // a `text` part must NOT be read as thinking, or prose lands in the
    // reasoning channel and the answer never appears
    CHECK(!dx::content_part_thinking_flat()(part_text).has_value());
    CHECK(!dx::content_part_text()(part_think).has_value());
}

// ═══ tool calls ══════════════════════════════════════════════════════════

TEST_CASE("dialect: tool-call index is optional, and absence is meaningful") {
    // OpenAI sends `index` so fragments reassemble across chunks.
    const json with = json::parse(R"({"index":2,"function":{"name":"f"}})");
    CHECK(dx::tool_call_index()(with).value() == 2);

    // Many clones omit it. Yolo-Auto's tool call (captured live 2026-09-23)
    // arrives complete in one element with no index at all.
    const json without = json::parse(
        R"({"id":"chatcmpl-tool-98fc71679a8b1fc3","type":"function",
            "function":{"name":"get_weather","arguments":"{\"city\": \"Paris\"}"}})");
    CHECK(!dx::tool_call_index()(without).has_value());
    // The caller must fall back to ARRAY POSITION. Defaulting to 0 here would
    // concatenate two different calls' arguments into one malformed blob.
}

// ═══ errors: three envelopes for one thing ═══════════════════════════════

TEST_CASE("dialect: every error envelope resolves to a message") {
    // OpenAI's shape — and exactly what Yolo-Auto returns on 401/403
    // (captured live 2026-09-23).
    const json nested = json::parse(
        R"({"error":{"message":"Your current plan does not include the 'yolo' model.",
                     "type":"invalid_request_error","code":"invalid_api_key"}})");
    CHECK(dx::error_message()(nested).value().starts_with("Your current plan"));

    // flattened by some gateways
    const json flat = json::parse(R"({"error":"rate limited"})");
    CHECK(dx::error_message()(flat).value() == "rate limited");

    // llama.cpp's bare form, no `error` wrapper
    const json bare = json::parse(R"({"code":500,"message":"context overflow"})");
    CHECK(dx::error_message()(bare).value() == "context overflow");

    // A success chunk must not look like an error, or every stream reports
    // failure.
    const json ok = json::parse(R"({"choices":[{"delta":{"content":"hi"}}]})");
    CHECK(!dx::error_message()(ok).has_value());
}

// ═══ model metadata ══════════════════════════════════════════════════════

TEST_CASE("dialect: context window is a vLLM extension, absent on OpenAI") {
    // CAPTURED LIVE 2026-09-23 from https://yolo-auto.com/v1/models. Both keys
    // present. Verified honoured: a 125,040-token prompt returned HTTP 200 on
    // a FREE key, so the number is real and not a marketing ceiling.
    //
    // NOTE this is the NARROW flat-key lens. The full ladder — ~15 spellings
    // across top_provider / model_info / meta and GGUF arch-prefixed keys —
    // lives in detail::advertised_window_tokens(), which is the SSOT. A
    // second partial list here would be worse than none: a spelling it missed
    // would silently fall back to a default window, the gauge would misread,
    // and auto-compaction would fire at the wrong point (issue #49).
    const json yolo = json::parse(
        R"({"id":"qwen3.8-flash","object":"model","owned_by":"yolo-auto",
            "context_length":131072,"max_model_len":131072})");
    CHECK(dx::model_context_window_flat()(yolo).value() == 131072);

    // Either key alone is enough.
    CHECK(dx::model_context_window_flat()(json::parse(R"({"max_model_len":8192})"))
              .value() == 8192);

    // OpenAI proper ships NEITHER — this field is not in its shape at all.
    // Absence must be representable, which is what Observed<> is for.
    const json openai = json::parse(
        R"({"id":"gpt-4o","object":"model","owned_by":"openai"})");
    CHECK(!dx::model_context_window_flat()(openai).has_value());
}

// ═══ Observed<T>: a claim is not an observation ══════════════════════════

TEST_CASE("Observed: a declared hint cannot impersonate a probed fact") {
    using Ctx = oa::Observed<int>;

    const auto probed   = Ctx::probed(131072);
    const auto declared = Ctx::declared(131072);
    const auto absent   = Ctx::absent();

    // Only a probe is authoritative.
    CHECK(probed.confirmed());
    CHECK(!declared.confirmed());      // <- the whole point
    CHECK(!absent.confirmed());

    // A hint is still usable where being wrong is cheap.
    CHECK(declared.believed());
    CHECK(probed.believed());
    CHECK(!absent.believed());

    // Absence carries a usable default without pretending it was measured.
    CHECK(absent.or_else(8192) == 8192);
    CHECK(probed.or_else(8192) == 131072);
}

TEST_CASE("Observed: the PR-53 failure mode, as a type") {
    // PR #53 proposed a registry row DECLARING that a provider served models
    // `yolo` and `yolo-small`. Probed with a real key, /v1/models listed
    // neither and both returned 403 for every free account.
    //
    // Under the old shape that claim was an ordinary bool or string and
    // nothing distinguished it from a measurement. Here the provenance is in
    // the type, so a caller that needs a FACT cannot accidentally accept a
    // CLAIM — it has to ask for `confirmed()`, and a declaration answers no.
    using Models = oa::Observed<std::vector<std::string>>;

    const auto row_claims = Models::declared({"yolo", "yolo-small"});
    const auto probe_saw  = Models::probed({"qwen3.8-flash", "qwen3.8-27b"});

    CHECK(!row_claims.confirmed());        // a registry row cannot assert this
    CHECK(probe_saw.confirmed());          // only the wire can

    // And the two genuinely disagree, which is the fact that matters.
    CHECK(row_claims.value != probe_saw.value);
}

// ── declared capability: the two spellings of "this model can reason" ────
//
// The fact that decides whether agentty offers an effort ladder and sends
// `reasoning_effort` at all. resolved_caps() resolves it as
//
//     per-model override  >  env  >  live catalog  >  id inference
//
// and these fixtures are what fill the CATALOG rung for a local server.
// Without them a llama.cpp / LM Studio model falls through to id
// inference — a heuristic over hosted naming (`:7b` tags, known family
// prefixes) that cannot classify a GGUF filename or a `publisher/model`
// key, so reasoning support ends up decided by filename.

TEST_CASE("Observed: Mistral declares reasoning as a bare bool") {
    // /v1/models row, Mistral (the shape the reader was originally written
    // for). Both polarities matter: `false` is a real declaration that the
    // model CANNOT reason, and must not read as "absent".
    CHECK(oa::dialect::declared_reasoning()(json::parse(R"({"reasoning":true})"))
              == std::optional<bool>{true});
    CHECK(oa::dialect::declared_reasoning()(json::parse(R"({"reasoning":false})"))
              == std::optional<bool>{false});
}

TEST_CASE("Observed: LM Studio declares reasoning as an OBJECT") {
    // GET /api/v1/models, LM Studio's native API (documented in its REST
    // reference). A model that cannot reason OMITS the key entirely, so the
    // object's presence is the declaration — there is nothing inside it to
    // read for the yes/no question.
    //
    // THE BUG THIS PINS: the reader used to require is_boolean() and skip
    // anything else, so every LM Studio model silently fell through to id
    // inference.
    const auto on = json::parse(
        R"({"vision":true,"trained_for_tool_use":true,
            "reasoning":{"allowed_options":["off","on"],"default":"on"}})");
    CHECK(oa::dialect::declared_reasoning()(on) == std::optional<bool>{true});

    // default:"off" still means SUPPORTED — it just needs asking. The
    // ladder default is a separate observation, deliberately not folded
    // into the yes/no.
    const auto off = json::parse(
        R"({"reasoning":{"allowed_options":["off","on"],"default":"off"}})");
    CHECK(oa::dialect::declared_reasoning()(off) == std::optional<bool>{true});
    CHECK(oa::dialect::declared_reasoning_default()(off)
              == std::optional<std::string>{"off"});
}

TEST_CASE("Observed: no reasoning key is ABSENT, not false") {
    // The distinction the catalog depends on. "Absent" leaves the rung
    // empty so id inference still gets a say; "false" would overwrite it
    // with a claim the server never made.
    CHECK(!oa::dialect::declared_reasoning()(json::parse(R"({"vision":true})")));
    CHECK(!oa::dialect::declared_reasoning()(json::parse(R"({})")));
}

TEST_CASE("key<bool> reads a JSON boolean, not an integer") {
    // Regression: std::is_integral_v<bool> is TRUE, so `key<bool>` fell into
    // the integral arm and tested is_number_integer() — which is false for a
    // real JSON bool. Every `"reasoning": true` read as ABSENT, which is
    // exactly the failure this whole table exists to prevent, and it would
    // have broken Mistral while fixing LM Studio.
    CHECK(oa::key<bool>("x")(json::parse(R"({"x":true})")) == std::optional<bool>{true});
    CHECK(oa::key<bool>("x")(json::parse(R"({"x":false})")) == std::optional<bool>{false});
    // Strict: 0/1 and "true" are NOT booleans. A server that means a bool
    // sends a bool; anything else is a different field shape and should
    // fall through to the next lens in the chain rather than be coerced.
    CHECK(!oa::key<bool>("x")(json::parse(R"({"x":1})")));
    CHECK(!oa::key<bool>("x")(json::parse(R"({"x":"true"})")));
}

TEST_CASE("Observed: LM Studio declares tool training") {
    // Same capability object. Named for what it claims: the model was
    // TRAINED for tool use — stronger than "the server accepts a tools
    // array", which llama.cpp will do for any model and let fail at
    // generation time.
    const auto caps = json::parse(R"({"trained_for_tool_use":true,"vision":false})");
    CHECK(oa::dialect::declared_tool_use()(caps) == std::optional<bool>{true});
    CHECK(!oa::dialect::declared_tool_use()(json::parse(R"({"vision":true})")));
}

// ═══ llama.cpp: chat_template_caps ═════════════════════════════════════
//
// llama.cpp's /v1/models rows carry NO capability object, so the two lenses
// above never fire for it and every model fell to id inference over a GGUF
// path. /props does carry one, and it is the best evidence available: the
// server RUNS the model's own jinja template against probe inputs and diffs
// the output to see what it reacts to (common/jinja/caps.cpp, caps_get).
//
// Key spellings are fixed by caps::to_map() in that file. If upstream
// renames one these fixtures keep failing loudly instead of the feature
// silently going dark.

TEST_CASE("dialect: llama.cpp declares reasoning-effort support in /props") {
    // Trimmed real /props shape. The window fields live beside the caps and
    // are read by a different lens — both come off the one request.
    const auto props = json::parse(R"({
        "model_path": "/models/Qwen3-30B-A3B-Q4_K_M.gguf",
        "default_generation_settings": {"n_ctx": 65536},
        "chat_template_caps": {
            "supports_tools": true,
            "supports_tool_calls": true,
            "supports_reasoning_effort": true,
            "supports_preserve_reasoning": false
        }
    })");
    CHECK(oa::dialect::template_reasoning_effort()(props)
              == std::optional<bool>{true});
    CHECK(oa::dialect::template_tool_calls()(props) == std::optional<bool>{true});
}

TEST_CASE("dialect: a template that ignores reasoning_effort says so") {
    // The case that matters most. llama.cpp ACCEPTS reasoning_effort on any
    // request and forwards it to the template as a kwarg; a template that
    // does not read the kwarg drops it silently. So "server took the field"
    // is not evidence of anything, and without this declaration agentty
    // would offer an effort ladder wired to nothing.
    const auto props = json::parse(
        R"({"chat_template_caps":{"supports_reasoning_effort":false}})");
    CHECK(oa::dialect::template_reasoning_effort()(props)
              == std::optional<bool>{false});
}

TEST_CASE("dialect: an older llama.cpp build is ABSENT, not false") {
    // chat_template_caps is newer than llama.cpp's server, so a build
    // without it must leave the catalog rung EMPTY and let id inference
    // still get a say. Recording `false` here would actively disable
    // reasoning on a server that never said anything.
    const auto old = json::parse(
        R"({"default_generation_settings":{"n_ctx":4096},"total_slots":1})");
    CHECK(!oa::dialect::template_reasoning_effort()(old));
    CHECK(!oa::dialect::template_tool_calls()(old));

    // Present but missing the one key: same answer.
    const auto partial = json::parse(
        R"({"chat_template_caps":{"supports_tools":true}})");
    CHECK(!oa::dialect::template_reasoning_effort()(partial));
}

TEST_CASE("dialect: tool-call support reads supports_tool_calls, not supports_tools") {
    // A template can render the tools array and still fail to render the
    // assistant's tool_calls back into history. That breaks on turn TWO,
    // which presents as the model forgetting it ever called anything — a
    // much more confusing failure than plainly having no tools.
    const auto props = json::parse(R"({"chat_template_caps":{
        "supports_tools": true, "supports_tool_calls": false}})");
    CHECK(oa::dialect::template_tool_calls()(props) == std::optional<bool>{false});
}
