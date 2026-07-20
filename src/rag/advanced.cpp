// agentty::rag — ADVANCED retrieval implementation. See advanced.hpp for the
// design narrative. Pure C++/STL; the only network is the optional batched
// /api/embed call inside LateInteractionStage (same endpoint, same graceful
// degradation as the rest of the funnel).

#include "agentty/rag/advanced.hpp"
#include "agentty/io/http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
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
// GraphExpandStage — GraphRAG over the markdown link graph
//
// The structural half of "advanced GraphRAG" (Microsoft-style), minus the
// LLM-built entity graph: the DOCUMENT graph is real, built ONCE from the
// corpus's markdown links and memo-cached per corpus shape. On top of it:
//   • PageRank        — deterministic authority prior; hub docs win ties.
//   • Backlinks       — a doc that CITES a hit is usually the overview that
//                       contextualizes it (inbound edges, previously ignored).
//   • Communities     — deterministic label propagation (a Leiden stand-in);
//                       when the top hits concentrate in one community, its
//                       highest-authority hub is surfaced — the no-LLM
//                       analogue of a GraphRAG "community report".
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

// The document graph: one node per distinct corpus path; edges = resolved
// markdown links PLUS entity co-occurrence. Everything derived (PageRank,
// entities, communities, lead chunks) is computed at build time so
// process() only walks adjacency.
struct DocGraph {
    std::vector<std::string>                       paths;    // node → path
    std::vector<const Chunk*>                      lead;     // node → lead chunk
    std::vector<std::vector<std::uint32_t>>        out, in;  // link adjacency
    std::vector<std::vector<std::uint32_t>>        ent_adj;  // entity edges (undirected)
    std::vector<std::vector<std::string>>          ents;     // node → salient entities
    std::vector<double>                            rank;     // PageRank (link graph)
    std::vector<std::uint32_t>                     comm;     // community label
    std::unordered_map<std::string, std::uint32_t> id_of;    // path → node
    std::uint64_t                                  sig = 0;  // corpus signature
};

// Same order-sensitive structural fingerprint corpus.cpp uses for its HNSW
// cache (FNV-1a over path + line span, in chunk order): any drift in the
// chunk array — file edited, added, removed, reordered — changes it, which
// is exactly the invalidation the memo-cached graph needs. Chunk POINTERS
// stored in `lead` are only valid for the corpus shape that produced them.
std::uint64_t graph_signature(const std::vector<Chunk>& chunks) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    };
    for (const auto& c : chunks) {
        mix(c.path.data(), c.path.size());
        std::int32_t ls = c.line_start, le = c.line_end;
        mix(&ls, sizeof ls); mix(&le, sizeof le);
    }
    return h;
}

