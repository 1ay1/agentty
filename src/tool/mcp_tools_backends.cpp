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

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"

#include "agentty/rag/rag.hpp"
#include "agentty/rag/rerank.hpp"
#include "agentty/rag/expand.hpp"
#include "agentty/rag/knowledge.hpp"

#include "agentty/mcp/client.hpp"   // mcp_resources / mcp_read_resource seams
#include "agentty/util/dbglog.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
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

// ── DocRetriever ───────────────────────────────────────────────────────
//   Backs search_docs. Runs agentty's full SOTA RAG pipeline (hybrid
//   BM25+dense fused, wide-pool rerank, compress, optional RAG-Fusion query
//   expansion) and returns flat passages. The `mode` string carries the
//   rich provenance the native tool printed in its header (root path,
//   reranked, +N query variants) so no signal is lost when the shell
//   renders the result.
class AgenttyDocRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        std::vector<mt::DocPassage> out;
        try {
            auto root = resolve_docs_root();
            if (root.empty()) {
                err = "no knowledge directory configured. Set AGENTTY_DOCS_DIR "
                      "to a folder of documents (markdown/text/etc.) to enable "
                      "search_docs, or create ./docs.";
                return out;
            }

            const rag::EmbedConfig embed = embed_config_from_env();
            std::string root_str = root.string();

            static std::mutex mu;
            static rag::Corpus corpus;
            static std::string indexed_root;

            std::lock_guard<std::mutex> lock(mu);
            if (indexed_root != root_str) {
                corpus.build(root, embed);
                indexed_root = root_str;
            }

            rag::CorpusSource docs_source("docs", corpus, embed);
            rag::KnowledgeRouter router;
            router.add(std::shared_ptr<rag::KnowledgeSource>(
                &docs_source, [](rag::KnowledgeSource*) {}));

            // OPT-IN second source: index this session's MCP `resources/*`
            // and fuse them with the docs folder. Built once (process-wide,
            // lazy) and reused. Enabled by AGENTTY_RAG_MCP=1; with no MCP
            // configured the source self-indexes to empty and costs nothing.
            std::shared_ptr<rag::McpResourceSource> mcp_source;
            if (mcp_resources_enabled()) {
                mcp_source = mcp_resource_source(embed);
                if (mcp_source)
                    router.add(std::shared_ptr<rag::KnowledgeSource>(
                        mcp_source, mcp_source.get()));  // aliasing: shares ownership
            }

            const std::size_t pool_k =
                std::max<std::size_t>(static_cast<std::size_t>(q.k) * 5, 30);

            std::size_t variant_count = 0;
            rag::Context ctx;
            ctx.query = q.query;
            if (expand_enabled()) {
                rag::ExpandConfig ecfg = expand_config_from_env(embed);
                auto queries = rag::expand_query(ecfg, q.query);
                if (queries.size() > 1) variant_count = queries.size() - 1;
                // Multi-query fusion over the docs corpus. When an MCP source
                // is active, fold its best-of hits in via the router so the
                // two sources still fuse (the docs side keeps RAG-Fusion).
                auto fused = docs_source.retrieve_fused(queries, pool_k);
                if (mcp_source) {
                    auto extra = mcp_source->retrieve(q.query, pool_k);
                    fused.insert(fused.end(), extra.begin(), extra.end());
                }
                ctx = rag::Context::from_hits(q.query, std::move(fused));
            } else {
                ctx = rag::Context::from_hits(
                    q.query, router.retrieve(q.query, pool_k));
            }

            rag::Pipeline pipe;
            pipe.add(std::make_shared<rag::RerankStage>(static_cast<std::size_t>(q.k)))
                .add(std::make_shared<rag::CompressStage>(/*target_chars=*/600));
            ctx = pipe.run(std::move(ctx));

            std::string mode_str = corpus.has_embeddings() ? "hybrid" : "BM25-only";
            mode_str += ", reranked";
            if (variant_count > 0)
                mode_str += ", +" + std::to_string(variant_count) + " query variants";
            mode_str += " from " + root_str;
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
                } catch (const std::exception& e) { util::dbglog("rag.embed_endpoint.port", e.what()); /* keep default port */ }
                catch (...) { util::dbglog("rag.embed_endpoint.port", "non-std exception"); }
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
            } catch (const std::exception& e) { util::dbglog("rag.expand_n.env", e.what()); /* keep default */ }
            catch (...) { util::dbglog("rag.expand_n.env", "non-std exception"); }
        }
        return cfg;
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
         {"read", "grep", "glob", "list_dir", "find_definition", "web_search",
          "web_fetch"}},
        {"reviewer", true,
         "Your role: REVIEWER. Critically review the code or change the task "
         "names. Look for bugs, edge cases, race conditions, security issues, "
         "and deviations from the surrounding conventions. Return findings as "
         "a prioritised list (blocker / major / minor / nit), each with the "
         "exact file:line and a concrete fix suggestion. You are READ-ONLY.",
         {"read", "grep", "glob", "list_dir", "find_definition", "git_diff",
          "git_log", "git_status"}},
        {"tester", false,
         "Your role: TESTER. Reproduce, run, and diagnose. Build/run the "
         "relevant tests or commands the task names, read the failures, and "
         "report the root cause with the exact failing assertion and the "
         "file:line that produced it. Prefer running over guessing. Do NOT "
         "rewrite production code \xe2\x80\x94 only run, read, and diagnose.",
         {"read", "grep", "glob", "list_dir", "find_definition", "bash",
          "diagnostics", "git_diff", "git_status"}},
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
    namespace ap = provider::anthropic;
    ap::Request req;
    req.model         = cfg.model;
    req.system_prompt = subagent_system_prompt(type);
    req.auth          = auth::fresh_auth_header(cfg.auth);
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

    auto pump = [&] {
        std::string snap = log;
        if (!asst.text.empty()) {
            snap += "\n  \xe2\x96\xb8 ";
            snap += asst.text;
        }
        progress::emit(snap);
    };

    ap::run_stream_sync(std::move(req),
        [&](Msg m) {
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
                        } catch (const std::exception& ex) {
                            util::dbglog("subagent.tool_args.parse", ex.what());
                            /* leave null; marked failed below */
                        } catch (...) {
                            util::dbglog("subagent.tool_args.parse", "non-std exception");
                        }
                    }
                    cur_tool_json.clear();
                    if (!asst.tool_calls.empty()) {
                        log += "\n  \xe2\x9a\x99 ";
                        log += summarize_call(asst.tool_calls.back());
                        pump();
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    err_out = e.message;
                    log += "\n  \xe2\x9a\xa0 error: " + e.message;
                    pump();
                }
            }, *sm);
        });

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
        std::string log = "\xe2\x97\x86 " + std::string{type.name} + " subagent\n";
        std::string last_error;
        while (turns < subagent::kMaxTurns) {
            ++turns;
            log += (turns == 1 ? "" : "\n");
            log += "\xe2\x80\xa2 turn " + std::to_string(turns);
            progress::emit(log);
            std::string err;
            StopReason stop = run_one_completion(thread, cfg, type, log, err);
            if (!err.empty()) last_error = err;

            Message& asst = thread.messages.back();
            bool ran_a_tool = false;
            if (!asst.tool_calls.empty()) {
                const auto now = std::chrono::steady_clock::now();
                for (auto& tc : asst.tool_calls) {
                    if (tc.args.is_null()) {
                        tc.status = ToolUse::Failed{now, now,
                            "subagent tool args failed to parse"};
                        log += "\n    \xe2\x9c\x97 " + tc.name.value + ": bad args";
                        progress::emit(log);
                        continue;
                    }
                    ran_a_tool = true;
                    auto res = tool::DynamicDispatch::execute(tc.name.value, tc.args);
                    if (res) {
                        tc.status = ToolUse::Done{now, now, std::move(res->text)};
                        log += "\n    \xe2\x9c\x93 " + tc.name.value;
                    } else {
                        tc.status = ToolUse::Failed{now, now, res.error().render()};
                        log += "\n    \xe2\x9c\x97 " + tc.name.value + ": "
                             + res.error().render();
                    }
                    progress::emit(log);
                }
            }

            if (stop != StopReason::ToolUse && !ran_a_tool) break;
            if (stop == StopReason::ToolUse && !ran_a_tool) break;
            if (!err.empty() && !ran_a_tool) break;
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
    svc.subagent  = std::make_shared<AgenttySubagentRunner>();
    // svc.todo intentionally left null: the mcp todo shell renders identical
    // text to the native tool with no host state needed, and agentty's TUI
    // parses the rendered text — there is no structured sink to feed.
}

} // namespace agentty::tools
