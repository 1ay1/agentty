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
// deduplicated, preserving first-seen order.
[[nodiscard]] std::vector<std::string> query_terms(std::string_view query);

} // namespace agentty::rag
