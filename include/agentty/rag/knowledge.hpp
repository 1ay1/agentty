#pragma once
// agentty::rag — the KNOWLEDGE LAYER boundaries.
//
// This header turns agentty's retrieval from a single concrete `Corpus` into
// a set of swappable interfaces, WITHOUT paying any hot-path cost. The whole
// point (see the architecture essay this implements) is that the agent asks
//
//     retrieve(query) -> Context
//
// and never knows WHERE the information came from (a folder of markdown, a
// remote API, an MCP resource, a second corpus) or HOW it was found (BM25,
// dense, hybrid, graph). Those are independent axes:
//
//     KnowledgeSource  = WHERE knowledge lives   (folder / API / MCP / graph)
//     Retriever        = HOW a source is searched (BM25 / dense / hybrid)
//     RetrievalStage   = a COMPOSABLE step        (expand / rerank / compress)
//     KnowledgeRouter  = fan a query across many sources, fuse with RRF
//
// PERF CONTRACT (load-bearing — agentty must stay sub-ms cold start / ~9MB):
//   • Every virtual call here is paid on the search_docs path only — a COLD,
//     user-initiated, network-bound path. NONE of these interfaces touch the
//     render loop, the stream reducer, or any per-frame/per-token code.
//   • The fast inner loops (BM25 scoring, cosine, HNSW walk) stay NON-virtual
//     inside `Corpus`; `CorpusSource` is a thin adapter over the existing
//     concrete class, so the hot scoring code is unchanged and uninlined-by-
//     vtable nowhere.
//   • Single-source retrieval (the default) bypasses the router entirely:
//     one source, one Retriever, zero fusion overhead.

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/rag/rag.hpp"   // Chunk, Hit, Corpus, EmbedConfig, RRF
#include "agentty/rag/rerank.hpp"  // RerankWeights (RerankStage tuning)

namespace agentty::rag {

// ── Context: the first-class carrier (essay §4) ───────────────────────────
//
// Most frameworks pass bare strings between stages and lose everything the
// moment a chunk is stringified. We carry a structured, enrichable object
// instead. A ContextChunk wraps a retrieval Hit (which already owns the chunk
// pointer, the fused score, and the source provenance) and adds the one piece
// of derived state a pipeline produces: the compressed span. The shared
// Corpus Chunk is NEVER mutated — compression writes here.
struct ContextChunk {
    Hit         hit;          // chunk ptr + fused score + source provenance
    std::string compressed;   // best query-relevant span; empty == use hit.chunk->text

    // The text a consumer should actually show: the compressed span when a
    // CompressStage ran, else the full chunk body.
    [[nodiscard]] std::string_view text() const noexcept {
        if (!compressed.empty()) return compressed;
        return hit.chunk ? std::string_view{hit.chunk->text} : std::string_view{};
    }
};

struct Context {
    std::string               query;   // the (possibly normalized) probe
    std::vector<ContextChunk> chunks;  // ranked, enrichable by each stage

    // Confidence signal in [0,1]. CALIBRATED: anchored on the ABSOLUTE
    // lexical coverage of the query in the top hits (do the query's content
    // words actually appear in what we retrieved?), tempered by the relative
    // score agreement of the candidate set. Rank-derived scores alone are
    // NOT trustworthy here — the feature reranker min-max-normalizes per
    // candidate set, so the top hit's score is ~1.0 by construction even
    // when every candidate is garbage. Computed by compute_confidence()
    // after ranking and BEFORE MMR (diversification deliberately spreads
    // scores; reading that spread as doubt would punish MMR for working).
    double confidence = 0.0;

    [[nodiscard]] bool        empty()  const noexcept { return chunks.empty(); }
    [[nodiscard]] std::size_t size()   const noexcept { return chunks.size(); }

    // Seed a Context from a flat hit list (router/source output).
    [[nodiscard]] static Context from_hits(std::string query, std::vector<Hit> hits) {
        Context c;
        c.query = std::move(query);
        c.chunks.reserve(hits.size());
        for (auto& h : hits) c.chunks.push_back(ContextChunk{h, {}});
        c.compute_confidence();
        return c;
    }

    // Compute confidence from `query` + the ranked chunks. Call after
    // ranking, before MMR. Never throws (any internal failure → 0.0).
    // Defined in knowledge.cpp (needs the rerank tokenizer).
    void compute_confidence() noexcept;
};

// ── Retriever: HOW (essay §1) ─────────────────────────────────────────────
//
// A retrieval strategy over some backing store. The default impl is the
// hybrid BM25+dense+RRF `Corpus`; a GraphRetriever / KeywordRetriever / pure
// dense retriever can drop in without any caller change. Pure interface — no
// state — so it adds nothing to the binary beyond a vtable.
class Retriever {
public:
    virtual ~Retriever() = default;
    // Return up to `k` hits for `query`, best-first. Must not throw (degrade
    // to {} on any backend failure — retrieval never blocks the agent).
    [[nodiscard]] virtual std::vector<Hit>
    retrieve(std::string_view query, std::size_t k) const = 0;
};

// ── KnowledgeSource: WHERE (essay §5, §6, §10) ────────────────────────────
//
// A named place knowledge lives. The agent (and the router) see only this:
// they ask for hits and get hits, stamped with provenance. A folder corpus,
// a remote ratings API, an MCP `resources/read`, or a SQL view are all just
// implementations — exactly the "MCP and RAG are the same thing from the
// agent's view" insight: both are sources of information behind one seam.
class KnowledgeSource {
public:
    virtual ~KnowledgeSource() = default;

