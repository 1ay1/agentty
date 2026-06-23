// rag_rerank_test — unit tests for the feature-fusion reranker + extractive
// context compression (agentty::rag). Pure functions, no network. Same
// lightweight harness as tests/rag_test.cpp.

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── 1. query_terms ───────────────────────────────────────────────────────────
static void test_query_terms() {
    auto terms = rag::query_terms("How do I configure OAuth tokens?");
    std::unordered_set<std::string> s(terms.begin(), terms.end());
    CHECK(s.count("configure") == 1);
    CHECK(s.count("oauth") == 1);
    CHECK(s.count("tokens") == 1);
    // 1-char tokens ("I") are dropped.
    CHECK(s.count("i") == 0);
    // No duplicates.
    CHECK(terms.size() == s.size());

    auto dup = rag::query_terms("token token token");
    CHECK(dup.size() == 1);
    CHECK(dup[0] == "token");
}

// Helper: build a vector<Chunk> kept alive for the test, return parallel Hits.
// Reserve up front so addresses stay stable.
static std::vector<rag::Hit> make_hits(std::vector<rag::Chunk>& store,
                                       const std::vector<std::pair<std::string, double>>& items,
                                       const std::vector<std::string>& paths = {}) {
    store.clear();
    store.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        rag::Chunk c;
        c.path = (i < paths.size()) ? paths[i] : ("doc" + std::to_string(i) + ".md");
        c.line_start = 1;
        c.line_end = 2;
        c.text = items[i].first;
        store.push_back(std::move(c));
    }
    std::vector<rag::Hit> hits;
    hits.reserve(store.size());
    for (std::size_t i = 0; i < store.size(); ++i)
        hits.push_back(rag::Hit{&store[i], items[i].second});
    return hits;
}

// ── 2. rerank promotes term coverage ─────────────────────────────────────────
static void test_rerank_promotes_term_coverage() {
    std::vector<rag::Chunk> store;
    // Hit 0: highest first-pass score but mentions only ONE of the query terms.
    // Hit 1: lower first-pass score but covers ALL query terms.
    auto hits = make_hits(store, {
        {"the kubernetes cluster runs many things and other unrelated words here", 0.90},
        {"configure oauth tokens and refresh credentials for the kubernetes cluster", 0.40},
        {"a completely unrelated paragraph about weather and oceans", 0.20},
    });
    auto out = rag::rerank("configure oauth tokens", std::move(hits), 3);
    CHECK(out.size() == 3);
    // The full-coverage hit (originally store[1]) must now be first.
    CHECK(out.front().chunk == &store[1]);
}

// ── 3. rerank path match breaks a tie ────────────────────────────────────────
static void test_rerank_path_match() {
    std::vector<rag::Chunk> store;
    // Equal first-pass scores, equal-ish text. One path contains "oauth".
    auto hits = make_hits(store,
        {
            {"general notes about configuration and setup steps", 0.50},
            {"general notes about configuration and setup steps", 0.50},
        },
        { "misc/setup.md", "auth/oauth_guide.md" });
    auto out = rag::rerank("oauth", std::move(hits), 2);
    CHECK(out.size() == 2);
    // The path-matching chunk (store[1], path has "oauth") ranks first.
    CHECK(out.front().chunk == &store[1]);
}

// ── 4. rerank out_k ──────────────────────────────────────────────────────────
static void test_rerank_out_k() {
    std::vector<rag::Chunk> store;
    std::vector<std::pair<std::string, double>> items;
    for (int i = 0; i < 10; ++i)
        items.push_back({"some text number " + std::to_string(i), 0.5 + i * 0.01});
    auto hits = make_hits(store, items);
    auto out = rag::rerank("text", std::move(hits), 3);
    CHECK(out.size() == 3);
}

// ── 5. compress extracts the relevant span ───────────────────────────────────
static void test_compress_extracts_relevant() {
    std::string text =
        "Intro sentence about nothing in particular here. "
        "Another filler sentence with weather and oceans. "
        "To configure oauth tokens you must set the client id and secret. "
        "Then refresh credentials periodically. "
        "Closing remarks about unrelated topics and more filler text follows.";

    auto out = rag::compress("configure oauth tokens", text, 120);
    CHECK(!out.empty());
    CHECK(out.size() < text.size());
    CHECK(out.find("oauth") != std::string::npos);

    // Short text (< target) returns unchanged.
    std::string shortt = "just a short note about oauth";
    CHECK(rag::compress("oauth", shortt, 600) == shortt);

    // A query that matches nothing returns a non-empty head slice within budget.
    auto none = rag::compress("zzzznotpresent", text, 100);
    CHECK(!none.empty());
    CHECK(none.size() <= 100 + 8);  // small slack
}

// ── 6. compress is extractive (output is a substring of the source) ──────────
static void test_compress_preserves_bytes() {
    std::string text =
        "First sentence here is plain. "
        "The widget run command launches the daemon and binds the socket. "
        "Final sentence is also plain and unrelated to the query at all.";
    auto out = rag::compress("widget run command", text, 80);
    CHECK(!out.empty());
    // Extractive: the compressed passage appears verbatim in the source.
    CHECK(text.find(out) != std::string::npos);
}

int main() {
    test_query_terms();
    test_rerank_promotes_term_coverage();
    test_rerank_path_match();
    test_rerank_out_k();
    test_compress_extracts_relevant();
    test_compress_preserves_bytes();

    if (g_failures == 0) {
        std::printf("rag_rerank_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_rerank_test: %d check(s) failed\n", g_failures);
    return 1;
}
