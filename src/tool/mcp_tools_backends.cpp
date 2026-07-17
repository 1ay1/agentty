// mcp_tools_backends.cpp — the agentty-side HostServices backends the
// mcp-cpp toolset's host-coupled SHELLS dispatch into.
//
//   mcp-cpp owns the protocol surface for remember/forget/wipe, todo,
//   skill, search_docs, and task — names, schemas, arg parsing, scope
//   validation, dedup/dry-run messaging, formatting. But the DATA and the
//   WORK for those tools live in agentty: the JSONL memory store, the
//   Agent-Skills engine, the RAG pipeline, the subagent loop. This file is
//   the inversion-of-control seam: each backend is a small class deriving
//   an `mcp::tools::*` interface and delegating to agentty's existing
//   subsystem, with the EXACT arg→backend mapping, scope vocabulary, and
//   output formatting the native tool bodies had.
//
//   Built + installed by build_mcp_tool_defs() (mcp_tools_bridge.cpp).

#include "agentty/tool/mcp_tools_backends.hpp"

#include "agentty/tool/memory_store.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/registry.hpp"   // tools::progress::emit
#include "agentty/tool/tool.hpp"       // tool::DynamicDispatch, ToolUse, Message …
#include "agentty/tool/util/partial_json.hpp"   // args salvage for truncated tool JSON

#include "agentty/provider/anthropic/provider.hpp"
#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/selection.hpp"

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"
#include "agentty/rag/expand.hpp"
#include "agentty/rag/knowledge.hpp"

#include "agentty/mcp/client.hpp"   // mcp_resources / mcp_read_resource seams
#include "agentty/util/dbglog.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

namespace {

namespace mt  = ::mcp::tools;
namespace fs  = std::filesystem;
using json    = nlohmann::json;

// ── MemoryStore ────────────────────────────────────────────────────────
//   Backs remember / forget / wipe. The shell owns the schema + dedup/pin/
//   tag/supersede surface; this maps its requests onto agentty::tools::
//   memory free functions. Scope vocabulary is ["project","user"] so the
//   shell defaults to project (the safer default, matching the native
//   remember tool) and accepts "user" for cross-project facts.
// Drift guard for the parallel result structs bridged in append() below.
// agentty::tools::memory::AppendResult and mcp::tools::MemoryAppendResult carry
// identical fields in identical order; the bridge copies them one by one. If a
// field is added/removed on either side their sizes diverge and this trips at
// compile time — a reminder to update BOTH structs and the mapping. (The
// designated-init mapping below separately guards renames/removals.)
static_assert(sizeof(memory::AppendResult) == sizeof(mt::MemoryAppendResult),
              "AppendResult / MemoryAppendResult drifted — update both structs "
              "and the field mapping in AgenttyMemoryStore::append()");

class AgenttyMemoryStore final : public mt::MemoryStore {
public:
    std::vector<std::string> scopes() const override {
        return {"project", "user"};   // scopes()[0] == default
    }

    mt::MemoryAppendResult append(const mt::MemoryAppendRequest& req) override {
        auto scope = memory::parse_scope(req.scope);
        if (!scope)
            return mt::MemoryAppendResult{.error = "unknown scope '" + req.scope + "'"};

        memory::AppendOptions opts;
        opts.pinned        = req.pinned;
        opts.tags          = req.tags;
        opts.supersedes_id = req.supersedes_id;
        const auto r = memory::append(*scope, req.text, opts);

        // Bridge the agentty result onto the mcp result. Named designated
        // initialisers so a renamed/removed field is a compile error rather
        // than a silent miscopy; the static_assert above catches size drift.
        return mt::MemoryAppendResult{
            .id      = r.id,
            .error   = r.error,
            .note    = r.note,
            .rolled  = r.rolled,
            .deduped = r.deduped,
        };
    }

    std::size_t forget_by_id(const std::string& id) override {
        return memory::forget_by_id(id);
    }
    std::size_t forget_by_substring(const std::string& needle) override {
        return memory::forget_by_substring(needle);
    }
    std::vector<mt::MemoryRecord> preview_forget(const std::string& needle) override {
        std::vector<mt::MemoryRecord> out;
        // The wipe shell calls preview_forget("") to enumerate a scope; the
        // native forget refused an empty needle. Treat empty as "no preview"
        // (the wipe shell only uses the call's existence, not its contents).
        if (needle.empty()) return out;
        for (const auto& r : memory::preview_forget_by_substring(needle)) {
            mt::MemoryRecord m;
            m.id     = r.id;
            m.text   = r.text;
            m.scope  = std::string{memory::to_string(r.scope)};
            m.pinned = r.pinned;
            m.tags   = r.tags;
            m.ts     = r.ts;
            m.hits   = r.hits;
            out.push_back(std::move(m));
        }
        return out;
    }

