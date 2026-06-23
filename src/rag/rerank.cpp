// agentty::rag — feature-fusion reranker + extractive context compression.
// Pure C++/STL, deterministic, no network. See rerank.hpp for rationale.

#include "agentty/rag/rerank.hpp"

#include <algorithm>
#include <cctype>
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

std::vector<std::string> query_terms(std::string_view query) {
    auto toks = tokenize(query);
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (auto& t : toks) if (seen.insert(t).second) out.push_back(t);
    return out;
}

std::vector<Hit>
rerank(std::string_view query, std::vector<Hit> hits,
       std::size_t out_k, const RerankWeights& w) {
    if (hits.empty()) return hits;

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
    std::vector<double> f_fused(n), f_cover(n), f_prox(n), f_path(n), f_phrase(n);

    std::unordered_set<std::string> qset(qterms.begin(), qterms.end());

    for (std::size_t i = 0; i < n; ++i) {
        const Chunk* c = hits[i].chunk;
        f_fused[i] = hits[i].score;
        if (!c) continue;

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
    normalize01(f_cover);
    normalize01(f_prox);
    // path/phrase are already 0/1; leave as-is.

    std::vector<std::pair<double, std::size_t>> scored(n);
    for (std::size_t i = 0; i < n; ++i) {
        double s = w.fused        * f_fused[i]
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

    // If NOTHING matched, fall back to the head slice (better than nothing).
    if (score[seed] <= 0.0)
        return std::string{text.substr(0, target_chars)};

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

} // namespace agentty::rag