    // Stable identifier for provenance/citations ("docs", "wiki", "mcp:foo").
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

    // Retrieve up to `k` hits. Implementations MUST stamp `hit.source = this`
    // so downstream stages and the router can attribute every chunk. Must not
    // throw.
    [[nodiscard]] virtual std::vector<Hit>
    retrieve(std::string_view query, std::size_t k) const = 0;
};

// ── CorpusSource: the built-in folder source ──────────────────────────────
//
// Adapts the existing concrete `Corpus` (hybrid BM25+dense+RRF over a folder
// of docs, on-disk incremental cache) to the KnowledgeSource interface. Owns
// nothing heavy: it holds a non-owning pointer to a Corpus the caller keeps
// alive (search_docs keeps a process-wide static Corpus), plus the embed
// config to use per query. This is the adapter that keeps the fast scoring
// loops inside Corpus non-virtual.
class CorpusSource final : public KnowledgeSource {
public:
    CorpusSource(std::string name, const Corpus& corpus, EmbedConfig embed)
        : name_(std::move(name)), corpus_(&corpus), embed_(std::move(embed)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    [[nodiscard]] std::vector<Hit>
    retrieve(std::string_view query, std::size_t k) const override;

    // Multi-query (RAG-Fusion) retrieval — used when query expansion is on.
    // Routed through Corpus::search_fused, then provenance-stamped.
    [[nodiscard]] std::vector<Hit>
    retrieve_fused(const std::vector<std::string>& queries, std::size_t k) const;

private:
    std::string   name_;
    const Corpus* corpus_;   // non-owning; outlives this adapter
    EmbedConfig   embed_;
};

// ── McpResourceSource: knowledge behind an MCP server (essay §6, §10) ──────
//
// "MCP and RAG are the same thing from the agent's view" — both are sources
// of information behind one seam. This source makes the `resources/*` an MCP
// server exposes SEARCHABLE through the exact same KnowledgeSource interface
// as a local docs folder: list the resources, read each one, chunk + embed
// the flattened text into a PRIVATE in-memory Corpus, and answer retrieve()
// from it (hybrid BM25 + dense, same scoring as the folder corpus).
//
// DEPENDENCY DIRECTION (load-bearing): the RAG layer must NOT depend on the
// MCP layer — keeping this header mcp-cpp-free AND mcp/client.hpp-free is
// what lets RAG stay a leaf. So MCP is injected as two plain std::function
// seams (list + read). The wiring layer (which already owns both) binds them
// to agentty::mcp::mcp_resources / mcp_read_resource. With no binding (or no
// MCP configured) the source is simply empty — zero cost, exactly like the
// rest of MCP's opt-in contract.
//
// PERF: the private Corpus is built LAZILY on the first retrieve() (a cold,
// user-initiated search_docs path) and reused for the process lifetime.
// There is NO disk cache — resources are remote and may change; refresh()
// drops the index so the next retrieve() rebuilds. Nothing here touches the
// render/stream loop.
class McpResourceSource final : public KnowledgeSource {
public:
    // One advertised resource: its URI and a human label (for provenance).
    struct ResourceRef { std::string uri; std::string label; };

    // Injection seams (bound by the wiring layer to the mcp::* free fns):
    //   list_fn()      -> the resources to index (URI + label)
    //   read_fn(uri)   -> flattened text, or std::nullopt on failure
    using ListFn = std::function<std::vector<ResourceRef>()>;
    using ReadFn = std::function<std::optional<std::string>(const std::string& uri)>;

    // `name` is the provenance label (e.g. "mcp"). `embed` selects the dense
    // endpoint (empty model → BM25-only). Both function seams may be empty,
    // in which case the source indexes nothing and retrieve() returns {}.
    McpResourceSource(std::string name, ListFn list_fn, ReadFn read_fn,
                      EmbedConfig embed = {})
        : name_(std::move(name)), list_(std::move(list_fn)),
          read_(std::move(read_fn)), embed_(std::move(embed)) {}

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

    // Lazily builds the private Corpus on first call, then hybrid-retrieves.
    // Stamps provenance. Never throws.
    [[nodiscard]] std::vector<Hit>
    retrieve(std::string_view query, std::size_t k) const override;

    // Drop the cached index so the next retrieve() re-reads every resource.
    // Call when the server emits resources/list_changed.
    void refresh() noexcept { built_ = false; }

    // Number of chunks currently indexed (0 before the first retrieve()).
    [[nodiscard]] std::size_t indexed_chunks() const noexcept {
        return corpus_.chunk_count();
    }

private:
    void build_index_() const;   // populates corpus_ from list_()/read_()

