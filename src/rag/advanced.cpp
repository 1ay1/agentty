// agentty::rag — ADVANCED retrieval implementation. See advanced.hpp for the
// design narrative. Pure C++/STL; the only network is the optional batched
// /api/embed call inside LateInteractionStage (same endpoint, same graceful
// degradation as the rest of the funnel).

#include "agentty/rag/advanced.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace agentty::rag {

namespace fs = std::filesystem;

// ══════════════════════════════════════════════════════════════════════════
// feedback — the learning loop
// ══════════════════════════════════════════════════════════════════════════
namespace feedback {
namespace {

struct Stat { std::uint32_t uses = 0; std::uint32_t wins = 0; };

struct Store {
    std::mutex mu;
    std::unordered_map<std::string, Stat> stats;   // key → counts
    // Recently-shown keys awaiting win attribution: (key, path-tail).
    std::deque<std::pair<std::string, std::string>> recent;
    bool loaded = false;
    unsigned dirty = 0;                            // writes since last flush

    static constexpr std::size_t kMaxRecent  = 64;
    static constexpr std::size_t kMaxStats   = 4096;
    static constexpr unsigned    kFlushEvery = 16;
};

Store& store() { static Store s; return s; }

bool learn_enabled() {
    const char* v = std::getenv("AGENTTY_RAG_LEARN");
    if (!v || v[0] == '\0') return true;
    std::string s{v};
    return s != "0" && s != "false" && s != "FALSE" && s != "False";
}

fs::path store_path() {
    if (const char* p = std::getenv("AGENTTY_RAG_FEEDBACK_PATH"); p && p[0])
        return fs::path{p};
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    return cwd / ".agentty" / "rag_feedback.tsv";
}

// TSV: key \t uses \t wins. Human-inspectable, merge-friendly, tiny.
void load_locked(Store& s) {
    if (s.loaded) return;
    s.loaded = true;
    auto p = store_path();
    if (p.empty()) return;
    std::ifstream in(p);
    if (!in) return;
    std::string line;
    while (std::getline(in, line) && s.stats.size() < Store::kMaxStats) {
        auto t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        try {
            Stat st;
            st.uses = static_cast<std::uint32_t>(
                std::stoul(line.substr(t1 + 1, t2 - t1 - 1)));
            st.wins = static_cast<std::uint32_t>(std::stoul(line.substr(t2 + 1)));
            if (st.wins > st.uses) st.wins = st.uses;   // corrupt row → clamp
            s.stats.emplace(line.substr(0, t1), st);
        } catch (...) { /* skip corrupt row */ }
    }
}

void flush_locked(Store& s) {
    auto p = store_path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        for (const auto& [k, st] : s.stats)
            out << k << '\t' << st.uses << '\t' << st.wins << '\n';
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    s.dirty = 0;
}

// Path tail used for win attribution: the last two path components,
// lowercased ("skill://name/SKILL.md" → "name/skill.md"). Suffix matching on
// this survives absolute-vs-relative and workspace-prefix differences
// between what retrieval indexed and what the read tool was handed.
std::string path_tail(std::string_view path) {
    std::string s;
    s.reserve(path.size());
    for (char c : path)
        s += (c == '\\') ? '/' : static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
    auto last = s.rfind('/');
    if (last == std::string::npos) return s;
    auto prev = last > 0 ? s.rfind('/', last - 1) : std::string::npos;
    return prev == std::string::npos ? s : s.substr(prev + 1);
}

} // namespace

void note_shown(const std::vector<std::string>& keys) {
    if (!learn_enabled() || keys.empty()) return;
    try {
        auto& s = store();
        std::lock_guard<std::mutex> lock(s.mu);
        load_locked(s);
        for (const auto& k : keys) {
            if (k.empty()) continue;
            if (s.stats.size() >= Store::kMaxStats && !s.stats.count(k))
                continue;   // bounded: never grow past the cap
            s.stats[k].uses++;
            // Attribution key: strip the "<source>:" prefix to get the path.
            auto colon = k.find(':');
            std::string path = colon == std::string::npos ? k : k.substr(colon + 1);
            s.recent.emplace_back(k, path_tail(path));
            if (s.recent.size() > Store::kMaxRecent) s.recent.pop_front();
        }
        if (++s.dirty >= Store::kFlushEvery) flush_locked(s);
    } catch (...) { /* never throws out */ }
}

void note_file_opened(std::string_view path) {
    if (!learn_enabled() || path.empty()) return;
    try {
        auto& s = store();
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.recent.empty()) return;   // common case: nothing pending, O(1)
        load_locked(s);
        const std::string tail = path_tail(path);
        if (tail.empty()) return;
        bool hit = false;
        for (auto it = s.recent.begin(); it != s.recent.end();) {
            const auto& [key, ptail] = *it;
            // Match when either tail suffixes the other (chunk paths are
            // corpus-relative; read paths are often absolute).
            const bool match = !ptail.empty()
                && (tail.size() >= ptail.size()
                        ? tail.ends_with(ptail)
                        : ptail.ends_with(tail));
            if (match) {
                auto sit = s.stats.find(key);
                if (sit != s.stats.end() && sit->second.wins < sit->second.uses)
                    sit->second.wins++;
                hit = true;
                it = s.recent.erase(it);   // one win per showing
            } else {
                ++it;
            }
        }
        if (hit && ++s.dirty >= Store::kFlushEvery) flush_locked(s);
    } catch (...) { /* never throws out */ }
}

