// rag_expand_test — unit tests for multi-query RAG-Fusion
// (Corpus::search_fused). NO network and NO LLM: expand_query itself needs a
// live Ollama generative model, so we do NOT exercise it here. Instead we
// drive search_fused directly with hand-written query variants and an EMPTY
// EmbedConfig (so the dense branch never fires → no network), proving the
// fusion logic: multiple BM25 ranked lists fuse via a single RRF pass.
//
// Lightweight harness mirroring tests/rag_test.cpp.

#include <cstdio>
#include <string>
#include <vector>

#include "agentty/rag/rag.hpp"
#include "agentty/rag/expand.hpp"

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

// ~5 distinct-keyword chunks. The kubernetes/deploy chunk is the one all the
// hand-written query variants point at (union of: kubernetes, deployment,
// k8s, replicas, container, orchestration, manifests).
rag::Corpus make_corpus() {
    std::vector<rag::Chunk> chunks;
    auto mk = [](const char* p, const char* text) {
        rag::Chunk c; c.path = p; c.line_start = 1; c.line_end = 2; c.text = text;
        return c;  // NOTE: no embedding → BM25-only
    };
    chunks.push_back(mk("auth.md",
        "configure oauth tokens and refresh credentials securely"));
    chunks.push_back(mk("deploy.md",
        "kubernetes deployment manifests define replicas and container "
        "orchestration for k8s clusters"));
    chunks.push_back(mk("logging.md",
        "structured logging with severity levels and rotation"));
    chunks.push_back(mk("network.md",
        "load balancer ingress routes traffic to backend services"));
    chunks.push_back(mk("storage.md",
        "persistent volumes claims and storage classes for stateful sets"));

    rag::Corpus corpus;
    corpus.set_chunks_for_test(std::move(chunks));
    return corpus;
}

// (a) Multi-query fusion: several variants all pointing at the deploy chunk
//     keep it on top; fusion doesn't break ranking vs single-query search.
void test_search_fused_multi_query() {
    auto corpus = make_corpus();
    CHECK(corpus.has_embeddings() == false);
    CHECK(corpus.chunk_count() == 5);

    rag::EmbedConfig embed;  // empty model → no dense, no network

    std::vector<std::string> queries = {
        "kubernetes deployment",
        "k8s replicas",
        "container orchestration manifests",
    };
    auto hits = corpus.search_fused(queries, embed, 3);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk != nullptr);
    CHECK(hits.front().chunk->path == "deploy.md");

    // Single-query search agrees on the top hit (fusion didn't break it).
    auto single = corpus.search("kubernetes deployment", embed, 3);
    CHECK(!single.empty());
    CHECK(single.front().chunk != nullptr);
    CHECK(single.front().chunk->path == "deploy.md");

    // MORE variants all pointing at the same chunk keep it on top.
    std::vector<std::string> more = {
        "kubernetes deployment",
        "k8s replicas",
        "container orchestration manifests",
        "deployment manifests for clusters",
        "orchestration replicas container",
    };
    auto hits_more = corpus.search_fused(more, embed, 3);
    CHECK(!hits_more.empty());
    CHECK(hits_more.front().chunk != nullptr);
    CHECK(hits_more.front().chunk->path == "deploy.md");
}

// (b) Empty queries vector returns {}.
void test_search_fused_empty_queries() {
    auto corpus = make_corpus();
    rag::EmbedConfig embed;
    auto hits = corpus.search_fused({}, embed, 3);
    CHECK(hits.empty());

    // Also k == 0 returns {}.
    auto hits0 = corpus.search_fused({"kubernetes"}, embed, 0);
    CHECK(hits0.empty());
}

// (c) A single query fused == that query's single-query search (fusion of
//     one list is the identity ranking).
void test_search_fused_single_query_matches_search() {
    auto corpus = make_corpus();
    rag::EmbedConfig embed;

    const std::string q = "structured logging severity";
    auto fused  = corpus.search_fused({q}, embed, 3);
    auto single = corpus.search(q, embed, 3);

    CHECK(!fused.empty());
    CHECK(!single.empty());
    CHECK(fused.front().chunk != nullptr);
    CHECK(single.front().chunk != nullptr);
    CHECK(fused.front().chunk->path == single.front().chunk->path);
    CHECK(fused.front().chunk->path == "logging.md");
}

// (d) HyDE degradation contract. hyde_document must NEVER throw and must
//     return an empty string on every failure mode, so the caller falls back
//     to the plain query and retrieval never regresses. We can't assert a
//     real generation (no LLM in CI), only the graceful-empty paths.
void test_hyde_degrades() {
    // Empty model → no call, empty result.
    {
        rag::ExpandConfig cfg;   // model empty
        CHECK(rag::hyde_document(cfg, "how do I configure oauth").empty());
    }
    // Empty query → empty result even with a model set.
    {
        rag::ExpandConfig cfg; cfg.model = "llama3.2";
        CHECK(rag::hyde_document(cfg, "").empty());
    }
    // Unreachable backend (bogus port) → empty, no throw/hang beyond timeout.
    {
        rag::ExpandConfig cfg;
        cfg.model = "llama3.2";
        cfg.host  = "127.0.0.1";
        cfg.port  = 1;            // nothing listens here
        CHECK(rag::hyde_document(cfg, "anything").empty());
    }
}

} // namespace

int main() {
    test_search_fused_multi_query();
    test_search_fused_empty_queries();
    test_search_fused_single_query_matches_search();
    test_hyde_degrades();

    if (g_failures == 0) {
        std::printf("rag_expand_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_expand_test: %d check(s) failed\n", g_failures);
    return 1;
}