    std::optional<std::size_t> wipe(const std::string& scope) override {
        auto s = memory::parse_scope(scope);
        if (!s) return std::nullopt;
        return memory::wipe(*s);
    }
};

// ── SkillResolver ──────────────────────────────────────────────────────
//   Backs the skill tool. The shell returns whatever string load() yields
//   verbatim, so load() returns the FULL activation payload (body wrapped
//   in <skill_content>, the absolute skill dir, the <skill_resources>
//   listing) — identical to the native tool — and applies the same
//   re-activation dedup (spec §5: don't re-inject a body already in
//   context). On an unknown name it leaves the body empty and fills `err`
//   with the available-skills recovery hint.
class AgenttySkillResolver final : public mt::SkillResolver {
public:
    std::optional<std::string> load(const std::string& name, std::string& err) override {
        const auto* s = skills::find(name);
        if (!s) {
            std::ostringstream avail;
            bool first = true;
            for (const auto& sk : skills::all()) {
                avail << (first ? "" : ", ") << sk.name;
                first = false;
            }
            err = "no skill named '" + name + "'";
            if (!first) err += " — available: " + avail.str();
            else        err += " — no skills are installed in this workspace";
            return std::nullopt;
        }
        if (!skills::note_activated(s->name)) {
            return "Skill '" + s->name + "' is already active in this "
                   "session — its instructions are in an earlier tool_result. "
                   "Refer to that instead of re-loading.";
        }
        return skills::activation_payload(*s);
    }
};

// ── DocRetriever ───────────────────────────────────────────────
//   Backs search_docs. Runs agentty's full SOTA RAG pipeline and returns
//   flat passages. The funnel (2026 canonical shape):
//
//     sources:  docs folder (contextual hybrid BM25+dense+RRF, HNSW)
//               ∪ skills (agentskills.io bodies, BM25, lazy)   [default ON]
//               ∪ memory (learned facts, BM25, lazy)           [default ON]
//               ∪ MCP resources                                [opt-in]
//     query:    optional RAG-Fusion expansion (opt-in, needs local LLM)
//     retrieve: WIDE pool (k*5, ≥30) fan-out + RRF fusion
//     rerank:   feature-fusion lexical rerank → k*3
//               → optional neural (cross-encoder-style) rerank → k*2
//     diversify: MMR (λ=0.75) → k
//     compress: extractive query-relevant span per chunk
//
//   The `mode` string carries the rich provenance (root path, mode,
//   reranked, +N variants, confidence) so no signal is lost when the
//   shell renders the result.
class AgenttyDocRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        std::vector<mt::DocPassage> out;
        try {
            const rag::EmbedConfig embed = embed_config_from_env();

            auto root = resolve_docs_root();
            std::string root_str = root.string();

            static std::mutex mu;
            static rag::Corpus corpus;
            static std::string indexed_root;

            std::lock_guard<std::mutex> lock(mu);
            if (!root.empty() && indexed_root != root_str) {
                corpus.build(root, embed);
                indexed_root = root_str;
                ++corpus_epoch_;   // #5: invalidate the per-turn query cache
            }

            rag::CorpusSource docs_source("docs", corpus, embed);
            const bool have_docs = !root.empty() && corpus.chunk_count() > 0;

            // OPT-IN: this session's MCP `resources/*` (shared by both funnel
            // passes below). Built once here so the corrective retry reuses it.
            std::shared_ptr<rag::McpResourceSource> mcp_source;
            if (mcp_resources_enabled())
                mcp_source = mcp_resource_source(embed);

            // "No knowledge configured" guard: at least one source must exist.
            const bool any_source = have_docs
                || (skills_rag_enabled() && skills_source())
                || (memory_rag_enabled() && memory_source())
                || static_cast<bool>(mcp_source);
            if (!any_source) {
                err = "no knowledge configured. Set AGENTTY_DOCS_DIR to a "
                      "folder of documents (markdown/text/etc.), create "
                      "./docs, install skills, or store memories to give "
                      "search_docs something to retrieve from.";
                return out;
            }

            const std::size_t k = static_cast<std::size_t>(q.k);
            const std::size_t pool_k = std::max<std::size_t>(k * 5, 30);

            // ── #5 PER-TURN QUERY CACHE ────────────────────────────────
            // A pipeline pass (expand + fan-out + rerank + MMR + compress)
            // is not free — and an agent frequently re-issues the same
            // search_docs query inside one ReAct turn (retry after a failed
            // edit, re-check a fact). Cache the full result keyed by
            // (query, k, corpus-shape). Cleared whenever the indexed root
            // changes shape (corpus rebuild bumps corpus_epoch). Bounded.
            const std::string cache_key =
                q.query + '\x1f' + std::to_string(k) + '\x1f' + root_str;
            {
                std::lock_guard<std::mutex> clk(cache_mu_);
                if (cache_epoch_ != corpus_epoch_) { query_cache_.clear();
                                                     cache_epoch_ = corpus_epoch_; }
                if (auto it = query_cache_.find(cache_key); it != query_cache_.end()) {
                    mode = it->second.mode + ", cached";
                    return it->second.passages;
                }
            }

            // ── #2 CORRECTIVE RETRIEVAL (CRAG) ─────────────────────────
            // Run the full funnel; if confidence comes back LOW, don't just
            // report it — ACT: widen the candidate pool and retry once with
            // a de-noised query (stopword/punct-stripped keywords), then keep
            // whichever attempt scored higher. Turns the confidence signal
            // from advisory into corrective. Disable with AGENTTY_RAG_CORRECT=0.
            std::size_t variant_count = 0;
            const bool neural = neural_rerank_enabled();
            const bool correct = corrective_enabled();
            constexpr double kLowConf = 0.25;

            rag::Context ctx = run_funnel_(q.query, k, pool_k, neural, have_docs,
                                           docs_source, mcp_source,
                                           embed, variant_count);

            bool corrected = false;
            if (correct && ctx.confidence < kLowConf) {
                std::string reduced = distill_query_(q.query);
                if (!reduced.empty() && reduced != q.query) {
                    std::size_t vc2 = 0;
                    const std::size_t wide_pool =
                        std::max<std::size_t>(pool_k * 2, 60);
                    rag::Context alt = run_funnel_(reduced, k, wide_pool, neural,
                                                   have_docs, docs_source,
                                                   mcp_source, embed, vc2);
                    if (alt.confidence > ctx.confidence) {
                        ctx = std::move(alt);
                        variant_count = vc2;
                        corrected = true;
                    }
                }
            }

            std::string mode_str =
                (have_docs && corpus.has_embeddings()) ? "hybrid+ctx" : "BM25-only";
            mode_str += neural ? ", neural-reranked" : ", reranked";
            if (variant_count > 0)
                mode_str += ", +" + std::to_string(variant_count) + " query variants";
            if (corrected) mode_str += ", corrected";
            // Retrieval confidence — the agentic-RAG signal: LOW tells the
            // model to fall back to grep/read instead of trusting weak hits.
            {
                char buf[32];
                std::snprintf(buf, sizeof buf, ", confidence %.2f", ctx.confidence);
                mode_str += buf;
                if (ctx.confidence < kLowConf)
                    mode_str += " (LOW \xe2\x80\x94 treat results as leads, verify "
                                "with grep/read)";
            }
            if (have_docs) mode_str += " from " + root_str;
            mode = std::move(mode_str);

            for (const auto& c : ctx.chunks) {
                if (!c.hit.chunk) continue;
                mt::DocPassage p;
                p.source = c.hit.source ? std::string{c.hit.source->name()}
                                        : std::string{"docs"};
                p.path       = c.hit.chunk->path;
                p.line_start = c.hit.chunk->line_start;
                p.line_end   = c.hit.chunk->line_end;
                p.score      = c.hit.score;
                p.text       = std::string{c.text()};
                out.push_back(std::move(p));
            }

            // #5: memoize this turn's result (bounded LRU-ish: clear when full).
            {
                std::lock_guard<std::mutex> clk(cache_mu_);
                if (query_cache_.size() >= kCacheMax) query_cache_.clear();
                query_cache_.emplace(cache_key, CacheEntry{out, mode});
            }
            return out;
        } catch (const std::exception& e) {
            err = std::string{"search_docs failed: "} + e.what();
            return {};
        } catch (...) {
            err = "search_docs failed";
            return {};
        }
    }

