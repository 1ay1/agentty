// wire_shared_test — the shared, single-source-of-truth transport helpers.
//
// After the provider-unification pass, two concerns that every transport used
// to hand-roll (and had DRIFTED on) live once in the wire / stream-epilogue
// headers:
//
//   • provider::parse_retry_after(headers) — the Retry-After backoff hint
//     parser. Ollama's copy was missing entirely (it ignored server backoff);
//     now all four transports call this one function.
//   • wire::scrub_utf8 / wire::is_valid_utf8 — strict UTF-8 validation. Two
//     transports (OpenAI, Ollama) carried a WEAKER copy that accepted overlong
//     encodings and surrogates; the canonical one rejects both.
//   • wire::could_be_tool_json — the leaked-tool-call prefix sniffer that the
//     OpenAI and Ollama transports each open-coded (identical logic, drifted
//     whitespace); now one classifier.
//   • usage::from_openai / from_responses / from_ollama — the three token-usage
//     shapes. The Ollama-native pair was copy-pasted THREE times.
//
// This locks the behaviour so a future edit can't silently re-diverge.

#include <cstdio>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agentty/io/http.hpp"
#include "agentty/provider/stream_epilogue.hpp"
#include "agentty/provider/usage.hpp"
#include "agentty/provider/wire.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using namespace agentty;
using agentty::provider::parse_retry_after;
namespace wire = agentty::provider::wire;

static http::Headers hdrs(std::initializer_list<std::pair<const char*, const char*>> kv) {
    http::Headers h;
    for (auto& [n, v] : kv) h.push_back({n, v});
    return h;
}

static void test_retry_after() {
    // Whole-second integer → parsed.
    {
        auto r = parse_retry_after(hdrs({{"retry-after", "42"}}));
        CHECK(r.has_value() && r->count() == 42);
    }
    // Case-insensitive header name (defensive against proxies).
    {
        auto r = parse_retry_after(hdrs({{"Retry-After", "7"}}));
        CHECK(r.has_value() && r->count() == 7);
    }
    // Absent header → nullopt (runtime falls back to its own schedule).
    {
        auto r = parse_retry_after(hdrs({{"content-type", "application/json"}}));
        CHECK(!r.has_value());
    }
    // Zero is rejected (a 0-second backoff is meaningless; fall back).
    {
        auto r = parse_retry_after(hdrs({{"retry-after", "0"}}));
        CHECK(!r.has_value());
    }
    // HTTP-date form is deliberately NOT parsed → nullopt.
    {
        auto r = parse_retry_after(hdrs({{"retry-after", "Wed, 21 Oct 2015 07:28:00 GMT"}}));
        CHECK(!r.has_value());
    }
    // Trailing junk after the integer → rejected (must consume fully).
    {
        auto r = parse_retry_after(hdrs({{"retry-after", "30s"}}));
        CHECK(!r.has_value());
    }
    // First matching header wins; scan stops.
    {
        auto r = parse_retry_after(hdrs({{"retry-after", "5"}, {"retry-after", "99"}}));
        CHECK(r.has_value() && r->count() == 5);
    }
    // Empty header set → nullopt, no crash.
    {
        auto r = parse_retry_after({});
        CHECK(!r.has_value());
    }
}

static void test_scrub_utf8_strict() {
    // Valid ASCII + multi-byte pass through unchanged (zero-copy fast path).
    CHECK(wire::is_valid_utf8("hello"));
    CHECK(wire::is_valid_utf8("caf\xC3\xA9"));           // café
    CHECK(wire::is_valid_utf8("\xE2\x9C\x93"));          // ✓ (U+2713)
    CHECK(wire::is_valid_utf8("\xF0\x9F\x9A\x80"));      // 🚀 (U+1F680)
    CHECK(wire::scrub_utf8("hello") == "hello");
    CHECK(wire::scrub_utf8("caf\xC3\xA9") == "caf\xC3\xA9");

    const std::string repl = "\xEF\xBF\xBD";   // U+FFFD

    // A lone continuation byte is invalid → replaced.
    CHECK(!wire::is_valid_utf8("\x80"));
    CHECK(wire::scrub_utf8("a\x80""b") == "a" + repl + "b");

    // A truncated multi-byte sequence at end-of-string → replaced. The lead
    // byte can't complete (only 1 of 3 bytes present) so it's replaced and we
    // advance one byte; the orphaned continuation byte is then replaced too.
    CHECK(!wire::is_valid_utf8("\xE2\x9C"));
    CHECK(wire::scrub_utf8("\xE2\x9C") == repl + repl);

    // STRICT: an OVERLONG encoding of '/' (0xC0 0xAF) must be rejected — the
    // weak per-byte-shape check the OpenAI/Ollama copies used would have
    // ACCEPTED this. This is the divergence the unification fixed.
    CHECK(!wire::is_valid_utf8("\xC0\xAF"));
    CHECK(wire::scrub_utf8("\xC0\xAF") == repl + repl);

    // STRICT: a surrogate code point (U+D800 = 0xED 0xA0 0x80) must be
    // rejected — also accepted by the old weak copies.
    CHECK(!wire::is_valid_utf8("\xED\xA0\x80"));
    CHECK(wire::scrub_utf8("\xED\xA0\x80") == repl + repl + repl);

    // STRICT: overlong 3-byte NUL (0xE0 0x80 0x80) rejected.
    CHECK(!wire::is_valid_utf8("\xE0\x80\x80"));

    // Above the Unicode ceiling (0xF4 0x90 ...) → invalid.
    CHECK(!wire::is_valid_utf8("\xF4\x90\x80\x80"));

    // Mixed valid + invalid: valid parts survive, only bad bytes replaced.
    CHECK(wire::scrub_utf8("ok\xC3\xA9""\x80""done") == "ok\xC3\xA9" + repl + "done");
}

