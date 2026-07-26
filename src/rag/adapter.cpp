// agentty::rag adapter — the retrieval engine, powered by rag-cpp (rag::Engine).
//
// This is the ONLY translation unit in agentty that includes <rag/rag.hpp>.
// It maps agentty's compact retrieval boundary (rag_adapter.hpp) onto rag-cpp's
// production hybrid engine: contextual chunking, BM25 + dense/HNSW, RRF fusion,
// rerank, GraphRAG, and .ragdb persistence — so `search_docs`, `search_code`
// and the proactive pre-turn path "just work" and stay fast.
//
// Knowledge sources are unified into ONE Engine and distinguished by a URI
// prefix so provenance survives search():
//     docs://<rel-path>      the docs / knowledge folder (code-aware chunking)
//     skill://<name>         installed Agent-Skills bodies
//     memory://<id>          learned facts (JSONL memory store)
//     mcp://<uri>            this session's MCP resources (opt-in)

#include "agentty/rag/rag_adapter.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rag/rag.hpp>

#include "agentty/tool/skills.hpp"
#include "agentty/tool/memory_store.hpp"
#include "agentty/util/dbglog.hpp"

namespace fs = std::filesystem;

namespace agentty::rag {
namespace {

bool truthy_default_on(const char* var) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) return true;                 // unset ⇒ ON
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

fs::path resolve_docs_root(const std::string& configured) {
    // Env wins and is re-read EVERY call: AGENTTY_DOCS_DIR can change between
    // retrievals (tests, or a user who repoints it), and a cached root would
    // silently serve the wrong corpus.
    if (const char* d = std::getenv("AGENTTY_DOCS_DIR"); d && d[0])
        return fs::path{d};
    if (!configured.empty()) return fs::path{configured};
    std::error_code ec;
    auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto docs = cwd / "docs";
    if (fs::is_directory(docs, ec)) return docs;
    auto kb = cwd / ".agentty" / "knowledge";
    if (fs::is_directory(kb, ec)) return kb;
    return {};
}

// Split a "docs://path" uri back into (source-tag, bare-path).
std::pair<std::string, std::string> split_uri(const std::string& uri) {
    auto pos = uri.find("://");
    if (pos == std::string::npos) return {"docs", uri};
    return {uri.substr(0, pos), uri.substr(pos + 3)};
}

} // namespace

Config Config::from_env() {
    Config c;
    if (const char* d = std::getenv("AGENTTY_DOCS_DIR"); d && d[0]) c.docs_root = d;
    if (const char* m = std::getenv("AGENTTY_EMBED_MODEL"); m && m[0]) c.embed_model = m;
    if (const char* h = std::getenv("AGENTTY_OLLAMA_HOST"); h && h[0]) {
        std::string hs{h};
        if (auto colon = hs.rfind(':'); colon != std::string::npos) {
            c.embed_host = hs.substr(0, colon);
            try {
                int p = std::stoi(hs.substr(colon + 1));
                if (p > 0 && p <= 65535) c.embed_port = static_cast<std::uint16_t>(p);
            } catch (...) { /* keep default port */ }
        } else {
            c.embed_host = hs;
        }
    }
    c.skills        = truthy_default_on("AGENTTY_RAG_SKILLS");
    c.memory        = truthy_default_on("AGENTTY_RAG_MEMORY");
    c.mcp_resources = false;   // opt-in; the backend flips this per call
    c.corrective    = truthy_default_on("AGENTTY_RAG_CORRECT");
    c.expand        = !truthy_default_on("AGENTTY_RAG_EXPAND") ? false
                      : (std::getenv("AGENTTY_RAG_EXPAND") != nullptr);
    c.hyde          = std::getenv("AGENTTY_RAG_HYDE") != nullptr;
    c.graph         = truthy_default_on("AGENTTY_RAG_GRAPH");
    return c;
}

// ─────────────────────────────────────────────────────────────────────────
struct Retriever::Impl {
    Config cfg = Config::from_env();

