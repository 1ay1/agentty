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

// agentty's retrieval engine is rag-cpp. On platforms where rag-cpp isn't
// buildable yet (MSVC — see CMakeLists.txt), AGENTTY_HAS_RAGCPP is 0 and this
// TU compiles to a tiny no-op implementation of the public boundary so the
// rest of agentty links and every non-retrieval feature works. The full engine
// ships in the Linux/macOS binaries.
#ifndef AGENTTY_HAS_RAGCPP
#  define AGENTTY_HAS_RAGCPP 1
#endif

#if AGENTTY_HAS_RAGCPP

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rag/rag.hpp>

#include "agentty/io/http.hpp"
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

bool truthy_default_off(const char* var) {
    const char* v = std::getenv(var);
    if (!v || !v[0]) return false;                // unset ⇒ OFF
    return !(v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N');
}

float env_float(const char* var, float dflt) {
    if (const char* v = std::getenv(var); v && v[0]) {
        try { return std::stof(v); } catch (...) {}
    }
    return dflt;
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

// ── FeedbackStore ─────────────────────────────────────────────────────────
//   The learning loop, made real. A single process-wide store backs BOTH the
//   write side (feedback::note_surfaced / note_file_opened free functions) and
//   the read side (Retriever::Impl::feedback_boost). It persists an append-only
//   TSV of `use`/`win` events to <cwd>/.agentty/rag_feedback.tsv and folds the
//   Beta-smoothed per-path win-rate back into ranking as a BOUNDED nudge.
//
//   Signal semantics (matches docs/website/retrieval.md):
//     use — search_docs surfaced this path (denominator).
//     win — the agent then `read` a path that was RECENTLY surfaced
//           (numerator). A read of a never-surfaced file is NOT a win: it
//           carries no relevance judgment about retrieval.
//
//   The nudge is a multiplicative factor in [1-kMax, 1+kMax] centred on 1.0,
//   scaled by how far the smoothed win-rate sits from the neutral prior and by
//   a confidence term that grows with sample count. Paths with no history are
//   untouched, so the loop can only help once it has evidence.
class FeedbackStore {
public:
    static FeedbackStore& instance() {
        static FeedbackStore s;
        return s;
    }

    // Ranking nudge for `path`, in roughly [0.85, 1.15]. 1.0 (neutral) when
    // learning is off, the path is unseen, or it has too little evidence.
    float boost(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        auto it = counts_.find(path);
        if (it == counts_.end()) return 1.0f;
        const double uses = it->second.uses;
        const double wins = it->second.wins;
        if (uses <= 0.0) return 1.0f;
        // Beta(1,1) smoothing: (wins+1)/(uses+2) — a bounded win-rate that
        // starts at the 0.5 prior and moves only as evidence accrues.
        const double rate = (wins + 1.0) / (uses + 2.0);
        // Confidence in the estimate grows with sample count (saturating).
        const double conf = uses / (uses + kPriorN);
        const double delta = (rate - 0.5) * 2.0;         // → [-1, 1]
        double factor = 1.0 + kMax * delta * conf;
        if (factor < 1.0 - kMax) factor = 1.0 - kMax;
        if (factor > 1.0 + kMax) factor = 1.0 + kMax;
        return static_cast<float>(factor);
    }

    // Record that these paths were surfaced by a retrieval (each a "use"), and
    // remember them as recently-surfaced so a following read can be a win.
    void note_surfaced(const std::vector<std::string>& paths) {
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        std::vector<std::string> to_write;
        for (const auto& p : paths) {
            if (p.empty()) continue;
            counts_[p].uses += 1.0;
            recent_.insert(p);
            to_write.push_back(p);
        }
        append_("use", to_write);
    }

    // Record that `path` was opened. It is a WIN only if it (or its basename)
    // was recently surfaced — otherwise the read tells us nothing about
    // retrieval quality and is ignored (no denominator inflation either).
    void note_opened(const std::string& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        ensure_loaded_();
        std::string matched = match_recent_(path);
        if (matched.empty()) return;               // not surfaced ⇒ not a signal
        counts_[matched].wins += 1.0;
        append_("win", {matched});
    }

private:
    struct Tally { double uses = 0.0; double wins = 0.0; };

    static constexpr double kMax    = 0.15;   // max ±15% ranking nudge
    static constexpr double kPriorN = 4.0;    // uses needed for ~half confidence

    std::mutex mu_;
    bool loaded_ = false;
    std::string loaded_for_;                   // cwd the TSV was loaded for
    std::unordered_map<std::string, Tally> counts_;
    std::unordered_set<std::string> recent_;   // surfaced this session

    fs::path tsv_path_() const {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return {};
        return cwd / ".agentty" / "rag_feedback.tsv";
    }

    // Load (once per cwd) the persisted counts so nudges survive restarts.
    void ensure_loaded_() {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        std::string cwds = ec ? std::string{} : cwd.string();
        if (loaded_ && loaded_for_ == cwds) return;
        counts_.clear();
        loaded_ = true;
        loaded_for_ = cwds;
        auto p = tsv_path_();
        if (p.empty()) return;
        std::ifstream f(p);
        if (!f) return;
        std::string line;
        while (std::getline(f, line)) {
            // <epoch>\t<use|win>\t<path>
            auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            auto t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string kind = line.substr(t1 + 1, t2 - t1 - 1);
            std::string path = line.substr(t2 + 1);
            if (path.empty()) continue;
            if (kind == "use") counts_[path].uses += 1.0;
            else if (kind == "win") counts_[path].wins += 1.0;
        }
    }

    void append_(const char* kind, const std::vector<std::string>& paths) {
        if (paths.empty()) return;
        auto p = tsv_path_();
        if (p.empty()) return;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::app);
        if (!f) return;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        for (const auto& path : paths)
            f << now << '\t' << kind << '\t' << path << '\n';
    }

    // A read matches a surfaced passage if the exact path is recent, or if a
    // recent path ends with the same basename (docs://foo/bar.md surfaced,
    // agent reads foo/bar.md or an absolute .../foo/bar.md).
    std::string match_recent_(const std::string& opened) {
        if (auto it = recent_.find(opened); it != recent_.end()) return opened;
        auto slash = opened.find_last_of("/\\");
        std::string base = slash == std::string::npos ? opened
                                                       : opened.substr(slash + 1);
        if (base.empty()) return {};
        for (const auto& r : recent_) {
            if (r.size() >= base.size() &&
                r.compare(r.size() - base.size(), base.size(), base) == 0) {
                // Guard against a bare-basename false match on a longer name.
                if (r.size() == base.size() ||
                    r[r.size() - base.size() - 1] == '/' ||
                    r[r.size() - base.size() - 1] == '\\')
                    return r;
            }
        }
        return {};
    }
};

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

    c.contextual  = truthy_default_on("AGENTTY_RAG_CONTEXTUAL");
    c.mmr         = truthy_default_on("AGENTTY_RAG_MMR");
    c.mmr_lambda  = env_float("AGENTTY_RAG_MMR_LAMBDA", 0.5f);
    c.stitch      = truthy_default_on("AGENTTY_RAG_STITCH");
    c.prf         = truthy_default_on("AGENTTY_RAG_PRF");
    c.corrective  = truthy_default_on("AGENTTY_RAG_CORRECT");
    c.graph       = truthy_default_on("AGENTTY_RAG_GRAPH");
    // Generative query expansion has a measurable latency cost even when its
    // model is local. It is an explicit power-mode, not a tax on every turn.
    c.expand      = truthy_default_off("AGENTTY_RAG_EXPAND");
    c.hyde        = truthy_default_off("AGENTTY_RAG_HYDE");
    if (const char* g = std::getenv("AGENTTY_RAG_GEN_MODEL"); g && g[0]) c.gen_model = g;
    c.persist     = truthy_default_on("AGENTTY_RAG_PERSIST");
    c.learn       = truthy_default_on("AGENTTY_RAG_LEARN");
    c.trace       = truthy_default_off("AGENTTY_RAG_TRACE");
    c.dense_weight = env_float("AGENTTY_RAG_DENSE_WEIGHT", 1.0f);
    c.bm25_weight  = env_float("AGENTTY_RAG_BM25_WEIGHT", 1.0f);
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

    // Optional LLM seam for HyDE / multi-query (agentty's provider).
    Retriever::Generator generator;

    // Separate engine for search_code (cwd source tree), with its own
    // edit-drift fingerprint. Kept apart from the docs engine so a docs
    // reindex never disturbs code search and vice-versa.
    ::rag::Engine code_engine{::rag::index::CorpusConfig{}};
    bool          code_embedder_ready = false;
    std::uint64_t code_fp = 0;

    Impl() : engine(make_engine_config()) {
        attach_embedder();
        apply_pipeline(engine);
        install_default_generator();
    }

    // Install a ZERO-COST local generator for HyDE / multi-query: a tiny model
    // on the SAME Ollama we embed with. No cloud tokens, no auth, no provider
    // plumbing. If Ollama isn't up, the call fails fast and HyDE/expand no-op
    // (plain hybrid still runs) — so this is free when unavailable and a recall
    // win when present. An explicit set_generator() overrides it.
    void install_default_generator() {
        std::string host = cfg.embed_host;
        std::uint16_t port = cfg.embed_port;
        std::string model = cfg.gen_model;
        generator = [host, port, model](const std::string& prompt, int n)
                        -> std::vector<std::string> {
            std::vector<std::string> outs;
            const int want = n > 0 ? n : 1;
            try {
                for (int i = 0; i < want; ++i) {
                    ::rag::plugin::Json body = {
                        {"model", model},
                        {"prompt", prompt},
                        {"stream", false},
                        {"options", {{"temperature", i == 0 ? 0.0 : 0.7},
                                      {"num_predict", 160}}},
                    };
                    ::agentty::http::Request req;
                    req.method    = ::agentty::http::HttpMethod::Post;
                    req.host      = host;
                    req.port      = port;
                    req.path      = "/api/generate";
                    req.plaintext = true;   // local Ollama speaks plain HTTP/1.1
                    req.headers.push_back({"content-type", "application/json"});
                    req.body      = body.dump();
                    req.max_body_bytes = 512 * 1024;

                    ::agentty::http::Timeouts to;
                    to.connect = std::chrono::milliseconds(600);   // Ollama absent → fail fast
                    to.total   = std::chrono::milliseconds(4000);  // hard cap per hypothetical
                    auto res = ::agentty::http::default_client().send(req, to);
                    if (!res || res->status != 200) break;   // Ollama down → give up
                    auto j = ::rag::plugin::Json::parse(res->body, nullptr, false);
                    if (j.is_discarded() || !j.contains("response")) continue;
                    std::string text = j["response"].get<std::string>();
                    if (!text.empty()) outs.push_back(std::move(text));
                }
            } catch (...) { /* best-effort; empty → plain hybrid */ }
            return outs;
        };
    }

    ::rag::index::CorpusConfig make_engine_config() {
        ::rag::index::CorpusConfig cc;
        // Anthropic Contextual Retrieval: situate each chunk in its document
        // before indexing so a fragment that lost its heading still ranks.
        // Index-time cost only; a large measured recall win. With no LLM
        // contextualizer set, rag-cpp uses its deterministic extractive
        // fallback (needs no model, never fails).
        cc.contextual = cfg.contextual;
        return cc;
    }

    // Compose the FULL-POWER retrieval pipeline agentty drives, per Config:
    //   [PRF expand] → hybrid(convex fusion) → filter → feature-rerank
    //   → [MMR diversity] → [parent stitch] → top-k
    // Every optional stage is measured in rag-cpp's own benchmarks; the convex
    // (TM2C2) fusion is rag-cpp's default because it beats RRF on NDCG.
    void apply_pipeline(::rag::Engine& eng) {
        namespace pl = ::rag::pipeline;
        pl::HybridRetrieveConfig hy;
        hy.candidate_k  = 60;
        hy.fusion       = pl::HybridRetrieveConfig::Fusion::convex;
        hy.bm25_weight  = cfg.bm25_weight;
        hy.dense_weight = cfg.dense_weight;

        pl::Pipeline p;
        if (cfg.prf)
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
        p.add(std::make_shared<pl::HybridRetrieveStage>(hy));
        p.add(std::make_shared<pl::FilterStage>());
        // The exact accuracy-preserving feature reranker the built-in
        // standard()/quality()/context() pipelines use (deterministic, no model).
        p.add(pl::make_feature_rerank_stage());
        if (cfg.mmr)
            p.add(::rag::rerank::make_mmr_stage(cfg.mmr_lambda));
        if (cfg.stitch)
            p.add(std::make_shared<pl::ParentStitchStage>(1));
        p.add(std::make_shared<pl::TopKStage>());
        eng.with_pipeline(std::move(p));
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
        apply_pipeline(engine);

        if (!skip_docs) {
            indexed_root = root.string();
            indexed_fp   = fingerprint(root);
        } else if (indexed_root.empty() && !root.empty()) {
            indexed_root = root.string();
        }
        skills_gen   = tools::skills::all().size();
        memory_gen   = tools::memory::load_all(tools::memory::Scope::User).size()
                       + tools::memory::load_all(tools::memory::Scope::Project).size();

        // Persist the built corpus to a .ragdb so a later session opens warm
        // without re-walking + re-embedding the whole folder. Best-effort.
        if (cfg.persist) {
            if (auto p = ragdb_path(); !p.empty()) {
                std::error_code ec;
                fs::create_directories(p.parent_path(), ec);
                (void)engine.save(p.string());
            }
        }
    }

    // Where the persisted docs index lives (under the workspace .agentty/).
    fs::path ragdb_path() {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return {};
        return cwd / ".agentty" / "rag_docs.ragdb";
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

void Retriever::set_generator(Generator g) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->generator = std::move(g);
}

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

        // ── RETRIEVE ──────────────────────────────────────────────────
        // 1. Base hybrid retrieval through the full pipeline (PRF → convex
        //    fusion → filter → feature-rerank → MMR → parent-stitch → top-k).
        //    Optionally trace each stage for the mode header.
        std::vector<std::string> trace;
        std::vector<std::string>* tracep = impl_->cfg.trace ? &trace : nullptr;
        const std::size_t want = static_cast<std::size_t>(kk);

        std::vector<::rag::SearchResult> hits;
        std::string retriever_mode = "hybrid+ctx";

        // 2. LLM-assisted retrieval (HyDE / multi-query) when a Generator is
        //    wired AND enabled — closes the query↔document asymmetry gap and
        //    lifts recall. Degrades gracefully to plain search when absent.
        //    SKIPPED on the warm/proactive path (skip_docs): that path is
        //    latency-budgeted (pre-turn hedge) and must not wait on generation.
        bool used_llm = false;
        if (!skip_docs && impl_->generator && (impl_->cfg.hyde || impl_->cfg.expand)) {
            ::rag::query::Generator gen =
                [&](std::string_view prompt) -> ::rag::Result<std::vector<std::string>> {
                    try {
                        int n = impl_->cfg.expand ? 3 : 1;
                        auto outs = impl_->generator(std::string(prompt), n);
                        return outs;
                    } catch (...) {
                        return std::vector<std::string>{};
                    }
                };
            ::rag::Result<std::vector<::rag::Hit>> lh =
                std::unexpected(::rag::Error{});
            if (impl_->cfg.expand)
                lh = ::rag::query::multi_query_search(impl_->engine.corpus(), query, want, gen, 3);
            else
                lh = ::rag::query::hyde_search(impl_->engine.corpus(), query, want, gen);
            if (lh && !lh->empty()) {
                for (const auto& h : *lh) hits.push_back(impl_->engine.corpus().resolve(h));
                used_llm = true;
                retriever_mode += impl_->cfg.expand ? "+multiquery" : "+hyde";
            }
        }

        if (!used_llm) {
            auto res = impl_->engine.search(query, want, {}, tracep);
            if (!res) { out.error = "retrieval failed"; return out; }
            hits = std::move(*res);
        }

        // 3. GraphRAG local expansion: multi-hop over the doc graph, fused with
        //    the base hits so a passage reachable only through a related
        //    document (shared entities) still surfaces. Best-effort.
        if (impl_->cfg.graph) {
            try {
                auto g = impl_->engine.graph_local(query, want);
                if (g && !g->empty()) {
                    // Union by uri+line, keeping the best score.
                    for (auto& gr : *g) {
                        bool dup = false;
                        for (auto& h : hits)
                            if (h.uri == gr.uri && h.start_line == gr.start_line) { dup = true; break; }
                        if (!dup) hits.push_back(std::move(gr));
                    }
                    retriever_mode += "+graph";
                }
            } catch (...) { /* graph optional */ }
        }

        // 4. CRAG corrective grading: a model-free retrieval evaluator that
        //    drops passages graded irrelevant and yields a real confidence in
        //    [0,1]. This turns retrieval from "always inject whatever came
        //    back" into a self-checking step.
        double crag_conf = -1.0;
        if (impl_->cfg.corrective && !hits.empty()) {
            try {
                std::vector<::rag::Hit> raw;
                raw.reserve(hits.size());
                for (const auto& h : hits) raw.push_back(::rag::Hit{h.chunk, h.score});
                auto corr = ::rag::crag::correct(impl_->engine.corpus(), query, raw);
                crag_conf = static_cast<double>(corr.confidence);
                if (!corr.kept.empty()) {
                    // Re-resolve only the kept chunks, preserving CRAG's order.
                    std::vector<::rag::SearchResult> kept;
                    for (const auto& h : corr.kept)
                        kept.push_back(impl_->engine.corpus().resolve(h));
                    if (!kept.empty()) { hits = std::move(kept); retriever_mode += "+crag"; }
                }
            } catch (...) { /* grading optional */ }
        }

        if (hits.empty()) {
            out.error = "no relevant passages (retrieval graded low-confidence)";
            return out;
        }
        if (hits.size() > want) hits.resize(want);

        // 5. Learning-loop read side: nudge each hit by its historical
        //    Beta-smoothed win-rate (bounded ±15%), then re-sort so a passage
        //    that has repeatedly proven useful in THIS workspace edges ahead of
        //    a near-tied one that hasn't. Neutral (×1.0) for unseen paths, so
        //    this can only refine — never invent — ranking. Off with
        //    AGENTTY_RAG_LEARN=0.
        bool learned = false;
        if (impl_->cfg.learn) {
            auto& fb = FeedbackStore::instance();
            for (auto& h : hits) {
                auto [src, path] = split_uri(h.uri);
                (void)src;
                float b = fb.boost(path);
                if (b != 1.0f) {
                    h.score.value *= b;
                    learned = true;
                }
            }
            if (learned)
                std::stable_sort(hits.begin(), hits.end(),
                                 [](const ::rag::SearchResult& a,
                                    const ::rag::SearchResult& b) {
                                     return a.score.value > b.score.value;
                                 });
        }

        double top = 0.0;
        std::vector<std::string> surfaced;
        surfaced.reserve(hits.size());
        for (const auto& r : hits) {
            auto [src, path] = split_uri(r.uri);
            Passage p;
            p.source     = src;
            p.path       = path;
            p.line_start = static_cast<int>(r.start_line);
            p.line_end   = static_cast<int>(r.end_line);
            double s = static_cast<double>(r.score.value);
            if (s < 0.0) s = 0.0;
            if (s > 1.0) s = 1.0;
            p.score      = s;
            p.text       = r.context.empty() ? r.text : (r.context + "\n" + r.text);
            top = std::max(top, p.score);
            surfaced.push_back(path);
            out.passages.push_back(std::move(p));
        }
        // Learning-loop write side (denominator): record what we surfaced so a
        // later `read` of one of these can be scored as a win.
        if (impl_->cfg.learn && !surfaced.empty())
            FeedbackStore::instance().note_surfaced(surfaced);
        // Prefer CRAG's calibrated confidence when available; else top score.
        out.confidence = crag_conf >= 0.0 ? crag_conf : top;

        // Human-readable provenance for the tool-shell header.
        std::string m = retriever_mode;
        m += ", reranked";
        if (impl_->cfg.mmr)    m += "+mmr";
        if (impl_->cfg.stitch) m += "+stitch";
        if (learned)           m += "+learned";
        if (!root.empty() && !skip_docs) m += ", docs=" + root.string();
        char buf[48];
        std::snprintf(buf, sizeof buf, ", confidence %.2f", out.confidence);
        m += buf;
        if (impl_->cfg.trace && !trace.empty()) {
            m += " [";
            for (std::size_t i = 0; i < trace.size(); ++i) {
                if (i) m += " → ";
                m += trace[i];
            }
            m += "]";
        }
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
            impl_->code_engine = ::rag::Engine(impl_->make_engine_config());
            impl_->attach_code_embedder();
            auto files = ::rag::loaders::load_directory(root, opts);
            if (files) {
                for (auto& d : *files) {
                    std::string rel = d.meta.count("rel") ? d.meta["rel"] : d.uri;
                    (void)impl_->code_engine.add("code://" + rel, std::move(d.text), d.meta, d.title);
                }
            }
            (void)impl_->code_engine.build();
            impl_->apply_pipeline(impl_->code_engine);
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

// ── Learning loop (write side) ─────────────
// Both halves delegate to the process-wide FeedbackStore, which owns the TSV
// and the read-side nudge. Best-effort; never throws.
namespace feedback {
void note_surfaced(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    try {
        if (truthy_default_on("AGENTTY_RAG_LEARN"))
            FeedbackStore::instance().note_surfaced(paths);
    } catch (...) { /* best-effort */ }
}
void note_file_opened(const std::string& path) {
    // A `read` of a file a recent passage pointed at is an IMPLICIT relevance
    // judgment. FeedbackStore credits a "win" ONLY if the path was recently
    // surfaced by retrieval — a read of a never-surfaced file is not a signal
    // and is dropped, so the win-rate stays attributable.
    if (path.empty()) return;
    try {
        if (truthy_default_on("AGENTTY_RAG_LEARN"))
            FeedbackStore::instance().note_opened(path);
    } catch (...) { /* best-effort */ }
}
} // namespace feedback

// ── CLI: `agentty rag-bench <root>` ────────────────────────────────────
// A pipeline you can't measure is a pile of vibes. rag-bench evaluates the
// retrieval funnel ON YOUR OWN CORPUS, offline, in milliseconds — no LLM, no
// network, fully deterministic.
//
// Method (known-item retrieval):
//   1. Index the corpus (BM25 + local hash-dense fallback if Ollama is down).
//   2. Sample chunks; for each, synthesize a query from its most
//      DISCRIMINATIVE terms (top BM25 term contributions, via Corpus::explain).
//      The chunk the query was minted from is that query's known GOLD answer.
//   3. Run every query through the retrieval LADDER — one stage added per rung:
//        bm25-only → hybrid+prf → +feature-rerank → +mmr
//      and report recall@k, MRR, and nDCG@10 per rung. Because each rung adds
//      exactly one stage, a metric that DROPS at a rung points at the stage
//      worth tuning — and every AGENTTY_RAG_* knob can be set against numbers.
namespace bench {
namespace {

struct Query {
    std::string text;
    std::uint32_t gold;   // ChunkId value of the source chunk
};

struct Metrics {
    double recall = 0.0, mrr = 0.0, ndcg = 0.0;
    std::size_t n = 0;
    void add(int rank /* 1-based, 0 = miss */, std::size_t k) {
        ++n;
        if (rank >= 1 && static_cast<std::size_t>(rank) <= k) {
            recall += 1.0;
            mrr    += 1.0 / rank;
        }
        // Single relevant doc ⇒ IDCG = 1; DCG = 1/log2(rank+1) when in top-10.
        if (rank >= 1 && rank <= 10)
            ndcg += 1.0 / std::log2(static_cast<double>(rank) + 1.0);
    }
    void finalize() { if (n) { recall /= n; mrr /= n; ndcg /= n; } }
};

// 1-based rank of the gold chunk in a hit list, or 0 if absent.
int rank_of(const std::vector<::rag::Hit>& hits, std::uint32_t gold, std::size_t k) {
    for (std::size_t i = 0; i < hits.size() && i < k; ++i)
        if (hits[i].chunk.value == gold) return static_cast<int>(i) + 1;
    return 0;
}

// Split a chunk's text into distinct alphanumeric terms (deterministic order).
std::vector<std::string> distinct_terms(const std::string& text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::string cur;
    auto flush = [&] {
        if (cur.size() >= 3 && seen.insert(cur).second) out.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        else flush();
    }
    flush();
    return out;
}

} // namespace

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
        bool have_dense = engine.with_embedder_spec(spec).has_value();
        if (!have_dense)
            engine.with_embedder(::rag::dense::AnyEmbedder{::rag::dense::HashEmbedder{256}});

        auto t0 = std::chrono::steady_clock::now();
        ::rag::loaders::DirOptions opts;
        auto docs = ::rag::loaders::load_directory(r, opts);
        std::size_t n = 0;
        if (docs) for (auto& d : *docs) { (void)engine.add(d.uri, std::move(d.text), d.meta, d.title); ++n; }
        (void)engine.build();
        auto t1 = std::chrono::steady_clock::now();
        auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        const auto& corpus = engine.corpus();
        std::size_t nchunks = corpus.chunk_count();
        std::printf("rag-bench: indexed %zu docs, %zu chunks from %s in %lld ms "
                    "(dense: %s)\n",
                    n, nchunks, r.string().c_str(),
                    static_cast<long long>(build_ms),
                    have_dense ? "ollama" : "hash-fallback");
        if (nchunks == 0) { std::fprintf(stderr, "rag-bench: empty corpus\n"); return 1; }

        // 2. Synthesize known-item queries. Sample up to ~200 chunks evenly so a
        //    large corpus still benches in milliseconds and reproducibly.
        const std::size_t kQueryBudget = 200;
        const std::size_t stride = nchunks > kQueryBudget ? nchunks / kQueryBudget : 1;
        std::vector<Query> queries;
        {
            auto lease = corpus.chunks();
            for (std::size_t i = 0; i < lease.size(); i += stride) {
                const auto& ch = lease[i];
                auto terms = distinct_terms(ch.text);
                if (terms.size() < 3) continue;
                // Rank terms by their BM25 contribution to THIS chunk, keep the
                // top few most discriminative — the known-item query.
                std::vector<std::pair<float, std::string>> scored;
                scored.reserve(terms.size());
                for (const auto& t : terms) {
                    auto ex = corpus.explain(t, ch.id);
                    scored.emplace_back(ex.lexical_score, t);
                }
                std::sort(scored.begin(), scored.end(),
                          [](auto& a, auto& b) { return a.first > b.first; });
                std::string q;
                for (std::size_t j = 0; j < scored.size() && j < 5; ++j) {
                    if (scored[j].first <= 0.0f) break;
                    if (!q.empty()) q += ' ';
                    q += scored[j].second;
                }
                if (q.empty()) continue;
                queries.push_back({std::move(q), ch.id.value});
            }
        }
        if (queries.empty()) { std::fprintf(stderr, "rag-bench: no queries synthesized\n"); return 1; }

        // 3. Assemble the ladder — one stage added per rung.
        namespace pl = ::rag::pipeline;
        auto hybrid_cfg = [&](bool dense) {
            pl::HybridRetrieveConfig hy;
            hy.candidate_k  = 60;
            hy.bm25_weight  = 1.0f;
            hy.dense_weight = dense ? 1.0f : 0.0f;
            // convex fusion IGNORES the per-retriever weights (it uses
            // convex.alpha), so to actually isolate BM25 for the first rung we
            // fall back to weighted RRF, which honours dense_weight=0.
            hy.fusion = dense ? pl::HybridRetrieveConfig::Fusion::convex
                              : pl::HybridRetrieveConfig::Fusion::rrf;
            return hy;
        };
        struct Rung { const char* name; pl::Pipeline pipe; };
        std::vector<Rung> ladder;
        {   // bm25-only
            pl::Pipeline p;
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(false)));
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"bm25-only", std::move(p)});
        }
        {   // hybrid + prf
            pl::Pipeline p;
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(true)));
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"hybrid+prf", std::move(p)});
        }
        {   // + feature-rerank
            pl::Pipeline p;
            p.add(std::make_shared<pl::PrfExpandStage>(pl::ExpandConfig{}));
            p.add(std::make_shared<pl::HybridRetrieveStage>(hybrid_cfg(true)));
            p.add(pl::make_feature_rerank_stage());
            p.add(std::make_shared<pl::TopKStage>());
            ladder.push_back({"+feature-rerank", std::move(p)});
        }
        {   // + mmr (the full quality funnel, PRF-prepended)
            pl::Pipeline p = pl::Pipeline::quality_with(hybrid_cfg(true), cfg.mmr_lambda);
            ladder.push_back({"+mmr", std::move(p)});
        }

        const std::size_t k = 10;
        std::printf("\n  %-16s  recall@%zu   MRR    nDCG@10   ms/query\n",
                    "stage", k);
        std::printf("  %-16s  --------  ------  -------  --------\n", "----------------");
        for (auto& rung : ladder) {
            Metrics m;
            auto s0 = std::chrono::steady_clock::now();
            for (const auto& q : queries) {
                auto hits = rung.pipe.run(corpus, q.text, k);
                if (!hits) { m.add(0, k); continue; }
                m.add(rank_of(*hits, q.gold, k), k);
            }
            auto s1 = std::chrono::steady_clock::now();
            double per = std::chrono::duration_cast<std::chrono::microseconds>(s1 - s0).count()
                         / 1000.0 / static_cast<double>(queries.size());
            m.finalize();
            std::printf("  %-16s   %.3f   %.3f   %.3f     %6.2f\n",
                        rung.name, m.recall, m.mrr, m.ndcg, per);
        }
        std::printf("\n  %zu known-item queries over %zu chunks. "
                    "A metric that DROPS at a rung is the stage to tune.\n",
                    queries.size(), nchunks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rag-bench: %s\n", e.what());
        return 1;
    }
}
} // namespace bench

} // namespace agentty::rag

