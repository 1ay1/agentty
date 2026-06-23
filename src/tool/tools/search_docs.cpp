// search_docs — agentic document/knowledge RAG. Retrieves the most relevant
// passages from the user's knowledge base (markdown / text / notes — NOT
// source code; use grep/read for code) using hybrid BM25 + dense-embedding
// retrieval fused with Reciprocal Rank Fusion. Embeddings come from the
// already-running local Ollama server; if none is reachable it degrades to
// BM25-only and never errors. See include/agentty/rag/rag.hpp for the design.

#include "agentty/tool/spec.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"

#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct SearchDocsArgs {
    std::string query;
    int         k = 6;
    std::string display_description;
};

std::expected<SearchDocsArgs, ToolError> parse_search_docs_args(const json& j) {
    util::ArgReader ar(j);
    auto q = ar.require_str("query");
    if (!q)
        return std::unexpected(ToolError::invalid_args("query required"));
    int k = ar.integer("k", 6);
    if (k < 1)  k = 1;
    if (k > 20) k = 20;
    return SearchDocsArgs{
        *std::move(q),
        k,
        ar.str("display_description", ""),
    };
}

// Resolve the knowledge directory: AGENTTY_DOCS_DIR wins; else ./docs; else
// ./.agentty/knowledge. Returns empty when none is configured/present.
fs::path resolve_docs_root() {
    if (const char* env = std::getenv("AGENTTY_DOCS_DIR")) {
        if (env[0] != '\0') return fs::path{env};
    }
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto docs = cwd / "docs";
    if (fs::is_directory(docs, ec)) return docs;
    auto kb = cwd / ".agentty" / "knowledge";
    if (fs::is_directory(kb, ec)) return kb;
    return {};
}

// Build the embed config from env: AGENTTY_EMBED_MODEL (default
// nomic-embed-text) + AGENTTY_OLLAMA_HOST ("host" or "host:port").
rag::EmbedConfig embed_config_from_env() {
    rag::EmbedConfig cfg;  // host=127.0.0.1, port=11434 by default
    if (const char* m = std::getenv("AGENTTY_EMBED_MODEL"); m && m[0] != '\0')
        cfg.model = m;
    else
        cfg.model = "nomic-embed-text";

    if (const char* h = std::getenv("AGENTTY_OLLAMA_HOST"); h && h[0] != '\0') {
        std::string hs{h};
        if (auto colon = hs.rfind(':'); colon != std::string::npos) {
            cfg.host = hs.substr(0, colon);
            std::string ps = hs.substr(colon + 1);
            try {
                int p = std::stoi(ps);
                if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
            } catch (...) { /* keep default port */ }
        } else {
            cfg.host = hs;
        }
    }
    return cfg;
}

ExecResult run_search_docs(const SearchDocsArgs& a) {
    try {
        auto root = resolve_docs_root();
        if (root.empty()) {
            return ToolOutput{
                "No knowledge directory configured. Set AGENTTY_DOCS_DIR to a "
                "folder of documents (markdown/text/etc.) to enable "
                "search_docs, or create ./docs.",
                std::nullopt};
        }

        const rag::EmbedConfig embed = embed_config_from_env();
        std::string root_str = root.string();

        // One process-wide corpus, lazily (re)built on the first call and
        // whenever the resolved root changes. corpus.build is itself
        // incremental (file size+mtime cache), but re-walking the dir on
        // every single search within a session is wasteful — so we only
        // rebuild when the root changed. The lazy-first-build keeps cold
        // start sub-ms: nothing happens until the model actually calls us.
        static std::mutex mu;
        static rag::Corpus corpus;
        static std::string indexed_root;

        std::lock_guard<std::mutex> lock(mu);
        if (indexed_root != root_str) {
            corpus.build(root, embed);
            indexed_root = root_str;
        }

        // Retrieve WIDE → rerank → narrow → compress (SOTA RAG pipeline).
        // The first-pass hybrid fusion is recall-oriented and noisy at the
        // top; a wide pool gives the feature-fusion reranker enough
        // candidates to lift precision@k. Then compress each survivor so a
        // weak small-context model isn't flooded with irrelevant chunk text.
        auto pool = corpus.search(a.query, embed,
                                  std::max<std::size_t>(
                                      static_cast<std::size_t>(a.k) * 5, 30));
        auto hits = rag::rerank(a.query, std::move(pool),
                                static_cast<std::size_t>(a.k));

        std::string body;
        if (!a.display_description.empty())
            body += a.display_description + "\n";

        if (hits.empty()) {
            return ToolOutput{
                body + "No matching documents found for: " + a.query,
                std::nullopt};
        }

        const char* mode = corpus.has_embeddings() ? "hybrid" : "BM25-only";
        body += std::to_string(hits.size()) + " results from " + root_str
              + " (mode: " + mode + ", reranked)\n";

        char score_buf[32];
        for (std::size_t i = 0; i < hits.size(); ++i) {
            const auto& h = hits[i];
            if (!h.chunk) continue;
            std::snprintf(score_buf, sizeof(score_buf), "%.4f", h.score);
            body += "\n── " + h.chunk->path + ":"
                  + std::to_string(h.chunk->line_start) + "-"
                  + std::to_string(h.chunk->line_end)
                  + "  (score " + score_buf + ")\n";
            std::string passage = rag::compress(a.query, h.chunk->text,
                                                 /*target_chars=*/600);
            body += passage;
            if (!body.empty() && body.back() != '\n') body += "\n";
        }

        return ToolOutput{std::move(body), std::nullopt};
    } catch (const std::exception& e) {
        return std::unexpected(
            ToolError::io(std::string{"search_docs failed: "} + e.what()));
    } catch (...) {
        return std::unexpected(ToolError::io("search_docs failed"));
    }
}

} // namespace

ToolDef tool_search_docs() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"search_docs">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description = "Search the user's knowledge base / documentation corpus (NOT source code — "
                    "use grep/read for code) and return the most relevant passages. Hybrid "
                    "retrieval (keyword BM25 + semantic embeddings via the local Ollama server, "
                    "fused with reciprocal rank fusion). Use this to ground answers in the "
                    "user's own manuals, specs, or notes instead of guessing from memory. "
                    "Point it at a folder via the AGENTTY_DOCS_DIR env var.";
    t.input_schema = json{
        {"type","object"},
        {"required", {"query"}},
        {"properties", {
            {"display_description", {{"type","string"},
                {"description","One-line summary shown in the UI. Optional."}}},
            {"query", {{"type","string"}, {"description","Natural-language or keyword query to retrieve relevant document passages for."}}},
            {"k",     {{"type","integer"}, {"description","Number of passages to return (default 6, max 20)."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<SearchDocsArgs>(parse_search_docs_args, run_search_docs);
    return t;
}

} // namespace agentty::tools
