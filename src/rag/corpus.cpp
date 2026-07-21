// agentty::rag — Ollama embeddings client + the Corpus (build, cache, hybrid
// search). The embeddings call reuses the already-running Ollama server over
// plaintext localhost HTTP — no new dependency. See rag.hpp for rationale.

#include "agentty/rag/rag.hpp"

#include "agentty/rag/rerank.hpp"   // is_stopword
#include "agentty/rag/stemmer.hpp"  // stem
#include "agentty/io/http.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace agentty::rag {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Embeddings (Ollama /api/embed) ─────────────────────────────────────────

namespace {
// Installed embedding backend override (tests / alternative embedders). Null
// = use the built-in Ollama HTTP path. See set_embed_backend in rag.hpp.
EmbedBackend g_embed_backend;
} // namespace

void set_embed_backend(EmbedBackend fn) { g_embed_backend = std::move(fn); }

std::optional<std::vector<std::vector<float>>>
embed_texts(const EmbedConfig& cfg, const std::vector<std::string>& texts) {
    // Backend override wins when installed — the whole HTTP path is bypassed
    // so a test (or an alternative embedder) sees the exact call shape,
    // including how many texts are batched per invocation.
    if (g_embed_backend) {
        try { return g_embed_backend(cfg, texts); } catch (...) { return std::nullopt; }
    }
    if (cfg.model.empty() || texts.empty()) return std::nullopt;
    json body;
    body["model"] = cfg.model;
    // /api/embed accepts a string OR an array of strings in `input` and
    // returns `embeddings: [[...], ...]` aligned to the input order.
    body["input"] = texts;

    std::string body_str;
    try { body_str = body.dump(); } catch (...) { return std::nullopt; }

    http::Request req;
    req.method    = http::HttpMethod::Post;
    req.host      = cfg.host;
    req.port      = cfg.port;
    req.path      = "/api/embed";
    req.plaintext = true;   // local Ollama serves plain HTTP/1.1, no TLS
    req.headers   = {{"content-type", "application/json"}};
    req.body      = std::move(body_str);
    // Embedding payloads are modest; cap to keep a misbehaving server bounded.
    req.max_body_bytes = 64ull * 1024 * 1024;

    http::Timeouts tos;
    tos.connect = std::chrono::milliseconds(3'000);
    tos.total   = std::chrono::milliseconds(
        cfg.timeout_ms > 0 ? cfg.timeout_ms : 120'000);

    auto resp = http::default_client().send(req, tos);
    if (!resp || resp->status != 200) return std::nullopt;

    try {
        auto j = json::parse(resp->body);
        const json* arr = nullptr;
        if (j.contains("embeddings") && j["embeddings"].is_array())
            arr = &j["embeddings"];
        else if (j.contains("embedding") && j["embedding"].is_array())
            arr = &j["embedding"];   // single-input legacy shape
        if (!arr) return std::nullopt;

        std::vector<std::vector<float>> out;
        out.reserve(arr->size());
        for (const auto& row : *arr) {
            if (!row.is_array()) return std::nullopt;
            std::vector<float> v;
            v.reserve(row.size());
            for (const auto& x : row) {
                if (!x.is_number()) return std::nullopt;
                v.push_back(static_cast<float>(x.get<double>()));
            }
            out.push_back(std::move(v));
        }
        if (out.empty()) return std::nullopt;
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {

// ── Asymmetric-retrieval task prefixes ─────────────────────────────
// nomic-embed-text (agentty's default embedder) is instruction-tuned: it
// expects documents as "search_document: …" and queries as
// "search_query: …". Embedding both raw measurably degrades doc↔query
// cosine (the model was never trained on prefix-less asymmetric pairs).
// e5-family models use the same idea with "passage: "/"query: ". Applied
// ONLY for models known to want them — a wrong prefix hurts other models.
// Cache note: prefixed embeddings are incompatible with prefix-less ones,
// which is one of the reasons kCacheMagic is at v4.
enum class PrefixStyle { None, Nomic, E5 };

PrefixStyle prefix_style(const std::string& model) {
    std::string m = model;
    for (auto& c : m) c = static_cast<char>(std::tolower((unsigned char)c));
    if (m.find("nomic-embed") != std::string::npos) return PrefixStyle::Nomic;
    if (m.find("e5") != std::string::npos &&
        m.find("nomic") == std::string::npos)       return PrefixStyle::E5;
    return PrefixStyle::None;
}

std::string with_doc_prefix(const EmbedConfig& cfg, std::string text) {
    switch (prefix_style(cfg.model)) {
        case PrefixStyle::Nomic: return "search_document: " + text;
        case PrefixStyle::E5:    return "passage: " + text;
        case PrefixStyle::None:  break;
    }
    return text;
}

std::string with_query_prefix(const EmbedConfig& cfg, std::string text) {
    switch (prefix_style(cfg.model)) {
        case PrefixStyle::Nomic: return "search_query: " + text;
        case PrefixStyle::E5:    return "query: " + text;
        case PrefixStyle::None:  break;
    }
    return text;
}

} // namespace

// ── Corpus ──────────────────────────────────────────────────────────────────

namespace {

constexpr char kCacheName[] = ".agentty_rag_cache.bin";
constexpr std::uint32_t kCacheMagic   = 0x52414705;  // "RAG\x05" — v5: + embed-model identity in header
[[maybe_unused]] constexpr std::uint32_t kCacheMagicV4 = 0x52414704;  // legacy v4 (task prefixes, no model id)
[[maybe_unused]] constexpr std::uint32_t kCacheMagicV3 = 0x52414703;  // legacy v3 (contextual, prefix-less embeddings)
[[maybe_unused]] constexpr std::uint32_t kCacheMagicV2 = 0x52414702;  // legacy v2 (HNSW, no context)
[[maybe_unused]] constexpr std::uint32_t kCacheMagicV1 = 0x52414701;  // legacy v1 (no HNSW)

// Which files we treat as knowledge documents. Code is intentionally
// excluded — agentic search (grep/read) covers code better than embeddings.
bool is_doc_file(const fs::path& p) {
    auto ext = p.extension().string();
    for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
    static const char* kExts[] = {
        ".md", ".markdown", ".txt", ".text", ".rst", ".org",
        ".adoc", ".asciidoc", ".csv", ".tsv", ".json", ".yaml", ".yml",
        ".html", ".htm", ".tex",
    };
    for (auto* e : kExts) if (ext == e) return true;
    return false;
}

std::string read_file(const fs::path& p, std::size_t cap) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
    if (s.size() > cap) s.resize(cap);
    return s;
}

// Binary cache I/O helpers (little-endian host assumed — these caches are
// machine-local and never shipped, so portability isn't a concern).
template <class T> void put(std::string& b, const T& v) {
    b.append(reinterpret_cast<const char*>(&v), sizeof(T));
}
void put_str(std::string& b, const std::string& s) {
    std::uint32_t n = static_cast<std::uint32_t>(s.size());
    put(b, n);
    b.append(s);
}
template <class T> bool get(std::string_view& b, T& v) {
    if (b.size() < sizeof(T)) return false;
    std::memcpy(&v, b.data(), sizeof(T));
    b.remove_prefix(sizeof(T));
    return true;
}
bool get_str(std::string_view& b, std::string& s) {
    std::uint32_t n;
    if (!get(b, n)) return false;
    if (b.size() < n) return false;
    s.assign(b.data(), n);
    b.remove_prefix(n);
    return true;
}

// A cheap, order-sensitive fingerprint of the chunk array's STRUCTURE
// (path + line span + embedding dim per chunk, in order). The HNSW graph
// stores POSITIONAL node ids (search() materializes via &chunks_[id]), so a
// cache-loaded graph is only safe to reuse if this session rebuilt chunks_
// into the exact same order/shape. A single changed file shifts later
// positions; recursive_directory_iterator order isn't even guaranteed stable
// run-to-run. Hashing the structure and persisting it next to the graph lets
// build() detect any drift and rebuild instead of returning wrong chunks.
// FNV-1a/64 — no allocation, deterministic, dependency-free.
std::uint64_t corpus_signature(const std::vector<Chunk>& chunks) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&h](const void* p, std::size_t n) noexcept {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    };
    for (const auto& c : chunks) {
        mix(c.path.data(), c.path.size());
        std::int32_t ls = c.line_start, le = c.line_end;
        std::uint32_t ed = static_cast<std::uint32_t>(c.embedding.size());
        mix(&ls, sizeof ls); mix(&le, sizeof le); mix(&ed, sizeof ed);
    }
    return h;
}

} // namespace

void Corpus::set_chunks_for_test(std::vector<Chunk> chunks) {
    chunks_ = std::move(chunks);
    dead_.clear();
    embed_dim_ = 0;
    for (const auto& c : chunks_)
        if (!c.embedding.empty()) { embed_dim_ = c.embedding.size(); break; }
    bm25_ = build_bm25(chunks_);
}

std::vector<const Chunk*>
Corpus::neighbors(const std::string& path, int line_start, int line_end,
                  std::size_t radius) const {
    if (radius == 0) return {};
    // Collect every live chunk from the same document, in document order.
    // Chunks of one file are appended contiguously during build(), and their
    // line spans are monotonic, so sorting by line_start recovers reading
    // order even across cache-merge paths.
    std::vector<std::uint32_t> sibs;
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
        if (!is_live_(i)) continue;
        if (chunks_[i].path == path) sibs.push_back(i);
    }
    if (sibs.size() < 2) return {};
    std::sort(sibs.begin(), sibs.end(), [&](std::uint32_t a, std::uint32_t b) {
        if (chunks_[a].line_start != chunks_[b].line_start)
            return chunks_[a].line_start < chunks_[b].line_start;
        return chunks_[a].line_end < chunks_[b].line_end;
    });
    // Locate the hit by its (line_start, line_end) identity.
    std::size_t at = sibs.size();
    for (std::size_t k = 0; k < sibs.size(); ++k) {
        const Chunk& c = chunks_[sibs[k]];
        if (c.line_start == line_start && c.line_end == line_end) { at = k; break; }
    }
    if (at == sibs.size()) return {};
    std::size_t lo = (at > radius) ? at - radius : 0;
    std::size_t hi = std::min(sibs.size() - 1, at + radius);
    std::vector<const Chunk*> out;
    for (std::size_t k = lo; k <= hi; ++k) {
        if (k == at) continue;            // exclude the hit itself
        out.push_back(&chunks_[sibs[k]]);
    }
    return out;
}

