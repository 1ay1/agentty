// agentty::rag — feature-fusion reranker + extractive context compression,
// plus the OPT-IN neural (Ollama) reranker and MMR diversifier.
// The lexical reranker/compressor is pure C++/STL, deterministic, no network.
// neural_rerank() is the only network path here and is opt-in. See
// rerank.hpp for rationale.

#include "agentty/rag/rerank.hpp"
#include "agentty/io/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <future>
#include <unordered_set>

namespace agentty::rag {

namespace {

void lower_inplace(std::string& s) {
    for (auto& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
}

// Tokenize to lowercase alphanumeric runs ≥2 chars.
std::vector<std::string> tokenize(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] { if (cur.size() >= 2) out.push_back(cur); cur.clear(); };
    for (unsigned char c : s) {
        if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
        else                 flush();
    }
    flush();
    return out;
}

// Min-normalize a vector of doubles to [0,1]; constant vectors → all 1.0
// (so a feature that's uniform across candidates doesn't zero out).
void normalize01(std::vector<double>& v) {
    if (v.empty()) return;
    double lo = v[0], hi = v[0];
    for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
    double span = hi - lo;
    if (span <= 0.0) { for (double& x : v) x = (hi > 0.0) ? 1.0 : 0.0; return; }
    for (double& x : v) x = (x - lo) / span;
}

} // namespace

bool is_stopword(std::string_view token) noexcept {
    // Compact English stopword list shared by the reranker's query_terms and
    // (via this export) the CRAG distiller. Deliberately conservative: only
    // glue words that essentially never carry retrieval intent. Domain-y
    // short words ("api", "gpu", "ssh") are NOT here.
    static const std::unordered_set<std::string_view> kStop = {
        "the","a","an","of","to","in","on","for","and","or","is","are",
        "was","were","be","been","being","do","does","did","how","what",
        "why","when","where","which","who","whom","can","could","will",
        "would","should","you","your","yours","me","my","mine","we","our",
        "ours","it","its","this","that","these","those","with","about",
        "tell","show","please","there","here","from","into","than","then",
        "them","they","their","have","has","had","not","but","all","any",
        "some","just","also","very","more","most","such","so","at","by",
        "as","if","up","out","get","got","one","two","way","i","am","us"};
    return kStop.count(token) != 0;
}

std::vector<std::string> query_terms(std::string_view query) {
    auto toks = tokenize(query);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (auto& t : toks)
        if (!is_stopword(t) && seen.insert(t).second) out.push_back(t);
    if (!out.empty()) return out;
    // All-stopword query ("what is this"): fall back to the raw tokens so
    // coverage/proximity still have something to match rather than zeroing.
    for (auto& t : toks) if (seen.insert(t).second) out.push_back(t);
    return out;
}

std::vector<Hit>
rerank(std::string_view query, std::vector<Hit> hits,
       std::size_t out_k, const RerankWeights& w,
       const std::vector<float>* query_vec) {
    if (hits.empty()) return hits;

    // COLLAPSE overlapping-window near-duplicates from the SAME file before
    // scoring. The chunker emits overlapping windows (overlap_lines), so a
    // relevant region can surface as 2-3 chunks with near-identical text; left
    // in the pool they let one document crowd out the top-k and starve other
    // sources. Keep, per (path, overlapping line-range cluster), only the
    // highest first-pass-scored chunk. Sources are distinguished so identical
    // ranges from different KnowledgeSources are NOT merged.
    {
        // Highest score first so the survivor of each cluster is the best hit;
        // stable so equal scores keep first-pass order.
        std::vector<std::size_t> ord(hits.size());
        for (std::size_t i = 0; i < hits.size(); ++i) ord[i] = i;
        std::stable_sort(ord.begin(), ord.end(),
            [&](std::size_t a, std::size_t b) { return hits[a].score > hits[b].score; });

        struct Range { const void* src; std::string path; int lo, hi; };
        std::vector<Range> kept_ranges;
        std::vector<Hit> deduped;
        deduped.reserve(hits.size());
        for (std::size_t idx : ord) {
            const Hit& h = hits[idx];
            const Chunk* c = h.chunk;
            if (!c) { deduped.push_back(h); continue; }
            bool dup = false;
            for (const auto& r : kept_ranges) {
                if (r.src != static_cast<const void*>(h.source)) continue;
                if (r.path != c->path) continue;
                // Overlap (inclusive ranges) or touching → same region.
                if (c->line_start <= r.hi && r.lo <= c->line_end) { dup = true; break; }
            }
            if (dup) continue;
            kept_ranges.push_back({static_cast<const void*>(h.source), c->path,
                                   c->line_start, c->line_end});
            deduped.push_back(h);
        }
        hits = std::move(deduped);
    }

    const auto qterms = query_terms(query);
    std::string qlower{query};
    lower_inplace(qlower);
    // Trim the phrase to its alnum/space core for the verbatim check.
    std::string qphrase;
    for (char c : qlower)
        qphrase.push_back(std::isalnum((unsigned char)c) ? c : ' ');
    // collapse spaces
    {
        std::string t; bool sp = false;
        for (char c : qphrase) {
            if (c == ' ') { if (!sp && !t.empty()) t.push_back(' '); sp = true; }
            else { t.push_back(c); sp = false; }
        }
        while (!t.empty() && t.back() == ' ') t.pop_back();
        qphrase.swap(t);
    }

    const std::size_t n = hits.size();
    std::vector<double> f_fused(n), f_dense(n), f_cover(n), f_prox(n), f_path(n), f_phrase(n);

    // Dense feature is live only when we were handed a usable query vector.
    const bool have_qvec = query_vec && !query_vec->empty();

    std::unordered_set<std::string> qset(qterms.begin(), qterms.end());

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk* c = hits[i].chunk;
        f_fused[i] = hits[i].score;
        if (!c) continue;

        // CALIBRATED dense similarity: cosine(query, chunk) recovers the score
        // magnitude RRF's rank fusion threw away. 0 when either side lacks a
        // (matching-dim) embedding — BM25-only builds behave exactly as before.
        if (have_qvec && c->embedding.size() == query_vec->size())
            f_dense[i] = std::max(0.0, cosine(*query_vec, c->embedding));

        // Tokenize the chunk once.
        auto ctoks = tokenize(c->text);

        // term_coverage: fraction of DISTINCT query terms present.
        if (!qterms.empty()) {
            std::unordered_set<std::string> present;
            for (auto& t : ctoks) if (qset.count(t)) present.insert(t);
            f_cover[i] = static_cast<double>(present.size()) /
                         static_cast<double>(qterms.size());
        }

        // proximity: 1/(1+min window span covering the most query terms).
        // Cheap approximation — smallest index gap between any two distinct
        // query-term hits; fewer gaps / tighter = higher. We scan positions
        // of query terms and take the tightest pair-of-distinct-terms window.
        if (qterms.size() >= 2) {
            std::size_t best = SIZE_MAX;
            std::size_t last_pos = SIZE_MAX;
            std::string last_term;
            for (std::size_t p = 0; p < ctoks.size(); ++p) {
                if (!qset.count(ctoks[p])) continue;
                if (last_pos != SIZE_MAX && ctoks[p] != last_term)
                    best = std::min(best, p - last_pos);
                last_pos = p;
                last_term = ctoks[p];
            }
            if (best != SIZE_MAX)
                f_prox[i] = 1.0 / (1.0 + static_cast<double>(best));
        } else {
            f_prox[i] = f_cover[i];  // single-term query: proximity == coverage
        }

        // path_match: any query term appears in the (lowercased) file path.
        {
            std::string pl = c->path;
            lower_inplace(pl);
            for (auto& t : qterms)
                if (pl.find(t) != std::string::npos) { f_path[i] = 1.0; break; }
        }

        // phrase_match: the verbatim query phrase appears in the chunk.
        if (!qphrase.empty()) {
            std::string tl = c->text;
            lower_inplace(tl);
            if (tl.find(qphrase) != std::string::npos) f_phrase[i] = 1.0;
        }
    }

