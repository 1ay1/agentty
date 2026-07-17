#pragma once
// agentty::rag — reranking + extractive context compression.
//
// These are the two SOTA pipeline stages that sit BETWEEN wide retrieval and
// the LLM, and both are pure, deterministic, model-free functions (so they
// add zero dependencies and zero network cost, and are fully unit-testable):
//
//   • rerank() — the "most important stage" of modern RAG. Retrieve WIDE
//     (a big candidate pool from hybrid BM25+dense+RRF), then re-score each
//     candidate with cheap lexical/structural signals the first-pass fusion
//     ignores — exact query-term coverage, phrase proximity, title/path
//     match — and re-sort. This is a feature-fusion reranker (the weighted
//     score the literature describes); it consistently lifts precision@k
//     because first-pass dense/BM25 ranking is recall-oriented and noisy at
//     the top. A cross-encoder would do better but needs a model we can't
//     assume; this captures most of the gain for free.
//
//   • compress() — context compression. Don't dump a whole 1600-char chunk
//     (much of it irrelevant) into a weak model's small window: split the
//     chunk into sentences, score each by query relevance, and keep only the
//     best contiguous span up to a token/char budget. Turns "20k noisy
//     tokens" into "2k useful tokens", which is exactly what lets a 7B with
//     a small context actually benefit from RAG.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/rag/rag.hpp"

namespace agentty::rag {

// Tunable weights for the feature-fusion reranker. Defaults chosen so the
// first-pass fused score still dominates (it already blends BM25+dense) while
// the cheap lexical/structural signals break ties and pull exact-coverage
// passages up. All terms are normalized to [0,1] before weighting.
struct RerankWeights {
    double fused          = 0.40;  // first-pass RRF score (BM25+dense)
    double term_coverage  = 0.25;  // fraction of distinct query terms present
    double proximity      = 0.15;  // query terms appearing close together
    double path_match     = 0.10;  // query term appears in the file path
    double phrase_match   = 0.10;  // the full query phrase appears verbatim
};

// Re-score and re-sort `hits` against `query`, returning the top `out_k`.
// `hits` is expected to be the WIDE candidate pool from Corpus::search; the
// reranker narrows it. Deterministic; no network. Stable tie-break by the
// chunk's original position so equal scores keep first-pass order.
[[nodiscard]] std::vector<Hit>
rerank(std::string_view query, std::vector<Hit> hits,
       std::size_t out_k, const RerankWeights& w = {});

// Extractive context compression. Split `text` into sentences, score each by
// overlap with the query terms, and return the best contiguous run of
// sentences whose combined length stays under `target_chars`. Always returns
// a non-empty span when `text` is non-empty (falls back to a head slice when
// nothing matches). The returned span preserves original order + spacing.
[[nodiscard]] std::string
compress(std::string_view query, std::string_view text,
         std::size_t target_chars = 600);

// Helper exposed for tests: lowercase alphanumeric query terms (≥2 chars),
// deduplicated, preserving first-seen order. English STOPWORDS are dropped
// ("how does the X work" → {x, work}): glue words carry no retrieval signal
// and, worse, inflate term-coverage/proximity features toward chunks full of
// prose filler. Falls back to the unfiltered tokens when filtering would
// leave nothing (all-stopword query).
[[nodiscard]] std::vector<std::string> query_terms(std::string_view query);

// True when `token` (already lowercase) is in the shared English stopword
// set used by query_terms and the CRAG query distiller.
[[nodiscard]] bool is_stopword(std::string_view token) noexcept;

// ── Neural (cross-encoder style) reranking via Ollama ─────────────────────
//
// A cross-encoder is the SOTA reranker: for each (query, doc) pair it runs a
// full transformer pass and outputs a relevance score. We approximate this
// using a generative model (already running on Ollama) with a scoring prompt.
// Each chunk is scored independently, then re-sorted. This beats lexical
// reranking by 10-20% precision@k but costs one LLM call per chunk.
//
// OPT-IN: expensive (N LLM calls for N chunks). Only call when the user
// explicitly enables it or the corpus is small. On any failure, degrades to
// the lexical feature-fusion reranker above.

struct NeuralRerankConfig {
    std::string host  = "127.0.0.1";
    std::uint16_t port = 11434;
    std::string model;        // generative model (e.g. "llama3.2"); empty → skip
    std::size_t batch_size = 4;  // parallel scoring (bounded by Ollama concurrency)
    double timeout_s = 30.0;  // per-batch timeout
};

// Neural rerank: score each hit via Ollama with a relevance prompt, re-sort.
// Returns at most `out_k` hits. On any failure, returns the input truncated to
// out_k (graceful degradation). The input should already be the narrow pool
// from the lexical reranker (don't pass 1000 chunks — costs 1000 LLM calls).
[[nodiscard]] std::vector<Hit>
neural_rerank(std::string_view query, std::vector<Hit> hits,
              std::size_t out_k, const NeuralRerankConfig& cfg);

// ── MMR (Maximal Marginal Relevance) diversification ───────────────────────
//
// After reranking, the top-k can contain near-duplicate chunks (e.g. overlapping
// windows from the same file). MMR greedily selects chunks that are both relevant
// AND diverse: each selection penalizes candidates too similar to already-selected.
//
// MMR(d) = λ * sim(q, d) - (1-λ) * max_{d' ∈ S} sim(d, d')
//
// λ=1.0 → pure relevance (no diversification), λ=0.0 → pure diversity.
// Default λ=0.7 balances relevance with diversity.

[[nodiscard]] std::vector<Hit>
mmr_diversify(std::vector<Hit> hits, std::size_t out_k, double lambda = 0.7);

} // namespace agentty::rag
