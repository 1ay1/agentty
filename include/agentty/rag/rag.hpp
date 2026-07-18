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
#include <functional>
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

    // CONTEXTUAL RETRIEVAL (Anthropic 2024: −49% retrieval failures).
    // A short situating string — the document name + the markdown heading
    // breadcrumb the chunk sits under ("guide.md › Installation › Linux").
    // It participates in BOTH index sides (tokenized into the BM25 doc and
    // prepended to the embedding input) so a chunk that says "run the
    // installer" is findable by "linux install" even though neither word
    // appears in the body. It is NEVER shown to the model as content —
    // `text` stays the verbatim body; provenance already carries the path.
    // Built for free during chunking (the chunker tracks the heading stack
    // anyway) — the LLM-generated variant of contextual retrieval costs a
    // model call per chunk; the breadcrumb captures most of the win at zero
    // cost and stays fully deterministic/offline.
    std::string  context;

    // Dense embedding. Empty when embeddings are unavailable (BM25-only
    // mode) or not yet computed. Length == Corpus::embed_dim when present.
    std::vector<float> embedding;

    // Metadata for filtering/faceting. Key examples: "category", "author",
    // "type" ("api", "tutorial", "reference"), "language", "date".
    // Populated from frontmatter or directory structure during chunking.
    std::unordered_map<std::string, std::string> metadata;

    // The string the EMBEDDER sees: context-prefixed body (contextual
    // embeddings). BM25 gets the same treatment inside build_bm25.
    [[nodiscard]] std::string embed_input() const {
        if (context.empty()) return text;
        std::string s;
        s.reserve(context.size() + 2 + text.size());
        s += context; s += '\n'; s += text;
        return s;
    }
};

// Forward decl so Hit can carry source provenance without a cycle.
class KnowledgeSource;

// A retrieval hit: a chunk plus its fused relevance score.
struct Hit {
    const Chunk* chunk = nullptr;
    double       score = 0.0;   // fused RRF score (higher = more relevant)

    // PROVENANCE (essay: "never discard where information came from").
    // Which KnowledgeSource produced this hit. nullptr when retrieval went
    // through a bare Corpus (single-source path) — only the KnowledgeRouter
    // stamps it, so every existing single-source call site is unaffected.
    // Non-owning: the source outlives the Hit (router owns its sources for
    // the duration of a retrieve()).
    const KnowledgeSource* source = nullptr;
};

// The embedding endpoint to call. Reuses the running Ollama server. When
// `model` is empty OR the host is unreachable, retrieval falls back to
// BM25-only and never errors.
struct EmbedConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 11434;
    std::string model;        // e.g. "nomic-embed-text"; empty → BM25-only
    // Total HTTP budget per /api/embed call, ms. Index-time batches keep the
    // generous default; QUERY-time single-text embeds are separately capped
    // at 10s inside Corpus so one wedged Ollama can never stall a search
    // (let alone the pre-turn proactive path) for two minutes.
    long timeout_ms = 120'000;
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

// ── Corpus + hybrid retrieval ─────────────────────────────────────────

// Filter predicate for metadata-based filtering. Returns true to KEEP a chunk.
using ChunkFilter = std::function<bool(const Chunk&)>;

