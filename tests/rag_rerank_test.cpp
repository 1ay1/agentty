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

    // A query that matches nothing returns EMPTY — the "no relevant span
    // found" signal. Consumers (ContextChunk::text()) fall back to the full
    // chunk instead of a misleading head truncation.
    auto none = rag::compress("zzzznotpresent", text, 100);
    CHECK(none.empty());
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

// ── 7. rerank dense feature: calibrated cosine breaks a lexical tie ───────────
static void test_rerank_dense_feature() {
    std::vector<rag::Chunk> store;
    // Two chunks, identical lexical relevance to the query (same words), same
    // first-pass score — the lexical features can't separate them. Their
    // embeddings differ: store[1] is far closer to the query vector. The dense
    // feature must promote store[1].
    auto hits = make_hits(store, {
        {"widget run command launches the daemon", 0.50},
        {"widget run command launches the daemon", 0.50},
    }, {"a.md", "b.md"});
    // 3-dim toy embeddings; query points along +z.
    std::vector<float> qv = {0.0f, 0.0f, 1.0f};
    store[0].embedding = {1.0f, 0.0f, 0.0f};   // orthogonal → cosine 0
    store[1].embedding = {0.0f, 0.2f, 0.98f};  // nearly parallel → cosine ~1

    // Without the query vector: tie — first-pass order preserved (store[0]).
    {
        auto h2 = hits;   // copy (same chunk pointers)
        auto out = rag::rerank("widget run command", std::move(h2), 2, rag::RerankWeights{});
        CHECK(out.size() == 2);
        CHECK(out.front().chunk == &store[0]);
    }
    // With the query vector: the semantically-closer chunk wins.
    {
        auto out = rag::rerank("widget run command", std::move(hits), 2,
                               rag::RerankWeights{}, &qv);
        CHECK(out.size() == 2);
        CHECK(out.front().chunk == &store[1]);
    }
}

// ── 8. rerank collapses same-file overlapping-window near-dupes ───────────────
static void test_rerank_dedup_overlapping_windows() {
    std::vector<rag::Chunk> store;
    store.reserve(4);
    auto mk = [&](const char* path, int lo, int hi, const char* text, double sc) {
        rag::Chunk c; c.path = path; c.line_start = lo; c.line_end = hi; c.text = text;
        store.push_back(std::move(c));
        return sc;
    };
    std::vector<double> scores;
    scores.push_back(mk("guide.md", 1, 10, "install the widget on linux", 0.90));
    scores.push_back(mk("guide.md", 6, 15, "install the widget on linux and configure", 0.85)); // overlaps 1-10
    scores.push_back(mk("guide.md", 40, 50, "unrelated troubleshooting section", 0.60));         // disjoint
    scores.push_back(mk("other.md", 1, 10, "install the widget on linux", 0.55));                // different file
    std::vector<rag::Hit> hits;
    for (std::size_t i = 0; i < store.size(); ++i)
        hits.push_back(rag::Hit{&store[i], scores[i]});

    auto out = rag::rerank("install widget linux", std::move(hits), 10);
    // The two overlapping guide.md windows collapse to one (the higher-scored,
    // store[0]); the disjoint guide.md range and other.md survive → 3 hits.
    CHECK(out.size() == 3);
    bool saw_first = false, saw_overlap = false, saw_disjoint = false, saw_other = false;
    for (const auto& h : out) {
        if (h.chunk == &store[0]) saw_first = true;
        if (h.chunk == &store[1]) saw_overlap = true;
        if (h.chunk == &store[2]) saw_disjoint = true;
        if (h.chunk == &store[3]) saw_other = true;
    }
    CHECK(saw_first);
    CHECK(!saw_overlap);   // collapsed into store[0]
    CHECK(saw_disjoint);
    CHECK(saw_other);
}

// ── 9. BM25 heading field boost lifts a heading match ────────────────────────
static void test_bm25_heading_boost() {
    // Two chunks. store[0] has "kubernetes" only in its heading breadcrumb;
    // store[1] mentions it once in the body. With the field boost, the
    // heading match should score at least as high as the incidental body
    // mention — heading terms are stronger relevance signals.
    std::vector<rag::Chunk> chunks;
    {
        rag::Chunk c;
        c.path = "a.md"; c.line_start = 1; c.line_end = 5;
        c.context = "guide.md › Kubernetes › Scaling";
        c.text = "increase the replica count and the pods spread across nodes";
        chunks.push_back(std::move(c));
    }
    {
        rag::Chunk c;
        c.path = "b.md"; c.line_start = 1; c.line_end = 5;
        c.context = "guide.md › Appendix";
        c.text = "a passing mention of kubernetes buried in a long unrelated "
                 "paragraph about many other topics and words filler filler";
        chunks.push_back(std::move(c));
    }
    auto idx = rag::build_bm25(chunks);
    auto res = rag::bm25_search(idx, "kubernetes", 5);
    CHECK(!res.empty());
    // The heading-boosted chunk (id 0) must rank first.
    CHECK(res.front().first == 0u);
}

// ── 10. BM25 stemming (on by default) matches morphological variants ───────
static void test_bm25_stemming_default_on() {
    // The doc says "running"; the query says "runs". With Porter stemming
    // (default on) both stem to "run" and match — a miss with raw tokens.
    std::vector<rag::Chunk> chunks;
    {
        rag::Chunk c; c.path = "a.md"; c.line_start = 1; c.line_end = 3;
        c.text = "the service is running across the cluster tonight";
        chunks.push_back(std::move(c));
    }
    {
        rag::Chunk c; c.path = "b.md"; c.line_start = 1; c.line_end = 3;
        c.text = "an unrelated paragraph about weather patterns and oceans";
        chunks.push_back(std::move(c));
    }
    auto idx = rag::build_bm25(chunks);
    auto res = rag::bm25_search(idx, "runs", 5);
    CHECK(!res.empty());
    if (!res.empty())
        CHECK(res.front().first == 0u);   // "running" chunk matched via stem
}

int main() {
    test_query_terms();
    test_rerank_promotes_term_coverage();
    test_rerank_path_match();
    test_rerank_out_k();
    test_compress_extracts_relevant();
    test_compress_preserves_bytes();
    test_rerank_dense_feature();
    test_rerank_dedup_overlapping_windows();
    test_bm25_heading_boost();
    test_bm25_stemming_default_on();

    if (g_failures == 0) {
        std::printf("rag_rerank_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_rerank_test: %d check(s) failed\n", g_failures);
    return 1;
}