std::shared_ptr<const DocGraph> build_doc_graph(const Corpus& corpus,
                                                std::uint64_t sig) {
    auto g = std::make_shared<DocGraph>();
    g->sig = sig;
    const auto& all = corpus.raw_chunks();

    // ── Nodes: one per distinct path; lead chunk = smallest line_start.
    // Outbound link targets are collected per doc as lowercased filenames
    // (corpus paths are root-relative, link targets doc-relative — the
    // filename is the stable join key, as before). Doc-level term counts
    // are accumulated in the same pass for the entity layer.
    std::vector<std::vector<std::string>> tgt_fnames;
    std::unordered_map<std::string, std::vector<std::uint32_t>> by_fname;
    std::vector<std::unordered_map<std::string, int>> doc_tf;
    for (const auto& ch : all) {
        auto [it, fresh] = g->id_of.try_emplace(
            ch.path, static_cast<std::uint32_t>(g->paths.size()));
        if (fresh) {
            g->paths.push_back(ch.path);
            g->lead.push_back(&ch);
            tgt_fnames.emplace_back();
            doc_tf.emplace_back();
            by_fname[file_name_lower(ch.path)].push_back(it->second);
        }
        const std::uint32_t id = it->second;
        if (ch.line_start < g->lead[id]->line_start) g->lead[id] = &ch;
        std::vector<std::string> targets;
        extract_link_targets(ch.text, targets);
        for (auto& t : targets) {
            auto f = file_name_lower(t);
            if (!f.empty()) tgt_fnames[id].push_back(std::move(f));
        }
        // Entity candidates: lowercase alnum/underscore runs ≥4 chars, not
        // stopwords, not pure digits. Bounded per doc.
        auto& tf = doc_tf[id];
        std::string cur;
        bool has_alpha = false;
        auto flush = [&] {
            if (cur.size() >= 4 && has_alpha && !is_stopword(cur)
                && tf.size() < 4096)
                ++tf[cur];
            cur.clear();
            has_alpha = false;
        };
        for (unsigned char c : ch.text) {
            if (std::isalnum(c) || c == '_') {
                if (std::isalpha(c)) has_alpha = true;
                cur += static_cast<char>(std::tolower(c));
            } else flush();
        }
        flush();
    }
    const std::size_t n = g->paths.size();
    g->out.resize(n);
    g->in.resize(n);
    g->ent_adj.resize(n);
    g->ents.resize(n);

    // ── Link edges: resolve target filenames to nodes. A filename claimed
    // by more than one doc is AMBIGUOUS — skip it rather than guess (a wrong
    // edge poisons PageRank and communities; a missing one just no-ops).
    for (std::uint32_t u = 0; u < n; ++u) {
        std::unordered_set<std::uint32_t> seen;
        for (const auto& f : tgt_fnames[u]) {
            auto it = by_fname.find(f);
            if (it == by_fname.end() || it->second.size() != 1) continue;
            const std::uint32_t v = it->second[0];
            if (v == u || !seen.insert(v).second) continue;
            g->out[u].push_back(v);
            g->in[v].push_back(u);
        }
    }

    // ── Entity layer (the LLM-free half of GraphRAG's entity graph). A
    // term is a SALIENT ENTITY of doc D when it's among D's top-8 terms by
    // tf·idf AND is rare corpus-wide (df ≥ 2 so it can link, df ≤ max(3,
    // n/4) so it discriminates). Docs sharing ≥2 salient entities get an
    // entity edge — related even when the author drew no markdown link.
    if (n >= 2) {
        std::unordered_map<std::string, int> df;
        for (std::uint32_t u = 0; u < n; ++u)
            for (const auto& [t, f] : doc_tf[u]) ++df[t];
        const int df_max = std::max(3, static_cast<int>(n / 4));
        std::unordered_map<std::string, std::vector<std::uint32_t>> ent_docs;
        for (std::uint32_t u = 0; u < n; ++u) {
            std::vector<std::pair<double, std::string>> scored;
            scored.reserve(doc_tf[u].size());
            for (const auto& [t, f] : doc_tf[u]) {
                const int d = df[t];
                if (d < 2 || d > df_max) continue;
                scored.emplace_back(
                    f * std::log(static_cast<double>(n) / d), t);
            }
            // Deterministic top-8: score desc, then term asc.
            std::sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) {
                          if (a.first != b.first) return a.first > b.first;
                          return a.second < b.second;
                      });
            for (std::size_t i = 0; i < scored.size() && i < 8; ++i) {
                g->ents[u].push_back(scored[i].second);
                ent_docs[scored[i].second].push_back(u);
            }
        }
        // Pairwise co-occurrence → edges. Entities claimed by >8 docs are
        // too common to be discriminative (and quadratic) — skipped.
        std::unordered_map<std::uint64_t, int> pair_count;
        for (const auto& [t, ds] : ent_docs) {
            if (ds.size() < 2 || ds.size() > 8) continue;
            for (std::size_t i = 0; i < ds.size(); ++i)
                for (std::size_t j = i + 1; j < ds.size(); ++j) {
                    const std::uint64_t key =
                        (static_cast<std::uint64_t>(ds[i]) << 32) | ds[j];
                    ++pair_count[key];
                }
        }
        for (const auto& [key, cnt] : pair_count) {
            if (cnt < 2) continue;   // ≥2 shared entities — one may be noise
            const auto a = static_cast<std::uint32_t>(key >> 32);
            const auto b = static_cast<std::uint32_t>(key & 0xffffffffu);
            g->ent_adj[a].push_back(b);
            g->ent_adj[b].push_back(a);
        }
        for (auto& adj : g->ent_adj) std::sort(adj.begin(), adj.end());
    }

    // ── PageRank over the LINK graph only (authority = being cited by an
    // author; entity edges are symmetric similarity, not endorsement).
    // Power iteration, damping 0.85, dangling mass spread uniformly.
    // Deterministic; ~20 iterations is ample at doc-graph scale.
    g->rank.assign(n, n ? 1.0 / static_cast<double>(n) : 0.0);
    if (n >= 2) {
        const double d = 0.85;
        std::vector<double> next(n);
        for (int iter = 0; iter < 20; ++iter) {
            double dangling = 0.0;
            for (std::uint32_t u = 0; u < n; ++u)
                if (g->out[u].empty()) dangling += g->rank[u];
            const double base =
                (1.0 - d) / static_cast<double>(n) +
                d * dangling / static_cast<double>(n);
            std::fill(next.begin(), next.end(), base);
            for (std::uint32_t u = 0; u < n; ++u) {
                if (g->out[u].empty()) continue;
                const double share =
                    d * g->rank[u] / static_cast<double>(g->out[u].size());
                for (std::uint32_t v : g->out[u]) next[v] += share;
            }
            double delta = 0.0;
            for (std::uint32_t u = 0; u < n; ++u)
                delta += std::fabs(next[u] - g->rank[u]);
            g->rank.swap(next);
            if (delta < 1e-9) break;
        }
    }

    // ── Communities: asynchronous label propagation over the UNDIRECTED
    // union of link + entity edges (GraphRAG's communities live on the
    // entity graph; ours span both evidence kinds), fixed ascending node
    // order + smallest-label tiebreak — fully deterministic. Converges in a
    // handful of rounds at this scale.
    g->comm.resize(n);
    for (std::uint32_t u = 0; u < n; ++u) g->comm[u] = u;
    for (int iter = 0; iter < 8; ++iter) {
        bool changed = false;
        for (std::uint32_t u = 0; u < n; ++u) {
            std::unordered_map<std::uint32_t, int> freq;
            for (std::uint32_t v : g->out[u])     ++freq[g->comm[v]];
            for (std::uint32_t v : g->in[u])      ++freq[g->comm[v]];
            for (std::uint32_t v : g->ent_adj[u]) ++freq[g->comm[v]];
            if (freq.empty()) continue;
            std::uint32_t best = g->comm[u];
            int best_n = 0;
            for (const auto& [label, k] : freq)
                if (k > best_n || (k == best_n && label < best)) {
                    best = label;
                    best_n = k;
                }
            if (best != g->comm[u]) { g->comm[u] = best; changed = true; }
        }
        if (!changed) break;
    }
    return g;
}