private:
    // ── #5 query-cache state ──────────────────────────────────────
    struct CacheEntry { std::vector<mt::DocPassage> passages; std::string mode; };
    static constexpr std::size_t kCacheMax = 64;
    static inline std::mutex cache_mu_;
    static inline std::unordered_map<std::string, CacheEntry> query_cache_;
    static inline unsigned long corpus_epoch_ = 0;   // bumped on every rebuild
    static inline unsigned long cache_epoch_  = ~0UL; // cache's view of the corpus

    static bool corrective_enabled() { return truthy_default_on("AGENTTY_RAG_CORRECT"); }

    // Run the full retrieval funnel for one query and return the ranked,
    // confidence-scored Context. Shared by the first pass and the CRAG
    // retry so both paths are identical modulo query + pool width.
    static rag::Context run_funnel_(
        const std::string& query, std::size_t k, std::size_t pool_k,
        bool neural, bool have_docs,
        rag::CorpusSource& docs_source,
        const std::shared_ptr<rag::McpResourceSource>& mcp_source,
        const rag::EmbedConfig& embed, std::size_t& variant_count) {

        rag::KnowledgeRouter router;
        if (have_docs)
            router.add(std::shared_ptr<rag::KnowledgeSource>(
                &docs_source, [](rag::KnowledgeSource*) {}));
        if (skills_rag_enabled())
            if (auto s = skills_source()) router.add(std::move(s));
        if (memory_rag_enabled())
            if (auto s = memory_source()) router.add(std::move(s));
        if (mcp_source)
            router.add(std::shared_ptr<rag::KnowledgeSource>(
                mcp_source, mcp_source.get()));

        rag::Context ctx;
        ctx.query = query;
        if (expand_enabled() && have_docs) {
            rag::ExpandConfig ecfg = expand_config_from_env(embed);
            auto queries = rag::expand_query(ecfg, query);
            if (queries.size() > 1) variant_count = queries.size() - 1;
            auto fused = docs_source.retrieve_fused(queries, pool_k);
            rag::KnowledgeRouter rest;
            if (skills_rag_enabled())
                if (auto s = skills_source()) rest.add(std::move(s));
            if (memory_rag_enabled())
                if (auto s = memory_source()) rest.add(std::move(s));
            if (mcp_source)
                rest.add(std::shared_ptr<rag::KnowledgeSource>(
                    mcp_source, mcp_source.get()));
            if (rest.source_count() > 0) {
                auto extra = rest.retrieve(query, pool_k);
                fused.insert(fused.end(), extra.begin(), extra.end());
            }
            ctx = rag::Context::from_hits(query, std::move(fused));
        } else {
            ctx = rag::Context::from_hits(query, router.retrieve(query, pool_k));
        }

        rag::Pipeline pipe;
        pipe.add(std::make_shared<rag::RerankStage>(
            neural ? std::max<std::size_t>(k * 3, 12)
                   : std::max<std::size_t>(k * 2, 8)));
        if (neural)
            pipe.add(std::make_shared<rag::NeuralRerankStage>(
                std::max<std::size_t>(k * 2, 8), neural_config_from_env(embed)));
        pipe.add(std::make_shared<rag::MMRStage>(k, /*lambda=*/0.75))
            .add(std::make_shared<rag::CompressStage>(/*target_chars=*/600));
        ctx = pipe.run(std::move(ctx));
        ctx.compute_confidence();
        return ctx;
    }

    // De-noise a query for the CRAG retry: lowercase, strip punctuation to
    // spaces, drop a small English stopword set, keep tokens ≥3 chars. The
    // retry probes with the content words only — recovering hits when the
    // original query was buried in conversational phrasing ("can you tell me
    // how the foo widget handles bar" → "foo widget handles bar").
    static std::string distill_query_(const std::string& q) {
        static const std::unordered_set<std::string> stop = {
            "the","a","an","of","to","in","on","for","and","or","is","are",
            "was","were","be","do","does","did","how","what","why","when",
            "where","which","who","can","you","me","i","we","it","this",
            "that","with","about","tell","show","please","my","our","your"};
        std::string cur, out;
        auto flush = [&] {
            if (cur.size() >= 3 && !stop.count(cur)) {
                if (!out.empty()) out += ' ';
                out += cur;
            }
            cur.clear();
        };
        for (char c : q) {
            if (std::isalnum(static_cast<unsigned char>(c)))
                cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            else flush();
        }
        flush();
        return out;
    }

    static fs::path resolve_docs_root() {
        if (const char* env = std::getenv("AGENTTY_DOCS_DIR"))
            if (env[0] != '\0') return fs::path{env};
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) return {};
        auto docs = cwd / "docs";
        if (fs::is_directory(docs, ec)) return docs;
        auto kb = cwd / ".agentty" / "knowledge";
        if (fs::is_directory(kb, ec)) return kb;
        return {};
    }

    static rag::EmbedConfig embed_config_from_env() {
        rag::EmbedConfig cfg;
        if (const char* m = std::getenv("AGENTTY_EMBED_MODEL"); m && m[0] != '\0')
            cfg.model = m;
        else
            cfg.model = "nomic-embed-text";
        if (const char* h = std::getenv("AGENTTY_OLLAMA_HOST"); h && h[0] != '\0') {
            std::string hs{h};
            if (auto colon = hs.rfind(':'); colon != std::string::npos) {
                cfg.host = hs.substr(0, colon);
                try {
                    int p = std::stoi(hs.substr(colon + 1));
                    if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
                } catch (const std::exception& e) { ::agentty::util::dbglog("rag.embed_endpoint.port", e.what()); /* keep default port */ }
                catch (...) { ::agentty::util::dbglog("rag.embed_endpoint.port", "non-std exception"); }
            } else {
                cfg.host = hs;
            }
        }
        return cfg;
    }

    static bool expand_enabled() {
        const char* v = std::getenv("AGENTTY_RAG_EXPAND");
        if (!v || v[0] == '\0') return false;
        std::string s{v};
        return s != "0" && s != "false" && s != "FALSE" && s != "False";
    }

    // Default-ON toggles (set =0 to disable): skills + memory as fused
    // knowledge sources. Both are tiny, local, BM25-only — zero network,
    // sub-ms — so on-by-default is safe; the env vars exist for A/B.
    static bool truthy_default_on(const char* var) {
        const char* v = std::getenv(var);
        if (!v || v[0] == '\0') return true;
        std::string s{v};
        return s != "0" && s != "false" && s != "FALSE" && s != "False";
    }
    static bool skills_rag_enabled() { return truthy_default_on("AGENTTY_RAG_SKILLS"); }
    static bool memory_rag_enabled() { return truthy_default_on("AGENTTY_RAG_MEMORY"); }

    // Neural (cross-encoder-style) rerank via a local Ollama generative
    // model. OPT-IN: one LLM call per candidate chunk.
    static bool neural_rerank_enabled() {
        const char* v = std::getenv("AGENTTY_RAG_NEURAL");
        if (!v || v[0] == '\0') return false;
        std::string s{v};
        return s != "0" && s != "false" && s != "FALSE" && s != "False";
    }
    static rag::NeuralRerankConfig neural_config_from_env(const rag::EmbedConfig& embed) {
        rag::NeuralRerankConfig cfg;
        cfg.host = embed.host;
        cfg.port = embed.port;
        if (const char* m = std::getenv("AGENTTY_RAG_NEURAL_MODEL"); m && m[0])
            cfg.model = m;
        else
            cfg.model = "llama3.2";
        return cfg;
    }

    // ── Skills as a knowledge source ──────────────────────────────
    // Every installed SKILL.md body, chunked + BM25-indexed in a private
    // in-memory corpus. A hit tells the model WHICH skill covers the topic
    // (provenance path = "skill://<name>/SKILL.md") — it can then activate
    // the skill with the `skill` tool. Lazy: built on the first search_docs
    // call, rebuilt only when the discovered skill set changes shape.
    static std::shared_ptr<rag::KnowledgeSource> skills_source() {
        class SkillsSource final : public rag::KnowledgeSource {
        public:
            std::string_view name() const noexcept override { return "skills"; }
            std::vector<rag::Hit>
            retrieve(std::string_view query, std::size_t k) const override {
                try {
                    ensure_built_();
                    if (corpus_.chunk_count() == 0) return {};
                    auto hits = corpus_.search(query, {}, k);   // BM25-only
                    for (auto& h : hits) h.source = this;
                    return hits;
                } catch (...) { return {}; }
            }
        private:
            void ensure_built_() const {
                const auto& all = skills::all();
                // Cheap shape key: rebuild when skills appear/disappear.
                std::size_t key = all.size();
                for (const auto& s : all) key ^= std::hash<std::string>{}(s.name);
                if (built_ && key == built_key_) return;
                built_ = true;
                built_key_ = key;
                std::vector<std::pair<std::string, std::string>> docs;
                docs.reserve(all.size());
                for (const auto& s : all) {
                    if (s.body.empty()) continue;
                    // Description leads the doc so the skill's own summary
                    // vocabulary is retrievable even when the body is terse.
                    std::string body = s.description;
                    if (!body.empty()) body += "\n\n";
                    body += s.body;
                    docs.emplace_back("skill://" + s.name + "/SKILL.md",
                                      std::move(body));
                }
                corpus_.build_from_memory(docs, {});   // BM25-only
            }
            mutable rag::Corpus corpus_;
            mutable bool        built_ = false;
            mutable std::size_t built_key_ = 0;
        };
        static auto src = std::make_shared<SkillsSource>();
        return src;
    }

    // ── Learned memory as a knowledge source ────────────────────────
    // Both memory scopes (user + project) as one BM25 corpus, one doc per
    // record (path = "memory://<scope>/<id>"). The system prompt only
    // carries the tail-N/6 KiB slice of memory; retrieval reaches ALL 200
    // records per scope — facts that rolled out of the prompt stay
    // findable. Rebuilt whenever the store's record-count/id shape moves
    // (remember/forget touch it rarely; rebuild is sub-ms on ≤400 records).
    static std::shared_ptr<rag::KnowledgeSource> memory_source() {
        class MemorySource final : public rag::KnowledgeSource {
        public:
            std::string_view name() const noexcept override { return "memory"; }
            std::vector<rag::Hit>
            retrieve(std::string_view query, std::size_t k) const override {
                try {
                    ensure_built_();
                    if (corpus_.chunk_count() == 0) return {};
                    auto hits = corpus_.search(query, {}, k);   // BM25-only
                    for (auto& h : hits) h.source = this;
                    return hits;
                } catch (...) { return {}; }
            }
        private:
            void ensure_built_() const {
                auto user = memory::load_all(memory::Scope::User);
                auto proj = memory::load_all(memory::Scope::Project);
                std::size_t key = user.size() * 31 + proj.size();
                for (const auto& r : user) key ^= std::hash<std::string>{}(r.id);
                for (const auto& r : proj) key ^= std::hash<std::string>{}(r.id);
                if (built_ && key == built_key_) return;
                built_ = true;
                built_key_ = key;
                std::vector<std::pair<std::string, std::string>> docs;
                docs.reserve(user.size() + proj.size());
                auto add = [&](const memory::Record& r) {
                    if (r.text.empty()) return;
                    std::string body = r.text;
                    for (const auto& t : r.tags) { body += ' '; body += t; }
                    docs.emplace_back(
                        "memory://" + std::string{memory::to_string(r.scope)}
                            + "/" + r.id,
                        std::move(body));
                };
                for (const auto& r : user) add(r);
                for (const auto& r : proj) add(r);
                corpus_.build_from_memory(docs, {});   // BM25-only
            }
            mutable rag::Corpus corpus_;
            mutable bool        built_ = false;
            mutable std::size_t built_key_ = 0;
        };
        static auto src = std::make_shared<MemorySource>();
        return src;
    }

    // OPT-IN: fold MCP `resources/*` into the search_docs corpus. Off unless
    // AGENTTY_RAG_MCP is truthy AND an MCP config is present (cheap probe).
    static bool mcp_resources_enabled() {
        const char* v = std::getenv("AGENTTY_RAG_MCP");
        if (!v || v[0] == '\0') return false;
        std::string s{v};
        if (s == "0" || s == "false" || s == "FALSE" || s == "False") return false;
        return mcp::mcp_config_present();
    }

    // Process-wide, lazily-built MCP resource source. The list/read seams are
    // bound to the mcp:: free functions; the RAG layer itself stays MCP-free.
    // Rebuilt when the server's tool/resource generation changes.
    static std::shared_ptr<rag::McpResourceSource>
    mcp_resource_source(const rag::EmbedConfig& embed) {
        static std::mutex mu;
        static std::shared_ptr<rag::McpResourceSource> src;
        static unsigned long built_gen = ~0UL;

        std::lock_guard<std::mutex> lock(mu);
        unsigned long gen = mcp::mcp_generation();
        if (!src) {
            auto list_fn = [] {
                std::vector<rag::McpResourceSource::ResourceRef> refs;
                for (auto& r : mcp::mcp_resources()) {
                    std::string label = !r.title.empty() ? r.title
                                      : !r.name.empty()  ? r.name : r.uri;
                    refs.push_back({r.uri, std::move(label)});
                }
                return refs;
            };
            auto read_fn = [](const std::string& uri) -> std::optional<std::string> {
                std::string err;
                return mcp::mcp_read_resource(uri, err);
            };
            src = std::make_shared<rag::McpResourceSource>(
                "mcp", std::move(list_fn), std::move(read_fn), embed);
            built_gen = gen;
        } else if (gen != built_gen) {
            src->refresh();          // resource set changed → reindex on next use
            built_gen = gen;
        }
        return src;
    }

    static rag::ExpandConfig expand_config_from_env(const rag::EmbedConfig& embed) {
        rag::ExpandConfig cfg;
        cfg.host = embed.host;
        cfg.port = embed.port;
        if (const char* m = std::getenv("AGENTTY_RAG_EXPAND_MODEL"); m && m[0])
            cfg.model = m;
        else if (const char* gm = std::getenv("AGENTTY_MODEL"); gm && gm[0])
            cfg.model = gm;
        else
            cfg.model = "llama3.2";
        cfg.n = 4;
        if (const char* ns = std::getenv("AGENTTY_RAG_EXPAND_N"); ns && ns[0]) {
            try {
                int n = std::stoi(ns);
                if (n < 1) n = 1;
                if (n > 8) n = 8;
                cfg.n = static_cast<std::size_t>(n);
            } catch (const std::exception& e) { ::agentty::util::dbglog("rag.expand_n.env", e.what()); /* keep default */ }
            catch (...) { ::agentty::util::dbglog("rag.expand_n.env", "non-std exception"); }
        }
        return cfg;
    }
};