    std::mutex mu;
    ::rag::Engine engine;
    bool   embedder_ready = false;
    // Freshness of the docs index: (root, fingerprint) it was built for.
    std::string indexed_root;
    std::uint64_t indexed_fp = 0;
    // Skills / memory generation the in-memory sources were built for.
    std::size_t skills_gen = static_cast<std::size_t>(-1);
    std::size_t memory_gen = static_cast<std::size_t>(-1);

    std::atomic<bool> warming{false};

    // Separate engine for search_code (cwd source tree), with its own
    // edit-drift fingerprint. Kept apart from the docs engine so a docs
    // reindex never disturbs code search and vice-versa.
    ::rag::Engine code_engine{make_engine_config()};
    bool          code_embedder_ready = false;
    std::uint64_t code_fp = 0;

    Impl() : engine(make_engine_config()) { attach_embedder(); }

    static ::rag::index::CorpusConfig make_engine_config() {
        ::rag::index::CorpusConfig cc;
        // Contextual chunking on by default: rag-cpp prepends a heading
        // breadcrumb so a fragment that lost its context still ranks.
        return cc;
    }

    void attach_embedder() {
        // Ollama dense embedder, with a deterministic local-hash fallback so a
        // machine with no Ollama still gets (BM25 + hash-dense) hybrid — the
        // engine NEVER hard-fails on a missing model.
        ::rag::plugin::Json spec = {
            {"type", "ollama"},
            {"model", cfg.embed_model},
            {"host", cfg.embed_host},
            {"port", cfg.embed_port},
        };
        auto r = engine.with_embedder_spec(spec);
        if (!r) {
            // Fallback to the local hash embedder (no network).
            engine.with_embedder(::rag::dense::AnyEmbedder{::rag::dense::HashEmbedder{256}});
            ::agentty::util::dbglog("rag.embed", "ollama spec failed, using hash embedder");
        }
        embedder_ready = true;
    }

