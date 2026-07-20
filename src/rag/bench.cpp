// agentty::rag — eval harness implementation (`agentty rag-bench`).
//
// Deterministic known-item benchmark over the user's own corpus. See
// bench.hpp for the design. Rungs share ONE corpus build; each adds a stage
// so a delta is attributable to exactly one stage.

#include "agentty/rag/bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"
#include "agentty/rag/stemmer.hpp"

namespace agentty::rag::bench {

namespace fs = std::filesystem;

namespace {

// Same resolution the DocRetriever uses (kept in sync by hand — the
// retriever's version is a private static in the tool backend).
fs::path resolve_root(const std::string& arg) {
    if (!arg.empty()) return fs::path{arg};
    if (const char* env = std::getenv("AGENTTY_DOCS_DIR"); env && env[0])
        return fs::path{env};
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    if (auto d = cwd / "docs"; fs::is_directory(d, ec)) return d;
    if (auto d = cwd / ".agentty" / "knowledge"; fs::is_directory(d, ec)) return d;
    return {};
}

EmbedConfig embed_from_env() {
    EmbedConfig cfg;
    if (const char* m = std::getenv("AGENTTY_EMBED_MODEL"); m && m[0]) cfg.model = m;
    else cfg.model = "nomic-embed-text";
    if (const char* h = std::getenv("AGENTTY_OLLAMA_HOST"); h && h[0]) {
        std::string hs{h};
        if (auto colon = hs.rfind(':'); colon != std::string::npos) {
            cfg.host = hs.substr(0, colon);
            try {
                int p = std::stoi(hs.substr(colon + 1));
                if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
            } catch (...) { /* default port */ }
        } else cfg.host = hs;
    }
    return cfg;
}

struct Metrics {
    double recall = 0, mrr = 0, ndcg = 0;
    double us = 0;                       // mean per-query latency
    void add_rank(std::ptrdiff_t rank) { // -1 = miss; 0-based hit rank
        if (rank < 0) return;
        recall += 1.0;
        mrr    += 1.0 / static_cast<double>(rank + 1);
        ndcg   += 1.0 / std::log2(static_cast<double>(rank) + 2.0);
    }
    void finalize(std::size_t n) {
        if (n == 0) return;
        recall /= static_cast<double>(n);
        mrr    /= static_cast<double>(n);
        ndcg   /= static_cast<double>(n);
        us     /= static_cast<double>(n);
    }
};

// The gold LABEL for a synthesized query: the exact chunk it was drawn from.
// We keep its identity (path + owning source + line span) rather than a bare
// "path:line_start" string so rank_of can credit a COVERING chunk — see below.
struct Gold {
    std::string  path;
    const void*  source = nullptr;   // owning KnowledgeSource (opaque identity)
    int          line_start = 0;
    int          line_end   = 0;
};

// A returned chunk counts as a HIT when it COVERS the gold region: same file,
// same owning source, and overlapping line spans. Exact-identity matching
// ("path:line_start") systematically undercounts the reranker, whose dedup
// step deliberately collapses overlapping-window siblings into ONE survivor —
// the survivor's line_start may differ from gold's even though it contains the
// exact gold region and is what the user actually reads. Coverage matching
// measures true retrieval success, which is what every downstream stage (dedup,
// parent-stitch) is engineered to preserve.
std::ptrdiff_t rank_of(const std::vector<Hit>& hits, const Gold& gold) {
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const Chunk* c = hits[i].chunk;
        if (!c) continue;
        if (c->path != gold.path) continue;
        if (static_cast<const void*>(hits[i].source) != gold.source) continue;
        // Inclusive line-range overlap (touching ranges count as covering).
        if (c->line_start <= gold.line_end && gold.line_start <= c->line_end)
            return static_cast<std::ptrdiff_t>(i);
    }
    return -1;
}

// Local lowercase alnum-run tokenizer (bm25's tokenize() is file-private).
// Stemming is applied only for the df LOOKUP (the index vocabulary is
// stemmed when BM25_USE_STEMMER is on); the emitted query keeps the surface
// form — corpus.search stems it again on its own path.
void simple_tokens(std::string_view s, std::vector<std::string>& out) {
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3) out.push_back(cur);
        cur.clear();
    };
    for (unsigned char c : s) {
        if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
        else flush();
    }
    flush();
}

double doc_freq(const Bm25Index& idx, const std::string& term) {
    if (auto it = idx.term_ids.find(term); it != idx.term_ids.end())
        return static_cast<double>(idx.postings[it->second].size());
    if (auto it = idx.term_ids.find(stem(term)); it != idx.term_ids.end())
        return static_cast<double>(idx.postings[it->second].size());
    return 1.0;
}

} // namespace