std::vector<std::string>
Corpus::prf_expansion_terms(std::string_view query, std::size_t fb_docs,
                            std::size_t fb_terms) const {
    if (fb_terms == 0 || fb_docs == 0 || chunks_.empty()) return {};

    // 1) Initial BM25 pass → the pseudo-relevant set (assume the top hits are
    //    relevant; harvest their vocabulary). Uses the SAME tokenizer/stemmer
    //    the index was built with, so terms stay in one vocabulary.
    auto seeds = bm25_search(bm25_, query, fb_docs);
    if (seeds.empty()) return {};

    // Tokenizer mirroring bm25.cpp: lowercase alnum runs ≥2 chars, Porter-
    // stemmed when the index was (BM25_USE_STEMMER, default on). Query terms
    // and stopwords are excluded so expansion adds NEW discriminative signal.
    static const bool do_stem = [] {
        const char* v = std::getenv("BM25_USE_STEMMER");
        if (!v || !v[0]) return true;
        return v[0] != '0' && std::string_view{v} != "false"
                           && std::string_view{v} != "FALSE";
    }();
    auto toks_of = [](std::string_view s, std::vector<std::string>& out) {
        std::string cur; cur.reserve(24);
        auto flush = [&] {
            if (cur.size() >= 2) out.push_back(do_stem ? stem(cur) : cur);
            cur.clear();
        };
        for (unsigned char c : s) {
            if (std::isalnum(c)) cur.push_back(static_cast<char>(std::tolower(c)));
            else flush();
        }
        flush();
    };

    // Original query terms — never re-emit them as "expansion".
    std::unordered_set<std::string> qterms;
    { std::vector<std::string> qt; toks_of(query, qt);
      for (auto& t : qt) qterms.insert(std::move(t)); }

    // 2) Accumulate feedback term frequency across the pseudo-relevant docs.
    std::unordered_map<std::string, std::uint32_t> fb_tf;
    for (auto& [doc, score] : seeds) {
        if (doc >= chunks_.size() || !is_live_(doc)) continue;
        std::vector<std::string> toks;
        toks_of(chunks_[doc].text, toks);
        // Count each term once per doc (document frequency within feedback
        // set) — blunts a single verbose chunk from dominating the expansion.
        std::unordered_set<std::string> seen_here;
        for (auto& t : toks) {
            if (qterms.count(t) || is_stopword(t)) continue;
            if (seen_here.insert(t).second) ++fb_tf[t];
        }
    }
    if (fb_tf.empty()) return {};

    // 3) Score each candidate by feedback-df × corpus-idf: a term that recurs
    //    across the relevant set AND is rare corpus-wide is the strongest
    //    discriminator (classic RM3/tf-idf feedback weighting).
    const double N = static_cast<double>(bm25_.doc_count);
    std::vector<std::pair<std::string, double>> scored;
    scored.reserve(fb_tf.size());
    for (auto& [term, df] : fb_tf) {
        if (df < 2 && seeds.size() > 2) continue;  // require corroboration
        double idf = 1.0;
        auto it = bm25_.term_ids.find(term);
        if (it != bm25_.term_ids.end() && N > 0.0) {
            double cdf = static_cast<double>(bm25_.postings[it->second].size());
            idf = std::log((N - cdf + 0.5) / (cdf + 0.5) + 1.0);
            if (idf < 0.0) idf = 0.0;
        }
        scored.emplace_back(term, static_cast<double>(df) * idf);
    }
    if (scored.empty()) return {};
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;   // deterministic tiebreak
    });

    std::vector<std::string> out;
    out.reserve(std::min(fb_terms, scored.size()));
    for (std::size_t i = 0; i < scored.size() && out.size() < fb_terms; ++i)
        out.push_back(std::move(scored[i].first));
    return out;
}

