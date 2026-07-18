// rag_test — unit tests for the document/knowledge RAG module
// (agentty::rag). NO network: the embedding path is exercised only with an
// empty model (which short-circuits to BM25-only), so no Ollama server is
// required. Covers the chunker, BM25 ranking, RRF fusion, cosine, and the
// Corpus BM25-only search path (via set_chunks_for_test).
//
// Lightweight harness mirroring tests/ollama_transport_test.cpp: a global
// failure counter + CHECK macro, a main() that runs each test fn and prints
// "all checks passed" / returns nonzero on any failure.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "agentty/rag/rag.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── 1. Chunker ──────────────────────────────────────────────────────────────
static void test_chunker_line_aligned() {
    std::string body =
        "# Introduction\n"
        "This document explains the widget system.\n"
        "\n"
        "## Installation\n"
        "Run the installer and follow the prompts.\n"
        "\n"
        "## Usage\n"
        "Invoke the widget with the run command.\n";

    auto chunks = rag::chunk_document("manual.md", body);
    CHECK(chunks.size() >= 1);

    bool saw_install = false;
    bool saw_usage   = false;
    for (const auto& c : chunks) {
        CHECK(c.line_start >= 1);
        CHECK(c.line_end >= c.line_start);
        CHECK(c.path == "manual.md");
        if (c.text.find("Installation") != std::string::npos) saw_install = true;
        if (c.text.find("Usage") != std::string::npos)        saw_usage = true;
    }
    // The headings must be preserved somewhere across the chunks.
    CHECK(saw_install);
    CHECK(saw_usage);
}

// ── 2. BM25 ranks the chunk containing the exact term first ──────────────────
static void test_bm25_ranks_exact_term() {
    std::vector<rag::Chunk> chunks;
    auto mk = [](const char* p, const char* text) {
        rag::Chunk c; c.path = p; c.line_start = 1; c.line_end = 1; c.text = text;
        return c;
    };
    chunks.push_back(mk("a", "the quick brown fox jumps"));
    chunks.push_back(mk("b", "lazy dogs sleep all afternoon"));
    chunks.push_back(mk("c", "pelican migration patterns over oceans"));
    chunks.push_back(mk("d", "compiler optimization passes and inlining"));

    auto idx = rag::build_bm25(chunks);

    // "pelican" appears in exactly one chunk (id 2) — it must rank first.
    auto hits = rag::bm25_search(idx, "pelican", 4);
    CHECK(!hits.empty());
    CHECK(hits.front().first == 2u);

    // "compiler inlining" → chunk 3.
    auto hits2 = rag::bm25_search(idx, "compiler inlining", 4);
    CHECK(!hits2.empty());
    CHECK(hits2.front().first == 3u);
}

// ── 3. RRF fusion ────────────────────────────────────────────────────────────
static void test_rrf_fusion() {
    // doc 2 is near the top of BOTH lists → should fuse to first.
    // doc 9 appears in only one list → still present in the output.
    std::vector<std::vector<std::uint32_t>> lists = {
        {5, 2, 1, 9},   // list A
        {7, 2, 3},      // list B
    };
    auto fused = rag::reciprocal_rank_fusion(lists, 60.0, 10);
    CHECK(!fused.empty());
    CHECK(fused.front().first == 2u);

    bool saw_9 = false;
    for (const auto& [id, score] : fused) if (id == 9u) saw_9 = true;
    CHECK(saw_9);
}

// ── 3b. Weighted RRF ─────────────────────────────────────────────────────────
static void test_rrf_weighted() {
    // Two lists rank two different docs first. With equal weight the tie
    // breaks by lower id; with a heavy weight on list B, B's top doc wins.
    std::vector<std::vector<std::uint32_t>> lists = {
        {10, 20},   // list A (lexical): 10 first
        {20, 10},   // list B (dense):   20 first
    };
    // Equal weights → symmetric scores → lower id (10) wins the tie.
    auto even = rag::reciprocal_rank_fusion_weighted(lists, {1.0, 1.0}, 60.0, 10);
    CHECK(even.front().first == 10u);

    // Heavy weight on the dense list (B) pulls its top doc (20) to first.
    auto tilt = rag::reciprocal_rank_fusion_weighted(lists, {1.0, 5.0}, 60.0, 10);
    CHECK(tilt.front().first == 20u);

    // Empty weights vector == all-ones == the unweighted overload.
    auto same = rag::reciprocal_rank_fusion_weighted(lists, {}, 60.0, 10);
    auto base = rag::reciprocal_rank_fusion(lists, 60.0, 10);
    CHECK(same.size() == base.size());
    CHECK(same.front().first == base.front().first);

    // A zero weight drops a list entirely.
    auto dropped = rag::reciprocal_rank_fusion_weighted(lists, {0.0, 1.0}, 60.0, 10);
    CHECK(dropped.front().first == 20u);  // only list B counted
}