// ── CodeRetriever ──────────────────────────────────────────
//   Backs search_code — SEMANTIC retrieval over source files, the hybrid
//   complement to grep (2025 consensus: agentic grep for exact/structural,
//   embeddings for conceptual — Cursor/Sourcegraph ship both; agentty's own
//   rag.hpp design note reserves doc-RAG for documents and this class fills
//   the code half). BM25 over identifier-tokenized chunks is always on;
//   dense embeddings join when Ollama is reachable. Edit-invalidated: a
//   cheap (size,mtime) fingerprint over the walked tree is recomputed per
//   call and any drift rebuilds the index — embeddings can go stale between
//   rebuilds, BM25 cannot lie for long. Bounded walk: skips VCS/build/dep
//   dirs, binary files, >256 KiB files, and caps the corpus at 4000 files.
class AgenttyCodeRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        std::vector<mt::DocPassage> out;
        try {
            const rag::EmbedConfig embed = embed_config_from_env_();
            std::error_code ec;
            auto root = fs::current_path(ec);
            if (ec) { err = "search_code: cannot resolve cwd"; return out; }

            static std::mutex mu;
            static rag::Corpus corpus;
            static std::uint64_t fingerprint = 0;

            std::lock_guard<std::mutex> lock(mu);

            // Walk + fingerprint. FNV-1a over (path, size, mtime) of every
            // indexable file — any edit/add/delete flips it.
            std::vector<std::pair<std::string, fs::path>> files;
            std::uint64_t fp = 1469598103934665603ULL;
            auto mix = [&fp](std::uint64_t v) { fp = (fp ^ v) * 1099511628211ULL; };
            walk_(root, root, files, mix);

            if (files.empty()) {
                err = "search_code: no source files found under " + root.string();
                return out;
            }

            if (fp != fingerprint || corpus.chunk_count() == 0) {
                std::vector<std::pair<std::string, std::string>> docs;
                docs.reserve(files.size());
                for (const auto& [rel, abs] : files) {
                    std::ifstream in(abs, std::ios::binary);
                    if (!in) continue;
                    std::string body((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                    if (body.empty()) continue;
                    docs.emplace_back(rel, std::move(body));
                }
                corpus.build_from_memory(docs, embed);
                fingerprint = fp;
            }

            const std::size_t k = static_cast<std::size_t>(q.k);
            auto hits = corpus.search(q.query, embed,
                                      std::max<std::size_t>(k * 3, 12));

            // Light MMR-free cut: corpus.search already RRF-fused; take top-k.
            if (hits.size() > k) hits.resize(k);

            mode = corpus.has_embeddings() ? "hybrid" : "BM25-only";
            mode += ", " + std::to_string(corpus.chunk_count()) + " chunks from "
                  + root.string();

            for (const auto& h : hits) {
                if (!h.chunk) continue;
                mt::DocPassage p;
                p.source     = "code";
                p.path       = h.chunk->path;
                p.line_start = h.chunk->line_start;
                p.line_end   = h.chunk->line_end;
                p.score      = h.score;
                p.text       = h.chunk->text;
                out.push_back(std::move(p));
            }
            return out;
        } catch (const std::exception& e) {
            err = std::string{"search_code failed: "} + e.what();
            return {};
        } catch (...) {
            err = "search_code failed";
            return {};
        }
    }

private:
    static rag::EmbedConfig embed_config_from_env_() {
        // Same resolution as the docs retriever (model + host env vars).
        rag::EmbedConfig cfg;
        if (const char* m = std::getenv("AGENTTY_EMBED_MODEL"); m && m[0]) cfg.model = m;
        else cfg.model = "nomic-embed-text";
        if (const char* h = std::getenv("AGENTTY_OLLAMA_HOST"); h && h[0]) {
            std::string hs{h};
            if (auto colon = hs.rfind(':'); colon != std::string::npos) {
                cfg.host = hs.substr(0, colon);
                try {
                    int p = std::stoi(hs.substr(colon + 1));
                    if (p > 0 && p <= 65535) cfg.port = static_cast<std::uint16_t>(p);
                } catch (...) { /* keep default */ }
            } else cfg.host = hs;
        }
        return cfg;
    }

    static bool source_ext_(const fs::path& p) {
        auto ext = p.extension().string();
        for (auto& c : ext) c = static_cast<char>(std::tolower((unsigned char)c));
        static const char* kExts[] = {
            ".c",".cc",".cpp",".cxx",".h",".hh",".hpp",".hxx",".inl",
            ".py",".js",".jsx",".ts",".tsx",".mjs",".go",".rs",".java",
            ".kt",".swift",".rb",".php",".cs",".scala",".sh",".bash",
            ".zig",".lua",".pl",".el",".ex",".exs",".erl",".hs",".ml",
            ".sql",".proto",".cmake",".gradle"};
        for (const char* e : kExts) if (ext == e) return true;
        auto name = p.filename().string();
        return name == "CMakeLists.txt" || name == "Makefile";
    }

    static bool skip_dir_(const std::string& name) {
        static const char* kSkip[] = {
            ".git",".hg",".svn","node_modules","build","dist","out",
            "target","venv",".venv","__pycache__",".cache","_deps",
            "CMakeFiles",".agentty","vendor","third_party"};
        for (const char* s : kSkip) if (name == s) return true;
        return !name.empty() && name[0] == '.';
    }

    template <class MixFn>
    static void walk_(const fs::path& root, const fs::path& dir,
                      std::vector<std::pair<std::string, fs::path>>& files,
                      MixFn&& mix) {
        constexpr std::size_t kMaxFiles     = 4000;
        constexpr std::uintmax_t kMaxBytes  = 256 * 1024;
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; it != end && files.size() < kMaxFiles;
             it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            auto name = entry.path().filename().string();
            if (entry.is_directory(ec)) {
                if (!skip_dir_(name)) walk_(root, entry.path(), files, mix);
                continue;
            }
            if (!entry.is_regular_file(ec) || !source_ext_(entry.path())) continue;
            auto sz = entry.file_size(ec);
            if (ec || sz == 0 || sz > kMaxBytes) continue;
            auto mt = fs::last_write_time(entry.path(), ec);
            if (ec) continue;
            auto rel = fs::relative(entry.path(), root, ec);
            std::string rel_s = ec ? entry.path().string() : rel.string();
            mix(std::hash<std::string>{}(rel_s));
            mix(static_cast<std::uint64_t>(sz));
            mix(static_cast<std::uint64_t>(mt.time_since_epoch().count()));
            files.emplace_back(std::move(rel_s), entry.path());
        }
    }
};

