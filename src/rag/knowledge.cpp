// agentty::rag — KNOWLEDGE LAYER implementation.
//
// CorpusSource adapter, KnowledgeRouter (RRF fan-out), and the composable
// RetrievalStage pipeline (RetrieveStage / RerankStage / CompressStage).
// Every stage wraps an EXISTING free function (Corpus::search[_fused],
// rag::rerank, rag::compress) — no ranking/scoring logic is reimplemented
// here; this file only provides the swappable seams + the fusion glue. None
// of it runs on a hot path (search_docs is a cold, user-initiated call), so
// the virtual dispatch is free in practice.

#include "agentty/rag/knowledge.hpp"
#include "agentty/rag/rerank.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace agentty::rag {

// ── CorpusSource ──────────────────────────────────────────────────────────

std::vector<Hit>
CorpusSource::retrieve(std::string_view query, std::size_t k) const {
    auto hits = corpus_->search(query, embed_, k);
    for (auto& h : hits) h.source = this;   // stamp provenance
    return hits;
}

std::vector<Hit>
CorpusSource::retrieve_fused(const std::vector<std::string>& queries,
                             std::size_t k) const {
    auto hits = corpus_->search_fused(queries, embed_, k);
    for (auto& h : hits) h.source = this;
    return hits;
}

// ── KnowledgeRouter ─────────────────────────────────────────────────────────

void KnowledgeRouter::add(std::shared_ptr<KnowledgeSource> src) {
    if (src) sources_.push_back(std::move(src));
}

std::vector<Hit>
KnowledgeRouter::retrieve(std::string_view query, std::size_t k,
                          std::size_t per_source_k) const {
    if (sources_.empty() || k == 0) return {};

    // Single-source: short-circuit. No fusion, no extra allocation — the
    // common case (just the docs folder) pays nothing for being routable.
    if (sources_.size() == 1)
        return sources_.front()->retrieve(query, k);

    if (per_source_k == 0) per_source_k = k;

    // Gather each source's ranked list AND keep the hits addressable by a
    // stable key so we can rebuild the fused list after RRF chooses ids.
    // RRF operates on per-list rank position, so we feed it integer ids that
    // index a flat pool of (source-stamped) hits.
    std::vector<Hit>                       pool;     // id -> hit
    std::vector<std::vector<std::uint32_t>> lists;   // per-source ranked ids
    lists.reserve(sources_.size());

    for (const auto& src : sources_) {
        auto hits = src->retrieve(query, per_source_k);
        std::vector<std::uint32_t> ids;
        ids.reserve(hits.size());
        for (auto& h : hits) {
            ids.push_back(static_cast<std::uint32_t>(pool.size()));
            pool.push_back(h);   // already source-stamped by the source
        }
        if (!ids.empty()) lists.push_back(std::move(ids));
    }
    if (pool.empty()) return {};

    // Fuse with the SAME RRF used inside Corpus (k=60 canonical). Returns
    // (pool-id, fused-score) sorted desc, truncated to k.
    auto fused = reciprocal_rank_fusion(lists, /*k=*/60.0, k);

    std::vector<Hit> out;
    out.reserve(fused.size());
    for (auto& [id, score] : fused) {
        if (id >= pool.size()) continue;
        Hit h = pool[id];
        h.score = score;         // carry the FUSED score forward
        out.push_back(h);
    }
    return out;
}

// ── Pipeline ─────────────────────────────────────────────────────────────

Pipeline& Pipeline::add(std::shared_ptr<RetrievalStage> stage) {
    if (stage) stages_.push_back(std::move(stage));
    return *this;
}

Context Pipeline::run(Context seed) const {
    Context ctx = std::move(seed);
    for (const auto& stage : stages_) {
        if (!stage) continue;
        ctx = stage->process(std::move(ctx));
    }
    return ctx;
}

// ── Built-in stages ──────────────────────────────────────────────────────

Context RetrieveStage::process(Context ctx) const {
    // Seed the candidate pool from the source. Preserves any query the seed
    // Context already carried (e.g. set by a normalize/expand step upstream).
    auto hits = src_->retrieve(ctx.query, pool_k_);
    return Context::from_hits(std::move(ctx.query), std::move(hits));
}

Context RerankStage::process(Context ctx) const {
    // rerank() works on a flat Hit vector; lift the hits out of the Context,
    // rerank, then re-wrap. Compressed text (if any was set earlier — it
    // isn't, rerank runs before compress) would be dropped, which is correct:
    // rerank precedes compress in the canonical pipeline.
    std::vector<Hit> hits;
    hits.reserve(ctx.chunks.size());
    for (auto& c : ctx.chunks) hits.push_back(c.hit);

    auto ranked = rerank(ctx.query, std::move(hits), out_k_, w_);
    return Context::from_hits(std::move(ctx.query), std::move(ranked));
}

Context CompressStage::process(Context ctx) const {
    for (auto& c : ctx.chunks) {
        if (!c.hit.chunk) continue;
        c.compressed = compress(ctx.query, c.hit.chunk->text, target_chars_);
    }
    return ctx;
}

} // namespace agentty::rag
