#pragma once
// agentty::rag — ADVANCED retrieval: the capabilities that separate a good
// static funnel from an engine that actually solves the user's problem.
//
// Everything here follows the module's iron rules: pure C++/STL, zero new
// dependencies, deterministic-by-default (the only network is the SAME
// optional localhost /api/embed call the rest of the funnel already makes),
// and graceful degradation — every feature no-ops to the existing behaviour
// on any failure. Nothing touches a hot path; all of it rides the cold
// search_docs funnel.
//
//   • feedback  — the LEARNING LOOP. agentty is uniquely positioned to close
//     the loop nobody else closes: it sees which retrieved passages the
//     agent actually ACTS on (it `read`s the file a passage pointed at).
//     Every surfaced passage counts a "use"; every follow-up file-open of a
//     surfaced path counts a "win". The Beta-smoothed win-rate becomes a
//     per-passage PRIOR the reranker folds in — chunks that repeatedly help
//     rise, sources that produce noise sink. Persisted per-workspace
//     (.agentty/rag_feedback.tsv) so the engine gets better with use.
//
//   • carryover — CONVERSATION-AWARE query rewriting. "how does it handle
//     errors?" retrieves nothing because the entity lives two turns back.
//     A tiny recency-decayed salience tracker observes each query/user
//     message; a vague follow-up (pronouns / too few content terms) gets the
//     top carried-over terms appended as an EXTRA probe. Deterministic, no
//     model call, never replaces the original query — recall can only rise.
//
//   • decompose_query — MULTI-HOP for compositional questions. "how does
//     auth interact with the sandbox policy" has no single covering chunk.
//     Deterministic clause-splitting (connectives: and / vs / versus / ";")
//     turns it into per-facet probes that ride the existing multi-query RRF
//     fusion, so each facet retrieves on its own strength.
//
//   • LateInteractionStage — ColBERT-style late interaction on a budget.
//     Chunk-level single-vector cosine blurs multi-topic chunks; sentence-
//     level MaxSim keeps the query aligned to the BEST sentence in each
//     candidate. One batched /api/embed round-trip (query + top-pool
//     sentences) — the same cost class as the chunk-level EmbedRerankStage
//     it upgrades. Degrades to input order when the backend is unreachable.
//
//   • GraphExpandStage — GraphRAG-lite. Markdown links between docs are an
//     author-curated relevance graph nobody uses. Follow the top hits'
//     outbound links one hop and fold the linked documents' lead chunks into
//     the pool (deterministic, in-memory, no model) — the cross-document
//     cousin of parent-document expansion.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "agentty/rag/knowledge.hpp"
#include "agentty/rag/rerank.hpp"   // EmbedRerankConfig