void Corpus::build(const fs::path& root, const EmbedConfig& embed) {
    root_ = root;
    chunks_.clear();
    dead_.clear();
    embed_dim_ = 0;
    embed_model_ = embed.model;   // v5 cache identity: who made these vectors

    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
        bm25_ = build_bm25(chunks_);
        return;
    }

    // ── Load cache: path → (size, mtime, chunks) so unchanged files are
    // reused without re-reading or re-embedding. ──────────────────────────
    struct CachedFile {
        std::uint64_t size = 0;
        std::int64_t  mtime = 0;
        std::vector<Chunk> chunks;
    };
    std::unordered_map<std::string, CachedFile> cache;
    bool hnsw_loaded = false;
    std::uint64_t cached_sig = 0;   // corpus signature stored with the graph
    {
        std::string blob = read_file(root / kCacheName, 512ull * 1024 * 1024);
        std::string_view b{blob};
        std::uint32_t magic = 0;
        // v1–v4 caches predate the embed-model identity header (v3 also
        // lacks task prefixes; v1/v2 additionally lack the context field) —
        // loading them would risk mixing vector spaces or pinning stale-
        // quality vectors. Treat them as a miss: one-time full re-chunk +
        // re-embed, then v5 persists.
        if (get(b, magic) && magic == kCacheMagic) {
            bool is_v2 = true;   // v5 keeps the v2 layout + context per chunk
            std::uint32_t dim = 0, nfiles = 0;
            std::string cached_model;
            get_str(b, cached_model);   // v5: model the vectors came from
            get(b, dim);
            get(b, nfiles);
            // EMBED-MODEL IDENTITY: two models can share a dimension while
            // living in disjoint vector spaces. If the session's model
            // differs from the one the cached vectors were computed with,
            // keep the chunk TEXT (BM25 side is model-independent) but drop
            // every cached embedding + the HNSW graph so build() re-embeds
            // under the new model instead of silently misranking.
            // A BM25-only session (empty model — e.g. Ollama not running)
            // is NOT a switch: keep the vectors and their identity so the
            // next embedding-capable session doesn't re-embed from scratch.
            const bool model_match =
                embed.model.empty() || cached_model == embed.model;
            if (embed.model.empty() && !cached_model.empty())
                embed_model_ = cached_model;   // carry identity through
            for (std::uint32_t fi = 0; fi < nfiles; ++fi) {
                std::string path;
                CachedFile cf;
                std::uint32_t nchunks = 0;
                if (!get_str(b, path) || !get(b, cf.size) ||
                    !get(b, cf.mtime) || !get(b, nchunks)) break;
                cf.chunks.reserve(nchunks);
                bool ok = true;
                for (std::uint32_t ci = 0; ci < nchunks; ++ci) {
                    Chunk c;
                    c.path = path;
                    std::int32_t ls = 0, le = 0;
                    std::uint32_t elen = 0;
                    if (!get(b, ls) || !get(b, le) || !get_str(b, c.text) ||
                        !get_str(b, c.context) ||
                        !get(b, elen)) { ok = false; break; }
                    c.line_start = ls;
                    c.line_end   = le;
                    if (elen) {
                        if (b.size() < elen * sizeof(float)) { ok = false; break; }
                        c.embedding.resize(elen);
                        std::memcpy(c.embedding.data(), b.data(),
                                    elen * sizeof(float));
                        b.remove_prefix(elen * sizeof(float));
                    }
                    cf.chunks.push_back(std::move(c));
                }
                if (!ok) break;
                if (!model_match)
                    for (auto& cc : cf.chunks) cc.embedding.clear();
                cache.emplace(std::move(path), std::move(cf));
            }
            // v2 cache: HNSW graph follows the chunk data, preceded by the
            // corpus signature it was built against (so a stale graph whose
            // positional ids no longer match this session's chunk order is
            // detected and rebuilt below rather than returning wrong chunks).
            if (is_v2 && model_match && !b.empty()) {
                if (get(b, cached_sig))
                    hnsw_loaded = hnsw_.deserialize(b);
                if (hnsw_loaded) embed_dim_ = dim;
            }
        }
    }

    // ── Walk the knowledge dir, reusing cache for unchanged files. ────────
    std::vector<Chunk*> need_embed;       // chunks missing an embedding
    auto add_chunks = [&](std::vector<Chunk>&& cs) {
        for (auto& c : cs) {
            if (embed_dim_ == 0 && !c.embedding.empty())
                embed_dim_ = c.embedding.size();
            chunks_.push_back(std::move(c));
        }
    };

    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const auto& entry = *it;
        std::error_code e2;
        if (entry.is_directory(e2)) {
            auto name = entry.path().filename().string();
            if (name.starts_with(".")) it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(e2)) continue;
        if (!is_doc_file(entry.path())) continue;

        auto rel = fs::relative(entry.path(), root, e2).string();
        if (rel.empty()) rel = entry.path().filename().string();

        std::uint64_t sz = static_cast<std::uint64_t>(entry.file_size(e2));
        auto mt = fs::last_write_time(entry.path(), e2);
        std::int64_t mtime = static_cast<std::int64_t>(
            mt.time_since_epoch().count());

        auto cit = cache.find(rel);
        if (cit != cache.end() && cit->second.size == sz &&
            cit->second.mtime == mtime && !cit->second.chunks.empty()) {
            add_chunks(std::move(cit->second.chunks));
            continue;
        }

        // Changed / new file — re-chunk. Cap each doc at 4 MiB.
        std::string body = read_file(entry.path(), 4ull * 1024 * 1024);
        if (body.empty()) continue;
        auto cs = chunk_document(rel, body);
        // Record which chunks still need an embedding (deferred batch below).
        for (auto& c : cs) chunks_.push_back(std::move(c));
    }

    // ── Batch-embed any chunk lacking an embedding (changed/new files). ──
    if (!embed.model.empty()) {
        for (auto& c : chunks_)
            if (c.embedding.empty()) need_embed.push_back(&c);

        // Embed in batches to bound request size.
        constexpr std::size_t kBatch = 64;
        for (std::size_t i = 0; i < need_embed.size(); i += kBatch) {
            std::size_t hi = std::min(i + kBatch, need_embed.size());
            std::vector<std::string> texts;
            texts.reserve(hi - i);
            for (std::size_t j = i; j < hi; ++j)
                texts.push_back(with_doc_prefix(
                    embed, need_embed[j]->embed_input()));   // contextual + task prefix
            auto vecs = embed_texts(embed, texts);
            if (!vecs || vecs->size() != texts.size()) break;  // degrade to BM25
            for (std::size_t j = i; j < hi; ++j) {
                need_embed[j]->embedding = std::move((*vecs)[j - i]);
                if (embed_dim_ == 0) embed_dim_ = need_embed[j]->embedding.size();
            }
        }
        // Enforce a single dim — drop ragged embeddings rather than misrank.
        if (embed_dim_ > 0)
            for (auto& c : chunks_)
                if (c.embedding.size() != embed_dim_) c.embedding.clear();
    }

    // ── Rebuild BM25 + persist the cache. ─────────────────────────────────
    bm25_ = build_bm25(chunks_);

    // Build an HNSW ANN index over the embeddings when the corpus is large
    // enough that brute-force cosine per query would hurt. Below the
    // threshold, exact brute force is both faster and exact, so skip it.
    // The HNSW graph is persisted in the v2 cache format alongside the
    // embeddings, so large corpora don't rebuild the graph every session:
    // if the cache loader already deserialized a graph of the right dim, we
    // keep it; otherwise rebuild_hnsw_() builds fresh from the embeddings.
    constexpr std::size_t kHnswThreshold = 2000;
    if (embed_dim_ > 0 && chunks_.size() >= kHnswThreshold &&
        hnsw_loaded && !hnsw_.empty() && hnsw_.dim() == effective_ann_dim_() &&
        cached_sig != 0 && corpus_signature(chunks_) == cached_sig) {
        hnsw_built_ = true;          // reuse the cache-loaded graph
    } else {
        rebuild_hnsw_();
    }

    write_cache_();
}