// Memo: one graph per (corpus identity, chunk storage address, corpus
// shape). The stored Chunk pointers are only valid while the corpus's chunk
// vector still lives at the same address with the same structural layout —
// keying on data() catches a rebuild that reallocates while producing an
// identical signature (same docs re-indexed), where sig alone would alias.
struct GraphMemo {
    std::mutex mu;
    const Corpus* corpus = nullptr;
    const void*   store  = nullptr;   // chunks_.data() the graph points into
    std::uint64_t sig = 0;
    std::shared_ptr<const DocGraph> g;
};

GraphMemo& graph_memo() { static GraphMemo m; return m; }

std::shared_ptr<const DocGraph> doc_graph_for(const Corpus& corpus) {
    const auto& chunks = corpus.raw_chunks();
    const std::uint64_t sig = graph_signature(chunks);
    const void* store = static_cast<const void*>(chunks.data());
    auto& m = graph_memo();
    std::lock_guard<std::mutex> lk(m.mu);
    if (m.g && m.corpus == &corpus && m.store == store && m.sig == sig)
        return m.g;
    auto g = build_doc_graph(corpus, sig);
    m.corpus = &corpus;
    m.store  = store;
    m.sig    = sig;
    m.g      = g;
    return g;
}

// ── Community summaries (full GraphRAG, opt-in) ───────────────────────
// One 2-3 sentence LLM report per community, generated at most once per
// (corpus signature, community member set) and persisted to
// .agentty/rag_graph_summaries.tsv so the cost is paid once per corpus
// shape — across sessions. TSV: key \t summary (summary
// newline-escaped). Any failure → empty string (caller degrades to the
// plain hub chunk); a failure is NOT cached so a later session with the
// backend up can fill it in.

