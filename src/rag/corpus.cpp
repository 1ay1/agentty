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
    tos.total   = std::chrono::milliseconds(120'000);  // batch can be large

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

// ── Corpus ──────────────────────────────────────────────────────────────────

namespace {

constexpr char kCacheName[] = ".agentty_rag_cache.bin";
constexpr std::uint32_t kCacheMagic = 0x52414701;  // "RAG\x01"

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
    {
        std::string blob = read_file(root / kCacheName, 512ull * 1024 * 1024);
        std::string_view b{blob};
        std::uint32_t magic = 0;
        if (get(b, magic) && magic == kCacheMagic) {
            std::uint32_t dim = 0, nfiles = 0;
            get(b, dim);
            get(b, nfiles);
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
                cache.emplace(std::move(path), std::move(cf));
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
                texts.push_back(need_embed[j]->text);
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
    // NOTE: we do NOT persist the HNSW graph into the on-disk cache — the
    // expensive part (embeddings) IS cached, and rebuilding the in-memory
    // graph from those vectors is fast, so we rebuild it per session rather
    // than risk entangling the existing cache format. The graph references
    // chunk ids as the i-th chunk index (matching how search() materializes
    // hits via &chunks_[id]); do NOT reorder chunks_ after building HNSW.
    hnsw_built_ = false;
    constexpr std::size_t kHnswThreshold = 2000;
    if (embed_dim_ > 0 && chunks_.size() >= kHnswThreshold) {
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

    write_cache_();
}

std::vector<Hit> Corpus::search(std::string_view query,
                                const EmbedConfig& embed,
                                std::size_t k) const {
    if (chunks_.empty() || k == 0) return {};

    // Pull a generous candidate pool from each retriever, then fuse + cut to
    // k (SOTA pattern: retrieve wide, fuse, return narrow).
    const std::size_t pool = std::max<std::size_t>(k * 8, 32);

    // BM25 ranked list (always available).
    std::vector<std::uint32_t> bm25_rank;
    for (auto& [id, score] : bm25_search(bm25_, query, pool))
        bm25_rank.push_back(id);

    std::vector<std::vector<std::uint32_t>> lists;
    lists.push_back(std::move(bm25_rank));

    // Dense ranked list (only when the corpus AND the query can be embedded).
    if (embed_dim_ > 0 && !embed.model.empty()) {
        auto qv = embed_texts(embed, {std::string{query}});
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
            if (!dense_rank.empty()) lists.push_back(std::move(dense_rank));
        }
    }

    // Fuse with RRF (k=60 canonical) and materialize the hits.
    auto fused = reciprocal_rank_fusion(lists, /*k=*/60.0, k);
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
            put(blob, static_cast<std::uint32_t>(c->embedding.size()));
            if (!c->embedding.empty())
                blob.append(reinterpret_cast<const char*>(c->embedding.data()),
                            c->embedding.size() * sizeof(float));
        }
    }

    std::ofstream f(root_ / kCacheName, std::ios::binary | std::ios::trunc);
    if (f) f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
}

} // namespace agentty::rag