// ── SubagentRunner ─────────────────────────────────────────────────────
//   Backs the task tool. The ENTIRE isolated agent loop — agent-type role
//   prompts, the per-completion stream reassembly, local tool dispatch, the
//   activity feed pumped to the parent card via progress::emit, the report
//   harvest — lives here, lifted verbatim from the native task tool. The
//   shell owns only the schema + the availability gate; run() does the work.
//
//   The native task tool's `display_description` arg is UI-only and stays in
//   the shell's schema; run() never needs it.

struct AgentType {
    std::string_view              name;
    bool                          read_only;
    std::string_view              role;
    std::vector<std::string_view> allow;   // empty ⇒ all (minus task)
};

const AgentType& resolve_agent_type(std::string_view t) {
    static const std::vector<AgentType> kTypes = {
        {"explorer", true,
         "Your role: EXPLORER. Map and explain the codebase region the task "
         "names. Read widely, trace call sites and definitions, and return a "
         "precise map: the key files, the functions/types involved, how they "
         "connect, and any gotchas. Cite exact file paths and line numbers. "
         "You are READ-ONLY \xe2\x80\x94 never modify anything.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "web_search", "web_fetch"}},
        {"reviewer", true,
         "Your role: REVIEWER. Critically review the code or change the task "
         "names. Look for bugs, edge cases, race conditions, security issues, "
         "and deviations from the surrounding conventions. Return findings as "
         "a prioritised list (blocker / major / minor / nit), each with the "
         "exact file:line and a concrete fix suggestion. You are READ-ONLY.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "git_diff", "git_log", "git_status"}},
        {"tester", false,
         "Your role: TESTER. Reproduce, run, and diagnose. Build/run the "
         "relevant tests or commands the task names, read the failures, and "
         "report the root cause with the exact failing assertion and the "
         "file:line that produced it. Prefer running over guessing. Do NOT "
         "rewrite production code \xe2\x80\x94 only run, read, and diagnose.",
         {"read", "grep", "glob", "list_dir", "find_definition", "repo_map",
          "bash", "diagnostics", "git_diff", "git_status"}},
        {"coder", false,
         "Your role: CODER. Implement the change the task names end-to-end: "
         "read the relevant code first, make the edits, and verify they build/"
         "compile if a build command is obvious. Follow the surrounding "
         "conventions exactly. Report what you changed (files + a one-line "
         "summary each) and whether it built.",
         {}},
        {"general", false,
         "Your role: GENERAL. Complete the delegated task end-to-end using "
         "whatever tools fit, then report the outcome.",
         {}},
    };
    for (const auto& a : kTypes)
        if (a.name == t) return a;
    return kTypes.back();
}

std::string subagent_system_prompt(const AgentType& type) {
    std::string base = provider::anthropic::default_system_prompt();
    base += "\n\n<subagent>\n";
    base += std::string{type.role};
    base +=
        "\n\nYou are a SUBAGENT spawned to complete ONE delegated task in "
        "isolation. You do NOT see the parent conversation and cannot ask it "
        "questions \xe2\x80\x94 work fully autonomously from the task prompt alone. "
        "Use your tools to investigate and act, then STOP calling tools and "
        "write your final report as plain text.\n\n"
        "Your final message is the ONLY thing the parent receives \xe2\x80\x94 not your "
        "transcript, not your tool output. So the report must stand alone. "
        "Structure it as:\n"
        "  \xe2\x80\xa2 A one-line OUTCOME (what you found / did).\n"
        "  \xe2\x80\xa2 The key details the parent needs to act, with exact file:line "
        "references where relevant.\n"
        "  \xe2\x80\xa2 Anything you could NOT determine, stated plainly.\n"
        "Be concrete and cite evidence (paths, line numbers, command output). "
        "Do not pad. If the task is impossible or underspecified, say so and "
        "explain what's missing rather than guessing.";
    if (type.read_only)
        base += "\n\nYou are READ-ONLY: you have no tools that modify files, "
                "run commands, or reach the network. Investigate and report only.";
    base += "\n</subagent>";
    return base;
}

std::string summarize_call(const ToolUse& tc) {
    std::string s = tc.name.value;
    if (tc.args.is_object()) {
        for (const char* key : {"path", "file_path", "pattern", "command",
                                "url", "query", "symbol", "prompt"}) {
            auto it = tc.args.find(key);
            if (it != tc.args.end() && it->is_string()) {
                std::string v = it->get<std::string>();
                if (v.size() > 80) { v.resize(77); v += "..."; }
                for (auto& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
                s += "  ";
                s += v;
                break;
            }
        }
    }
    return s;
}

StopReason run_one_completion(Thread& thread, const subagent::Config& cfg,
                              const AgentType& type,
                              std::string& log, std::string& err_out) {
    // Provider-agnostic request — the generic shape every transport accepts.
    // fresh_auth_header refreshes the ANTHROPIC OAuth token from disk; on any
    // other backend it would CLOBBER the provider's key with Anthropic
    // credentials, so gate it on the active provider kind.
    provider::Request req;
    req.model         = cfg.model;
    req.system_prompt = subagent_system_prompt(type);
    req.auth          = provider::active().kind == provider::Kind::Anthropic
                      ? auth::fresh_auth_header(cfg.auth)
                      : cfg.auth;
    req.max_tokens    = 32000;
    req.messages      = thread.messages;

    auto allowed = [&](const tools::ToolDef& t) -> bool {
        if (t.name.value == "task") return false;
        if (!type.allow.empty()) {
            bool listed = false;
            for (auto n : type.allow)
                if (n == t.name.value) { listed = true; break; }
            if (!listed) return false;
        }
        if (type.read_only) {
            tools::EffectSet eff = t.effects;
            if (const auto* sp = tools::spec::lookup(t.name.value)) eff = sp->effects;
            using tools::Effect;
            if (eff.has(Effect::WriteFs) || eff.has(Effect::Exec) || eff.has(Effect::Net))
                return false;
        }
        return true;
    };
    for (const auto& t : tools::wire_tools()) {
        if (!allowed(t)) continue;
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }

    Message asst;
    asst.role = Role::Assistant;
    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;

    // Throttled feed pump: a fast model streams hundreds of text deltas
    // per second, and every progress::emit crosses a thread boundary as a
    // ToolExecProgress Msg. ~12 fps is indistinguishable on the card and
    // keeps a parallel fan-out of subagents from flooding the UI queue.
    auto last_pump = std::chrono::steady_clock::now()
                   - std::chrono::milliseconds(100);
    auto pump = [&](bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && now - last_pump < std::chrono::milliseconds(80)) return;
        last_pump = now;
        std::string snap = log;
        if (!asst.text.empty()) {
            snap += "\n  \xe2\x96\xb8 ";
            snap += asst.text;
        }
        progress::emit(snap);
    };

    auto sink = [&](Msg m) {
            auto* sm = std::get_if<msg::StreamMsg>(&m);
            if (!sm) return;
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    asst.text += e.text;
                    pump();
                } else if constexpr (std::same_as<T, StreamToolUseStart>) {
                    ToolUse tc;
                    tc.id     = e.id;
                    tc.name   = e.name;
                    tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                    asst.tool_calls.push_back(std::move(tc));
                    cur_tool_json.clear();
                } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                    cur_tool_json += e.partial_json;
                } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                    if (!asst.tool_calls.empty() && !cur_tool_json.empty()) {
                        try {
                            asst.tool_calls.back().args = json::parse(cur_tool_json);
                        } catch (...) {
                            // Truncated/unbalanced args JSON (stream cut, weak
                            // model). Salvage by synthesising the missing
                            // closers — but NEVER when the cut landed inside a
                            // string VALUE: the repaired JSON would parse fine
                            // and silently run a tool with a half-written body.
                            if (!util::ended_inside_string(cur_tool_json)) {
                                try {
                                    asst.tool_calls.back().args = json::parse(
                                        util::close_partial_json(cur_tool_json));
                                    ::agentty::util::dbglog("subagent.tool_args.repaired",
                                                 cur_tool_json);
                                } catch (...) {
                                    ::agentty::util::dbglog("subagent.tool_args.parse",
                                                 cur_tool_json);
                                }
                            } else {
                                ::agentty::util::dbglog("subagent.tool_args.mid_string",
                                             cur_tool_json);
                            }
                        }
                    }
                    cur_tool_json.clear();
                    if (!asst.tool_calls.empty()) {
                        log += "\n  \xe2\x9a\x99 ";
                        log += summarize_call(asst.tool_calls.back());
                        pump(/*force=*/true);
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    err_out = e.message;
                }
            }, *sm);
        };

    // Route through the SAME provider dispatch the parent uses (installed
    // at startup); fall back to the Anthropic transport when no seam is
    // wired (tests that install only auth+model).
    if (cfg.stream) {
        cfg.stream(std::move(req), sink);
    } else {
        provider::anthropic::AnthropicProvider p;
        p.stream(std::move(req), sink);
    }
    pump(/*force=*/true);   // flush the throttled tail

    thread.messages.push_back(std::move(asst));
    return stop;
}

