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

// A pathologically long single line (minified blob) must be HARD-SPLIT so no
// chunk overruns max_chars, and multibyte UTF-8 must not be cut mid-codepoint.
static void test_chunker_hard_splits_long_line() {
    const std::size_t kMax = 200;
    // One 5000-char line, no newlines — the old chunker took it whole.
    std::string body(5000, 'x');
    auto chunks = rag::chunk_document("min.js", body, /*max_lines=*/40, kMax, 0);
    CHECK(!chunks.empty());
    for (const auto& c : chunks) {
        // text carries a trailing '\n' per line; allow a small slack for it.
        CHECK(c.text.size() <= kMax + 2);
    }

    // UTF-8 safety: a long run of 3-byte codepoints (‘…’ U+2026, 0xE2 0x80 0xA6)
    // split at kMax must never leave a dangling continuation byte at a chunk
    // edge (which would be invalid UTF-8 the embedder/model chokes on).
    std::string ell;
    for (int i = 0; i < 400; ++i) ell += "\xE2\x80\xA6";  // 1200 bytes
    auto uc = rag::chunk_document("u.md", ell, 40, kMax, 0);
    CHECK(!uc.empty());
    for (const auto& c : uc) {
        std::string_view t = c.text;
        // Strip the trailing newline the assembler adds.
        if (!t.empty() && t.back() == '\n') t.remove_suffix(1);
        // First byte of the chunk must be a lead byte, not a continuation.
        if (!t.empty())
            CHECK((static_cast<unsigned char>(t.front()) & 0xC0) != 0x80);
        // Byte count must be a multiple of 3 (whole … codepoints only).
        CHECK(t.size() % 3 == 0);
    }
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

// ── 9. Multi-query dense retrieval batches into ONE embed round-trip ────────
// Regression lock for the search_fused batching win: N query variants must
// embed in a SINGLE /api/embed call (one batch of N texts), not N serial
// calls. Uses the pluggable embed backend to count invocations + batch sizes
// without a network.
static void test_multiquery_embed_batched() {
    // Deterministic 8-dim embedder: hashes each text to a unit-ish vector.
    // Records how many times it was CALLED and the batch size of each call.
    static int calls = 0;
    static std::vector<std::size_t> batch_sizes;
    calls = 0; batch_sizes.clear();
    rag::set_embed_backend([](const rag::EmbedConfig&,
                              const std::vector<std::string>& texts)
            -> std::optional<std::vector<std::vector<float>>> {
        ++calls;
        batch_sizes.push_back(texts.size());
        std::vector<std::vector<float>> out;
        out.reserve(texts.size());
        for (const auto& t : texts) {
            std::vector<float> v(8, 0.0f);
            std::size_t h = std::hash<std::string>{}(t);
            for (int d = 0; d < 8; ++d) v[d] = float((h >> (d * 4)) & 0xF) + 1.0f;
            out.push_back(std::move(v));
        }
        return out;
    });

    // Build a small corpus WITH embeddings so the dense path is live (brute-
    // force cosine; set_chunks_for_test sets embed_dim_ from the vectors).
    std::vector<rag::Chunk> chunks;
    for (int i = 0; i < 6; ++i) {
        rag::Chunk c;
        c.path = "doc" + std::to_string(i) + ".md";
        c.line_start = 1; c.line_end = 3;
        c.text = "widget installation guide section number " + std::to_string(i);
        c.embedding.assign(8, float(i % 3) + 0.5f);
        chunks.push_back(std::move(c));
    }
    rag::Corpus corpus;
    corpus.set_chunks_for_test(std::move(chunks));
    CHECK(corpus.has_embeddings());

    rag::EmbedConfig embed;
    embed.model = "test-embed";   // non-empty → dense path active

    // FIVE query variants (the shape of carryover + multihop + expansion).
    std::vector<std::string> queries = {
        "widget installation", "how to install the widget",
        "setup guide", "widget setup steps", "install instructions"};
    auto hits = corpus.search_fused(queries, embed, /*k=*/4);

    // THE assertion: one batched round-trip, not five serial ones.
    CHECK(calls == 1);
    CHECK(batch_sizes.size() == 1);
    if (!batch_sizes.empty()) CHECK(batch_sizes[0] == queries.size());
    CHECK(!hits.empty());   // dense retrieval actually produced results

    rag::set_embed_backend(nullptr);   // reset so later tests are unaffected
}

// ── 10. Relative Score Fusion (min-max normalized weighted sum) ─────────────
// RSF should: (a) normalize each list to [0,1] before summing, so an
// unbounded BM25 list and a cosine list contribute comparably; (b) honour
// per-list weights; (c) rank a doc that scores WELL in both lists above one
// that merely appears in both at mediocre scores (the magnitude signal RRF
// discards).
static void test_relative_score_fusion() {
    // List A (say BM25, unbounded scale): doc 1 dominates, doc 2 mid, 3 low.
    // List B (say cosine, [0,1] scale): doc 2 dominates, doc 1 mid, 3 low.
    std::vector<std::vector<std::pair<std::uint32_t, double>>> lists = {
        {{1, 40.0}, {2, 12.0}, {3, 1.0}},     // A
        {{2, 0.95}, {1, 0.60}, {3, 0.10}},    // B
    };
    // Equal weights. Per-list min-max: A -> {1:1.0, 2:0.282, 3:0}; B ->
    // {2:1.0, 1:0.588, 3:0}. Sums: 1:1.588, 2:1.282, 3:0. So 1 > 2 > 3.
    auto out = rag::relative_score_fusion_weighted(lists, {1.0, 1.0}, 3);
    CHECK(out.size() == 3);
    CHECK(out[0].first == 1);
    CHECK(out[1].first == 2);
    CHECK(out[2].first == 3);
    CHECK(out[0].second > out[1].second);

    // Weighting list B up flips the winner to doc 2 (B's champion).
    auto outw = rag::relative_score_fusion_weighted(lists, {1.0, 3.0}, 3);
    CHECK(outw.size() == 3);
    CHECK(outw[0].first == 2);

    // A degenerate all-equal list votes a flat 1.0 for each member (no NaN).
    std::vector<std::vector<std::pair<std::uint32_t, double>>> flat = {
        {{5, 7.0}, {6, 7.0}},
    };
    auto outf = rag::relative_score_fusion_weighted(flat, {2.0}, 5);
    CHECK(outf.size() == 2);
    CHECK(std::abs(outf[0].second - 2.0) < 1e-9);
    CHECK(std::abs(outf[1].second - 2.0) < 1e-9);

    // Empty input is empty (no crash).
    CHECK(rag::relative_score_fusion_weighted({}, {}, 5).empty());
}

// End-to-end RSF toggle is intentionally NOT unit-tested here: fusion_is_rsf_
// reads AGENTTY_RAG_FUSION once per process (a static local), and earlier
// tests in this binary trigger a search first — so the env can't be flipped
// mid-run to exercise the RSF branch deterministically. test_relative_score_
// fusion above proves the fusion math; the search()/search_fused() wiring is
// a one-line branch verified by inspection + compilation.

int main() {
    test_chunker_line_aligned();
    test_chunker_hard_splits_long_line();
    test_bm25_ranks_exact_term();
    test_rrf_fusion();
    test_rrf_weighted();
    test_cosine();
    test_corpus_bm25_only_search();
    test_contextual_breadcrumb();
    test_multiquery_embed_batched();
    test_relative_score_fusion();

    if (g_failures == 0) {
        std::printf("rag_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_test: %d check(s) failed\n", g_failures);
    return 1;
}