    normalize01(f_fused);
    normalize01(f_dense);
    normalize01(f_cover);
    normalize01(f_prox);
    // path/phrase are already 0/1; leave as-is.

    std::vector<std::pair<double, std::size_t>> scored(n);
    for (std::size_t i = 0; i < n; ++i) {
        double s = w.fused        * f_fused[i]
                 + w.dense        * f_dense[i]
                 + w.term_coverage* f_cover[i]
                 + w.proximity    * f_prox[i]
                 + w.path_match   * f_path[i]
                 + w.phrase_match * f_phrase[i];
        scored[i] = {s, i};
    }
    std::stable_sort(scored.begin(), scored.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Hit> out;
    out.reserve(std::min(out_k, n));
    for (std::size_t i = 0; i < n && i < out_k; ++i) {
        Hit h = hits[scored[i].second];
        h.score = scored[i].first;   // expose the rerank score
        out.push_back(h);
    }
    return out;
}

// ── Context compression ──────────────────────────────────────────────────

namespace {

// Split text into sentence-ish spans. Boundaries: '.', '!', '?', newline, and
// double-newline (paragraph). Keeps spans pointing into the original text so
// the returned compression preserves exact bytes.
std::vector<std::pair<std::size_t, std::size_t>>
sentence_spans(std::string_view t) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    std::size_t start = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        bool boundary = (c == '.' || c == '!' || c == '?' || c == '\n');
        if (boundary) {
            // consume trailing closing chars / whitespace into this span
            std::size_t end = i + 1;
            while (end < t.size() && (t[end] == '"' || t[end] == ')' ||
                                       t[end] == '\'' || t[end] == ' ' ||
                                       t[end] == '\t')) ++end;
            if (end > start) spans.push_back({start, end});
            start = end;
            i = end - 1;
        }
    }
    if (start < t.size()) spans.push_back({start, t.size()});
    return spans;
}

} // namespace

