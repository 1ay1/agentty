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

// Chunk identity for cross-list dedup: path + line span. A chunk surfaced by
// two query variants OR two sources must collapse to ONE fused entry so its
// per-list rank contributions REINFORCE instead of appearing as duplicates.
static std::string hit_key_(const Hit& h) {
    const Chunk* c = h.chunk;
    if (!c) return std::string{};
    std::string kk = c->path;
    kk.push_back('\0');
    kk += std::to_string(c->line_start);
    kk.push_back(':');
    kk += std::to_string(c->line_end);
    return kk;
}

// Fuse several ranked hit-lists into ONE via RRF (k=60 canonical), deduping
// identical chunks by hit_key_ so shared hits reinforce. The FUSED score is
// written onto each surviving hit; provenance is taken from the first
// occurrence. Shared by the KnowledgeSource multi-query default and the
// KnowledgeRouter cross-source fuse — one fusion implementation, two callers.
static std::vector<Hit>
fuse_hit_lists_(const std::vector<std::vector<Hit>>& hitlists, std::size_t k) {
    std::vector<Hit>                                pool;
    std::vector<std::vector<std::uint32_t>>         lists;
    std::unordered_map<std::string, std::uint32_t>  id_of;
    lists.reserve(hitlists.size());
    for (const auto& hits : hitlists) {
        std::vector<std::uint32_t> ids;
        ids.reserve(hits.size());
        std::unordered_set<std::uint32_t> seen;   // de-dup within one list
        for (const auto& h : hits) {
            std::string key = hit_key_(h);
            std::uint32_t id;
            auto it = key.empty() ? id_of.end() : id_of.find(key);
            if (it != id_of.end()) {
                id = it->second;
            } else {
                id = static_cast<std::uint32_t>(pool.size());
                pool.push_back(h);
                if (!key.empty()) id_of.emplace(std::move(key), id);
            }
            if (seen.insert(id).second) ids.push_back(id);
        }
        if (!ids.empty()) lists.push_back(std::move(ids));
    }
    if (pool.empty()) return {};
    auto fused = reciprocal_rank_fusion(lists, /*k=*/60.0, k);
    std::vector<Hit> out;
    out.reserve(fused.size());
    for (auto& [id, score] : fused) {
        if (id >= pool.size()) continue;
        Hit h = pool[id];
        h.score = score;
        out.push_back(h);
    }
    return out;
}

// Base KnowledgeSource::retrieve_multi: fuse per-variant retrieve() calls.
// This is the fallback for any source WITHOUT a batched embedding path; a
// source that can embed all variants in one round-trip (CorpusSource) should
// override. Never throws (each retrieve() already degrades to {}).
std::vector<Hit>
KnowledgeSource::retrieve_multi(const std::vector<std::string>& queries,
                                std::size_t k) const {
    if (queries.empty()) return {};
    if (queries.size() == 1) return retrieve(queries.front(), k);
    std::vector<std::vector<Hit>> lists;
    lists.reserve(queries.size());
    for (const auto& q : queries)
        if (!q.empty()) lists.push_back(retrieve(q, k));
    return fuse_hit_lists_(lists, k);
}

std::vector<Hit>
CorpusSource::retrieve(std::string_view query, std::size_t k) const {
    auto hits = corpus_->search(query, embed_, k);
    for (auto& h : hits) h.source = this;   // stamp provenance
    return hits;
}