static void test_could_be_tool_json() {
    // Ambiguous / still-resolving prefixes stay OPEN (return true) so the
    // transport keeps holding rather than flushing a half-formed call as prose.
    CHECK(wire::could_be_tool_json(""));               // nothing yet
    CHECK(wire::could_be_tool_json("   "));            // only whitespace
    CHECK(wire::could_be_tool_json("{"));              // bare open brace
    CHECK(wire::could_be_tool_json("{ "));             // brace + ws
    CHECK(wire::could_be_tool_json("{\"name\""));       // key start
    CHECK(wire::could_be_tool_json("{}"));             // empty object
    CHECK(wire::could_be_tool_json("  {\"a\":1}"));      // leading ws + object
    CHECK(wire::could_be_tool_json("[{"));             // array of objects
    CHECK(wire::could_be_tool_json("["));              // lone bracket
    CHECK(wire::could_be_tool_json("<tool_call>"));    // full tag
    CHECK(wire::could_be_tool_json("<tool"));          // incomplete tag prefix
    CHECK(wire::could_be_tool_json("```"));            // bare fence
    CHECK(wire::could_be_tool_json("```json"));        // json fence
    CHECK(wire::could_be_tool_json("```\n{"));          // fence then object

    // Committed prose CLOSES the hold (return false) the moment it can't be a
    // tool call any more.
    CHECK(!wire::could_be_tool_json("Sure, I'll do that"));
    CHECK(!wire::could_be_tool_json("{not_a_key"));    // `{` then non-key char
    CHECK(!wire::could_be_tool_json("```cpp"));        // committed code fence
    CHECK(!wire::could_be_tool_json("<div>"));         // not <tool_call>
    CHECK(!wire::could_be_tool_json("[1,2,3]"));       // JSON array of scalars
    CHECK(!wire::could_be_tool_json("hello {world}")); // prose containing braces
}

static void test_usage_extractors() {
    using nlohmann::json;
    namespace usage = agentty::provider::usage;

    // OpenAI /v1/chat/completions shape, incl. cached-token detail.
    {
        auto u = json::parse(R"({"prompt_tokens":100,"completion_tokens":40,
            "prompt_tokens_details":{"cached_tokens":64}})");
        auto su = usage::from_openai(u);
        CHECK(su.has_value());
        CHECK(su->input_tokens == 100);
        CHECK(su->output_tokens == 40);
        CHECK(su->cache_read_input_tokens == 64);
    }
    // OpenAI: no details object → cache_read stays 0.
    {
        auto su = usage::from_openai(json::parse(R"({"prompt_tokens":5,"completion_tokens":1})"));
        CHECK(su.has_value() && su->cache_read_input_tokens == 0);
    }
    // All-zero / empty → nullopt (matches the transports' "only sink if
    // non-empty" guard).
    CHECK(!usage::from_openai(json::object()).has_value());
    CHECK(!usage::from_openai(json::parse(R"({"prompt_tokens":0,"completion_tokens":0})")).has_value());
    // Non-object → nullopt, no throw.
    CHECK(!usage::from_openai(json("not-an-object")).has_value());

    // Responses / Codex shape: input_tokens / output_tokens +
    // input_tokens_details.cached_tokens.
    {
        auto u = json::parse(R"({"input_tokens":200,"output_tokens":50,
            "input_tokens_details":{"cached_tokens":128}})");
        auto su = usage::from_responses(u);
        CHECK(su.has_value());
        CHECK(su->input_tokens == 200);
        CHECK(su->output_tokens == 50);
        CHECK(su->cache_read_input_tokens == 128);
    }
    CHECK(!usage::from_responses(json::object()).has_value());

    // Ollama native DONE frame: top-level prompt_eval_count / eval_count.
    {
        auto frame = json::parse(R"({"done":true,"prompt_eval_count":300,"eval_count":80})");
        auto su = usage::from_ollama(frame);
        CHECK(su.has_value());
        CHECK(su->input_tokens == 300);
        CHECK(su->output_tokens == 80);
        CHECK(su->cache_read_input_tokens == 0);  // no cache concept for local
    }
    CHECK(!usage::from_ollama(json::parse(R"({"done":true})")).has_value());
}

int main() {
    test_retry_after();
    test_scrub_utf8_strict();
    test_could_be_tool_json();
    test_usage_extractors();
    if (g_failures == 0) {
        std::printf("wire_shared_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "wire_shared_test: %d check(s) failed\n", g_failures);
    return 1;
}