    std::string          name_;
    ListFn               list_;
    ReadFn               read_;
    EmbedConfig          embed_;
    mutable Corpus       corpus_;        // private, in-memory (no disk cache)
    mutable bool         built_ = false;
};

// ── KnowledgeRouter: fan-out + fuse (essay §6, §10) ───────────────────────
//
// Holds N knowledge sources. retrieve() asks EACH for its top-k, fuses the
// ranked lists with the SAME Reciprocal Rank Fusion already used inside
// Corpus (no new ranking math), and returns a single provenance-stamped
// ranked list. With one source it short-circuits to that source's own
// retrieve() — zero fusion overhead for the common case.
class KnowledgeRouter {
public:
    // Register a source. Order is irrelevant to ranking (RRF is symmetric).
    void add(std::shared_ptr<KnowledgeSource> src);

    [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

    // Fan `query` across all sources, fuse, return top `k`. Per-source pool
    // defaults to `k` but can be widened by the caller for better fusion
    // recall. Never throws.
    [[nodiscard]] std::vector<Hit>
    retrieve(std::string_view query, std::size_t k, std::size_t per_source_k = 0) const;

private:
    std::vector<std::shared_ptr<KnowledgeSource>> sources_;
};

// ── RetrievalStage + Pipeline: composable steps (essay §3) ────────────────
//
// A pipeline stage transforms a Context. The canonical SOTA pipeline is
//   normalize -> expand -> retrieve -> merge -> rerank -> compress
// and each stage here wraps the EXISTING free functions (expand_query,
// rerank, compress) — no logic is reimplemented, the stages just make the
// pipeline assemblable and reorderable. A stage owns no per-call allocation
// beyond what its wrapped function already does.
class RetrievalStage {
public:
    virtual ~RetrievalStage() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Transform ctx in place / return the next Context. Must not throw.
    [[nodiscard]] virtual Context process(Context ctx) const = 0;
};

class Pipeline {
public:
    // Fluent assembly: p.add(...).add(...). Stages run in insertion order.
    Pipeline& add(std::shared_ptr<RetrievalStage> stage);

    [[nodiscard]] std::size_t stage_count() const noexcept { return stages_.size(); }

    // Run every stage left-to-right over the seed Context. Never throws.
    [[nodiscard]] Context run(Context seed) const;

private:
    std::vector<std::shared_ptr<RetrievalStage>> stages_;
};

// ── Built-in stages (thin wrappers over the existing free functions) ──────

// RetrieveStage — seeds the Context by querying a KnowledgeSource (or router)
// for a WIDE candidate pool. This is the only stage that talks to a backend.
class RetrieveStage final : public RetrievalStage {
public:
    RetrieveStage(const KnowledgeSource& src, std::size_t pool_k)
        : src_(&src), pool_k_(pool_k) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "retrieve"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    const KnowledgeSource* src_;
    std::size_t            pool_k_;
};

// RerankStage — wraps rag::rerank (feature-fusion reranker). Narrows the wide
// pool down to out_k by re-scoring against the original query.
class RerankStage final : public RetrievalStage {
public:
    RerankStage(std::size_t out_k, RerankWeights w = {})
        : out_k_(out_k), w_(w) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "rerank"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    std::size_t   out_k_;
    RerankWeights w_;
};

// CompressStage — wraps rag::compress. Fills each surviving ContextChunk's
// `compressed` field with the best query-relevant span under target_chars.
// The shared Corpus chunk is never mutated.
class CompressStage final : public RetrievalStage {
public:
    explicit CompressStage(std::size_t target_chars = 600)
        : target_chars_(target_chars) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "compress"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    std::size_t target_chars_;
};

// NeuralRerankStage — wraps rag::neural_rerank. Scores chunks via Ollama
// using a cross-encoder-style relevance prompt. OPT-IN and expensive: only
// enable when explicitly requested. Falls back to lexical rerank on failure.
class NeuralRerankStage final : public RetrievalStage {
public:
    NeuralRerankStage(std::size_t out_k, NeuralRerankConfig cfg)
        : out_k_(out_k), cfg_(std::move(cfg)) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "neural_rerank"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    std::size_t         out_k_;
    NeuralRerankConfig  cfg_;
};

// MMRStage — wraps rag::mmr_diversify. Greedily re-selects chunks to balance
// relevance and diversity. Useful after reranking when top-k contains
// near-duplicate overlapping chunks.
class MMRStage final : public RetrievalStage {
public:
    MMRStage(std::size_t out_k, double lambda = 0.7)
        : out_k_(out_k), lambda_(lambda) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "mmr"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    std::size_t out_k_;
    double      lambda_;
};

// NormalizeQueryStage — preprocesses the query before retrieval. Applies:
//   1. Lowercasing
//   2. Whitespace normalization
// Runs BEFORE retrieval to ensure consistent query representation.
class NormalizeQueryStage final : public RetrievalStage {
public:
    struct Config {
        bool lowercase            = true;
        bool normalize_whitespace = true;

        Config() = default;
    };
    NormalizeQueryStage() : cfg_{} {}
    explicit NormalizeQueryStage(Config cfg) : cfg_(cfg) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "normalize"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    Config cfg_;
};

} // namespace agentty::rag
