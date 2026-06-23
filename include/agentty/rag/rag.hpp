#pragma once
// agentty::rag — lightweight document/knowledge retrieval (RAG).
//
// DESIGN (researched against SOTA agent retrieval, 2025):
//
//   • CODE retrieval is already solved by agentty's agentic search tools
//     (bash/grep/read/glob/find_definition). Claude Code & Aider converged
//     on agentic search over vector RAG for code: code questions are
//     structural/exact ("where is X defined?"), embeddings go stale on every
//     edit, and grep is always fresh + zero-setup. So this module does NOT
//     index source code.
//
//   • This module is DOCUMENT/knowledge RAG: the "7B + RAG > 70B" lever for
//     a user's own manuals / internal docs that no model was trained on. It
//     is exposed to the agent as the `search_docs` TOOL (agentic RAG) — the
//     model retrieves on demand inside its ReAct loop, which composes for
//     free with the weak-model JSON-protocol / grammar path.
//
//   • Retrieval is HYBRID + Reciprocal Rank Fusion (RRF), the single most
//     cost-effective SOTA win:
//       – BM25 (pure C++, no dependency, ALWAYS available) catches exact
//         terms / proper nouns the user typed.
//       – Dense cosine over embeddings from the already-running Ollama
//         server (/api/embed, nomic-embed-text) catches paraphrases.
//       – RRF fuses the two ranked lists without scale-matching.
//     If no embedding model is reachable, it degrades gracefully to
//     BM25-only — still useful, never blocks.
//
//   • PERF: the corpus is a flat std::vector<Chunk> with cosine/BM25 scored
//     by trivial loops (no FAISS, no vector DB). The index is built LAZILY
//     on the first search_docs call and cached on disk keyed by file hash
//     (Cursor-style incremental refresh), so cold start stays sub-ms and a
//     re-index only re-embeds changed files.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "agentty/rag/hnsw.hpp"

namespace agentty::rag {

// One retrievable unit: a bounded, line-aligned slice of a source document.
struct Chunk {
    std::string  path;        // source file, relative to the knowledge root
    int          line_start = 0;  // 1-based, inclusive
    int          line_end   = 0;  // 1-based, inclusive
    std::string  text;        // the chunk body (what gets fed to the model)

    // Dense embedding. Empty when embeddings are unavailable (BM25-only
    // mode) or not yet computed. Length == Corpus::embed_dim when present.
    std::vector<float> embedding;
};

// A retrieval hit: a chunk plus its fused relevance score.
struct Hit {
    const Chunk* chunk = nullptr;
    double       score = 0.0;   // fused RRF score (higher = more relevant)
};

// The embedding endpoint to call. Reuses the running Ollama server. When
// `model` is empty OR the host is unreachable, retrieval falls back to
// BM25-only and never errors.
struct EmbedConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11434;
    std::string model;        // e.g. "nomic-embed-text"; empty → BM25-only
};

// Embed a batch of texts via Ollama /api/embed. Returns one vector per input
// (in order), or std::nullopt on any failure (no model, network, parse) so
// the caller degrades to BM25. All vectors share the same dimension.
[[nodiscard]] std::optional<std::vector<std::vector<float>>>
embed_texts(const EmbedConfig& cfg, const std::vector<std::string>& texts);

// ── BM25 ────────────────────────────────────────────────────────────────
// Classic Okapi BM25 over the chunk corpus. Tokenization is lowercase
// alphanumeric runs; no stemming (kept simple + fast + dependency-free).

struct Bm25Index {
    // Per-term document frequency and the postings needed to score.
    // Built once from the corpus; cheap to rebuild on a re-index.
    struct Posting { std::uint32_t doc; std::uint32_t tf; };
    std::vector<std::vector<Posting>> postings;   // term-id → postings
    std::vector<std::uint32_t>        doc_len;     // chunk-id → token count
    double avg_doc_len = 0.0;
    std::size_t doc_count = 0;

    // term string → term-id. Built during indexing and owned BY the index
    // (not a module global) so query terms resolve to the same ids as the
    // postings, and multiple corpora can coexist. bm25_search reads it.
    std::unordered_map<std::string, std::uint32_t> term_ids;

    void clear();
};

// Build a BM25 index over the chunk texts.
[[nodiscard]] Bm25Index build_bm25(const std::vector<Chunk>& chunks);

// Score every chunk against the query; returns (chunk-id, score) sorted by
// score desc, truncated to `k`. Chunks with zero overlap are omitted.
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
bm25_search(const Bm25Index& idx, std::string_view query, std::size_t k);

// ── Corpus + hybrid retrieval ─────────────────────────────────────────────

class Corpus {
public:
    // Build (or load-from-cache) the corpus by indexing every text/markdown
    // file under `root`. Embeddings are computed via `embed` when its model
    // is set; otherwise the corpus is BM25-only. The on-disk cache lives at
    // root/.agentty_rag_cache.bin and is reused for files whose size+mtime
    // are unchanged (incremental re-embed). Safe to call repeatedly; cheap
    // when nothing changed.
    void build(const std::filesystem::path& root, const EmbedConfig& embed);

    // Top-k hybrid retrieval for `query`. Runs BM25 + (when embeddings are
    // present) dense cosine, fuses with Reciprocal Rank Fusion, returns the
    // top `k` hits. Never throws; returns empty when the corpus is empty.
    [[nodiscard]] std::vector<Hit> search(std::string_view query,
                                          const EmbedConfig& embed,
                                          std::size_t k) const;

    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] bool        has_embeddings() const noexcept { return embed_dim_ > 0; }
    [[nodiscard]] std::size_t embed_dim() const noexcept { return embed_dim_; }

    // Exposed for tests: install a prebuilt corpus without disk/network.
    void set_chunks_for_test(std::vector<Chunk> chunks);

private:
    void write_cache_() const;

    std::filesystem::path  root_;
    std::vector<Chunk>     chunks_;
    Bm25Index              bm25_;
    HnswIndex              hnsw_;
    bool                   hnsw_built_ = false;
    std::size_t            embed_dim_ = 0;
};

// ── Reciprocal Rank Fusion ────────────────────────────────────────────────
// RRF(d) = Σ_lists 1/(k + rank_list(d)). k=60 is the canonical constant
// (Cormack et al.). Exposed for testing; `Corpus::search` uses it.
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    double k, std::size_t out_k);

// Cosine similarity of two equal-length dense vectors. Returns 0 for a
// length mismatch or a zero vector.
[[nodiscard]] double cosine(const std::vector<float>& a,
                            const std::vector<float>& b) noexcept;

// ── Chunker ────────────────────────────────────────────────────────────────
// Split one document into bounded, line-aligned chunks. Prefers to break on
// blank lines / markdown headings (semantic boundaries) and never mid-line.
// `max_lines` / `max_chars` bound each chunk; `overlap_lines` repeats a few
// trailing lines into the next chunk so a fact spanning a boundary survives.
[[nodiscard]] std::vector<Chunk>
chunk_document(const std::string& path, const std::string& body,
               std::size_t max_lines = 40, std::size_t max_chars = 1600,
               std::size_t overlap_lines = 4);

} // namespace agentty::rag