std::string
compress(std::string_view query, std::string_view text,
         std::size_t target_chars) {
    if (text.empty()) return {};
    if (text.size() <= target_chars) return std::string{text};

    const auto qterms = query_terms(query);
    std::unordered_set<std::string> qset(qterms.begin(), qterms.end());

    auto spans = sentence_spans(text);
    if (spans.empty()) return std::string{text.substr(0, target_chars)};

    // Score each sentence by query-term hits (with a small length penalty so
    // a giant sentence doesn't win purely by containing more tokens).
    std::vector<double> score(spans.size(), 0.0);
    for (std::size_t s = 0; s < spans.size(); ++s) {
        std::string_view seg = text.substr(spans[s].first,
                                            spans[s].second - spans[s].first);
        auto toks = tokenize(seg);
        int hits = 0;
        for (auto& t : toks) if (qset.count(t)) ++hits;
        double len = static_cast<double>(seg.size());
        score[s] = (hits > 0)
            ? static_cast<double>(hits) / (1.0 + len / 200.0)
            : 0.0;
    }

    // Find the best contiguous window of sentences under target_chars: a
    // simple expand-around-best-seed. Pick the highest-scoring sentence, then
    // greedily grow outward (whichever neighbour has the higher score) while
    // staying under budget. Keeps a coherent passage rather than scattered
    // sentence fragments.
    std::size_t seed = 0;
    for (std::size_t s = 1; s < spans.size(); ++s)
        if (score[s] > score[seed]) seed = s;

    // If NOTHING matched lexically, DON'T pretend the head of the chunk is
    // the relevant part — a zero-overlap chunk that survived to top-k got
    // there via DENSE similarity, and its semantically-relevant span is as
    // likely to be in the middle or tail as the head. Return empty: the
    // ContextChunk contract (`compressed.empty() → use the full chunk`)
    // keeps the whole passage, which is strictly safer than a misleading
    // truncation. Callers that need a hard budget can slice AFTER deciding
    // the chunk is worth keeping.
    if (score[seed] <= 0.0)
        return {};

    std::size_t lo = seed, hi = seed;
    auto span_len = [&](std::size_t a, std::size_t b) {
        return spans[b].second - spans[a].first;
    };
    while (true) {
        bool can_left  = lo > 0;
        bool can_right = hi + 1 < spans.size();
        if (!can_left && !can_right) break;
        // Prefer growing toward the higher-scoring neighbour.
        double ls = can_left  ? score[lo - 1] : -1.0;
        double rs = can_right ? score[hi + 1] : -1.0;
        bool grow_left;
        if (can_left && can_right) grow_left = (ls >= rs);
        else                       grow_left = can_left;

        std::size_t nlo = grow_left ? lo - 1 : lo;
        std::size_t nhi = grow_left ? hi : hi + 1;
        if (span_len(nlo, nhi) > target_chars) {
            // Can't grow that side under budget; try the other once.
            if (grow_left && can_right && span_len(lo, hi + 1) <= target_chars) {
                hi = hi + 1; continue;
            }
            if (!grow_left && can_left && span_len(lo - 1, hi) <= target_chars) {
                lo = lo - 1; continue;
            }
            break;
        }
        lo = nlo; hi = nhi;
    }

    std::string out{text.substr(spans[lo].first, spans[hi].second - spans[lo].first)};
    // Trim leading whitespace for a clean passage.
    std::size_t b = 0;
    while (b < out.size() && (out[b] == ' ' || out[b] == '\n' ||
                              out[b] == '\t' || out[b] == '\r')) ++b;
    if (b) out.erase(0, b);
    return out;
}