double prior(std::string_view key) noexcept {
    if (!learn_enabled()) return 0.5;
    try {
        auto& s = store();
        std::lock_guard<std::mutex> lock(s.mu);
        load_locked(s);
        auto it = s.stats.find(std::string{key});
        if (it == s.stats.end()) return 0.5;
        return (it->second.wins + 1.0) / (it->second.uses + 2.0);
    } catch (...) { return 0.5; }
}

void flush() noexcept {
    try {
        auto& s = store();
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.loaded && s.dirty > 0) flush_locked(s);
    } catch (...) { /* best-effort */ }
}

void reset_for_test() {
    auto& s = store();
    std::lock_guard<std::mutex> lock(s.mu);
    s.stats.clear();
    s.recent.clear();
    s.loaded = true;   // do NOT reload from disk mid-test
    s.dirty  = 0;
}

} // namespace feedback

// ══════════════════════════════════════════════════════════════════════════
// carryover — conversation-aware query rewriting
// ══════════════════════════════════════════════════════════════════════════
namespace carryover {
namespace {

struct Salience {
    std::mutex mu;
    std::unordered_map<std::string, double> weight;   // term → decayed weight
    static constexpr std::size_t kMaxTerms = 128;
    static constexpr double      kDecay    = 0.6;     // per observation
};

Salience& sal() { static Salience s; return s; }

bool is_pronoun(std::string_view t) {
    static const std::unordered_set<std::string_view> kSet = {
        "it", "its", "this", "that", "these", "those", "they", "them",
        "he", "she", "his", "her", "there", "one"};
    return kSet.count(t) > 0;
}

// Lowercased whitespace/punct-split tokens; keeps pronouns (the vagueness
// signal) separate from content terms (what we carry over).
void split_terms(std::string_view text, std::vector<std::string>& content,
                 bool& saw_pronoun) {
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        if (is_pronoun(cur)) saw_pronoun = true;
        else if (cur.size() >= 3 && !is_stopword(cur)) content.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else flush();
    }
    flush();
}

} // namespace

void note(std::string_view text) {
    if (text.empty() || text.size() > 4096) return;
    try {
        std::vector<std::string> terms;
        bool pron = false;
        split_terms(text, terms, pron);
        if (terms.empty()) return;
        auto& s = sal();
        std::lock_guard<std::mutex> lock(s.mu);
        for (auto& [t, w] : s.weight) w *= Salience::kDecay;
        for (const auto& t : terms) s.weight[t] += 1.0;
        // Bounded: evict the weakest when over cap.
        while (s.weight.size() > Salience::kMaxTerms) {
            auto weakest = s.weight.begin();
            for (auto it = s.weight.begin(); it != s.weight.end(); ++it)
                if (it->second < weakest->second) weakest = it;
            s.weight.erase(weakest);
        }
    } catch (...) { /* best-effort */ }
}