// (Re)build (or drop) the HNSW graph from the current chunks_. Single source
// of truth for the ANN index lifecycle — every structural mutation routes
// here so node ids stay aligned with chunk positions.
std::size_t Corpus::ann_dim_env_() {
    // Read once for the process. AGENTTY_RAG_ANN_DIM=256 (say) truncates the
    // ANN graph to the leading 256 dims; 0 / unset / invalid = full width.
    static const std::size_t v = [] {
        const char* s = std::getenv("AGENTTY_RAG_ANN_DIM");
        if (s && s[0]) {
            try { long d = std::stol(s); if (d > 0) return static_cast<std::size_t>(d); }
            catch (...) {}
        }
        return static_cast<std::size_t>(0);
    }();
    return v;
}

std::size_t Corpus::effective_ann_dim_() const noexcept {
    const std::size_t a = ann_dim_env_();
    return (a > 0 && a < embed_dim_) ? a : embed_dim_;
}

HnswIndex Corpus::make_hnsw_() const {
    HnswConfig cfg;
    cfg.ann_dim = ann_dim_env_();
    return HnswIndex{cfg};
}

bool Corpus::fusion_is_rsf_() {
    // AGENTTY_RAG_FUSION=rsf (or "relative"/"score") selects Relative Score
    // Fusion; anything else (unset, "rrf") keeps the rank-based default.
    static const bool rsf = [] {
        const char* v = std::getenv("AGENTTY_RAG_FUSION");
        if (!v || !v[0]) return false;
        std::string s;
        for (const char* p = v; *p; ++p) s.push_back(static_cast<char>(std::tolower((unsigned char)*p)));
        return s == "rsf" || s == "relative" || s == "score";
    }();
    return rsf;
}

