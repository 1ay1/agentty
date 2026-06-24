// knowledge_test — unit tests for the KNOWLEDGE LAYER seams
// (KnowledgeSource / Retriever / KnowledgeRouter / Pipeline + stages).
//
// NO network and NO LLM: we drive everything with a BM25-only Corpus
// (empty EmbedConfig → dense branch never fires) and a tiny hand-rolled
// fake KnowledgeSource, proving:
//   • CorpusSource adapts Corpus and stamps provenance (hit.source).
//   • KnowledgeRouter single-source short-circuits (no fusion).
//   • KnowledgeRouter multi-source fuses via RRF and stamps provenance.
//   • Pipeline runs RerankStage → CompressStage and produces compressed text.
//   • Context::from_hits / ContextChunk::text() carry the right data.
//
// Lightweight harness mirroring tests/rag_expand_test.cpp.

#include <cstdio>
#include <string>
#include <vector>

#include "agentty/rag/rag.hpp"
#include "agentty/rag/knowledge.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

// Build a small BM25-only corpus over a few distinct-keyword chunks.
rag::Corpus make_corpus() {
    std::vector<rag::Chunk> chunks;
    auto mk = [](const char* p, const char* text) {
        rag::Chunk c;
        c.path = p;
        c.line_start = 1;
        c.line_end = 10;
        c.text = text;
        return c;
    };
    chunks.push_back(mk("k8s.md",
        "kubernetes deployment scales replicas across the cluster. "
        "Containers and pods are orchestrated by the control plane. "
        "A deployment manifest declares the desired replica count."));
    chunks.push_back(mk("net.md",
        "tcp handshake establishes a reliable byte stream over ip. "
        "Packets are acknowledged and retransmitted on loss."));
    chunks.push_back(mk("db.md",
        "the database stores rows in tables indexed by a btree. "
        "Transactions provide atomicity and isolation guarantees."));
    rag::Corpus corpus;
    corpus.set_chunks_for_test(std::move(chunks));
    return corpus;
}

// A trivial second KnowledgeSource: returns one synthetic hit pointing at a
// static chunk it owns, so we can test multi-source fusion + provenance
// without a second corpus.
class FakeSource final : public rag::KnowledgeSource {
public:
    explicit FakeSource(std::string n) : name_(std::move(n)) {
        chunk_.path = "fake.md";
        chunk_.line_start = 1;
        chunk_.line_end = 2;
        chunk_.text = "kubernetes notes from the fake source about replicas";
    }
    std::string_view name() const noexcept override { return name_; }
    std::vector<rag::Hit> retrieve(std::string_view, std::size_t k) const override {
        if (k == 0) return {};
        rag::Hit h;
        h.chunk = &chunk_;
        h.score = 1.0;
        h.source = this;   // stamp provenance
        return {h};
    }
private:
    std::string       name_;
    rag::Chunk        chunk_;
};

// A source whose hit has a CONFIGURABLE identity (path + line span). Two of
// these with the same identity simulate the same logical chunk surfacing from
// two distinct sources (overlapping corpora) — the router must collapse them.
class IdSource final : public rag::KnowledgeSource {
public:
    IdSource(std::string n, std::string path, int ls, int le, std::string body)
        : name_(std::move(n)) {
        chunk_.path = std::move(path);
        chunk_.line_start = ls;
        chunk_.line_end = le;
        chunk_.text = std::move(body);
    }
    std::string_view name() const noexcept override { return name_; }
    std::vector<rag::Hit> retrieve(std::string_view, std::size_t k) const override {
        if (k == 0) return {};
        rag::Hit h; h.chunk = &chunk_; h.score = 1.0; h.source = this;
        return {h};
    }
private:
    std::string name_;
    rag::Chunk  chunk_;
};

const rag::EmbedConfig kNoEmbed{};  // empty model → BM25-only, no network

// (a) CorpusSource adapts a Corpus and stamps provenance.
void test_corpus_source_provenance() {
    auto corpus = make_corpus();
    rag::CorpusSource src("docs", corpus, kNoEmbed);
    CHECK(src.name() == "docs");

    auto hits = src.retrieve("kubernetes deployment replicas", 5);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk != nullptr);
    CHECK(hits.front().chunk->path == "k8s.md");
    // Every hit must be source-stamped.
    for (const auto& h : hits) CHECK(h.source == &src);
}

// (b) Router with ONE source short-circuits to that source's output.
void test_router_single_source() {
    auto corpus = make_corpus();
    auto src = std::make_shared<rag::CorpusSource>("docs", corpus, kNoEmbed);
    rag::KnowledgeRouter router;
    router.add(src);
    CHECK(router.source_count() == 1);

    auto hits = router.retrieve("kubernetes deployment", 3);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk->path == "k8s.md");
    CHECK(hits.front().source == src.get());  // provenance preserved
}

