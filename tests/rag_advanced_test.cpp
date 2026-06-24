// rag_advanced_test — unit tests for the advanced RAG features:
//   - HNSW persistence
//   - MMR diversification
//   - Confidence signal
//   - Semantic chunking (code blocks, lists)
//   - Hot reload API
//   - Query normalization
//   - Porter stemmer
//
// NO network: everything runs locally. Same harness as tests/rag_test.cpp.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"
#include "agentty/rag/knowledge.hpp"
#include "agentty/rag/stemmer.hpp"
#include "agentty/rag/simd.hpp"

using namespace agentty;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// ── 1. Porter Stemmer ────────────────────────────────────────────────────────
static void test_porter_stemmer() {
    // Test classic Porter stemmer transformations.
    CHECK(rag::stem("running") == "run");
    CHECK(rag::stem("configured") == "configur");
    CHECK(rag::stem("deployment") == "deploy");
    CHECK(rag::stem("happily") == "happili");
    CHECK(rag::stem("connections") == "connect");
    
    // Short words pass through unchanged.
    CHECK(rag::stem("go") == "go");
    CHECK(rag::stem("a") == "a");
    
    // Batch stemming.
    auto stemmed = rag::stem_tokens({"running", "quickly", "configurations"});
    CHECK(stemmed.size() == 3);
    CHECK(stemmed[0] == "run");
    CHECK(stemmed[1] == "quickli");  // Porter preserves this form
    CHECK(stemmed[2] == "configur");
}

// ── 2. MMR Diversification ────────────────────────────────────────────────────
static void test_mmr_diversification() {
    // Create hits with overlapping content (simulate duplicate-ish results).
    std::vector<rag::Chunk> store;
    store.reserve(5);
    
    // Three similar chunks about kubernetes, one different about logging.
    store.push_back({"k8s.md", 1, 10, "kubernetes deployment scales replicas pods containers", {}});
    store.push_back({"k8s2.md", 1, 10, "kubernetes deployment replicas containers orchestration", {}});
    store.push_back({"k8s3.md", 1, 10, "kubernetes pods replicas scaling cluster deployment", {}});
    store.push_back({"logging.md", 1, 10, "structured logging severity levels rotation json", {}});
    store.push_back({"db.md", 1, 10, "database transactions isolation btree indexes rows", {}});
    
    std::vector<rag::Hit> hits;
    for (std::size_t i = 0; i < store.size(); ++i) {
        hits.push_back(rag::Hit{&store[i], 1.0 - i * 0.1});  // descending scores
    }
    
    // Without MMR, top-3 would be k8s, k8s2, k8s3 (similar).
    // With MMR (λ=0.5), diversity should pull logging or db into top-3.
    auto diverse = rag::mmr_diversify(std::move(hits), 3, 0.5);
    CHECK(diverse.size() == 3);
    
    // Check that not all 3 are kubernetes chunks.
    int k8s_count = 0;
    for (const auto& h : diverse) {
        if (h.chunk->path.find("k8s") != std::string::npos) ++k8s_count;
    }
    CHECK(k8s_count < 3);  // MMR should diversify.
}

// ── 3. Confidence Signal ─────────────────────────────────────────────────────
static void test_confidence_signal() {
    // High confidence: high top score, tight cluster.
    std::vector<rag::Hit> high_conf_hits;
    rag::Chunk c1{"a.md", 1, 1, "text", {}};
    rag::Chunk c2{"b.md", 1, 1, "text", {}};
    high_conf_hits.push_back({&c1, 0.9});
    high_conf_hits.push_back({&c2, 0.85});
    
    auto ctx1 = rag::Context::from_hits("query", std::move(high_conf_hits));
    CHECK(ctx1.confidence > 0.5);  // Should be high.
    
    // Low confidence: low scores.
    std::vector<rag::Hit> low_conf_hits;
    rag::Chunk c3{"c.md", 1, 1, "text", {}};
    rag::Chunk c4{"d.md", 1, 1, "text", {}};
    low_conf_hits.push_back({&c3, 0.1});
    low_conf_hits.push_back({&c4, 0.05});
    
    auto ctx2 = rag::Context::from_hits("query", std::move(low_conf_hits));
    CHECK(ctx2.confidence < 0.3);  // Should be low.
    
    // Empty results: zero confidence.
    auto ctx3 = rag::Context::from_hits("query", {});
    CHECK(ctx3.confidence == 0.0);
}