void Corpus::rebuild_hnsw_() {
    constexpr std::size_t kHnswThreshold = 2000;
    hnsw_ = make_hnsw_();
    hnsw_built_ = false;
    if (embed_dim_ == 0 || chunks_.size() < kHnswThreshold) return;

    std::vector<std::uint32_t> ids;
    std::vector<const std::vector<float>*> embs;
    ids.reserve(chunks_.size());
    embs.reserve(chunks_.size());
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
        if (!is_live_(i)) continue;   // tombstoned — keep out of the graph
        if (chunks_[i].embedding.size() == embed_dim_) {
            ids.push_back(i);
            embs.push_back(&chunks_[i].embedding);
        }
    }
    if (!ids.empty()) { hnsw_.build(ids, embs); hnsw_built_ = !hnsw_.empty(); }
}

// Extend the live graph with the appended chunks [first_new, size). Appends
// never move existing positions, so node id == chunk position stays true.
bool Corpus::append_to_hnsw_(std::size_t first_new) {
    constexpr std::size_t kHnswThreshold = 2000;
    if (embed_dim_ == 0) { hnsw_ = make_hnsw_(); hnsw_built_ = false; return false; }

    // If we don't yet have a live graph but the corpus has now grown past the
    // threshold, cross over to a one-time full build (which also indexes the
    // pre-existing chunks). Below threshold, brute-force cosine handles it.
    if (!hnsw_built_ || hnsw_.empty()) {
        if (chunks_.size() >= kHnswThreshold) rebuild_hnsw_();
        return hnsw_built_;
    }
    // Dim drift invalidates the graph: a model switch mid-session (embed_dim_
    // changed) OR a cross-session change to AGENTTY_RAG_ANN_DIM (the graph on
    // disk was built at a different truncation). Compare against the graph's
    // EFFECTIVE working dim, which encodes the Matryoshka prefix.
    if (hnsw_.dim() != effective_ann_dim_()) { rebuild_hnsw_(); return hnsw_built_; }

    for (std::uint32_t i = static_cast<std::uint32_t>(first_new);
         i < chunks_.size(); ++i) {
        if (!is_live_(i)) continue;
        if (chunks_[i].embedding.size() == embed_dim_)
            hnsw_.add(i, chunks_[i].embedding);
    }
    hnsw_built_ = !hnsw_.empty();
    return hnsw_built_;
}

// Physically drop tombstoned chunks and rebuild from scratch, restoring the
// dense chunk-position == node-id invariant. Amortizes removals to O(1): we
// only pay this O(N) pass once tombstones exceed kCompactFraction.
void Corpus::compact_() {
    if (dead_.empty()) return;
    std::vector<Chunk> live;
    live.reserve(chunks_.size() - dead_.size());
    for (std::uint32_t i = 0; i < chunks_.size(); ++i)
        if (is_live_(i)) live.push_back(std::move(chunks_[i]));
    chunks_ = std::move(live);
    dead_.clear();
    bm25_ = build_bm25(chunks_);
    rebuild_hnsw_();
}