class AgenttySubagentRunner final : public mt::SubagentRunner {
public:
    bool available() const override {
        auto cfg = subagent::current();
        if (!cfg.installed || cfg.model.empty() || auth::is_empty(cfg.auth))
            return false;
        if (subagent::current_depth() >= subagent::kMaxDepth)
            return false;
        return true;
    }

    std::string run(const mt::SubagentRequest& sreq, bool& is_error) override {
        is_error = false;
        auto cfg = subagent::current();
        if (!cfg.installed || cfg.model.empty() || auth::is_empty(cfg.auth)) {
            is_error = true;
            return "subagents are unavailable in this context (no model/auth wired)";
        }
        if (subagent::current_depth() >= subagent::kMaxDepth) {
            is_error = true;
            return "subagent depth limit reached — a subagent cannot spawn "
                   "further subagents at this nesting level";
        }

        subagent::push_depth();
        struct DepthGuard { ~DepthGuard() { subagent::pop_depth(); } } depth_guard;

        const AgentType& type = resolve_agent_type(sreq.agent_type);

        Thread thread;
        {
            Message user;
            user.role = Role::User;
            user.text = sreq.prompt;
            thread.messages.push_back(std::move(user));
        }

        int turns = 0;
        std::string log = "\xe2\x97\x86 " + std::string{type.name} + " agent";
        std::string last_error;
        // Transient stream failures (429/529 brown-out, TLS reset, transport
        // hiccup) are RETRIED with backoff instead of aborting the whole
        // subagent — "task fails a lot" was mostly one flaky completion
        // killing an otherwise healthy loop. Consecutive counter: any
        // successful completion resets it.
        constexpr int kMaxStreamRetries = 3;
        int stream_failures = 0;
        // Repeat-failure breaker (same rule as the parent's doom-loop
        // breaker): the identical tool call failing 3× means the loop is
        // stuck — stop burning turns and report what we have.
        std::unordered_map<std::string, int> failed_calls;
        bool doomed = false;

        while (turns < subagent::kMaxTurns && !doomed) {
            ++turns;
            std::string err;
            StopReason stop = run_one_completion(thread, cfg, type, log, err);
            (void)stop;   // loop advance is decided by ran_a_tool below

            if (!err.empty()) {
                last_error = err;
                ++stream_failures;
                // run_one_completion pushed a (possibly partial) assistant
                // message for the failed completion. Drop it unconditionally:
                // a partial turn carrying tool_use blocks with no tool_results
                // would 400 the retry request (wire pairing), and partial text
                // would duplicate once the retry streams the full reply.
                if (!thread.messages.empty()
                    && thread.messages.back().role == Role::Assistant)
                    thread.messages.pop_back();
                if (stream_failures > kMaxStreamRetries) {
                    log += "\n  \xe2\x9a\xa0 stream failed "
                         + std::to_string(stream_failures) + "\xc3\x97 \xe2\x80\x94 giving up: "
                         + err;
                    progress::emit(log);
                    break;
                }
                const int wait_s = 1 << (stream_failures - 1);   // 1,2,4s
                log += "\n  \xe2\x86\xbb retry " + std::to_string(stream_failures)
                     + "/" + std::to_string(kMaxStreamRetries)
                     + " in " + std::to_string(wait_s) + "s (" + err + ")";
                progress::emit(log);
                std::this_thread::sleep_for(std::chrono::seconds(wait_s));
                --turns;   // a retried completion doesn't consume budget
                continue;
            }
            stream_failures = 0;
            last_error.clear();

            Message& asst = thread.messages.back();
            bool ran_a_tool = false;
            if (!asst.tool_calls.empty()) {
                const auto now = std::chrono::steady_clock::now();
                for (auto& tc : asst.tool_calls) {
                    if (tc.args.is_null()) {
                        tc.status = ToolUse::Failed{now, now,
                            "tool args failed to parse \xe2\x80\x94 re-emit the call "
                            "with complete, valid JSON arguments"};
                        log += "\n    \xe2\x9c\x97 " + tc.name.value + ": bad args";
                        progress::emit(log);
                        // A parse failure still counts as a tool ROUND-TRIP:
                        // the model sees the error tool_result and can
                        // re-emit. Without this the loop broke out on the
                        // first bad-args call and the whole task died.
                        ran_a_tool = true;
                        continue;
                    }
                    ran_a_tool = true;
                    auto res = tool::DynamicDispatch::execute(tc.name.value, tc.args);
                    if (res) {
                        tc.status = ToolUse::Done{now, now, std::move(res->text)};
                        log += "\n    \xe2\x9c\x93 " + summarize_call(tc);
                    } else {
                        tc.status = ToolUse::Failed{now, now, res.error().render()};
                        log += "\n    \xe2\x9c\x97 " + summarize_call(tc) + "  \xe2\x80\x94 "
                             + res.error().render();
                        // Identical failing call 3× → the loop is stuck.
                        std::string key = tc.name.value + '\0'
                            + (tc.args.is_null() ? std::string{} : tc.args.dump());
                        if (++failed_calls[key] >= 3) {
                            doomed = true;
                            log += "\n  \xe2\x9a\xa0 same call failed 3\xc3\x97 \xe2\x80\x94 stopping";
                        }
                    }
                    progress::emit(log);
                }
            }

            if (!ran_a_tool) break;   // final text answer (or nothing left to do)
        }

        std::string report;
        for (auto it = thread.messages.rbegin(); it != thread.messages.rend(); ++it) {
            if (it->role == Role::Assistant && !it->text.empty()) {
                report = it->text;
                break;
            }
        }
        if (report.empty()) {
            std::string why;
            if (!last_error.empty())
                why = "[subagent failed: " + last_error + "]";
            else if (doomed)
                why = "[subagent stopped: the same tool call failed 3\xc3\x97 "
                      "in a row without converging]";
            else if (turns >= subagent::kMaxTurns)
                why = "[subagent hit its turn budget without producing a final report]";
            else
                why = "[subagent finished without a text report]";
            report = log.empty() ? why : (why + "\n\nActivity:\n" + log);
            // A bare error (no salvageable report) propagates as an error so
            // the shell tags the tool_result is_error.
            if (!last_error.empty()) is_error = true;
        }

        std::ostringstream out;
        out << "Subagent report (" << type.name << ", " << turns << " turn"
            << (turns == 1 ? "" : "s") << "):\n\n" << report;
        return out.str();
    }
};

} // namespace