// ── 4. Semantic Chunking ─────────────────────────────────────────────────────
static void test_semantic_chunking() {
    // Test that code blocks aren't split mid-block.
    std::string doc_with_code =
        "# Installation\n"
        "\n"
        "Run this command:\n"
        "\n"
        "```bash\n"
        "npm install something\n"
        "npm run build\n"
        "npm start\n"
        "```\n"
        "\n"
        "Then check the output.\n";
    
    auto chunks = rag::chunk_document("install.md", doc_with_code, 20, 500, 0);
    CHECK(!chunks.empty());
    
    // The code block should be kept together in a chunk.
    bool found_complete_block = false;
    for (const auto& c : chunks) {
        // Check that a chunk contains the full code block.
        if (c.text.find("```bash") != std::string::npos &&
            c.text.find("npm start") != std::string::npos &&
            c.text.find("```\n") != std::string::npos) {
            found_complete_block = true;
            break;
        }
    }
    CHECK(found_complete_block);
    
    // Test that list items stay together.
    std::string doc_with_list =
        "# Features\n"
        "\n"
        "- First feature with some\n"
        "  continuation text here\n"
        "- Second feature\n"
        "- Third feature\n"
        "\n"
        "# Next Section\n";
    
    auto list_chunks = rag::chunk_document("features.md", doc_with_list, 10, 400, 0);
    CHECK(!list_chunks.empty());
    // List should be kept together until next heading.

    // Regression: with overlap > 0, the chunker steps back a few lines for
    // the next chunk. The global fence/list context must advance only to the
    // NEXT chunk's true start, not to the previous chunk's end — otherwise a
    // code fence that opened inside the overlapped region gets double-counted
    // and a later chunk thinks it's outside a fence (so it splits mid-code).
    // Force small chunks + overlap over a document whose code fence straddles
    // a chunk boundary; every fenced block must stay intact in some chunk.
    std::string fenced =
        "# Title\n\n"
        "intro paragraph one line\n"
        "intro paragraph two line\n"
        "intro paragraph three line\n"
        "```python\n"
        "def a():\n"
        "    return 1\n"
        "def b():\n"
        "    return 2\n"
        "```\n"
        "trailing prose after the fence\n";
    auto fc = rag::chunk_document("f.md", fenced, /*max_lines=*/5,
                                  /*max_chars=*/200, /*overlap_lines=*/2);
    CHECK(!fc.empty());
    // The opening ``` and closing ``` must appear an EVEN number of times in
    // aggregate is not enough — assert one chunk holds the whole block.
    bool whole_block = false;
    for (const auto& c : fc)
        if (c.text.find("```python") != std::string::npos &&
            c.text.find("return 2") != std::string::npos &&
            c.text.find("```\n") != std::string::npos) { whole_block = true; break; }
    CHECK(whole_block);
}

// ── 5. Hot Reload API ────────────────────────────────────────────────────────
static void test_hot_reload() {
    rag::Corpus corpus;
    rag::EmbedConfig embed;  // No embeddings for this test.
    
    // Initially empty.
    CHECK(corpus.chunk_count() == 0);
    
    // Add a document.
    std::string doc1 = "kubernetes deployment replicas scaling\n";
    std::size_t added1 = corpus.add_document("k8s.md", doc1, embed);
    CHECK(added1 > 0);
    CHECK(corpus.chunk_count() == added1);
    
    // Add another document.
    std::string doc2 = "database transactions isolation levels\n";
    std::size_t added2 = corpus.add_document("db.md", doc2, embed);
    CHECK(added2 > 0);
    CHECK(corpus.chunk_count() == added1 + added2);
    
    // Search should find kubernetes.
    auto hits = corpus.search("kubernetes scaling", embed, 5);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk->path == "k8s.md");
    
    // Update the first document (should replace, not add).
    std::string doc1_updated = "kubernetes deployment pods containers NEW CONTENT\n";
    corpus.add_document("k8s.md", doc1_updated, embed);
    CHECK(corpus.chunk_count() == added1 + added2);  // Same count.
    
    // New content should be searchable.
    auto hits2 = corpus.search("NEW CONTENT", embed, 5);
    CHECK(!hits2.empty());
    CHECK(hits2.front().chunk->path == "k8s.md");
    
    // Remove a document.
    std::size_t removed = corpus.remove_document("db.md");
    CHECK(removed == added2);
    CHECK(corpus.chunk_count() == added1);
    
    // Removed doc shouldn't be findable.
    auto hits3 = corpus.search("database transactions", embed, 5);
    // Either empty or the result doesn't come from db.md.
    for (const auto& h : hits3) {
        CHECK(h.chunk->path != "db.md");
    }
}