void Corpus::ranked_lists_for_query_(
    std::string_view query, const EmbedConfig& embed, std::size_t pool,
    std::vector<std::vector<std::uint32_t>>& lists,
    std::vector<double>* weights,
    const std::vector<float>* precomputed_qvec,
    std::vector<std::vector<std::pair<std::uint32_t, double>>>* scored) const {
    // Fusion weights (SOTA hybrid tuning). Dense retrieval catches paraphrase
    // and semantic near-matches; BM25 catches exact terms/proper nouns. The
    // literature consistently finds a modest dense-over-lexical tilt wins on
    // natural-language doc corpora, so dense defaults to 1.3× lexical. Both
    // are env-tunable (AGENTTY_RAG_W_LEXICAL / AGENTTY_RAG_W_DENSE) for A/B
    // without a rebuild. Read once, cached for the process.
    static const double w_lex = [] {
        const char* v = std::getenv("AGENTTY_RAG_W_LEXICAL");
        if (v && v[0]) { try { double d = std::stod(v); if (d >= 0) return d; } catch (...) {} }
        return 1.0;
    }();
    static const double w_dense = [] {
        const char* v = std::getenv("AGENTTY_RAG_W_DENSE");
        if (v && v[0]) { try { double d = std::stod(v); if (d >= 0) return d; } catch (...) {} }
        return 1.3;
    }();
    // Pseudo-relevance-feedback expansion weight (its BM25 list is fused in as
    // a SECONDARY, down-weighted probe so it lifts recall without diluting the
    // primary query's precision). Default-on at 0.5× lexical; set to 0 (or
    // AGENTTY_RAG_PRF=0) to disable entirely. Read once, cached.
    static const double w_prf = [] {
        const char* off = std::getenv("AGENTTY_RAG_PRF");
        if (off && (off[0] == '0' || std::string_view{off} == "false"
                                  || std::string_view{off} == "FALSE")) return 0.0;
        const char* v = std::getenv("AGENTTY_RAG_W_PRF");
        if (v && v[0]) { try { double d = std::stod(v); if (d >= 0) return d; } catch (...) {} }
        return 0.5;
    }();

    // BM25 ranked list (always available). Skip tombstoned ids so a lazily
    // removed chunk never surfaces before the next compaction. When `scored`
    // is requested (RSF mode) we capture the raw BM25 score alongside each id.
    std::vector<std::uint32_t> bm25_rank;
    std::vector<std::pair<std::uint32_t, double>> bm25_scored;
    for (auto& [id, score] : bm25_search(bm25_, query, pool))
        if (is_live_(id)) {
            bm25_rank.push_back(id);
            if (scored) bm25_scored.push_back({id, score});
        }
    lists.push_back(std::move(bm25_rank));
    if (weights) weights->push_back(w_lex);
    if (scored) scored->push_back(std::move(bm25_scored));

    // PSEUDO-RELEVANCE FEEDBACK (RM3-lite): harvest discriminative terms from
    // the top BM25 hits and issue a SECOND, down-weighted BM25 probe over
    // {query + expansion terms}. A chunk that both the literal query AND the
    // expanded vocabulary surface is reinforced by RRF — recovering
    // vocabulary-mismatch hits with zero model/network cost. Deterministic
    // and default-on; degrades to nothing when the corpus is small or the
    // initial pass is empty.
    if (w_prf > 0.0) {
        auto exp_terms = prf_expansion_terms(query, /*fb_docs=*/5, /*fb_terms=*/6);
        if (!exp_terms.empty()) {
            std::string eq{query};
            for (const auto& t : exp_terms) { eq.push_back(' '); eq += t; }
            std::vector<std::uint32_t> prf_rank;
            std::vector<std::pair<std::uint32_t, double>> prf_scored;
            for (auto& [id, score] : bm25_search(bm25_, eq, pool))
                if (is_live_(id)) {
                    prf_rank.push_back(id);
                    if (scored) prf_scored.push_back({id, score});
                }
            if (!prf_rank.empty()) {
                lists.push_back(std::move(prf_rank));
                if (weights) weights->push_back(w_prf);
                if (scored) scored->push_back(std::move(prf_scored));
            }
        }
    }

    // Dense ranked list (only when the corpus AND the query can be embedded).
    if (embed_dim_ > 0 && !embed.model.empty()) {
        // The query vector is either handed in already-computed (search_fused
        // batches ALL variants into ONE /api/embed call and passes each row
        // here) or embedded on the spot for the single-query search() path.
        // QUERY-time embed gets a short leash: this call sits on the
        // search_docs path (and the pre-turn proactive path). A single
        // short text should embed in tens of ms; if Ollama is wedged or
        // cold-loading a model, degrade to BM25-only for THIS query rather
        // than hanging the search for the index-time 120s budget.
        std::vector<float> local_qv;
        const std::vector<float>* qptr = nullptr;
        if (precomputed_qvec && precomputed_qvec->size() == embed_dim_) {
            qptr = precomputed_qvec;   // batched upstream — no round-trip here
        } else {
            EmbedConfig qcfg = embed;
            qcfg.timeout_ms = std::min<long>(qcfg.timeout_ms, 10'000);
            auto qv = embed_texts(qcfg, {with_query_prefix(embed, std::string{query})});
            if (qv && qv->size() == 1 && (*qv)[0].size() == embed_dim_)
                { local_qv = std::move((*qv)[0]); qptr = &local_qv; }
        }
        if (qptr) {
            const auto& q = *qptr;
            std::vector<std::uint32_t> dense_rank;
            std::vector<std::pair<std::uint32_t, double>> dense_scored;
            if (hnsw_built_) {
                // ANN candidate generation — O(log n) vs the brute-force
                // O(n) scan below. Widen ef beyond the pool for recall.
                for (auto& [id, sim] : hnsw_.search(
                         q, pool, std::max<std::size_t>(pool * 2, 64)))
                    if (is_live_(id)) {
                        dense_rank.push_back(id);
                        if (scored) dense_scored.push_back({id, sim});
                    }
            } else {
                std::vector<std::pair<std::uint32_t, double>> sims;
                sims.reserve(chunks_.size());
                for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
                    if (!is_live_(i)) continue;
                    if (chunks_[i].embedding.size() != embed_dim_) continue;
                    sims.push_back({i, cosine(q, chunks_[i].embedding)});
                }
                std::sort(sims.begin(), sims.end(), [](auto& a, auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                });
                if (sims.size() > pool) sims.resize(pool);
                dense_rank.reserve(sims.size());
                for (auto& [id, s] : sims) {
                    dense_rank.push_back(id);
                    if (scored) dense_scored.push_back({id, s});
                }
            }
            if (!dense_rank.empty()) {
                lists.push_back(std::move(dense_rank));
                if (weights) weights->push_back(w_dense);
                if (scored) scored->push_back(std::move(dense_scored));
            }
        }
    }
}