// (c) Router with TWO sources fuses via RRF and keeps provenance from each.
void test_router_multi_source_fusion() {
    auto corpus = make_corpus();
    auto docs = std::make_shared<rag::CorpusSource>("docs", corpus, kNoEmbed);
    auto fake = std::make_shared<FakeSource>("fake");
    rag::KnowledgeRouter router;
    router.add(docs);
    router.add(fake);
    CHECK(router.source_count() == 2);

    auto hits = router.retrieve("kubernetes replicas", 10);
    CHECK(hits.size() >= 2);

    // Both sources should be represented in the fused list.
    bool saw_docs = false, saw_fake = false;
    for (const auto& h : hits) {
        CHECK(h.source != nullptr);              // never lose provenance
        if (h.source == docs.get()) saw_docs = true;
        if (h.source == fake.get()) saw_fake = true;
        // Fused score is the RRF score (positive, < 1 for k=60).
        CHECK(h.score > 0.0);
    }
    CHECK(saw_docs);
    CHECK(saw_fake);
}

// (d) Empty router / k==0 degrade gracefully.
void test_router_edge_cases() {
    rag::KnowledgeRouter empty;
    CHECK(empty.retrieve("anything", 5).empty());

    auto corpus = make_corpus();
    auto src = std::make_shared<rag::CorpusSource>("docs", corpus, kNoEmbed);
    rag::KnowledgeRouter router;
    router.add(src);
    router.add(nullptr);                 // nullptr ignored
    CHECK(router.source_count() == 1);
    CHECK(router.retrieve("kubernetes", 0).empty());  // k==0
}

// (e) Pipeline: rerank → compress yields ranked, compressed Context.
void test_pipeline_rerank_compress() {
    auto corpus = make_corpus();
    rag::CorpusSource src("docs", corpus, kNoEmbed);

    // Seed a wide pool, then run the composable pipeline.
    auto ctx = rag::Context::from_hits(
        "kubernetes deployment replicas",
        src.retrieve("kubernetes deployment replicas", 30));
    CHECK(!ctx.empty());

    rag::Pipeline pipe;
    pipe.add(std::make_shared<rag::RerankStage>(/*out_k=*/2))
        .add(std::make_shared<rag::CompressStage>(/*target_chars=*/80));
    CHECK(pipe.stage_count() == 2);

    auto out = pipe.run(std::move(ctx));
    CHECK(out.size() <= 2);              // rerank narrowed to out_k
    CHECK(!out.empty());

    // Top hit is the kubernetes chunk; compression produced a span.
    const auto& top = out.chunks.front();
    CHECK(top.hit.chunk != nullptr);
    CHECK(top.hit.chunk->path == "k8s.md");
    CHECK(!top.compressed.empty());                       // compress ran
    CHECK(top.text() == std::string_view{top.compressed}); // text() prefers compressed
    CHECK(top.compressed.size() <= top.hit.chunk->text.size());
    // Provenance survives the full pipeline.
    CHECK(top.hit.source == &src);
}

// (f) ContextChunk::text() falls back to full chunk body when not compressed.
void test_context_chunk_text_fallback() {
    auto corpus = make_corpus();
    rag::CorpusSource src("docs", corpus, kNoEmbed);
    auto ctx = rag::Context::from_hits("kubernetes", src.retrieve("kubernetes", 1));
    CHECK(!ctx.empty());
    const auto& c = ctx.chunks.front();
    CHECK(c.compressed.empty());
    CHECK(c.text() == std::string_view{c.hit.chunk->text});  // full body
}

// (g) The SAME chunk (identical path + line span) surfaced by two sources is
// collapsed to ONE fused entry, and its RRF score is REINFORCED (sum of both
// per-list contributions) rather than duplicated. This is the whole point of
// cross-source fusion; a regression here silently double-lists chunks.
void test_router_cross_source_dedup() {
    // Two sources advertising the identical logical chunk, plus one unique
    // chunk only the second source has.
    auto a = std::make_shared<IdSource>("a", "shared.md", 1, 5, "shared content");
    auto b = std::make_shared<IdSource>("b", "shared.md", 1, 5, "shared content");
    auto c = std::make_shared<IdSource>("c", "unique.md", 1, 3, "unique content");

    rag::KnowledgeRouter router;
    router.add(a);
    router.add(b);
    router.add(c);

    auto hits = router.retrieve("content", 10);

    // shared.md must appear EXACTLY once despite two sources returning it.
    int shared_count = 0, unique_count = 0;
    double shared_score = 0.0, unique_score = 0.0;
    for (const auto& h : hits) {
        if (h.chunk->path == "shared.md") { ++shared_count; shared_score = h.score; }
        if (h.chunk->path == "unique.md") { ++unique_count; unique_score = h.score; }
    }
    CHECK(shared_count == 1);                 // collapsed, not duplicated
    CHECK(unique_count == 1);
    // Reinforced: shared chunk got RRF contributions from TWO lists (both at
    // rank 0 → 2/(60+1)), the unique chunk from ONE (1/(60+1)). So the shared
    // chunk's fused score must be (about) double and it must rank first.
    CHECK(shared_score > unique_score);
    CHECK(hits.front().chunk->path == "shared.md");
}

} // namespace

int main() {
    test_corpus_source_provenance();
    test_router_single_source();
    test_router_multi_source_fusion();
    test_router_edge_cases();
    test_pipeline_rerank_compress();
    test_context_chunk_text_fallback();
    test_router_cross_source_dedup();

    if (g_failures == 0) {
        std::printf("knowledge_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "knowledge_test: %d failure(s)\n", g_failures);
    return 1;
}