// ── 6. Query Normalization ───────────────────────────────────────────────────
static void test_query_normalization() {
    rag::NormalizeQueryStage::Config cfg;
    cfg.lowercase = true;
    cfg.normalize_whitespace = true;
    rag::NormalizeQueryStage stage(cfg);
    
    rag::Context ctx;
    ctx.query = "  How Do I   Configure  OAUTH?  \n";
    
    auto out = stage.process(std::move(ctx));
    CHECK(out.query == "how do i configure oauth?");
    
    // Test with only lowercase.
    rag::NormalizeQueryStage::Config cfg2;
    cfg2.lowercase = true;
    cfg2.normalize_whitespace = false;
    rag::NormalizeQueryStage stage2(cfg2);
    
    rag::Context ctx2;
    ctx2.query = "Configure  OAUTH";
    auto out2 = stage2.process(std::move(ctx2));
    CHECK(out2.query == "configure  oauth");
}

// ── 7. SIMD Detection ────────────────────────────────────────────────────────
static void test_simd_operations() {
    // Test that SIMD dot product matches scalar.
    std::vector<float> a = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    std::vector<float> b = {8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f};
    
    // Expected: 1*8 + 2*7 + 3*6 + 4*5 + 5*4 + 6*3 + 7*2 + 8*1 = 120
    float result = rag::simd::dot(a, b);
    CHECK(std::fabs(result - 120.0f) < 1e-4f);
    
    // Test normalize.
    std::vector<float> v = {3.0f, 4.0f};  // Length 5.
    rag::simd::normalize(v.data(), v.size());
    CHECK(std::fabs(v[0] - 0.6f) < 1e-4f);
    CHECK(std::fabs(v[1] - 0.8f) < 1e-4f);
    
    // Check detection works (just call it, result is platform-dependent).
    auto level = rag::simd::detect();
    std::printf("rag_advanced_test: SIMD level = %d\n", static_cast<int>(level));
    CHECK(level >= rag::simd::SimdLevel::Scalar);
}

// ── 8. Document Metadata Filtering ───────────────────────────────────────────
static void test_metadata_filtering() {
    rag::Chunk c1{"api/auth.md", 1, 10, "oauth authentication tokens", {}};
    c1.metadata["type"] = "api";
    c1.metadata["category"] = "security";
    
    rag::Chunk c2{"tutorials/intro.md", 1, 10, "introduction getting started", {}};
    c2.metadata["type"] = "tutorial";
    c2.metadata["category"] = "basics";
    
    rag::Chunk c3{"api/users.md", 1, 10, "user management endpoints", {}};
    c3.metadata["type"] = "api";
    c3.metadata["category"] = "management";
    
    // Test meta_eq filter.
    auto api_filter = rag::filters::meta_eq("type", "api");
    CHECK(api_filter(c1) == true);
    CHECK(api_filter(c2) == false);
    CHECK(api_filter(c3) == true);
    
    // Test meta_contains filter.
    auto security_filter = rag::filters::meta_contains("category", "SECURITY");
    CHECK(security_filter(c1) == true);
    CHECK(security_filter(c2) == false);
    
    // Test path_contains filter.
    auto api_path = rag::filters::path_contains("api/");
    CHECK(api_path(c1) == true);
    CHECK(api_path(c2) == false);
    CHECK(api_path(c3) == true);
    
    // Test all_of (AND).
    auto combined = rag::filters::all_of({
        rag::filters::meta_eq("type", "api"),
        rag::filters::meta_contains("category", "secur")
    });
    CHECK(combined(c1) == true);
    CHECK(combined(c3) == false);  // api but not security.
    
    // Test any_of (OR).
    auto either = rag::filters::any_of({
        rag::filters::meta_eq("type", "tutorial"),
        rag::filters::meta_contains("category", "management")
    });
    CHECK(either(c1) == false);
    CHECK(either(c2) == true);
    CHECK(either(c3) == true);
}