std::vector<Hit> Corpus::search(std::string_view query,
                                const EmbedConfig& embed,
                                std::size_t k) const {
    if (chunks_.empty() || k == 0) return {};

    // Pull a generous candidate pool from each retriever, then fuse + cut to
    // k (SOTA pattern: retrieve wide, fuse, return narrow).
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);

    std::vector<std::vector<std::uint32_t>> lists;
    std::vector<double> weights;
    std::vector<std::vector<std::pair<std::uint32_t, double>>> scored;
    const bool rsf = fusion_is_rsf_();
    ranked_lists_for_query_(query, embed, pool, lists, &weights,
                            /*precomputed_qvec=*/nullptr,
                            rsf ? &scored : nullptr);

    // Fuse and materialize the hits. RRF (rank-based, k=60 canonical) is the
    // robust default; RSF (min-max score fusion) is opt-in via env for corpora
    // where the raw score magnitudes are informative.
    auto fused = rsf ? relative_score_fusion_weighted(scored, weights, k)
                     : reciprocal_rank_fusion_weighted(lists, weights, 60.0, k);
    std::vector<Hit> hits;
    hits.reserve(fused.size());
    for (auto& [id, score] : fused)
        if (id < chunks_.size())   // defensive: never OOB on a stale/corrupt id
            hits.push_back(Hit{&chunks_[id], score});
    return hits;
}

std::size_t Corpus::add_document(const std::string& path, const std::string& body,
                                 const EmbedConfig& embed) {
    // Remove existing chunks for this path first (update semantics).
    remove_document(path);
    
    // Chunk the document.
    auto new_chunks = chunk_document(path, body);
    if (new_chunks.empty()) return 0;
    
    // Embed if model available.
    if (!embed.model.empty()) {
        std::vector<std::string> texts;
        texts.reserve(new_chunks.size());
        for (const auto& c : new_chunks)
            texts.push_back(with_doc_prefix(embed, c.embed_input()));
        
        auto vecs = embed_texts(embed, texts);
        if (vecs && vecs->size() == texts.size()) {
            for (std::size_t i = 0; i < new_chunks.size(); ++i) {
                new_chunks[i].embedding = std::move((*vecs)[i]);
                if (embed_dim_ == 0 && !new_chunks[i].embedding.empty())
                    embed_dim_ = new_chunks[i].embedding.size();
            }
        }
    }
    
    std::size_t added = new_chunks.size();
    std::size_t first_new = chunks_.size();
    for (auto& c : new_chunks)
        chunks_.push_back(std::move(c));

    // Appends never shift existing chunk positions, so HNSW node id == chunk
    // position stays true: extend the live graph incrementally instead of the
    // O(N log N) full rebuild. BM25 is rebuilt (cheap, and it has no stable
    // incremental-append API here); the O(N) win we care about is the graph.
    bm25_ = build_bm25(chunks_);
    append_to_hnsw_(first_new);
    return added;
}

std::size_t Corpus::remove_document(const std::string& path) {
    // TOMBSTONE the matching chunks instead of erasing them: erasing shifts
    // every later chunk's position and invalidates all HNSW node ids (forcing
    // an O(N) rebuild on every edit). Dead ids are kept out of results by
    // is_live_() and physically dropped by compact_() once they pile up.
    std::size_t removed = 0;
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
        if (is_live_(i) && chunks_[i].path == path) {
            dead_.insert(i);
            ++removed;
        }
    }
    if (removed == 0) return 0;

    // Rebuild BM25 over the (still position-stable) corpus; is_live_() filters
    // dead ids from its results. Compact — which physically drops tombstones
    // and rebuilds the graph — only fires once the dead fraction crosses the
    // threshold, amortizing removal to O(1).
    bm25_ = build_bm25(chunks_);
    if (!chunks_.empty() &&
        static_cast<double>(dead_.size()) >=
            kCompactFraction * static_cast<double>(chunks_.size())) {
        compact_();
    }
    return removed;
}

std::size_t Corpus::build_from_memory(
    const std::vector<std::pair<std::string, std::string>>& docs,
    const EmbedConfig& embed) {
    // Wholesale replace: this is a non-folder build path (no disk cache, no
    // root, no fs walk). Mirrors build()'s tail but sources documents from
    // memory.
    root_.clear();
    chunks_.clear();
    dead_.clear();
    embed_dim_ = 0;
    embed_model_ = embed.model;   // keep identity coherent on this path too
    hnsw_ = make_hnsw_();
    hnsw_built_ = false;

    for (const auto& [path, body] : docs) {
        if (body.empty()) continue;
        auto cs = chunk_document(path, body);
        for (auto& c : cs) chunks_.push_back(std::move(c));
    }

    // Batch-embed (bounded request size) when a model is configured.
    if (!embed.model.empty() && !chunks_.empty()) {
        constexpr std::size_t kBatch = 64;
        for (std::size_t i = 0; i < chunks_.size(); i += kBatch) {
            std::size_t hi = std::min(i + kBatch, chunks_.size());
            std::vector<std::string> texts;
            texts.reserve(hi - i);
            for (std::size_t j = i; j < hi; ++j)
                texts.push_back(with_doc_prefix(embed, chunks_[j].embed_input()));
            auto vecs = embed_texts(embed, texts);
            if (!vecs || vecs->size() != texts.size()) break;  // degrade to BM25
            for (std::size_t j = i; j < hi; ++j) {
                chunks_[j].embedding = std::move((*vecs)[j - i]);
                if (embed_dim_ == 0) embed_dim_ = chunks_[j].embedding.size();
            }
        }
        // Enforce a single dim — drop ragged embeddings rather than misrank.
        if (embed_dim_ > 0)
            for (auto& c : chunks_)
                if (c.embedding.size() != embed_dim_) c.embedding.clear();
    }

    bm25_ = build_bm25(chunks_);
    rebuild_hnsw_();
    return chunks_.size();
}