int run(const std::string& docs_root, std::size_t queries, std::size_t k) {
    auto root = resolve_root(docs_root);
    if (root.empty()) {
        std::fprintf(stderr,
            "rag-bench: no docs corpus. Set AGENTTY_DOCS_DIR or create ./docs\n");
        return 1;
    }

    const EmbedConfig embed = embed_from_env();
    std::printf("rag-bench: indexing %s ...\n", root.string().c_str());
    Corpus corpus;
    corpus.build(root, embed);
    const auto& chunks = corpus.raw_chunks();
    if (corpus.chunk_count() < 4) {
        std::fprintf(stderr, "rag-bench: corpus too small (%zu chunks)\n",
                     corpus.chunk_count());
        return 1;
    }
    std::printf("rag-bench: %zu chunks, %s embeddings\n\n",
                corpus.chunk_count(),
                corpus.has_embeddings() ? "WITH" : "NO (BM25-only —"
                " start a local Ollama with an embed model for the dense rungs)");

    // ── Synthesize known-item queries ────────────────────────────────────
    // Per sampled chunk: score every term by tf(chunk) × idf(corpus) and
    // take the top 4 — the terms that most SELECT this chunk. Document
    // frequency comes from a throwaway BM25 build over the same chunks.
    Bm25Index idx = build_bm25(chunks);
    const double N = static_cast<double>(chunks.size());

    struct Q { std::string text; Gold gold; };
    std::vector<Q> qs;
    const std::size_t stride = std::max<std::size_t>(1, chunks.size() / queries);
    for (std::size_t ci = 0; ci < chunks.size() && qs.size() < queries; ci += stride) {
        const Chunk& c = chunks[ci];
        if (c.text.size() < 120) continue;   // fragments make degenerate queries

        // tf over this chunk's tokens (surface forms; stemmed df lookup).
        std::vector<std::string> toks;
        simple_tokens(c.text, toks);
        std::unordered_map<std::string, int> tf;
        for (auto& t : toks)
            if (!is_stopword(t)) tf[t]++;

        std::vector<std::pair<double, std::string>> scored;
        scored.reserve(tf.size());
        for (auto& [t, f] : tf) {
            const double df = doc_freq(idx, t);
            scored.emplace_back(f * std::log((N + 1.0) / (df + 0.5)), t);
        }
        if (scored.size() < 3) continue;
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string q;
        for (std::size_t i = 0; i < scored.size() && i < 4; ++i) {
            if (!q.empty()) q += ' ';
            q += scored[i].second;
        }
        qs.push_back({std::move(q),
                      Gold{c.path, nullptr, c.line_start, c.line_end}});
    }
    if (qs.empty()) {
        std::fprintf(stderr, "rag-bench: could not synthesize queries\n");
        return 1;
    }
    std::printf("rag-bench: %zu synthetic known-item queries, k=%zu\n\n",
                qs.size(), k);

    // ── The ladder ───────────────────────────────────────────────────────
    const std::size_t pool = std::max<std::size_t>(k * 5, 30);
    const EmbedConfig no_embed;   // model empty → BM25-only rung

    struct Rung { const char* name; Metrics m; };
    Rung rungs[] = {
        {"bm25-only        (lexical floor)", {}},
        {"hybrid+prf       (default retrieve)", {}},
        {"+feature-rerank  (default)", {}},
        {"+mmr             (default full funnel)", {}},
    };

    using clock = std::chrono::steady_clock;
    for (const auto& q : qs) {
        // Rung 0: BM25 only, straight top-k.
        auto t0 = clock::now();
        auto h0 = corpus.search(q.text, no_embed, k);
        rungs[0].m.us += std::chrono::duration<double, std::micro>(clock::now() - t0).count();
        rungs[0].m.add_rank(rank_of(h0, q.gold));

        // Rung 1: hybrid (dense joins when embeddings exist) wide pool → top-k.
        t0 = clock::now();
        auto wide = corpus.search(q.text, embed, pool);
        auto h1 = wide;
        if (h1.size() > k) h1.resize(k);
        rungs[1].m.us += std::chrono::duration<double, std::micro>(clock::now() - t0).count();
        rungs[1].m.add_rank(rank_of(h1, q.gold));

        // Rung 2: + feature-fusion rerank over the wide pool.
        t0 = clock::now();
        auto h2 = rerank(q.text, wide, k);
        rungs[2].m.us += std::chrono::duration<double, std::micro>(clock::now() - t0).count();
        rungs[2].m.add_rank(rank_of(h2, q.gold));

        // Rung 3: + MMR diversification (the shipped funnel's tail).
        t0 = clock::now();
        auto h3 = mmr_diversify(rerank(q.text, wide, k * 2), k, 0.75);
        rungs[3].m.us += std::chrono::duration<double, std::micro>(clock::now() - t0).count();
        rungs[3].m.add_rank(rank_of(h3, q.gold));
    }
    for (auto& r : rungs) r.m.finalize(qs.size());

    std::printf("  %-38s %9s %8s %8s %10s\n",
                "stage", "recall@k", "MRR", "nDCG", "mean µs");
    std::printf("  %-38s %9s %8s %8s %10s\n",
                "-----", "--------", "---", "----", "-------");
    for (const auto& r : rungs)
        std::printf("  %-38s %8.1f%% %8.3f %8.3f %10.0f\n",
                    r.name, r.m.recall * 100.0, r.m.mrr, r.m.ndcg, r.m.us);

    std::printf(
        "\nReading the ladder: each rung adds one stage. A rung that DROPS a\n"
        "metric on YOUR corpus is a stage worth tuning (env toggles:\n"
        "AGENTTY_RAG_PRF / AGENTTY_RAG_EMBED_RERANK / AGENTTY_RAG_PARENT ...).\n"
        "Known-item queries measure mechanics, not paraphrase — dense wins\n"
        "show mainly when a local embedder is running.\n");
    return 0;
}

} // namespace agentty::rag::bench