std::string rewrite(const std::string& query) {
    try {
        std::vector<std::string> terms;
        bool pron = false;
        split_terms(query, terms, pron);
        // Only rewrite VAGUE follow-ups: a pronoun, or almost no content.
        if (!pron && terms.size() >= 2) return query;

        auto& s = sal();
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.weight.empty()) return query;

        // Top-3 carried terms not already present in the query.
        std::unordered_set<std::string> have(terms.begin(), terms.end());
        std::vector<std::pair<std::string, double>> ranked(
            s.weight.begin(), s.weight.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string out = query;
        std::size_t added = 0;
        for (const auto& [t, w] : ranked) {
            if (added >= 3) break;
            if (w < 0.5) break;              // decayed away → stale, don't inject
            if (have.count(t)) continue;
            out += ' ';
            out += t;
            ++added;
        }
        return added > 0 ? out : query;
    } catch (...) { return query; }
}

void reset_for_test() {
    auto& s = sal();
    std::lock_guard<std::mutex> lock(s.mu);
    s.weight.clear();
}

} // namespace carryover

// ══════════════════════════════════════════════════════════════════════════
// decompose_query — deterministic multi-hop facets
// ══════════════════════════════════════════════════════════════════════════
namespace {

std::size_t content_term_count(std::string_view s) {
    std::vector<std::string> terms;
    bool pron = false;
    // Reuse carryover's splitter semantics via a local copy of the loop —
    // (the carryover splitter is file-local to its namespace).
    std::string cur;
    std::size_t n = 0;
    auto flush = [&] {
        if (cur.size() >= 3 && !is_stopword(cur)) ++n;
        cur.clear();
    };
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else flush();
    }
    flush();
    (void)terms; (void)pron;
    return n;
}

std::string trim_ws(std::string s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, e - b + 1);
}

} // namespace

std::vector<std::string> decompose_query(const std::string& query) {
    try {
        if (query.size() < 16 || query.size() > 1024) return {};

        // Split on clause connectives. Case-insensitive scan for the word
        // boundaries; ";" splits unconditionally.
        static const std::string_view kConnectives[] = {
            " and ", " vs ", " vs. ", " versus ", " compared to "};
        std::string lower;
        lower.reserve(query.size());
        for (char c : query)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        std::vector<std::string> parts;
        std::size_t start = 0;
        for (std::size_t i = 0; i < query.size();) {
            bool cut = false;
            std::size_t skip = 1;
            if (query[i] == ';') { cut = true; }
            else {
                for (auto conn : kConnectives) {
                    if (lower.compare(i, conn.size(), conn) == 0) {
                        cut = true;
                        skip = conn.size();
                        break;
                    }
                }
            }
            if (cut) {
                parts.push_back(trim_ws(query.substr(start, i - start)));
                i += skip;
                start = i;
            } else {
                ++i;
            }
        }
        parts.push_back(trim_ws(query.substr(start)));

        // Conservative gate: ≥2 clauses, each with ≥2 content terms —
        // "black and white photos" ("white photos" has 2, "black" has 1)
        // does NOT fire; "how auth works and how the sandbox blocks paths"
        // does.
        std::vector<std::string> facets;
        for (auto& p : parts) {
            if (p.empty()) continue;
            if (content_term_count(p) < 2) return {};
            facets.push_back(std::move(p));
            if (facets.size() >= 3) break;
        }
        if (facets.size() < 2) return {};
        return facets;
    } catch (...) { return {}; }
}