fs::path summaries_path() {
    if (const char* p = std::getenv("AGENTTY_RAG_GRAPH_SUMMARIES_PATH"); p && p[0])
        return fs::path{p};
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    return cwd / ".agentty" / "rag_graph_summaries.tsv";
}

struct SummaryStore {
    std::mutex mu;
    bool loaded = false;
    std::unordered_map<std::string, std::string> cache;   // key → summary
    static constexpr std::size_t kMax = 512;
};

SummaryStore& summary_store() { static SummaryStore s; return s; }

std::string escape_nl(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += ' ';
        else if (c != '\r') out += c;
    }
    return out;
}

std::string unescape_nl(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            out += '\n';
            ++i;
        } else out += s[i];
    }
    return out;
}

void summaries_load_locked(SummaryStore& s) {
    if (s.loaded) return;
    s.loaded = true;
    auto p = summaries_path();
    if (p.empty()) return;
    std::ifstream in(p);
    if (!in) return;
    std::string line;
    while (std::getline(in, line) && s.cache.size() < SummaryStore::kMax) {
        auto t = line.find('\t');
        if (t == std::string::npos || t == 0) continue;
        s.cache.emplace(line.substr(0, t), unescape_nl(
            std::string_view{line}.substr(t + 1)));
    }
}