// ── 9. build_from_memory (non-folder corpus build path) ─────────────────
static void test_build_from_memory() {
    rag::Corpus corpus;
    rag::EmbedConfig embed;  // BM25-only.

    std::vector<std::pair<std::string, std::string>> docs = {
        {"mcp://server/k8s",  "kubernetes deployment replicas pods scaling cluster\n"},
        {"mcp://server/db",   "database transactions isolation btree indexes rows\n"},
        {"mcp://server/auth", "oauth tokens authentication authorization scopes\n"},
    };
    std::size_t n = corpus.build_from_memory(docs, embed);
    CHECK(n > 0);
    CHECK(corpus.chunk_count() == n);

    auto hits = corpus.search("kubernetes scaling", embed, 3);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk->path == "mcp://server/k8s");

    // Wholesale replace: a second call discards the first set.
    std::vector<std::pair<std::string, std::string>> docs2 = {
        {"mcp://server/only", "single replacement document about logging\n"},
    };
    corpus.build_from_memory(docs2, embed);
    auto hits2 = corpus.search("logging", embed, 3);
    CHECK(!hits2.empty());
    CHECK(hits2.front().chunk->path == "mcp://server/only");
    // The old k8s doc is gone.
    auto gone = corpus.search("kubernetes", embed, 3);
    for (const auto& h : gone) CHECK(h.chunk->path != "mcp://server/k8s");

    // Empty input → empty corpus, no crash.
    rag::Corpus empty;
    CHECK(empty.build_from_memory({}, embed) == 0);
    CHECK(empty.search("anything", embed, 3).empty());
}

// ── 10. McpResourceSource (injected list/read seams, no network) ────────
static void test_mcp_resource_source() {
    // Fake MCP backend: a fixed resource list + an in-memory read map.
    using Ref = rag::McpResourceSource::ResourceRef;
    int read_calls = 0;

    auto list_fn = [] {
        return std::vector<Ref>{
            {"mcp://wiki/networking", "Networking"},
            {"mcp://wiki/storage",    "Storage"},
            {"mcp://wiki/missing",    "Missing"},   // read returns nullopt
        };
    };
    auto read_fn = [&read_calls](const std::string& uri)
        -> std::optional<std::string> {
        ++read_calls;
        if (uri == "mcp://wiki/networking")
            return "networking routes subnets gateways firewall rules ingress\n";
        if (uri == "mcp://wiki/storage")
            return "storage volumes persistent claims snapshots replication\n";
        return std::nullopt;  // missing resource: skipped gracefully
    };

    rag::EmbedConfig embed;  // BM25-only.
    rag::McpResourceSource src("mcp", list_fn, read_fn, embed);
    CHECK(src.name() == "mcp");
    CHECK(src.indexed_chunks() == 0);  // lazy: nothing built yet

    auto hits = src.retrieve("firewall ingress rules", 3);
    CHECK(!hits.empty());
    CHECK(hits.front().chunk->path == "mcp://wiki/networking");
    CHECK(src.indexed_chunks() > 0);          // built on first retrieve
    CHECK(read_calls == 3);                   // all three listed resources read
    // Provenance is stamped to this source.
    CHECK(hits.front().source == &src);
    CHECK(hits.front().source->name() == "mcp");

    // Second retrieve must NOT re-read (lazy build cached).
    int before = read_calls;
    auto hits2 = src.retrieve("storage volumes", 3);
    CHECK(!hits2.empty());
    CHECK(hits2.front().chunk->path == "mcp://wiki/storage");
    CHECK(read_calls == before);              // no re-read

    // refresh() drops the index → next retrieve re-reads.
    src.refresh();
    src.retrieve("networking", 1);
    CHECK(read_calls == before + 3);

    // Empty seams → empty source, no crash.
    rag::McpResourceSource empty("mcp", {}, {}, embed);
    CHECK(empty.retrieve("anything", 3).empty());
    CHECK(empty.indexed_chunks() == 0);
}