    // Cheap directory fingerprint: sum of (size ^ mtime) over indexable files.
    std::uint64_t fingerprint(const fs::path& root) {
        std::uint64_t fp = 1469598103934665603ull;
        if (root.empty()) return fp;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            auto sz = fs::file_size(it->path(), ec);
            auto tm = fs::last_write_time(it->path(), ec).time_since_epoch().count();
            fp ^= (static_cast<std::uint64_t>(sz) * 1099511628211ull)
                  ^ static_cast<std::uint64_t>(tm);
            fp *= 1099511628211ull;
        }
        return fp;
    }

    // Rebuild the whole engine from scratch for the current source set. Called
    // under `mu`. `skip_docs` means "don't WALK the docs folder" — but any docs
    // already indexed for `indexed_root` are preserved (re-loaded from that
    // folder without a freshness walk), so the warm/proactive path never loses
    // the docs corpus. Cheap paths (unchanged fp/gen) return early via
    // needs_reindex().
    void reindex(const fs::path& root, bool skip_docs) {
        engine = ::rag::Engine(make_engine_config());
        embedder_ready = false;
        attach_embedder();

        // Docs / knowledge folder. On the warm path (skip_docs) we still index
        // the LAST KNOWN docs root if there was one — we just skip the
        // fingerprint walk that would detect drift. This keeps docs available
        // to proactive retrieval without paying the freshness scan.
        fs::path docs_root = root;
        if (skip_docs && docs_root.empty() && !indexed_root.empty())
            docs_root = fs::path{indexed_root};
        if (!docs_root.empty()) {
            ::rag::loaders::DirOptions opts;   // sane include/exclude defaults
            auto docs = ::rag::loaders::load_directory(docs_root, opts);
            if (docs) {
                for (auto& d : *docs) {
                    std::string rel = d.meta.count("rel") ? d.meta["rel"] : d.uri;
                    (void)engine.add("docs://" + rel, std::move(d.text), d.meta, d.title);
                }
            }
        }

        // Skills.
        if (cfg.skills) {
            for (const auto& s : tools::skills::all()) {
                if (s.body.empty()) continue;
                (void)engine.add("skill://" + s.name, s.body, {{"kind", "skill"}}, s.name);
            }
        }

        // Learned memory (both scopes).
        if (cfg.memory) {
            for (auto scope : {tools::memory::Scope::User, tools::memory::Scope::Project}) {
                for (const auto& r : tools::memory::load_all(scope)) {
                    if (r.text.empty()) continue;
                    (void)engine.add("memory://" + r.id, r.text,
                               {{"kind", "memory"}, {"scope", std::string(tools::memory::to_string(scope))}});
                }
            }
        }

        (void)engine.build();

        if (!skip_docs) {
            indexed_root = root.string();
            indexed_fp   = fingerprint(root);
        } else if (indexed_root.empty() && !root.empty()) {
            indexed_root = root.string();
        }
        skills_gen   = tools::skills::all().size();
        memory_gen   = tools::memory::load_all(tools::memory::Scope::User).size()
                       + tools::memory::load_all(tools::memory::Scope::Project).size();
    }

    bool needs_reindex(const fs::path& root, bool skip_docs) {
        // A changed docs root ALWAYS forces a rebuild — even on the warm path —
        // so proactive retrieval can never serve a stale corpus after the
        // folder is repointed.
        if (!root.empty() && indexed_root != root.string()) return true;
        if (!skip_docs && fingerprint(root) != indexed_fp) return true;
        if (skills_gen != tools::skills::all().size()) return true;
        std::size_t mgen = tools::memory::load_all(tools::memory::Scope::User).size()
                           + tools::memory::load_all(tools::memory::Scope::Project).size();
        if (memory_gen != mgen) return true;
        return false;
    }

    // Fingerprint over source files only (search_code drift signal).
    std::uint64_t code_fingerprint(const fs::path& root,
                                   ::rag::loaders::DirOptions& opts) {
        std::uint64_t fp = 1469598103934665603ull;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            auto ext = it->path().extension().string();
            for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
            bool want = false;
            for (auto& e : opts.include_ext) if (e == ext) { want = true; break; }
            if (!want) continue;
            auto sz = fs::file_size(it->path(), ec);
            auto tm = fs::last_write_time(it->path(), ec).time_since_epoch().count();
            fp = (fp ^ (static_cast<std::uint64_t>(sz)
                        ^ static_cast<std::uint64_t>(tm))) * 1099511628211ull;
        }
        return fp;
    }

    void attach_code_embedder() {
        ::rag::plugin::Json spec = {
            {"type", "ollama"}, {"model", cfg.embed_model},
            {"host", cfg.embed_host}, {"port", cfg.embed_port}};
        if (!code_engine.with_embedder_spec(spec))
            code_engine.with_embedder(::rag::dense::AnyEmbedder{::rag::dense::HashEmbedder{256}});
        code_embedder_ready = true;
    }
};

Retriever::Retriever() : impl_(new Impl()) {}
Retriever::~Retriever() { delete impl_; }