// ── Neural reranking via Ollama (OPT-IN) ───────────────────────────────────
//
// A true cross-encoder would run a single transformer forward pass per
// (query, passage) pair. We approximate that with a deterministic
// (temperature 0) generative scoring prompt against the already-running
// Ollama server: each passage gets an integer relevance 0–10. This is the
// only network path in this file and is explicitly opt-in (cfg.model set).

namespace {

using json = nlohmann::json;

// Parse the first integer appearing in `txt`; std::nullopt if none. Uses
// std::from_chars (no exceptions, no locale) per the no-throw convention.
std::optional<int> first_int(std::string_view txt) noexcept {
    std::size_t i = 0;
    while (i < txt.size() && !std::isdigit(static_cast<unsigned char>(txt[i]))) ++i;
    if (i >= txt.size()) return std::nullopt;
    std::size_t j = i;
    while (j < txt.size() && std::isdigit(static_cast<unsigned char>(txt[j]))) ++j;
    int v = 0;
    auto [ptr, ec] = std::from_chars(txt.data() + i, txt.data() + j, v);
    if (ec != std::errc{}) return std::nullopt;
    return v;
}

// Score one (query, passage) pair via Ollama /api/generate. Returns a score
// in [0,1], or std::nullopt on ANY failure (network, parse, no number) so
// the caller can detect a total backend outage and degrade. No exceptions
// escape: json::parse is called in the no-throw (accept_invalid) form.
std::optional<double> score_one(const NeuralRerankConfig& cfg,
                                std::string_view query,
                                std::string_view passage) noexcept {
    // Bound the prompt: long passages neither help scoring nor are worth the
    // tokens/latency.
    std::string_view pass = passage.substr(0, std::min<std::size_t>(passage.size(), 2000));

    json body;
    body["model"]   = cfg.model;
    // Graded rubric with anchored levels scores FAR more consistently across
    // small local models than a bare "0–10" instruction: the anchors pin the
    // scale so "6" means the same thing call-to-call, and the emphasis on
    // ANSWERING (not merely mentioning terms) is what separates a real
    // cross-encoder from lexical overlap. Still deterministic (temp 0),
    // still a single integer out — the parser is unchanged.
    body["prompt"]  = std::string(
        "You are a precise relevance judge (a cross-encoder). Rate how well "
        "the PASSAGE answers or directly informs the QUERY, on this scale:\n"
        "  0  = unrelated; different topic entirely\n"
        "  2  = same broad area but does NOT address the query\n"
        "  4  = mentions the query's terms but doesn't answer it\n"
        "  6  = partially answers, or answers a related sub-question\n"
        "  8  = answers the query, possibly needing minor inference\n"
        "  10 = directly and completely answers the query\n"
        "Judge by whether the passage ANSWERS the query, not by shared "
        "keywords. Output ONLY the single integer, nothing else.\n\nQUERY: ")
        + std::string(query) + "\n\nPASSAGE: " + std::string(pass) + "\n\nRELEVANCE:";
    body["stream"]  = false;
    body["options"] = {{"temperature", 0.0}, {"num_predict", 8}};

    http::Request req;
    req.method         = http::HttpMethod::Post;
    req.host           = cfg.host;
    req.port           = cfg.port;
    req.path           = "/api/generate";
    req.plaintext      = true;
    req.headers        = {{"content-type", "application/json"}};
    req.body           = body.dump();
    req.max_body_bytes = 64 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(3'000);
    tos.total   = std::chrono::milliseconds(
        static_cast<long long>(cfg.timeout_s * 1000.0));

    auto resp = http::default_client().send(req, tos);
    if (!resp || resp->status != 200) return std::nullopt;

    json j = json::parse(resp->body, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.contains("response") || !j["response"].is_string())
        return std::nullopt;
    auto n = first_int(j["response"].get_ref<const std::string&>());
    if (!n) return std::nullopt;
    return std::clamp(*n / 10.0, 0.0, 1.0);
}

} // namespace

std::vector<Hit>
neural_rerank(std::string_view query, std::vector<Hit> hits,
              std::size_t out_k, const NeuralRerankConfig& cfg) {
    auto truncate = [&](std::vector<Hit> h) {
        if (h.size() > out_k) h.resize(out_k);
        return h;
    };
    if (cfg.model.empty() || hits.empty() || out_k == 0) return truncate(std::move(hits));

    // Bounded fan-out: at most `batch_size` concurrent requests in flight
    // (clamped to a sane ceiling so a huge pool can't spawn hundreds of
    // threads). We score in waves of `width`, joining each wave before the
    // next — std::future fan-out without an unbounded thread explosion.
    const std::size_t width =
        std::clamp<std::size_t>(cfg.batch_size, 1, 16);

    std::vector<std::optional<double>> scores(hits.size());
    bool backend_alive = false;

    for (std::size_t i = 0; i < hits.size(); i += width) {
        const std::size_t hi = std::min(i + width, hits.size());
        std::vector<std::future<std::optional<double>>> wave;
        wave.reserve(hi - i);
        for (std::size_t j = i; j < hi; ++j) {
            if (!hits[j].chunk) { wave.emplace_back(); continue; }
            std::string_view text = hits[j].chunk->text;
            wave.push_back(std::async(std::launch::async,
                [&cfg, query, text] { return score_one(cfg, query, text); }));
        }
        for (std::size_t j = i; j < hi; ++j) {
            auto& f = wave[j - i];
            if (!f.valid()) continue;
            scores[j] = f.get();
            if (scores[j]) backend_alive = true;
        }
    }

    // Total backend outage — keep the upstream (lexical) order untouched.
    if (!backend_alive) return truncate(std::move(hits));

    // Stable sort by neural score desc; unscored chunks sink (score 0) but
    // keep their upstream relative order via the original-score tiebreak.
    std::vector<std::size_t> order(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        double sa = scores[a].value_or(0.0);
        double sb = scores[b].value_or(0.0);
        if (sa != sb) return sa > sb;
        return hits[a].score > hits[b].score;
    });