namespace agentty::rag {

// ── Learning loop ─────────────────────────────────────────────────────────
namespace feedback {

// Record that these passages (keys: "<source>:<path>") were surfaced to the
// model by search_docs / proactive injection. Counts one "use" each and
// remembers them in a bounded recent window for win attribution. Never throws.
void note_shown(const std::vector<std::string>& keys);

// Record that the agent opened `path` (the `read` tool ran). Every recently-
// shown key whose path suffix-matches counts a "win" — the passage pointed
// somewhere the agent found worth acting on. Never throws; sub-microsecond
// when nothing was recently shown (the common case).
void note_file_opened(std::string_view path);

// Beta(1,1)-smoothed win-rate for a key: (wins+1)/(uses+2) ∈ (0,1).
// Unknown key → 0.5 (neutral). Never throws.
[[nodiscard]] double prior(std::string_view key) noexcept;

// Persist to the workspace store (.agentty/rag_feedback.tsv — override path
// with AGENTTY_RAG_FEEDBACK_PATH; set AGENTTY_RAG_LEARN=0 to disable the
// whole loop). Atomic tmp+rename; best-effort. Called automatically by the
// note_* functions on a cadence; exposed for tests/shutdown.
void flush() noexcept;

// Test seam: drop in-memory state (does not touch the on-disk store).
void reset_for_test();

} // namespace feedback

// ── Conversation carryover ────────────────────────────────────────────────
namespace carryover {

// Observe a query / user message: its content terms enter the salience pool
// (existing weights decay first, so recency wins). Bounded; never throws.
void note(std::string_view text);

// If `query` looks like a vague follow-up (contains a pronoun, or has <2
// content terms), return it with the top carried-over terms appended
// ("how does it handle errors" → "how does it handle errors sandbox bwrap").
// Otherwise (or when nothing is carried over) returns `query` unchanged.
[[nodiscard]] std::string rewrite(const std::string& query);

// Test seam: clear the salience pool.
void reset_for_test();

} // namespace carryover

// ── Multi-hop decomposition ───────────────────────────────────────────────
// Split a compositional query into per-facet sub-queries on clause
// connectives (" and ", " vs ", " versus ", ";"). Only fires when the split
// yields ≥2 clauses that EACH carry ≥2 content terms — a conservative gate
// so ordinary queries pass through untouched. Returns {} when it doesn't
// fire; capped at 3 sub-queries. Deterministic, model-free.
[[nodiscard]] std::vector<std::string>
decompose_query(const std::string& query);

// ── Learned-prior rerank stage ────────────────────────────────────────────
// Multiplies each ranked chunk's score by a factor derived from its feedback
// prior: score × (0.85 + 0.30·prior) — neutral (×1.0) at prior 0.5, so a
// fresh workspace behaves exactly as before; only accumulated evidence moves
// ranks. Runs after the heavyweight rerankers as a final nudge.
class LearnedPriorStage final : public RetrievalStage {
public:
    LearnedPriorStage() = default;
    [[nodiscard]] std::string_view name() const noexcept override { return "learned_prior"; }
    [[nodiscard]] Context process(Context ctx) const override;
};

// ── Late-interaction (sentence MaxSim) rerank stage ───────────────────────
// Re-scores the top pool by sentence-level MaxSim: every candidate's
// sentences + the query go out in ONE batched /api/embed call; a chunk's
// score is max_j cos(q, sⱼ) blended with the mean of its top-2 sentences.
// Precision upgrade over chunk-level cosine at the same network cost.
// Backend down / no model / dim mismatch → returns the input order.
class LateInteractionStage final : public RetrievalStage {
public:
    LateInteractionStage(std::size_t out_k, EmbedRerankConfig cfg)
        : out_k_(out_k), cfg_(std::move(cfg)) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "late_interaction"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    std::size_t       out_k_;
    EmbedRerankConfig cfg_;
};

// ── GraphRAG expansion stage ─────────────────────────────────────
// Retrieval over the corpus's DOCUMENT GRAPH: nodes = docs, edges = markdown
// links PLUS entity co-occurrence (salient identifiers/terms shared by few
// docs — deterministic tf-idf extraction, no LLM), built once and
// memo-cached per corpus shape. Candidates come from four tiers around the
// top hits — outbound links (docs a hit vouches for), BACKLINKS (docs that
// cite a hit: usually the overview that contextualizes it), ENTITY
// NEIGHBOURS (docs sharing the hit's rare entities — related even when the
// author drew no link), and the highest-PageRank hub of the top hits'
// shared community (deterministic label propagation). Ties break on
// PageRank authority. Appended below the surviving pool as supporting
// material; adds at most `max_extra` chunks.
//
// COMMUNITY SUMMARIES (full GraphRAG, opt-in): with `summary.model` set, a
// 2-3 sentence community report is generated ONCE per community via the
// local Ollama, persisted to .agentty/rag_graph_summaries.tsv, and attached
// to the hub passage — so a corpus-level question is answered by a
// pre-digested overview, not just a lead chunk. Any failure degrades to the
// plain hub chunk.
struct GraphSummaryConfig {
    std::string   host = "127.0.0.1";
    std::uint16_t port = 11434;
    std::string   model;          // empty → summaries off (default)
    double        timeout_s = 20.0;
};

class GraphExpandStage final : public RetrievalStage {
public:
    GraphExpandStage(const Corpus& corpus, std::size_t max_extra = 2,
                     GraphSummaryConfig summary = {})
        : corpus_(&corpus), max_extra_(max_extra),
          summary_(std::move(summary)) {}
    [[nodiscard]] std::string_view name() const noexcept override { return "graph_expand"; }
    [[nodiscard]] Context process(Context ctx) const override;
private:
    const Corpus*      corpus_;   // non-owning; outlives the stage (funnel-scoped)
    std::size_t        max_extra_;
    GraphSummaryConfig summary_;
};

} // namespace agentty::rag