void summaries_flush_locked(SummaryStore& s) {
    auto p = summaries_path();
    if (p.empty()) return;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        for (const auto& [k, v] : s.cache)
            out << k << '\t' << escape_nl(v) << '\n';
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

// One /api/generate call, temp 0. Returns "" on any failure. Mirrors the
// neural reranker's transport (same endpoint, same degradation contract).
std::string generate_summary(const GraphSummaryConfig& cfg,
                             const std::string& prompt) noexcept {
    try {
        nlohmann::json body;
        body["model"]   = cfg.model;
        body["prompt"]  = prompt;
        body["stream"]  = false;
        body["options"] = {{"temperature", 0.0}, {"num_predict", 160}};

        http::Request req;
        req.method         = http::HttpMethod::Post;
        req.host           = cfg.host;
        req.port           = cfg.port;
        req.path           = "/api/generate";
        req.plaintext      = true;
        req.headers        = {{"content-type", "application/json"}};
        req.body           = body.dump();
        req.max_body_bytes = 256 * 1024;

        http::Timeouts tos;
        tos.connect = std::chrono::milliseconds(3'000);
        tos.total   = std::chrono::milliseconds(
            static_cast<long long>(cfg.timeout_s * 1000.0));

        auto resp = http::default_client().send(req, tos);
        if (!resp || resp->status != 200) return {};
        auto j = nlohmann::json::parse(resp->body, nullptr, false);
        if (j.is_discarded() || !j.contains("response")) return {};
        std::string out = j["response"].get<std::string>();
        // Trim + bound: a summary is a paragraph, not a page.
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
            out.erase(out.begin());
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
            out.pop_back();
        if (out.size() > 900) out.resize(900);
        return out;
    } catch (...) { return {}; }
}

// Cache key: corpus signature + sorted community member paths — stable
// across sessions for the same corpus shape, invalidated by any doc change.
std::string community_key(std::uint64_t sig,
                          const std::vector<std::uint32_t>& members,
                          const DocGraph& g) {
    std::uint64_t h = sig;
    auto mix = [&h](const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    };
    for (std::uint32_t v : members) mix(g.paths[v].data(), g.paths[v].size());
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(h));
    return buf;
}

// Fetch-or-generate the community report for `label`. Members sorted for
// determinism; prompt carries each member doc's lead text + the community's
// shared entities. Cached hit → zero network.
std::string community_summary(const GraphSummaryConfig& cfg,
                              const DocGraph& g, std::uint64_t sig,
                              std::uint32_t label) {
    if (cfg.model.empty()) return {};
    try {
        std::vector<std::uint32_t> members;
        for (std::uint32_t v = 0;
             v < static_cast<std::uint32_t>(g.paths.size()); ++v)
            if (g.comm[v] == label) members.push_back(v);
        if (members.size() < 2) return {};   // singleton: the doc IS the report
        std::sort(members.begin(), members.end(),
                  [&](std::uint32_t a, std::uint32_t b) {
                      return g.paths[a] < g.paths[b];
                  });

        const std::string key = community_key(sig, members, g);
        auto& s = summary_store();
        std::lock_guard<std::mutex> lk(s.mu);
        summaries_load_locked(s);
        if (auto it = s.cache.find(key); it != s.cache.end()) return it->second;
        if (s.cache.size() >= SummaryStore::kMax) return {};

        // Build the report prompt: shared entities + per-doc lead excerpts.
        std::string prompt =
            "These documents form one topic cluster in a documentation set. "
            "Write a 2-3 sentence overview of what this cluster covers as a "
            "whole — the shared subject, the main components, and how the "
            "documents relate. Plain prose, no preamble, no list.\n\n";
        {
            std::unordered_map<std::string, int> ent_votes;
            for (std::uint32_t v : members)
                for (const auto& e : g.ents[v]) ++ent_votes[e];
            std::vector<std::pair<int, std::string>> top;
            for (auto& [e, k] : ent_votes)
                if (k >= 2) top.emplace_back(k, e);
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first > b.first;
                return a.second < b.second;
            });
            if (!top.empty()) {
                prompt += "Recurring terms:";
                for (std::size_t i = 0; i < top.size() && i < 10; ++i) {
                    prompt += ' ';
                    prompt += top[i].second;
                }
                prompt += "\n\n";
            }
        }
        std::size_t used = 0;
        for (std::uint32_t v : members) {
            if (used >= 8) break;   // bound the prompt on huge communities
            const Chunk* lead = g.lead[v];
            if (!lead) continue;
            prompt += "### " + g.paths[v] + "\n";
            std::string_view t = lead->text;
            prompt += std::string{t.substr(0, std::min<std::size_t>(t.size(), 500))};
            prompt += "\n\n";
            ++used;
        }

        std::string summary = generate_summary(cfg, prompt);
        if (summary.empty()) return {};   // failure NOT cached — retry later
        s.cache.emplace(key, summary);
        summaries_flush_locked(s);
        return summary;
    } catch (...) { return {}; }
}

} // namespace

