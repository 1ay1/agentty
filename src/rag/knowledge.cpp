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
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agentty::rag {

// ── Context::compute_confidence ───────────────────────────────────────
//
// CALIBRATED confidence. The previous formula read the score distribution
// of the ranked list — but rerank() min-max-normalizes features per
// candidate set, so the top score is ~1.0 BY CONSTRUCTION even when every
// candidate is garbage (a zero-overlap query over an unrelated corpus
// scored "high confidence" and the proactive path injected noise into the
// user's context). Confidence must be anchored on something ABSOLUTE.
//
// Anchor: does the retrieved text actually contain the query's content
// words? We measure per-chunk term coverage over the top hits:
//   • cover_top  — fraction of content terms present in the #1 hit
//   • cover_pool — fraction present anywhere in the top 3 hits
//   • corroboration bonus when ≥2 distinct sources each cover ≥half the
//     query (independent agreement is the classic precision signal)
// Stopwords and <3-char tokens are excluded so "the/and/it" matching
// everywhere can't inflate the score. When the query has NO content terms
// (emoji, pure stopwords) we return a deliberately UNCERTAIN score — the
// proactive HIGH bar (0.45) must not clear on evidence we can't measure.
namespace {

const std::unordered_set<std::string>& confidence_stopwords() {
    static const std::unordered_set<std::string> stop = {
        "the","and","for","are","was","were","you","your","our","has",
        "have","had","this","that","with","about","into","from","what",
        "when","where","which","who","why","how","can","could","should",
        "would","does","did","not","but","all","any","its","it","is",
        "in","on","of","to","a","an","or","be","do","me","my","we",
        "i","at","by","as","so","if","out","get","make","want","need",
        "tell","show","please","thanks","just","like","some","more"};
    return stop;
}

// Lowercase alnum-run tokens of length ≥2 (mirrors the rerank tokenizer
// closely enough for containment checks; no stemming — the prefix rule
// below absorbs mild morphological drift).
void confidence_tokens(std::string_view s, std::unordered_set<std::string>& out) {
    std::string cur;
    cur.reserve(24);
    auto flush = [&] {
        if (cur.size() >= 2) out.insert(cur);
        cur.clear();
    };
    for (unsigned char c : s) {
        if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
        else                 flush();
    }
    flush();
}

// Term ∈ tokens? Exact match, or (for terms ≥4 chars) either side is a
// prefix of the other ("deploy" ↔ "deployment", "config" ↔ "configure").
bool term_covered(const std::unordered_set<std::string>& toks,
                  const std::string& term) {
    if (toks.count(term)) return true;
    if (term.size() < 4) return false;
    for (const auto& t : toks) {
        if (t.size() < 4) continue;
        const auto& shorter = t.size() < term.size() ? t : term;
        const auto& longer  = t.size() < term.size() ? term : t;
        if (longer.compare(0, shorter.size(), shorter) == 0) return true;
    }
    return false;
}

} // namespace

void Context::compute_confidence() noexcept {
    confidence = 0.0;
    if (chunks.empty()) return;
    try {
        // Content terms of the query (stopwords + short tokens dropped).
        std::vector<std::string> qterms;
        {
            const auto& stop = confidence_stopwords();
            for (auto& t : query_terms(query))
                if (t.size() >= 3 && !stop.count(t)) qterms.push_back(std::move(t));
        }
        if (qterms.empty()) {
            // No measurable evidence — report UNCERTAIN, never confident.
            // (Scores are rank-relative; trusting them here re-opens the
            // "normalized garbage looks perfect" hole this fix closes.)
            double top = std::clamp(chunks[0].hit.score, 0.0, 1.0);
            confidence = 0.20 * top;
            return;
        }

        const std::size_t m = std::min<std::size_t>(3, chunks.size());
        std::vector<double> cov(m, 0.0);
        std::unordered_set<std::string> pool_covered;
        for (std::size_t i = 0; i < m; ++i) {
            const Chunk* c = chunks[i].hit.chunk;
            if (!c) continue;
            std::unordered_set<std::string> toks;
            confidence_tokens(c->text, toks);
            if (!c->context.empty()) confidence_tokens(c->context, toks);
            // The path often carries the topic ("docs/oauth.md").
            confidence_tokens(c->path, toks);
            std::size_t n = 0;
            for (const auto& t : qterms)
                if (term_covered(toks, t)) { ++n; pool_covered.insert(t); }
            cov[i] = static_cast<double>(n)
                   / static_cast<double>(qterms.size());
        }
        const double cover_top  = cov[0];
        const double cover_pool = static_cast<double>(pool_covered.size())
                                / static_cast<double>(qterms.size());

        // Corroboration: ≥2 hits from DISTINCT paths each covering ≥ half
        // the query is strong independent agreement.
        double bonus = 0.0;
        {
            std::unordered_set<std::string> paths;
            for (std::size_t i = 0; i < m; ++i)
                if (cov[i] >= 0.5 && chunks[i].hit.chunk)
                    paths.insert(chunks[i].hit.chunk->path);
            if (paths.size() >= 2) bonus = 0.10;
        }

        confidence = std::clamp(0.65 * cover_top + 0.35 * cover_pool + bonus,
                                0.0, 1.0);
    } catch (...) {
        confidence = 0.0;   // noexcept promise: any failure → unconfident
    }
}

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