// ══════════════════════════════════════════════════════════════════════════
// LearnedPriorStage
// ══════════════════════════════════════════════════════════════════════════
Context LearnedPriorStage::process(Context ctx) const {
    try {
        if (ctx.chunks.size() < 2) return ctx;
        for (auto& c : ctx.chunks) {
            if (!c.hit.chunk) continue;
            std::string key = c.hit.source
                ? std::string{c.hit.source->name()} + ":" + c.hit.chunk->path
                : "docs:" + c.hit.chunk->path;
            const double p = feedback::prior(key);
            // Neutral ×1.0 at p=0.5; range [×0.85, ×1.15]. A nudge, not a
            // veto — evidence reorders near-ties, never buries a strong hit.
            c.hit.score *= (0.85 + 0.30 * p);
        }
        std::stable_sort(ctx.chunks.begin(), ctx.chunks.end(),
                         [](const ContextChunk& a, const ContextChunk& b) {
                             return a.hit.score > b.hit.score;
                         });
        return ctx;
    } catch (...) { return ctx; }
}

// ══════════════════════════════════════════════════════════════════════════
// LateInteractionStage — sentence-level MaxSim
// ══════════════════════════════════════════════════════════════════════════
namespace {

// Sentence-ish spans (mirrors the compressor's boundaries: ./!/?/newline).
std::vector<std::string> split_sentences_capped(std::string_view t,
                                                std::size_t max_sents) {
    std::vector<std::string> out;
    std::size_t start = 0;
    auto push = [&](std::size_t b, std::size_t e) {
        while (b < e && std::isspace(static_cast<unsigned char>(t[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(t[e - 1]))) --e;
        if (e - b >= 20)                       // skip fragments: no signal
            out.emplace_back(t.substr(b, std::min<std::size_t>(e - b, 400)));
    };
    for (std::size_t i = 0; i < t.size() && out.size() < max_sents; ++i) {
        char c = t[i];
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
            push(start, i + 1);
            start = i + 1;
        }
    }
    if (out.size() < max_sents) push(start, t.size());
    return out;
}

double cos_f(const std::vector<float>& a, const std::vector<float>& b) {
    return cosine(a, b);
}

} // namespace

Context LateInteractionStage::process(Context ctx) const {
    try {
        if (ctx.chunks.size() < 2 || cfg_.embed.model.empty()) return ctx;

        // Cap the pool defensively — each candidate contributes ≤6 sentences.
        constexpr std::size_t kMaxCand  = 16;
        constexpr std::size_t kMaxSents = 6;
        const std::size_t n = std::min(ctx.chunks.size(), kMaxCand);

        std::vector<std::string> batch;
        batch.reserve(1 + n * kMaxSents);
        batch.push_back(ctx.query);
        std::vector<std::pair<std::size_t, std::size_t>> spans;   // per cand: [b,e)
        spans.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& c = ctx.chunks[i];
            std::string_view body = c.text();
            auto sents = split_sentences_capped(body, kMaxSents);
            std::size_t b = batch.size();
            for (auto& s : sents) batch.push_back(std::move(s));
            spans.emplace_back(b, batch.size());
        }
        if (batch.size() < 3) return ctx;   // nothing sentence-shaped

        auto vecs = embed_texts(cfg_.embed, batch);
        if (!vecs || vecs->size() != batch.size() || (*vecs)[0].empty())
            return ctx;                      // backend down → keep input order

        const auto& qv = (*vecs)[0];
        std::vector<double> maxsim(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            auto [b, e] = spans[i];
            double best = 0.0, second = 0.0;
            for (std::size_t j = b; j < e; ++j) {
                double s = cos_f(qv, (*vecs)[j]);
                if (s > best) { second = best; best = s; }
                else if (s > second) second = s;
            }
            // MaxSim blended with the runner-up: rewards a chunk whose
            // relevance is corroborated by a second sentence over a chunk
            // with one lucky sentence in a sea of noise.
            maxsim[i] = (e - b) >= 2 ? 0.75 * best + 0.25 * second : best;
        }

        std::vector<std::size_t> order(n);
        for (std::size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](std::size_t a, std::size_t b2) {
                             return maxsim[a] > maxsim[b2];
                         });