void install_host_backends(::mcp::tools::HostServices& svc) {
    svc.memory    = std::make_shared<AgenttyMemoryStore>();
    svc.skills    = std::make_shared<AgenttySkillResolver>();
    svc.retriever = std::make_shared<AgenttyDocRetriever>();
    svc.code_retriever = std::make_shared<AgenttyCodeRetriever>();
    svc.subagent  = std::make_shared<AgenttySubagentRunner>();
    // svc.todo intentionally left null: the mcp todo shell renders identical
    // text to the native tool with no host state needed, and agentty's TUI
    // parses the rendered text — there is no structured sink to feed.
}

// ── Proactive retrieval (SOTA active-RAG / FLARE / Self-RAG) ────────────
// Runs the SAME AgenttyDocRetriever the search_docs tool uses, but out of
// band — before the model sees the turn — and only surfaces its result when
// confidence clears a HIGH bar (higher than the tool's LOW floor: we're
// spending the user's context-window tokens unprompted, so the hit must be
// worth it). Parses the confidence out of the retriever's mode string so
// there's a single source of truth for the score.
std::optional<ProactiveHit> proactive_retrieve(const std::string& query, int k) {
    // Confidence bar for UNPROMPTED injection. The tool's LOW floor is 0.25;
    // we inject only well above it. Tunable via AGENTTY_RAG_PROACTIVE_MIN.
    double min_conf = 0.45;
    if (const char* mc = std::getenv("AGENTTY_RAG_PROACTIVE_MIN"); mc && mc[0]) {
        // Any non-negative value is honoured; a bar above 1.0 is a
        // legitimate "never inject" switch (confidence is clamped to [0,1]).
        try { double v = std::stod(mc); if (v >= 0) min_conf = v; }
        catch (...) { /* keep default */ }
    }

    try {
        AgenttyDocRetriever r;
        mt::DocQuery q;
        q.query = query;
        q.k     = k > 0 ? k : 3;
        std::string mode, err;
        auto passages = r.retrieve(q, mode, err);
        if (!err.empty() || passages.empty()) return std::nullopt;

        // Recover the confidence the pipeline computed (mode carries
        // ", confidence 0.NN"). If we can't parse it, be conservative and
        // don't inject.
        double conf = -1.0;
        if (auto p = mode.find("confidence "); p != std::string::npos) {
            try { conf = std::stod(mode.substr(p + 11)); } catch (...) {}
        }
        if (conf < min_conf) return std::nullopt;

        // Build the wire block. Fenced + provenance-labelled so the model
        // treats it as retrieved reference, not the user's words. Bounded.
        std::string block =
            "<retrieved-context>\n"
            "The following passages were auto-retrieved from the user's "
            "knowledge base (docs/skills/memory) because they look relevant "
            "to the request. Ground your answer in them where they apply; "
            "ignore any that don't. Cite the source path when you use one.\n\n";
        int n = 0;
        for (const auto& p : passages) {
            block += "[" + (p.source.empty() ? std::string{"docs"} : p.source)
                   + ":" + p.path;
            if (p.line_start > 0)
                block += ":" + std::to_string(p.line_start);
            block += "]\n";
            block += p.text;
            if (!p.text.empty() && p.text.back() != '\n') block += '\n';
            block += '\n';
            ++n;
        }
        block += "</retrieved-context>";

        return ProactiveHit{std::move(block), conf, n};
    } catch (...) {
        return std::nullopt;   // proactive retrieval is best-effort, never fatal
    }
}

} // namespace agentty::tools
