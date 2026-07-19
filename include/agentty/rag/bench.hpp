#pragma once
// agentty::rag — the EVAL HARNESS (`agentty rag-bench`).
//
// The unglamorous truth of retrieval engineering: a pipeline you can't
// measure is a pile of vibes. This harness makes every stage's contribution
// PROVABLE on the user's own corpus, offline, in milliseconds:
//
//   1. Build (or load-from-cache) the docs corpus exactly as search_docs
//      would.
//   2. Sample chunks and synthesize a KNOWN-ITEM query per chunk — its most
//      DISCRIMINATIVE terms (high in-chunk frequency × corpus rarity, the
//      same signal PRF harvests). No LLM: deterministic, reproducible,
//      zero-cost. The (query → source chunk) pair is the gold label.
//   3. Run the retrieval ladder — BM25-only → hybrid/PRF → +rerank →
//      +MMR — over every query and report recall@k, MRR@10, nDCG@10 per
//      rung, so a regression (or a win) is attributable to ONE stage.
//
// Known-item synthesis measures the funnel's mechanics, not natural-language
// paraphrase (that needs embeddings, which the harness exercises when a
// local embedder is configured — same env as the funnel).

#include <string>

namespace agentty::rag::bench {

// Run the benchmark against `docs_root` (empty → resolve the same way
// search_docs does: $AGENTTY_DOCS_DIR, ./docs, ./doc, ./.agentty/docs).
// Prints a human-readable report to stdout. Returns a process exit code
// (0 ok, 1 no corpus / nothing to measure).
int run(const std::string& docs_root, std::size_t queries = 100,
        std::size_t k = 5);

} // namespace agentty::rag::bench
