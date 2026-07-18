#pragma once
// agentty::rag — multi-query expansion (RAG-Fusion).
//
// The query a user (or an agent) writes is usually a poor retrieval probe:
// one phrasing, often missing the vocabulary the document actually uses. The
// single highest-recall trick in modern RAG is RAG-Fusion: ask the LLM to
// rewrite the query into several alternative phrasings, retrieve for EACH,
// and fuse the ranked lists with Reciprocal Rank Fusion. A passage that's
// relevant under multiple phrasings rises to the top; vocabulary mismatch on
// any single phrasing stops being fatal.
//
// We already have a local LLM in-process (the running Ollama server), so the
// expansion is one cheap non-streaming /api/generate call — no extra
// dependency, no cloud round-trip. It is OPT-IN (each variant costs a
// retrieval pass): callers enable it explicitly. On ANY failure (no model,
// network, unpar.seable output) it returns just the original query, so
// retrieval never regresses.

#include <string>
#include <vector>

#include "agentty/rag/rag.hpp"   // EmbedConfig (host/port reused)

namespace agentty::rag {

// Config for the expansion LLM call. Reuses the Ollama host/port; `model` is
// a GENERATIVE chat model (e.g. the one the user is already chatting with),
// NOT the embedding model.
struct ExpandConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11434;
    std::string model;        // generative model; empty → no expansion
    std::size_t n = 4;        // number of alternative phrasings to request
};

// Ask the local model for up to cfg.n alternative phrasings of `query`.
// Returns a list that ALWAYS includes the original query first, followed by
// the distinct non-empty variants the model produced. On any failure returns
// {query}. Never throws.
[[nodiscard]] std::vector<std::string>
expand_query(const ExpandConfig& cfg, const std::string& query);

// HyDE — Hypothetical Document Embeddings (Gao et al. 2022).
//
// A query is a QUESTION; the passages we're searching are ANSWERS. In
// embedding space a question and its answer can sit surprisingly far apart
// (different vocabulary, different register), which caps dense recall. HyDE
// closes that gap: ask the LLM to HALLUCINATE a short, plausible answer
// passage for the query, then embed THAT hypothetical document instead of
// (or alongside) the raw query. The fake answer need not be factually
// correct — it only has to LOOK like the kind of passage that would answer
// the query, so its embedding lands in the right neighbourhood and pulls in
// the real passages nearby.
//
// Reuses the same one-shot local /api/generate call as expand_query (the
// generative model, not the embedder). OPT-IN (a generation per search) and
// degrades to an empty string on ANY failure, so the caller falls back to
// the plain query and retrieval never regresses. Never throws.
[[nodiscard]] std::string
hyde_document(const ExpandConfig& cfg, const std::string& query);

} // namespace agentty::rag