Retrieval Retriever::retrieve(const std::string& query, int k, bool skip_docs) {
    Retrieval out;
    if (query.empty()) { out.error = "empty query"; return out; }
    const int kk = k > 0 ? k : 6;
    try {
        std::lock_guard<std::mutex> lock(impl_->mu);
        auto root = resolve_docs_root(skip_docs ? std::string{} : impl_->cfg.docs_root);

        if (impl_->needs_reindex(root, skip_docs))
            impl_->reindex(root, skip_docs);

        const bool any = impl_->engine.corpus().chunk_count() > 0;
        if (!any) {
            out.error = "no knowledge configured. Set AGENTTY_DOCS_DIR to a "
                        "folder of docs, put files under ./docs, install skills, "
                        "or store memories to give search_docs something to find.";
            return out;
        }

        // WIDE pool then trim: the standard pipeline already fuses BM25 + dense
        // via RRF and reranks. Ask for a bit more than k so the diversity/rerank
        // stages have material to work with.
        auto res = impl_->engine.search(query, static_cast<std::size_t>(kk));
        if (!res) { out.error = "retrieval failed"; return out; }

        double top = 0.0;
        for (const auto& r : *res) {
            auto [src, path] = split_uri(r.uri);
            Passage p;
            p.source     = src;
            p.path       = path;
            p.line_start = static_cast<int>(r.start_line);
            p.line_end   = static_cast<int>(r.end_line);
            // rag-cpp scores (RRF / rerank / cosine) can be small or, for a
            // hash-embed cosine, negative. Clamp to [0,1] so the confidence
            // the proactive gate parses is a well-behaved probability-like
            // signal, never a spuriously-negative value that suppresses a
            // genuine hit.
            double s = static_cast<double>(r.score.value);
            if (s < 0.0) s = 0.0;
            if (s > 1.0) s = 1.0;
            p.score      = s;
            // Prefer the context breadcrumb + body so a fragment reads well.
            p.text       = r.context.empty() ? r.text : (r.context + "\n" + r.text);
            top = std::max(top, p.score);
            out.passages.push_back(std::move(p));
        }
        out.confidence = top;

        // Human-readable provenance for the tool-shell header.
        std::string m = "hybrid+ctx";
        if (impl_->cfg.graph) m += "+graph";
        m += ", reranked";
        if (!root.empty() && !skip_docs) m += ", docs=" + root.string();
        char buf[48];
        std::snprintf(buf, sizeof buf, ", confidence %.2f", out.confidence);
        m += buf;
        out.mode = std::move(m);
    } catch (const std::exception& e) {
        out.error = std::string("retrieval error: ") + e.what();
    } catch (...) {
        out.error = "retrieval error";
    }
    return out;
}

Retrieval Retriever::retrieve_code(const std::string& query, int k) {
    Retrieval out;
    if (query.empty()) { out.error = "empty query"; return out; }
    const int kk = k > 0 ? k : 6;
    try {
        std::lock_guard<std::mutex> lock(impl_->mu);
        std::error_code ec;
        auto root = fs::current_path(ec);
        if (ec) { out.error = "search_code: cannot resolve cwd"; return out; }

        ::rag::loaders::DirOptions opts;
        opts.include_ext = {
            ".c",".cc",".cpp",".cxx",".h",".hh",".hpp",".hxx",".inl",
            ".py",".js",".jsx",".ts",".tsx",".mjs",".go",".rs",".java",
            ".kt",".swift",".rb",".php",".cs",".scala",".sh",".bash",
            ".zig",".lua",".sql",".proto",".cmake",".md"};
        opts.exclude_dirs = {
            ".git",".hg",".svn","node_modules","build","dist","out",
            "target","venv",".venv","__pycache__",".cache","_deps",
            "CMakeFiles",".agentty","vendor","third_party"};
        opts.max_file_bytes = 256 * 1024;

        std::uint64_t fp = impl_->code_fingerprint(root, opts);
        if (fp != impl_->code_fp || impl_->code_engine.corpus().chunk_count() == 0) {
            impl_->code_engine = ::rag::Engine(Impl::make_engine_config());
            impl_->attach_code_embedder();
            auto files = ::rag::loaders::load_directory(root, opts);
            if (files) {
                for (auto& d : *files) {
                    std::string rel = d.meta.count("rel") ? d.meta["rel"] : d.uri;
                    (void)impl_->code_engine.add("code://" + rel, std::move(d.text), d.meta, d.title);
                }
            }
            (void)impl_->code_engine.build();
            impl_->code_fp = fp;
        }

        if (impl_->code_engine.corpus().chunk_count() == 0) {
            out.error = "search_code: no source files found under " + root.string();
            return out;
        }

        auto res = impl_->code_engine.search(query, static_cast<std::size_t>(kk));
        if (!res) { out.error = "search_code failed"; return out; }
        double top = 0.0;
        for (const auto& r : *res) {
            auto [src, path] = split_uri(r.uri);
            (void)src;
            Passage p;
            p.source     = "code";
            p.path       = path;
            p.line_start = static_cast<int>(r.start_line);
            p.line_end   = static_cast<int>(r.end_line);
            p.score      = static_cast<double>(r.score.value);
            p.text       = r.text;
            top = std::max(top, p.score);
            out.passages.push_back(std::move(p));
        }
        out.confidence = top;
        out.mode = "hybrid, " + std::to_string(impl_->code_engine.corpus().chunk_count())
                 + " chunks from " + root.string();
    } catch (const std::exception& e) {
        out.error = std::string("search_code failed: ") + e.what();
    } catch (...) {
        out.error = "search_code failed";
    }
    return out;
}