// Pre-built filters for common patterns.
namespace filters {

// Match chunks where metadata[key] == value.
inline ChunkFilter meta_eq(const std::string& key, const std::string& value) {
    return [=](const Chunk& c) {
        auto it = c.metadata.find(key);
        return it != c.metadata.end() && it->second == value;
    };
}

// Match chunks where metadata[key] contains substr (case-insensitive).
inline ChunkFilter meta_contains(const std::string& key, const std::string& substr) {
    std::string lower_sub = substr;
    for (auto& ch : lower_sub) ch = static_cast<char>(std::tolower((unsigned char)ch));
    return [=](const Chunk& c) {
        auto it = c.metadata.find(key);
        if (it == c.metadata.end()) return false;
        std::string lower_val = it->second;
        for (auto& ch : lower_val) ch = static_cast<char>(std::tolower((unsigned char)ch));
        return lower_val.find(lower_sub) != std::string::npos;
    };
}

// Match chunks where path contains substr.
inline ChunkFilter path_contains(const std::string& substr) {
    return [=](const Chunk& c) { return c.path.find(substr) != std::string::npos; };
}

// Combine filters with AND.
inline ChunkFilter all_of(std::vector<ChunkFilter> filters) {
    return [filters = std::move(filters)](const Chunk& c) {
        for (const auto& f : filters) if (f && !f(c)) return false;
        return true;
    };
}

// Combine filters with OR.
inline ChunkFilter any_of(std::vector<ChunkFilter> filters) {
    return [filters = std::move(filters)](const Chunk& c) {
        for (const auto& f : filters) if (f && f(c)) return true;
        return false;
    };
}

} // namespace filters

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

    // Multi-query RAG-Fusion: retrieve for EACH query (BM25 + dense when
    // available) and fuse ALL the ranked lists with a single RRF pass. A
    // passage relevant under multiple phrasings rises to the top. Used by
    // the OPT-IN query-expansion path; the variants come from expand_query.
    // Returns {} when the corpus/queries are empty or k == 0.
    [[nodiscard]] std::vector<Hit> search_fused(
        const std::vector<std::string>& queries,
        const EmbedConfig& embed, std::size_t k) const;

    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    [[nodiscard]] bool        has_embeddings() const noexcept { return embed_dim_ > 0; }
    [[nodiscard]] std::size_t embed_dim() const noexcept { return embed_dim_; }

    // ── Hot reload API ───────────────────────────────────────────────────
    // Add/remove individual documents without a full rebuild. Useful for
    // live file watchers or editor integrations. Indices are rebuilt
    // incrementally; cache is NOT updated (call flush_cache() explicitly).

    // Add or update a document. Chunks, embeds (if embed model set), and
    // updates BM25. Returns the number of chunks added.
    std::size_t add_document(const std::string& path, const std::string& body,
                             const EmbedConfig& embed);

    // Remove all chunks for a document path. Returns the number removed.
    std::size_t remove_document(const std::string& path);

    // Flush current state to disk cache.
    void flush_cache() { write_cache_(); }

    // Build an in-memory corpus directly from raw (path, body) documents,
    // with NO disk cache and NO filesystem walk. Chunks every body, batch-
    // embeds (when `embed.model` is set), then builds BM25 + (above the size
    // threshold) HNSW in ONE pass. This is the build path for non-folder
    // sources — e.g. MCP resources, an API export, an in-memory test corpus
    // — that already hold their documents as strings. Replaces any existing
    // content. Returns the number of chunks indexed.
    std::size_t build_from_memory(
        const std::vector<std::pair<std::string, std::string>>& docs,
        const EmbedConfig& embed);

    // Exposed for tests: install a prebuilt corpus without disk/network.
    void set_chunks_for_test(std::vector<Chunk> chunks);

private:
    void write_cache_() const;

    // (Re)build the HNSW ANN graph from the current chunks_ when the corpus
    // is large enough to beat brute-force cosine; otherwise drop it. Keeps
    // node ids aligned with chunk positions (search() materializes via
    // &chunks_[id]), so it MUST run after any structural change to chunks_.
    void rebuild_hnsw_();

    // Append this query's ranked candidate lists (BM25, plus dense when the
    // corpus AND query embed) to `lists`, each as a vector of chunk ids.
    // When `weights` is non-null, append the matching per-list fusion weight
    // for each list pushed (lexical then dense), so callers can drive
    // reciprocal_rank_fusion_weighted. Shared by search() and search_fused()
    // so the per-query retrieval logic lives in exactly one place.
    void ranked_lists_for_query_(
        std::string_view query, const EmbedConfig& embed, std::size_t pool,
        std::vector<std::vector<std::uint32_t>>& lists,
        std::vector<double>* weights = nullptr) const;

    std::filesystem::path  root_;
    std::vector<Chunk>     chunks_;
    Bm25Index              bm25_;
    HnswIndex              hnsw_;
    bool                   hnsw_built_ = false;
    std::size_t            embed_dim_ = 0;
    // Identity of the model the persisted embeddings were computed with
    // (v4 cache header). Two models can share a dimension while living in
    // completely different vector spaces — without this, switching
    // AGENTTY_EMBED_MODEL would silently mix spaces and misrank everything.
    std::string            embed_model_;
};

// ── Reciprocal Rank Fusion ────────────────────────────────────────────────
// RRF(d) = Σ_lists 1/(k + rank_list(d)). k=60 is the canonical constant
// (Cormack et al.). Exposed for testing; `Corpus::search` uses it.
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    double k, std::size_t out_k);

// WEIGHTED RRF: RRF(d) = Σ_lists w_list · 1/(k + rank_list(d)). A per-list
// weight lets hybrid retrieval favour the dense list over lexical (or vice
// versa) — the single most effective hybrid-tuning lever in the literature,
// since dense catches paraphrase and BM25 catches exact terms, and which
// matters depends on the query. `weights` is aligned to `ranked_lists`;
// a shorter/empty weights vector defaults missing entries to 1.0 (so the
// unweighted overload above is exactly this with all-ones).
[[nodiscard]] std::vector<std::pair<std::uint32_t, double>>
reciprocal_rank_fusion_weighted(
    const std::vector<std::vector<std::uint32_t>>& ranked_lists,
    const std::vector<double>& weights,
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