        Context out;
        out.query      = ctx.query;
        out.confidence = ctx.confidence;
        out.chunks.reserve(std::min(out_k_, ctx.chunks.size()));
        for (std::size_t i = 0; i < order.size() && out.chunks.size() < out_k_; ++i) {
            ContextChunk c = std::move(ctx.chunks[order[i]]);
            c.hit.score = maxsim[order[i]];
            out.chunks.push_back(std::move(c));
        }
        // Tail beyond the re-scored pool keeps its old order.
        for (std::size_t i = n; i < ctx.chunks.size() && out.chunks.size() < out_k_; ++i)
            out.chunks.push_back(std::move(ctx.chunks[i]));
        return out;
    } catch (...) { return ctx; }
}

// ══════════════════════════════════════════════════════════════════════════
// GraphExpandStage — markdown link-graph expansion
// ══════════════════════════════════════════════════════════════════════════
namespace {

// Extract markdown link targets ("](path)") that look like local doc files.
// Fragment/query stripped; absolute URLs and anchors skipped.
void extract_link_targets(std::string_view text,
                          std::vector<std::string>& out) {
    std::size_t pos = 0;
    while ((pos = text.find("](", pos)) != std::string_view::npos) {
        std::size_t b = pos + 2;
        std::size_t e = text.find(')', b);
        pos = b;
        if (e == std::string_view::npos) break;
        std::string_view t = text.substr(b, e - b);
        if (t.empty() || t[0] == '#') continue;
        if (t.starts_with("http://") || t.starts_with("https://") ||
            t.starts_with("mailto:")) continue;
        if (auto h = t.find('#'); h != std::string_view::npos) t = t.substr(0, h);
        if (t.empty() || t.size() > 200) continue;
        // Normalize "./x" → "x"
        while (t.starts_with("./")) t.remove_prefix(2);
        out.emplace_back(t);
    }
}

// Lowercased path tail (filename) for suffix matching link targets against
// corpus-relative chunk paths.
std::string file_name_lower(std::string_view p) {
    auto slash = p.find_last_of("/\\");
    std::string_view n = slash == std::string_view::npos ? p : p.substr(slash + 1);
    std::string s;
    s.reserve(n.size());
    for (char c : n)
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

Context GraphExpandStage::process(Context ctx) const {
    try {
        if (!corpus_ || ctx.chunks.empty() || max_extra_ == 0) return ctx;

        // Docs already represented in the context (don't re-add).
        std::unordered_set<std::string> have_docs;
        for (const auto& c : ctx.chunks)
            if (c.hit.chunk) have_docs.insert(c.hit.chunk->path);

        // Collect outbound link targets from the TOP hits' chunk bodies
        // (top 3 — the graph should expand around what won, not the tail).
        std::vector<std::string> targets;
        std::size_t scanned = 0;
        for (const auto& c : ctx.chunks) {
            if (scanned >= 3) break;
            if (!c.hit.chunk) continue;
            extract_link_targets(c.hit.chunk->text, targets);
            ++scanned;
        }
        if (targets.empty()) return ctx;

        // Resolve each target to its document's LEAD chunk in the corpus
        // (filename suffix match — corpus paths are root-relative, link
        // targets are doc-relative; the filename is the stable join key).
        const auto& all = corpus_->raw_chunks();
        double floor_score = ctx.chunks.back().hit.score;
        std::size_t added = 0;
        std::unordered_set<std::string> seen_targets;
        for (const auto& t : targets) {
            if (added >= max_extra_) break;
            auto fname = file_name_lower(t);
            if (fname.empty() || !seen_targets.insert(fname).second) continue;
            const Chunk* lead = nullptr;
            for (const auto& ch : all) {
                if (file_name_lower(ch.path) != fname) continue;
                if (have_docs.count(ch.path)) { lead = nullptr; break; }
                if (!lead || ch.line_start < lead->line_start) lead = &ch;
            }
            if (!lead || have_docs.count(lead->path)) continue;
            ContextChunk c;
            c.hit.chunk = lead;
            // Below the surviving pool: supporting material, never above a
            // direct hit. Decay per addition keeps insertion order stable.
            c.hit.score = floor_score * 0.9 - 1e-6 * static_cast<double>(added);
            ctx.chunks.push_back(std::move(c));
            have_docs.insert(lead->path);
            ++added;
        }
        return ctx;
    } catch (...) { return ctx; }
}

} // namespace agentty::rag