std::vector<Hit>
CorpusSource::retrieve_multi(const std::vector<std::string>& queries,
                             std::size_t k) const {
    // ONE batched /api/embed round-trip for ALL variants (Corpus::search_fused
    // embeds them together), then RRF over each variant's BM25/PRF/dense
    // sub-lists internally. This is the seam that makes the batched dense path
    // actually reach the funnel: the router now calls retrieve_multi, so a
    // multi-variant search no longer fans out into N serial single-query
    // embeds.
    if (queries.empty()) return {};
    if (queries.size() == 1) return retrieve(queries.front(), k);
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

std::vector<Hit>
McpResourceSource::retrieve_multi(const std::vector<std::string>& queries,
                                  std::size_t k) const {
    if (queries.empty()) return {};
    if (queries.size() == 1) return retrieve(queries.front(), k);
    if (!built_) build_index_();
    if (corpus_.chunk_count() == 0) return {};
    auto hits = corpus_.search_fused(queries, embed_, k);   // batched embed
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

    return retrieve_multi({std::string(query)}, k, per_source_k);
}

std::vector<Hit>
KnowledgeRouter::retrieve_multi(const std::vector<std::string>& queries,
                                std::size_t k, std::size_t per_source_k) const {
    if (sources_.empty() || k == 0) return {};

    // Collapse to the empty / single-query case cleanly.
    std::vector<std::string> qs;
    for (const auto& q : queries) if (!q.empty()) qs.push_back(q);
    if (qs.empty()) return {};

    // Single source AND single query: short-circuit (zero fusion overhead).
    if (sources_.size() == 1 && qs.size() == 1)
        return sources_.front()->retrieve(qs.front(), k);

    if (per_source_k == 0) per_source_k = k;

    // Ask EACH source for ONE internally-fused list over ALL variants. This
    // is the load-bearing change: a source's retrieve_multi collapses the N
    // variant embeds into ONE batched /api/embed round-trip (CorpusSource /
    // McpResourceSource override), so the funnel's expansion + HyDE + multi-
    // hop probes no longer fan out into N serial single-query embeds PER
    // source. Sources are independent (own corpus, own embed) but retrieved
    // sequentially here — concurrent fan-out is a separate, thread-safety-
    // gated change (the HTTP embed client is shared).
    std::vector<std::vector<Hit>> per_source;
    per_source.reserve(sources_.size());
    for (const auto& src : sources_)
        per_source.push_back(src->retrieve_multi(qs, per_source_k));

    // One source: its list is already the fused answer (across all variants);
    // no cross-source step, just trim to k.
    if (per_source.size() == 1) {
        auto out = std::move(per_source.front());
        if (out.size() > k) out.resize(k);
        return out;
    }

    // Multiple sources: fuse their (already variant-fused) lists together,
    // deduping shared chunks by identity so a hit both a docs page and an MCP
    // resource surface reinforces. Hierarchical RRF (within-source, then
    // across-source) — each source contributes its full signal.
    return fuse_hit_lists_(per_source, k);
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

// True when `w` is the balanced default profile (RerankWeights{}). Used to
// decide whether query-shape-adaptive weights may override — an explicit
// caller-tuned profile is always respected.
static bool is_default_weights_(const RerankWeights& w) noexcept {
    const RerankWeights d{};
    auto eq = [](double a, double b) { return a == b; };
    return eq(w.fused, d.fused) && eq(w.dense, d.dense) &&
           eq(w.term_coverage, d.term_coverage) && eq(w.proximity, d.proximity) &&
           eq(w.path_match, d.path_match) && eq(w.phrase_match, d.phrase_match);
}

Context RerankStage::process(Context ctx) const {
    // rerank() works on a flat Hit vector; lift the hits out of the Context,
    // rerank, then re-wrap. Compressed text (if any was set earlier — it
    // isn't, rerank runs before compress) would be dropped, which is correct:
    // rerank precedes compress in the canonical pipeline.
    std::vector<Hit> hits;
    hits.reserve(ctx.chunks.size());
    for (auto& c : ctx.chunks) hits.push_back(c.hit);

    // Semantics-aware default reranking: feed the calibrated cosine(query,
    // chunk) in as a rerank feature. The query embedding is computed AT MOST
    // ONCE per funnel run and cached on the Context — if an upstream stage
    // already embedded it we reuse that vector for free; otherwise we embed
    // here (query-time capped, so a wedged Ollama can't stall the pipeline)
    // and stash the result so the downstream embed-rerank / late-interaction
    // tiers skip a second /api/embed round-trip. No model / no backend →
    // empty vector → rerank() falls back to pure lexical.
    std::vector<float> qvec = ctx.query_vec;
    if (ctx.query_vec_dims == 0 && !embed_.model.empty() && !hits.empty()) {
        EmbedConfig ec = embed_;
        ec.timeout_ms = 10'000;   // query path: never block on a slow backend
        if (auto v = embed_texts(ec, {std::string{ctx.query}}); v && !v->empty())
            qvec = std::move((*v)[0]);
        // Mark computed either way (empty on failure) so we don't retry the
        // same wedged backend on every later stage this run.
        ctx.query_vec      = qvec;
        ctx.query_vec_dims = 1;
    }

    // QUERY-SHAPE-ADAPTIVE weights: if the caller left w_ at the balanced
    // default and adaptivity is on, derive a per-query profile (lexical vs
    // conceptual). A caller that passed explicit non-default weights keeps
    // full control. Deterministic, zero network.
    RerankWeights eff = w_;
    if (adaptive_ && is_default_weights_(w_))
        eff = weights_for_query(ctx.query);

    auto ranked = rerank(ctx.query, std::move(hits), out_k_, eff,
                         qvec.empty() ? nullptr : &qvec);
    Context out = Context::from_hits(std::move(ctx.query), std::move(ranked));
    // from_hits builds a fresh Context; carry the shared query embedding
    // forward so downstream stages inherit the cache.
    out.query_vec      = std::move(qvec);
    out.query_vec_dims = ctx.query_vec_dims ? ctx.query_vec_dims : (out.query_vec.empty() ? 0 : 1);
    return out;
}

Context CompressStage::process(Context ctx) const {
    for (auto& c : ctx.chunks) {
        if (!c.hit.chunk) continue;
        c.compressed = compress(ctx.query, c.hit.chunk->text, target_chars_);
    }
    return ctx;
}

Context ParentExpandStage::process(Context ctx) const {
    if (radius_ == 0) return ctx;
    for (auto& c : ctx.chunks) {
        if (!c.hit.chunk || !c.hit.source) continue;   // no source → no siblings
        const Chunk& hit = *c.hit.chunk;
        auto sibs = c.hit.source->neighbors(hit, radius_);
        if (sibs.empty()) continue;

        // The text we're expanding AROUND is whatever the pipeline decided to
        // show so far (a compressed span, or the raw body). Siblings are the
        // FULL neighbouring bodies — their surrounding prose is the point.
        std::string_view shown = c.text();

        // Partition siblings into those reading BEFORE the hit and AFTER it,
        // by line position, so the stitch preserves document order.
        std::string before, after;
        for (const Chunk* s : sibs) {
            if (!s) continue;
            std::string& dst = (s->line_start < hit.line_start) ? before : after;
            if (!dst.empty()) dst += "\n\n";
            dst += s->text;
        }

        std::string stitched;
        stitched.reserve(before.size() + shown.size() + after.size() + 4);
        if (!before.empty()) { stitched += before; stitched += "\n\n"; }
        stitched += shown;
        if (!after.empty())  { stitched += "\n\n"; stitched += after; }

        // Cap the stitched window so one expanded chunk can't dominate the
        // context slice. If we'd overrun, prefer keeping the (higher-signal)
        // shown text + trailing context and trim from the leading side.
        if (budget_chars_ && stitched.size() > budget_chars_) {
            std::string kept = std::string(shown);
            if (!after.empty() && kept.size() + 2 + after.size() <= budget_chars_)
                kept += "\n\n" + after;
            if (kept.size() > budget_chars_) kept.resize(budget_chars_);
            stitched = std::move(kept);
        }

        // Only take the expansion when it actually adds context.
        if (stitched.size() > shown.size())
            c.expanded = std::move(stitched);
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

Context EmbedRerankStage::process(Context ctx) const {
    // Lift hits, run the batched embedding cross-encoder rerank, re-wrap.
    std::vector<Hit> hits;
    hits.reserve(ctx.chunks.size());
    for (auto& c : ctx.chunks) hits.push_back(c.hit);

    // Reuse the query embedding the rerank stage already cached this run.
    const std::vector<float>* qv =
        ctx.query_vec.empty() ? nullptr : &ctx.query_vec;
    auto ranked = embed_rerank(ctx.query, std::move(hits), out_k_, cfg_, qv);
    Context out = Context::from_hits(std::move(ctx.query), std::move(ranked));
    out.query_vec      = std::move(ctx.query_vec);
    out.query_vec_dims = ctx.query_vec_dims;
    return out;
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