    std::vector<Hit> out;
    out.reserve(std::min(out_k, order.size()));
    for (std::size_t i = 0; i < order.size() && i < out_k; ++i) {
        Hit h = hits[order[i]];
        h.score = scores[order[i]].value_or(0.0);  // expose the neural score
        out.push_back(h);
    }
    return out;
}

// ── Batched embedding cross-encoder rerank ──────────────────────────

namespace {

// Asymmetric query/document prefixes for the embed reranker. Mirrors the
// index-time logic (corpus.cpp) so query and passage land in the same
// asymmetric space the model was trained for. Kept local (small) rather than
// exporting the corpus statics.
std::string rr_prefix(const std::string& model, std::string text, bool is_query) {
    std::string m;
    for (char c : model) m.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    const bool nomic = m.find("nomic-embed") != std::string::npos;
    const bool e5    = !nomic && m.find("e5") != std::string::npos;
    if (nomic) return (is_query ? "search_query: " : "search_document: ") + text;
    if (e5)    return (is_query ? "query: " : "passage: ") + text;
    return text;
}

} // namespace

std::vector<Hit>
embed_rerank(std::string_view query, std::vector<Hit> hits,
             std::size_t out_k, const EmbedRerankConfig& cfg) {
    auto truncate = [&](std::vector<Hit> h) {
        if (h.size() > out_k) h.resize(out_k);
        return h;
    };
    if (cfg.embed.model.empty() || hits.empty() || out_k == 0)
        return truncate(std::move(hits));

    // Build ONE batch: [query, passage_0, passage_1, ...]. Passages are
    // bounded (long tails neither help scoring nor are worth the tokens).
    // hits with a null chunk contribute an empty passage (they'll score 0
    // and sink), keeping index alignment 1:1 with `hits`.
    std::vector<std::string> batch;
    batch.reserve(hits.size() + 1);
    batch.push_back(cfg.apply_prefixes
                        ? rr_prefix(cfg.embed.model, std::string(query), true)
                        : std::string(query));
    for (const auto& h : hits) {
        std::string_view t = h.chunk ? std::string_view(h.chunk->text)
                                     : std::string_view{};
        std::string p(t.substr(0, std::min<std::size_t>(t.size(), 2000)));
        batch.push_back(cfg.apply_prefixes
                            ? rr_prefix(cfg.embed.model, std::move(p), false)
                            : std::move(p));
    }

    // Single batched /api/embed round-trip (vs N generate calls). Reuse the
    // whole-batch timeout budget.
    EmbedConfig ec = cfg.embed;
    ec.timeout_ms = static_cast<long>(cfg.timeout_s * 1000.0);
    auto vecs = embed_texts(ec, batch);

    // Backend down / malformed / dim-inconsistent → degrade to input order.
    if (!vecs || vecs->size() != batch.size() || vecs->front().empty())
        return truncate(std::move(hits));

    const std::vector<float>& qv = vecs->front();
    std::vector<double> scores(hits.size(), 0.0);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const std::vector<float>& pv = (*vecs)[i + 1];
        if (pv.size() != qv.size() || pv.empty()) { scores[i] = 0.0; continue; }
        // Cosine in [-1,1]; clamp the negative tail to 0 so unscored/irrelevant
        // passages sink but never invert the ordering.
        scores[i] = std::max(0.0, cosine(qv, pv));
    }

    // Stable sort by cosine desc; ties keep upstream order via original score.
    std::vector<std::size_t> order(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (scores[a] != scores[b]) return scores[a] > scores[b];
        return hits[a].score > hits[b].score;
    });

    std::vector<Hit> out;
    out.reserve(std::min(out_k, order.size()));
    for (std::size_t i = 0; i < order.size() && i < out_k; ++i) {
        Hit h = hits[order[i]];
        h.score = scores[order[i]];   // expose the rerank cosine
        out.push_back(h);
    }
    return out;
}