#else  // !AGENTTY_HAS_RAGCPP  ─────────────────────────────────────────────

// Retrieval unavailable on this platform (e.g. MSVC, until rag-cpp is
// Windows-portable). Every public symbol from rag_adapter.hpp is defined here
// as a no-op so agentty links and runs; search_docs/search_code surface a
// clear "unavailable" message instead of crashing or returning stale data.

#include <cstdio>
#include <cstdlib>

namespace agentty::rag {

Config Config::from_env() {
    // A default-constructed Config is fine; nothing reads it in the no-op build,
    // but tests and callers may still construct one.
    return Config{};
}

// Impl is empty — the header stores an opaque pointer we simply leave null.
struct Retriever::Impl {};

Retriever::Retriever() : impl_(nullptr) {}
Retriever::~Retriever() { delete impl_; }

void Retriever::set_generator(Generator /*g*/) {}

static Retrieval unavailable_() {
    Retrieval r;
    r.mode  = "retrieval unavailable on this platform";
    r.error = "retrieval engine (rag-cpp) is not built on this platform";
    r.confidence = 0.0;
    return r;
}

Retrieval Retriever::retrieve(const std::string& /*query*/, int /*k*/,
                              bool /*skip_docs*/) {
    return unavailable_();
}

Retrieval Retriever::retrieve_code(const std::string& /*query*/, int /*k*/) {
    return unavailable_();
}

bool Retriever::warm() const { return true; }  // nothing to build → always warm
void Retriever::warm_async() {}

namespace feedback {
void note_surfaced(const std::vector<std::string>& /*paths*/) {}
void note_file_opened(const std::string& /*path*/) {}
}  // namespace feedback

namespace bench {
int run(const std::string& /*root*/) {
    std::fprintf(stderr,
                 "rag-bench: the retrieval engine is not built on this "
                 "platform (rag-cpp is not yet Windows-portable).\n");
    return 2;
}
}  // namespace bench

}  // namespace agentty::rag

#endif  // AGENTTY_HAS_RAGCPP
