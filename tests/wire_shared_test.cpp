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
//
// This locks the behaviour so a future edit can't silently re-diverge.

#include <cstdio>
#include <string>
#include <string_view>

#include "agentty/io/http.hpp"
#include "agentty/provider/stream_epilogue.hpp"
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

int main() {
    test_retry_after();
    test_scrub_utf8_strict();
    if (g_failures == 0) {
        std::printf("wire_shared_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "wire_shared_test: %d check(s) failed\n", g_failures);
    return 1;
}