// ── MMR diversification ─────────────────────────────────────────────

namespace {

// Jaccard similarity between two token SETS. Cheap, embedding-free diversity
// proxy. Both sets are precomputed once per chunk by the caller, so this is
// a pure set-intersection — no per-comparison tokenization or set building.
double jaccard_sim(const std::unordered_set<std::string>& a,
                   const std::unordered_set<std::string>& b) noexcept {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    // Iterate the smaller set for the intersection count.
    const auto& small = a.size() <= b.size() ? a : b;
    const auto& big   = a.size() <= b.size() ? b : a;
    std::size_t inter = 0;
    for (const auto& t : small) if (big.count(t)) ++inter;
    std::size_t uni = a.size() + b.size() - inter;
    return uni ? static_cast<double>(inter) / static_cast<double>(uni) : 0.0;
}

} // namespace

std::vector<Hit>
mmr_diversify(std::vector<Hit> hits, std::size_t out_k, double lambda) {
    if (hits.empty() || out_k == 0) return {};
    if (hits.size() <= out_k) return hits;
    lambda = std::clamp(lambda, 0.0, 1.0);

    // Similarity backend: EMBEDDING COSINE when the candidates carry dense
    // vectors (they're already in memory — free, and strictly better at
    // "same idea, different words" redundancy than lexical overlap), with
    // token-set Jaccard as the embedding-free fallback. Mixed pairs (one
    // side missing its vector) also fall back to Jaccard.
    std::vector<std::unordered_set<std::string>> toks(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].chunk) {
            auto v = tokenize(hits[i].chunk->text);
            toks[i] = std::unordered_set<std::string>(
                std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        }
    }
    auto pair_sim = [&](std::size_t a, std::size_t b) noexcept -> double {
        const Chunk* ca = hits[a].chunk;
        const Chunk* cb = hits[b].chunk;
        if (ca && cb && !ca->embedding.empty()
            && ca->embedding.size() == cb->embedding.size()) {
            // Cosine of normalized-ish embeddings lands in [-1,1]; clamp the
            // negative tail to 0 so it composes with the [0,1] relevance term.
            return std::max(0.0, cosine(ca->embedding, cb->embedding));
        }
        return jaccard_sim(toks[a], toks[b]);
    };

    // Normalize relevance to [0,1] so it composes with the [0,1] similarity.
    double max_rel = 0.0;
    for (const auto& h : hits) max_rel = std::max(max_rel, h.score);
    if (max_rel <= 0.0) max_rel = 1.0;

    std::vector<std::size_t> selected;
    selected.reserve(out_k);
    std::vector<char> used(hits.size(), 0);
    // sim_to_sel[i] = max similarity of candidate i to ANY already-selected
    // chunk. Maintained incrementally so each round only compares against
    // the ONE newly selected chunk — total work O(out_k · n), not O(out_k · n²).
    std::vector<double> sim_to_sel(hits.size(), 0.0);

    while (selected.size() < out_k) {
        double best_mmr = -1e18;
        std::size_t best = hits.size();
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (used[i]) continue;
            double rel = hits[i].score / max_rel;
            double mmr = lambda * rel - (1.0 - lambda) * sim_to_sel[i];
            if (mmr > best_mmr) { best_mmr = mmr; best = i; }
        }
        if (best == hits.size()) break;
        used[best] = 1;
        selected.push_back(best);
        // Update each remaining candidate's max-similarity against `best` only.
        for (std::size_t i = 0; i < hits.size(); ++i) {
            if (used[i]) continue;
            sim_to_sel[i] = std::max(sim_to_sel[i], pair_sim(i, best));
        }
    }

    std::vector<Hit> out;
    out.reserve(selected.size());
    for (std::size_t idx : selected) out.push_back(hits[idx]);
    return out;
}

} // namespace agentty::rag
