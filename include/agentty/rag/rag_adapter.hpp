#pragma once
// agentty::rag — the retrieval adapter.
//
// agentty's RAG engine is the external rag-cpp library (rag::Engine: contextual
// hybrid BM25 + dense/HNSW, RRF fusion, CRAG, HyDE, MMR rerank, GraphRAG,
// .ragdb persistence). This header is the ONLY surface the rest of agentty
// sees: it hides every rag:: type behind a compact, stable API so the app never
// depends on the engine's internals.
//
// The boundary is deliberately tiny — three things the app needs:
//
//   1. Retriever          — build/refresh a docs index from a folder, fuse it
//                           with skills + learned-memory + MCP-resource
//                           knowledge sources, and answer a query with ranked,
//                           compressed passages. Backs the `search_docs` tool
//                           and the pre-turn proactive-retrieval path.
//   2. feedback::note_file_opened  — the learning loop's write side: a `read`
//                           of a file a passage pointed at counts as a "win".
//   3. bench::run          — the `agentty rag-bench` CLI subcommand.
//
// Everything below is std types + POD; no rag:: type leaks. The heavy engine
// lives in the .cpp (src/rag/adapter.cpp).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agentty::rag {

// One retrieved passage, flattened for the app. Mirrors mcp::tools::DocPassage
// field-for-field so the backend maps it with a trivial copy.
struct Passage {
    std::string   source;      // provenance: "docs" / "skills" / "memory" / "mcp:<uri>"
    std::string   path;        // file path or virtual uri (skill://…, memory://…)
    int           line_start = 0;
    int           line_end   = 0;
    double        score      = 0.0;
    std::string   text;        // passage body (already compressed)
};

// The result of a retrieval: the ranked passages + a human-readable mode label
// (engine config + confidence) the tool shell renders, + the confidence signal
// so the proactive path can gate on it.
struct Retrieval {
    std::vector<Passage> passages;
    std::string          mode;            // e.g. "hybrid+ctx, reranked, confidence 0.62"
    double               confidence = 0.0;
    std::string          error;           // non-empty ⇒ failure (no knowledge, etc.)
};

// Knobs, all resolved from the environment by default (from_env()). Kept as a
// struct so tests can drive the adapter deterministically.
struct Config {
    std::string  docs_root;               // AGENTTY_DOCS_DIR (or ./docs, ./.agentty/knowledge)
    std::string  embed_model  = "nomic-embed-text";   // AGENTTY_EMBED_MODEL
    std::string  embed_host   = "127.0.0.1";          // AGENTTY_OLLAMA_HOST (host part)
    std::uint16_t embed_port  = 11434;                 // AGENTTY_OLLAMA_HOST (:port)
    bool         skills   = true;         // AGENTTY_RAG_SKILLS
    bool         memory   = true;         // AGENTTY_RAG_MEMORY
    bool         mcp_resources = false;   // opt-in
    bool         corrective   = true;     // AGENTTY_RAG_CORRECT (CRAG)
    bool         expand   = false;        // AGENTTY_RAG_EXPAND (RAG-Fusion, needs LLM)
    bool         hyde     = false;        // AGENTTY_RAG_HYDE (needs LLM)
    bool         graph    = true;         // AGENTTY_RAG_GRAPH (GraphRAG expand)

    [[nodiscard]] static Config from_env();
};

// The retriever. One long-lived instance backs search_docs + proactive
// retrieval (the backend holds a function-local static). Thread-safe: retrieve()
// may be called concurrently; the docs index is guarded internally.
class Retriever {
public:
    Retriever();
    ~Retriever();
    Retriever(const Retriever&) = delete;
    Retriever& operator=(const Retriever&) = delete;

    // Retrieve up to k passages for `query`. `skip_docs` runs the WARM path
    // (skills + memory + MCP only, no docs-folder walk) for the proactive
    // pre-turn hedge. Never throws; failures surface in Retrieval::error.
    [[nodiscard]] Retrieval retrieve(const std::string& query, int k,
                                     bool skip_docs = false);

    // SEMANTIC CODE SEARCH (backs search_code): index source files under the
    // current working directory (code-aware chunking) and answer `query` with
    // ranked passages. Edit-aware: a cheap fingerprint over the walked tree
    // rebuilds on drift. Independent of the docs index. Never throws.
    [[nodiscard]] Retrieval retrieve_code(const std::string& query, int k);

    // Non-blocking: is the docs index built & fresh for the current root
    // (or is there no docs root, in which case retrieval is always warm)?
    [[nodiscard]] bool warm() const;

    // Kick a detached background index build so a future turn is warm.
    // Single-flight; returns immediately.
    void warm_async();

private:
    struct Impl;
    Impl* impl_;   // owned; raw so the header pulls in no rag:: type
};

// ── Learning loop (write side) ─────────────────────────────────────────
// Record that the user opened `path` — if a recently-surfaced passage pointed
// at it, that's a "win" and the passage's file rises in future rankings.
// Best-effort, never throws.
namespace feedback {
void note_file_opened(const std::string& path);
}

// ── CLI: `agentty rag-bench <root>` ────────────────────────────────────
namespace bench {
int run(const std::string& root);
}

} // namespace agentty::rag