// ── 4. Cosine ────────────────────────────────────────────────────────────────
static void test_cosine() {
    std::vector<float> v = {1.0f, 2.0f, 3.0f};
    CHECK(std::fabs(rag::cosine(v, v) - 1.0) < 1e-6);

    std::vector<float> x = {1.0f, 0.0f};
    std::vector<float> y = {0.0f, 1.0f};
    CHECK(std::fabs(rag::cosine(x, y) - 0.0) < 1e-6);

    std::vector<float> short_v = {1.0f, 2.0f};
    std::vector<float> long_v  = {1.0f, 2.0f, 3.0f};
    CHECK(rag::cosine(short_v, long_v) == 0.0);
}

// ── 5. Corpus BM25-only search (no embeddings, no network) ───────────────────
static void test_corpus_bm25_only_search() {
    std::vector<rag::Chunk> chunks;
    auto mk = [](const char* p, const char* text) {
        rag::Chunk c; c.path = p; c.line_start = 1; c.line_end = 2; c.text = text;
        return c;  // NOTE: no embedding → BM25-only
    };
    chunks.push_back(mk("auth.md",    "configure oauth tokens and refresh credentials"));
    chunks.push_back(mk("deploy.md",  "kubernetes deployment manifests and replicas"));
    chunks.push_back(mk("logging.md", "structured logging with severity levels"));

    rag::Corpus corpus;
    corpus.set_chunks_for_test(std::move(chunks));

    CHECK(corpus.has_embeddings() == false);
    CHECK(corpus.chunk_count() == 3);

    // Empty embed model → no dense branch, no network call.
    rag::EmbedConfig embed;  // model is empty by default
    auto hits = corpus.search("kubernetes deployment", embed, 3);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk != nullptr);
    CHECK(hits.front().chunk->path == "deploy.md");
}

// ── 6. Contextual retrieval (Anthropic-style heading breadcrumb) ──────────
// The chunker situates each chunk with "path › heading › subheading", and
// build_bm25 indexes those tokens — so a chunk whose BODY never mentions the
// topic is still retrievable via the heading it sits under. This is the
// pipeline half of the −49%-failures technique; without it the test's query
// has zero body overlap and the chunk is unfindable.
static void test_contextual_breadcrumb() {
    const std::string doc =
        "# Installation\n"
        "\n"
        "## Linux\n"
        "\n"
        "Run the setup script and accept the license.\n"
        "\n"
        "## Windows\n"
        "\n"
        "Double-click the installer executable.\n";
    auto chunks = rag::chunk_document("guide.md", doc);
    CHECK(!chunks.empty());

    // Every chunk carries a breadcrumb starting with the doc path.
    bool crumbs_ok = true;
    for (const auto& c : chunks)
        if (c.context.rfind("guide.md", 0) != 0) crumbs_ok = false;
    CHECK(crumbs_ok);

    // The Linux body chunk's breadcrumb names the heading chain.
    bool linux_crumb = false;
    for (const auto& c : chunks)
        if (c.text.find("setup script") != std::string::npos
            && c.context.find("Linux") != std::string::npos
            && c.context.find("Installation") != std::string::npos)
            linux_crumb = true;
    CHECK(linux_crumb);

    // BM25 finds the setup-script chunk by HEADING terms absent from its
    // body ("linux installation") — the contextual-BM25 win.
    rag::Corpus corpus;
    corpus.set_chunks_for_test(std::move(chunks));
    auto hits = corpus.search("linux installation", {}, 3);
    bool found = false;
    for (const auto& h : hits)
        if (h.chunk && h.chunk->text.find("setup script") != std::string::npos)
            found = true;
    CHECK(found);

    // embed_input() prefixes the breadcrumb (contextual embeddings side).
    rag::Chunk c;
    c.text = "body";
    c.context = "doc.md \xe2\x80\xba Section";
    CHECK(c.embed_input() == "doc.md \xe2\x80\xba Section\nbody");
    c.context.clear();
    CHECK(c.embed_input() == "body");
}

int main() {
    test_chunker_line_aligned();
    test_bm25_ranks_exact_term();
    test_rrf_fusion();
    test_rrf_weighted();
    test_cosine();
    test_corpus_bm25_only_search();
    test_contextual_breadcrumb();

    if (g_failures == 0) {
        std::printf("rag_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_test: %d check(s) failed\n", g_failures);
    return 1;
}