Context GraphExpandStage::process(Context ctx) const {
    try {
        if (!corpus_ || ctx.chunks.empty() || max_extra_ == 0) return ctx;
        auto g = doc_graph_for(*corpus_);
        if (!g || g->paths.size() < 2) return ctx;

        // Docs already represented in the context (don't re-add).
        std::unordered_set<std::string> have_docs;
        for (const auto& c : ctx.chunks)
            if (c.hit.chunk) have_docs.insert(c.hit.chunk->path);

        // Distinct doc nodes of the TOP hits (≤3, rank order) — the graph
        // should expand around what won, not the tail. Hits from other
        // sources (skills/memory) simply don't resolve and are skipped.
        std::vector<std::uint32_t> tops;
        for (const auto& c : ctx.chunks) {
            if (tops.size() >= 3) break;
            if (!c.hit.chunk) continue;
            auto it = g->id_of.find(c.hit.chunk->path);
            if (it == g->id_of.end()) continue;
            if (std::find(tops.begin(), tops.end(), it->second) == tops.end())
                tops.push_back(it->second);
        }
        if (tops.empty()) return ctx;

        // Candidates in tiers: 0 = cited BY a hit (outbound — the hit
        // vouches for it), 1 = CITES a hit (backlink — usually the overview
        // that contextualizes it), 2 = ENTITY NEIGHBOUR (shares ≥2 salient
        // entities with a hit — related even without an authored link),
        // 3 = community hub (corpus-topology authority).
        struct Cand { std::uint32_t node; int tier; };
        std::vector<Cand> cands;
        std::unordered_set<std::uint32_t> cseen;
        auto push = [&](std::uint32_t v, int tier) {
            if (have_docs.count(g->paths[v])) return;
            if (!cseen.insert(v).second) return;
            cands.push_back({v, tier});
        };
        for (std::uint32_t u : tops)
            for (std::uint32_t v : g->out[u]) push(v, 0);
        for (std::uint32_t u : tops)
            for (std::uint32_t v : g->in[u]) push(v, 1);
        for (std::uint32_t u : tops)
            for (std::uint32_t v : g->ent_adj[u]) push(v, 2);

        // Community hub: only when the top hits AGREE on a community (≥2 in
        // the same one, or a lone top doc) — its highest-PageRank member is
        // the corpus-topology authority for the neighbourhood. With a
        // summary model configured (opt-in), the hub carries the cached
        // community REPORT — full GraphRAG global-ish search.
        std::uint32_t hub_node = 0;
        std::uint32_t hub_label = 0;
        bool have_hub = false;
        {
            std::unordered_map<std::uint32_t, int> votes;
            for (std::uint32_t u : tops) ++votes[g->comm[u]];
            std::uint32_t label = 0;
            int most = 0;
            for (const auto& [l, k] : votes)
                if (k > most || (k == most && l < label)) { label = l; most = k; }
            if (most >= 2 || tops.size() == 1) {
                std::uint32_t hub = 0;
                double best = -1.0;
                for (std::uint32_t v = 0;
                     v < static_cast<std::uint32_t>(g->paths.size()); ++v)
                    if (g->comm[v] == label && g->rank[v] > best) {
                        best = g->rank[v];
                        hub  = v;
                    }
                if (best > 0.0) {
                    push(hub, 3);
                    hub_node  = hub;
                    hub_label = label;
                    have_hub  = true;
                }
            }
        }
        if (cands.empty()) return ctx;

        // Deterministic priority: tier, then PageRank authority, then path.
        std::stable_sort(cands.begin(), cands.end(),
                         [&](const Cand& a, const Cand& b) {
                             if (a.tier != b.tier) return a.tier < b.tier;
                             if (g->rank[a.node] != g->rank[b.node])
                                 return g->rank[a.node] > g->rank[b.node];
                             return g->paths[a.node] < g->paths[b.node];
                         });

        const double floor_score = ctx.chunks.back().hit.score;
        std::size_t added = 0;
        for (const auto& cand : cands) {
            if (added >= max_extra_) break;
            const Chunk* lead = g->lead[cand.node];
            if (!lead) continue;
            ContextChunk c;
            c.hit.chunk = lead;
            // Below the surviving pool: supporting material, never above a
            // direct hit. Decay per addition keeps insertion order stable.
            c.hit.score = floor_score * 0.9 - 1e-6 * static_cast<double>(added);
            // OPT-IN community report: the hub passage carries the cached
            // 2-3 sentence cluster overview instead of its raw lead chunk
            // (via `compressed` — the pipeline's "text to show" slot).
            // Cache miss → ONE bounded LLM call, persisted; failure → the
            // plain lead chunk. Deterministic given the cache.
            if (have_hub && cand.node == hub_node && cand.tier == 3) {
                std::string report =
                    community_summary(summary_, *g, g->sig, hub_label);
                if (!report.empty())
                    c.compressed = "[community overview] " + std::move(report);
            }
            ctx.chunks.push_back(std::move(c));
            ++added;
        }
        return ctx;
    } catch (...) { return ctx; }
}

} // namespace agentty::rag