std::vector<std::vector<float>>
Corpus::embed_queries_(const std::vector<std::string>& queries,
                       const EmbedConfig& embed) const {
    std::vector<std::vector<float>> out;
    if (embed_dim_ == 0 || embed.model.empty() || queries.empty()) return out;
    // ONE round-trip for ALL variants. /api/embed accepts an array and
    // returns rows aligned to the input order (see embed_texts). Each query
    // gets the model's query-side prefix so the doc↔query asymmetry the index
    // was built with is preserved. Short QUERY leash: degrade to BM25 (empty
    // rows) rather than stall the search on a cold/wedged backend.
    EmbedConfig qcfg = embed;
    qcfg.timeout_ms = std::min<long>(qcfg.timeout_ms > 0 ? qcfg.timeout_ms
                                                          : 120'000, 10'000);
    std::vector<std::string> texts;
    texts.reserve(queries.size());
    for (const auto& q : queries)
        texts.push_back(with_query_prefix(embed, q));
    auto vs = embed_texts(qcfg, texts);
    if (!vs || vs->size() != queries.size()) return out;   // all-or-nothing
    // Keep only dimension-correct rows; a mismatched row (model drift) is
    // left empty so the per-query path treats it as "no vector" and falls
    // back to its own embed for just that variant.
    out = std::move(*vs);
    for (auto& v : out)
        if (v.size() != embed_dim_) v.clear();
    return out;
}

std::vector<Hit> Corpus::search_fused(const std::vector<std::string>& queries,
                                      const EmbedConfig& embed,
                                      std::size_t k) const {
    if (chunks_.empty() || k == 0 || queries.empty()) return {};
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);

    // BATCHED DENSE EMBED: collapse what used to be one blocking /api/embed
    // round-trip PER query variant (carryover + multi-hop + expansion + HyDE
    // can push that to 5-8 serial round-trips) into a SINGLE batched call.
    // The BM25/PRF lexical lists need no vector, so a failed/empty batch just
    // degrades this search to lexical — no per-query re-embed storm.
    const std::vector<std::vector<float>> qvecs = embed_queries_(queries, embed);

    std::vector<std::vector<std::uint32_t>> lists;
    std::vector<double> weights;
    std::vector<std::vector<std::pair<std::uint32_t, double>>> scored;
    const bool rsf = fusion_is_rsf_();
    for (std::size_t i = 0; i < queries.size(); ++i) {
        const std::vector<float>* qv =
            (i < qvecs.size() && !qvecs[i].empty()) ? &qvecs[i] : nullptr;
        ranked_lists_for_query_(queries[i], embed, pool, lists, &weights, qv,
                                rsf ? &scored : nullptr);
    }
    auto fused = rsf ? relative_score_fusion_weighted(scored, weights, k)
                     : reciprocal_rank_fusion_weighted(lists, weights, 60.0, k);
    std::vector<Hit> hits;
    hits.reserve(fused.size());
    for (auto& [id, score] : fused)
        if (id < chunks_.size())   // defensive: never OOB on a stale/corrupt id
            hits.push_back(Hit{&chunks_[id], score});
    return hits;
}

void Corpus::write_cache_() const {
    if (root_.empty()) return;
    // Group chunks by source path to write per-file records with size+mtime.
    // Tombstoned (lazily removed) chunks are skipped so a reload doesn't
    // resurrect them.
    std::unordered_map<std::string, std::vector<const Chunk*>> by_path;
    for (std::uint32_t i = 0; i < chunks_.size(); ++i)
        if (is_live_(i)) by_path[chunks_[i].path].push_back(&chunks_[i]);

    std::string blob;
    put(blob, kCacheMagic);
    put_str(blob, embed_model_);   // v5: identity of the vectors' model
    put(blob, static_cast<std::uint32_t>(embed_dim_));
    put(blob, static_cast<std::uint32_t>(by_path.size()));

    std::error_code ec;
    for (auto& [path, cs] : by_path) {
        std::uint64_t sz = 0;
        std::int64_t  mtime = 0;
        auto full = root_ / path;
        sz = static_cast<std::uint64_t>(fs::file_size(full, ec));
        if (ec) { ec.clear(); }
        auto mt = fs::last_write_time(full, ec);
        if (!ec) mtime = static_cast<std::int64_t>(mt.time_since_epoch().count());
        ec.clear();

        put_str(blob, path);
        put(blob, sz);
        put(blob, mtime);
        put(blob, static_cast<std::uint32_t>(cs.size()));
        for (const Chunk* c : cs) {
            put(blob, static_cast<std::int32_t>(c->line_start));
            put(blob, static_cast<std::int32_t>(c->line_end));
            put_str(blob, c->text);
            put_str(blob, c->context);   // v3: contextual-retrieval breadcrumb
            put(blob, static_cast<std::uint32_t>(c->embedding.size()));
            if (!c->embedding.empty())
                blob.append(reinterpret_cast<const char*>(c->embedding.data()),
                            c->embedding.size() * sizeof(float));
        }
    }

    // v2: append the corpus signature + HNSW graph after chunk data. The
    // signature is the structural fingerprint the graph's positional node ids
    // were built against; build() compares it before reusing the graph.
    // Skip persisting when tombstones are live: the on-disk chunk records are
    // compacted (dense) but the in-memory graph's node ids reference the
    // uncompacted positions, so its signature wouldn't match on reload.
    // Dropping it just triggers a one-time rebuild next session (safe).
    if (hnsw_built_ && !hnsw_.empty() && dead_.empty()) {
        put(blob, corpus_signature(chunks_));
        hnsw_.serialize(blob);
    }

    std::ofstream f(root_ / kCacheName, std::ios::binary | std::ios::trunc);
    if (f) f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

} // namespace agentty::rag