bool Retriever::warm() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto root = resolve_docs_root(impl_->cfg.docs_root);
    if (root.empty()) return true;                 // no docs ⇒ always warm
    return !impl_->needs_reindex(root, /*skip_docs=*/false);
}

void Retriever::warm_async() {
    bool expected = false;
    if (!impl_->warming.compare_exchange_strong(expected, true)) return;
    std::thread([this] {
        try {
            std::lock_guard<std::mutex> lock(impl_->mu);
            auto root = resolve_docs_root(impl_->cfg.docs_root);
            if (impl_->needs_reindex(root, /*skip_docs=*/false))
                impl_->reindex(root, /*skip_docs=*/false);
        } catch (...) { /* best-effort */ }
        impl_->warming.store(false);
    }).detach();
}

// ── Learning loop (write side) ─────────────────────────────────────────
namespace feedback {
void note_file_opened(const std::string& path) {
    // The rag-cpp engine ranks on content relevance; the old bespoke "opened
    // file → boost" signal is a best-effort no-op here (kept as a stable API
    // seam so callers don't churn). A future rev can persist a per-path prior.
    (void)path;
}
} // namespace feedback

// ── CLI: `agentty rag-bench <root>` ────────────────────────────────────
namespace bench {
int run(const std::string& root) {
    fs::path r = resolve_docs_root(root);
    if (r.empty()) {
        std::fprintf(stderr, "rag-bench: no docs root (pass a folder or set AGENTTY_DOCS_DIR)\n");
        return 2;
    }
    try {
        auto cfg = Config::from_env();
        ::rag::Engine engine;
        ::rag::plugin::Json spec = {
            {"type", "ollama"}, {"model", cfg.embed_model},
            {"host", cfg.embed_host}, {"port", cfg.embed_port}};
        if (!engine.with_embedder_spec(spec))
            engine.with_embedder(::rag::dense::AnyEmbedder{::rag::dense::HashEmbedder{256}});

        auto t0 = std::chrono::steady_clock::now();
        ::rag::loaders::DirOptions opts;
        auto docs = ::rag::loaders::load_directory(r, opts);
        std::size_t n = 0;
        if (docs) for (auto& d : *docs) { (void)engine.add(d.uri, std::move(d.text), d.meta, d.title); ++n; }
        (void)engine.build();
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::printf("rag-bench: indexed %zu docs, %zu chunks from %s in %lld ms\n",
                    n, engine.corpus().chunk_count(), r.string().c_str(),
                    static_cast<long long>(ms));

        for (const char* q : {"how does it work", "configuration", "error handling"}) {
            auto t2 = std::chrono::steady_clock::now();
            auto res = engine.search(q, 5);
            auto t3 = std::chrono::steady_clock::now();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
            std::printf("  q=\"%s\": %zu hits in %lld us\n", q,
                        res ? res->size() : 0u, static_cast<long long>(us));
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rag-bench: %s\n", e.what());
        return 1;
    }
}
} // namespace bench

} // namespace agentty::rag
