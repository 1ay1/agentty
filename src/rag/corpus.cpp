// agentty::rag — Ollama embeddings client + the Corpus (build, cache, hybrid
// search). The embeddings call reuses the already-running Ollama server over
// plaintext localhost HTTP — no new dependency. See rag.hpp for rationale.

#include "agentty/rag/rag.hpp"

#include "agentty/io/http.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace agentty::rag {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── Embeddings (Ollama /api/embed) ─────────────────────────────────────────

std::optional<std::vector<std::vector<float>>>
embed_texts(const EmbedConfig& cfg, const std::vector<std::string>& texts) {
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
    embed_dim_ = 0;
    for (const auto& c : chunks_)
        if (!c.embedding.empty()) { embed_dim_ = c.embedding.size(); break; }
    bm25_ = build_bm25(chunks_);
}

void Corpus::build(const fs::path& root, const EmbedConfig& embed) {
    root_ = root;
    chunks_.clear();
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
        hnsw_loaded && !hnsw_.empty() && hnsw_.dim() == embed_dim_ &&
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
void Corpus::rebuild_hnsw_() {
    constexpr std::size_t kHnswThreshold = 2000;
    hnsw_ = HnswIndex{};
    hnsw_built_ = false;
    if (embed_dim_ == 0 || chunks_.size() < kHnswThreshold) return;

    std::vector<std::uint32_t> ids;
    std::vector<const std::vector<float>*> embs;
    ids.reserve(chunks_.size());
    embs.reserve(chunks_.size());
    for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
        if (chunks_[i].embedding.size() == embed_dim_) {
            ids.push_back(i);
            embs.push_back(&chunks_[i].embedding);
        }
    }
    if (!ids.empty()) { hnsw_.build(ids, embs); hnsw_built_ = !hnsw_.empty(); }
}

void Corpus::ranked_lists_for_query_(
    std::string_view query, const EmbedConfig& embed, std::size_t pool,
    std::vector<std::vector<std::uint32_t>>& lists,
    std::vector<double>* weights) const {
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

    // BM25 ranked list (always available).
    std::vector<std::uint32_t> bm25_rank;
    for (auto& [id, score] : bm25_search(bm25_, query, pool))
        bm25_rank.push_back(id);
    lists.push_back(std::move(bm25_rank));
    if (weights) weights->push_back(w_lex);

    // Dense ranked list (only when the corpus AND the query can be embedded).
    if (embed_dim_ > 0 && !embed.model.empty()) {
        // QUERY-time embed gets a short leash: this call sits on the
        // search_docs path (and the pre-turn proactive path). A single
        // short text should embed in tens of ms; if Ollama is wedged or
        // cold-loading a model, degrade to BM25-only for THIS query rather
        // than hanging the search for the index-time 120s budget.
        EmbedConfig qcfg = embed;
        qcfg.timeout_ms = std::min<long>(qcfg.timeout_ms, 10'000);
        auto qv = embed_texts(qcfg, {with_query_prefix(embed, std::string{query})});
        if (qv && qv->size() == 1 && (*qv)[0].size() == embed_dim_) {
            const auto& q = (*qv)[0];
            std::vector<std::uint32_t> dense_rank;
            if (hnsw_built_) {
                // ANN candidate generation — O(log n) vs the brute-force
                // O(n) scan below. Widen ef beyond the pool for recall.
                for (auto& [id, sim] : hnsw_.search(
                         q, pool, std::max<std::size_t>(pool * 2, 64)))
                    dense_rank.push_back(id);
            } else {
                std::vector<std::pair<std::uint32_t, double>> sims;
                sims.reserve(chunks_.size());
                for (std::uint32_t i = 0; i < chunks_.size(); ++i) {
                    if (chunks_[i].embedding.size() != embed_dim_) continue;
                    sims.push_back({i, cosine(q, chunks_[i].embedding)});
                }
                std::sort(sims.begin(), sims.end(), [](auto& a, auto& b) {
                    if (a.second != b.second) return a.second > b.second;
                    return a.first < b.first;
                });
                if (sims.size() > pool) sims.resize(pool);
                dense_rank.reserve(sims.size());
                for (auto& [id, s] : sims) dense_rank.push_back(id);
            }
            if (!dense_rank.empty()) {
                lists.push_back(std::move(dense_rank));
                if (weights) weights->push_back(w_dense);
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
    ranked_lists_for_query_(query, embed, pool, lists, &weights);

    // Fuse with WEIGHTED RRF (k=60 canonical; dense out-weights lexical) and
    // materialize the hits.
    auto fused = reciprocal_rank_fusion_weighted(lists, weights, /*k=*/60.0, k);
    std::vector<Hit> hits;
    hits.reserve(fused.size());
    for (auto& [id, score] : fused)
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
    for (auto& c : new_chunks)
        chunks_.push_back(std::move(c));

    // Rebuild lexical + ANN indices over the full corpus (positions changed).
    bm25_ = build_bm25(chunks_);
    rebuild_hnsw_();
    return added;
}

std::size_t Corpus::remove_document(const std::string& path) {
    std::size_t before = chunks_.size();
    chunks_.erase(
        std::remove_if(chunks_.begin(), chunks_.end(),
            [&path](const Chunk& c) { return c.path == path; }),
        chunks_.end());

    std::size_t removed = before - chunks_.size();
    if (removed == 0) return 0;

    // Chunk positions shifted — rebuild both indices so HNSW node ids stay
    // aligned with chunks_ (search() materializes via &chunks_[id]).
    bm25_ = build_bm25(chunks_);
    rebuild_hnsw_();
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
    embed_dim_ = 0;
    embed_model_ = embed.model;   // keep identity coherent on this path too
    hnsw_ = HnswIndex{};
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

std::vector<Hit> Corpus::search_fused(const std::vector<std::string>& queries,
                                      const EmbedConfig& embed,
                                      std::size_t k) const {
    if (chunks_.empty() || k == 0 || queries.empty()) return {};
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);
    std::vector<std::vector<std::uint32_t>> lists;
    std::vector<double> weights;
    for (const auto& q : queries)
        ranked_lists_for_query_(q, embed, pool, lists, &weights);  // BM25(+dense) each
    auto fused = reciprocal_rank_fusion_weighted(lists, weights, 60.0, k);
    std::vector<Hit> hits;
    hits.reserve(fused.size());
    for (auto& [id, score] : fused)
        hits.push_back(Hit{&chunks_[id], score});
    return hits;
}

void Corpus::write_cache_() const {
    if (root_.empty()) return;
    // Group chunks by source path to write per-file records with size+mtime.
    std::unordered_map<std::string, std::vector<const Chunk*>> by_path;
    for (const auto& c : chunks_) by_path[c.path].push_back(&c);

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
    if (hnsw_built_ && !hnsw_.empty()) {
        put(blob, corpus_signature(chunks_));
        hnsw_.serialize(blob);
    }

    std::ofstream f(root_ / kCacheName, std::ios::binary | std::ios::trunc);
    if (f) f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

} // namespace agentty::rag
