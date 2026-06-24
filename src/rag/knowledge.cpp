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
#include <unordered_set>
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

// ── McpResourceSource ─────────────────────────────────────────────

void McpResourceSource::build_index_() const {
    built_ = true;                 // mark built even on empty/failure: don't
                                   // re-probe a dead server every retrieve().
    if (!list_ || !read_) return;

    std::vector<McpResourceSource::ResourceRef> refs = list_();
    if (refs.empty()) return;

    // Read each resource into a (path, body) doc. The URI is the chunk path
    // so provenance/citations point back at the exact resource. A resource
    // that fails to read is skipped — retrieval degrades, never blocks.
    std::vector<std::pair<std::string, std::string>> docs;
    docs.reserve(refs.size());
    for (auto& r : refs) {
        auto body = read_(r.uri);
        if (!body || body->empty()) continue;
        docs.emplace_back(r.uri, std::move(*body));
    }
    if (docs.empty()) return;

    corpus_.build_from_memory(docs, embed_);
}

std::vector<Hit>
McpResourceSource::retrieve(std::string_view query, std::size_t k) const {
    if (!built_) build_index_();
    if (corpus_.chunk_count() == 0) return {};
    auto hits = corpus_.search(query, embed_, k);
    for (auto& h : hits) h.source = this;   // stamp provenance
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
    //
    // CRUCIAL: the SAME chunk surfaced by two sources (overlapping corpora,
    // or one doc indexed in both a folder and an MCP source) must collapse to
    // ONE pool id. Otherwise RRF sees two distinct documents and (a) can't
    // reinforce a chunk that appears in multiple lists — the entire point of
    // fusion — and (b) the output carries visible duplicate chunks. Key on
    // (path, line span) so identical chunks share an id and their per-list
    // rank contributions sum.
    std::vector<Hit>                       pool;     // id -> hit
    std::vector<std::vector<std::uint32_t>> lists;   // per-source ranked ids
    std::unordered_map<std::string, std::uint32_t> id_of;  // chunk key -> id
    lists.reserve(sources_.size());

    auto key_of = [](const Hit& h) {
        const Chunk* c = h.chunk;
        if (!c) return std::string{};
        std::string k = c->path;
        k.push_back('\0');
        k += std::to_string(c->line_start);
        k.push_back(':');
        k += std::to_string(c->line_end);
        return k;
    };

    for (const auto& src : sources_) {
        auto hits = src->retrieve(query, per_source_k);
        std::vector<std::uint32_t> ids;
        ids.reserve(hits.size());
        std::unordered_set<std::uint32_t> seen_this_list;  // de-dup within a source
        for (auto& h : hits) {
            std::string key = key_of(h);
            std::uint32_t id;
            auto it = key.empty() ? id_of.end() : id_of.find(key);
            if (it != id_of.end()) {
                id = it->second;
            } else {
                id = static_cast<std::uint32_t>(pool.size());
                pool.push_back(h);   // already source-stamped by the source
                if (!key.empty()) id_of.emplace(std::move(key), id);
            }
            // A source listing the same chunk twice must not double-count it
            // in its own ranked list (RRF rank is per-list-position).
            if (seen_this_list.insert(id).second) ids.push_back(id);
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

Context NeuralRerankStage::process(Context ctx) const {
    // Lift hits, run neural rerank, re-wrap.
    std::vector<Hit> hits;
    hits.reserve(ctx.chunks.size());
    for (auto& c : ctx.chunks) hits.push_back(c.hit);

    auto ranked = neural_rerank(ctx.query, std::move(hits), out_k_, cfg_);
    return Context::from_hits(std::move(ctx.query), std::move(ranked));
}

Context MMRStage::process(Context ctx) const {
    // Lift hits, run MMR diversification, re-wrap.
    std::vector<Hit> hits;
    hits.reserve(ctx.chunks.size());
    for (auto& c : ctx.chunks) hits.push_back(c.hit);

    auto diverse = mmr_diversify(std::move(hits), out_k_, lambda_);
    return Context::from_hits(std::move(ctx.query), std::move(diverse));
}

Context NormalizeQueryStage::process(Context ctx) const {
    std::string q = ctx.query;
    
    // Lowercase.
    if (cfg_.lowercase) {
        for (auto& c : q)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    
    // Normalize whitespace: collapse runs, trim.
    if (cfg_.normalize_whitespace) {
        std::string out;
        out.reserve(q.size());
        bool in_space = true;  // Start true to skip leading space.
        for (char c : q) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!in_space && !out.empty()) {
                    out.push_back(' ');
                    in_space = true;
                }
            } else {
                out.push_back(c);
                in_space = false;
            }
        }
        // Trim trailing space.
        while (!out.empty() && out.back() == ' ') out.pop_back();
        q = std::move(out);
    }
    
    ctx.query = std::move(q);
    return ctx;
}

} // namespace agentty::rag