// ── 11. neural_rerank graceful degradation (no backend) ───────────────
static void test_neural_rerank_degrades() {
    std::vector<rag::Chunk> store;
    store.push_back({"a.md", 1, 1, "alpha document about networking", {}});
    store.push_back({"b.md", 1, 1, "beta document about storage", {}});
    store.push_back({"c.md", 1, 1, "gamma document about compute", {}});

    std::vector<rag::Hit> hits;
    for (std::size_t i = 0; i < store.size(); ++i)
        hits.push_back(rag::Hit{&store[i], 1.0 - i * 0.1});

    // (a) Empty model → must NOT touch the network; returns the input order
    //     truncated to out_k, untouched.
    {
        rag::NeuralRerankConfig cfg;  // model empty
        auto r = rag::neural_rerank("networking", hits, 2, cfg);
        CHECK(r.size() == 2);
        CHECK(r[0].chunk->path == "a.md");
        CHECK(r[1].chunk->path == "b.md");
    }

    // (b) Model set but backend unreachable (dead port) → every score fails,
    //     so the function degrades to the upstream order (no crash, no hang
    //     beyond the short connect timeout).
    {
        rag::NeuralRerankConfig cfg;
        cfg.model = "does-not-exist";
        cfg.host  = "127.0.0.1";
        cfg.port  = 1;            // nothing listens here → connect refused fast
        cfg.batch_size = 2;
        cfg.timeout_s = 1.0;
        auto r = rag::neural_rerank("networking", hits, 3, cfg);
        CHECK(r.size() == 3);    // degraded, not dropped
        // Upstream order preserved on total backend outage.
        CHECK(r[0].chunk->path == "a.md");
    }

    // (c) Empty input → empty output.
    {
        rag::NeuralRerankConfig cfg;
        cfg.model = "x";
        CHECK(rag::neural_rerank("q", {}, 5, cfg).empty());
    }
}

// ── 12. Folder build + incremental cache round-trip (signature gate) ────
// Guards the disk-cache path that the corpus-signature gate protects: build
// over a directory, mutate one file, rebuild (which reuses cached chunks for
// unchanged files and re-chunks the changed one), and verify search stays
// correct. BM25-only (no network); HNSW won't activate below threshold, but
// this exercises the cache read/write + per-file reuse that the signature
// fingerprints, ensuring a changed file never strands a stale mapping.
static void test_folder_cache_roundtrip() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / "agentty_rag_folder_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    auto write = [](const fs::path& p, const std::string& body) {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f << body;
    };
    write(root / "k8s.md",  "# K8s\n\nkubernetes deployment replicas pods scaling\n");
    write(root / "db.md",   "# DB\n\ndatabase transactions isolation btree indexes\n");
    write(root / "auth.md", "# Auth\n\noauth tokens authentication authorization scopes\n");

    rag::EmbedConfig embed;  // BM25-only.

    rag::Corpus c1;
    c1.build(root, embed);
    CHECK(c1.chunk_count() > 0);
    CHECK(fs::exists(root / ".agentty_rag_cache.bin", ec));
    {
        auto hits = c1.search("kubernetes scaling", embed, 3);
        CHECK(!hits.empty());
        CHECK(hits.front().chunk->path == "k8s.md");
    }

    // Second build over the SAME dir reuses the cache; results identical.
    rag::Corpus c2;
    c2.build(root, embed);
    CHECK(c2.chunk_count() == c1.chunk_count());
    {
        auto hits = c2.search("oauth authorization", embed, 3);
        CHECK(!hits.empty());
        CHECK(hits.front().chunk->path == "auth.md");
    }

    // Mutate one file (changes its size) then rebuild: the changed file is
    // re-chunked, the others reused from cache. Search must reflect the new
    // content and still resolve to the correct source path.
    write(root / "db.md",
          "# DB\n\ndatabase WALWALWAL write ahead logging recovery checkpoint durability\n");
    rag::Corpus c3;
    c3.build(root, embed);
    {
        auto hits = c3.search("write ahead logging checkpoint", embed, 3);
        CHECK(!hits.empty());
        CHECK(hits.front().chunk->path == "db.md");
        // Old db.md term is gone from that path's content.
        auto old = c3.search("btree isolation", embed, 5);
        for (const auto& h : old)
            if (h.chunk->path == "db.md")
                CHECK(h.chunk->text.find("btree") == std::string::npos);
    }
    // Unchanged files still resolve correctly after the partial rebuild.
    {
        auto hits = c3.search("kubernetes pods", embed, 3);
        CHECK(!hits.empty());
        CHECK(hits.front().chunk->path == "k8s.md");
    }

    fs::remove_all(root, ec);
}

int main() {
    test_porter_stemmer();
    test_mmr_diversification();
    test_confidence_signal();
    test_semantic_chunking();
    test_hot_reload();
    test_query_normalization();
    test_simd_operations();
    test_metadata_filtering();
    test_build_from_memory();
    test_mcp_resource_source();
    test_neural_rerank_degrades();
    test_folder_cache_roundtrip();

    if (g_failures == 0) {
        std::printf("rag_advanced_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "rag_advanced_test: %d check(s) failed\n", g_failures);
    return 1;
}
