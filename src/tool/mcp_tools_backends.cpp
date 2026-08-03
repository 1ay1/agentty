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
#include "agentty/provider/error_class.hpp"
#include "agentty/provider/provider.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/provider/wire.hpp"

#include "agentty/rag/rag_adapter.hpp"

#include "agentty/mcp/client.hpp"   // mcp_resources / mcp_read_resource seams
#include "agentty/util/dbglog.hpp"

#include <mcp/tools/host.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
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
    // scopes()[0] is the DEFAULT scope the shell uses when the model omits
    // `scope`. We only advertise "project" (and only make it the default)
    // when project storage is actually WRITABLE here. In a workspace whose
    // root is "/" or otherwise unwritable, path_for(Project) is empty and a
    // project append would fail — so offering "project" as the default made
    // the model's very first remember() call fail every time, forcing a
    // retry with scope="user". Dropping the unavailable scope from the
    // vocabulary makes the default "user", which always succeeds, WITHOUT
    // silently promoting an explicit project fact to user scope (append()
    // still refuses an explicit unavailable-project write — no cross-
    // workspace memory bleed).
    std::vector<std::string> scopes() const override {
        if (!memory::path_for(memory::Scope::Project).empty())
            return {"project", "user"};   // project writable → normal order
        return {"user"};                   // project unavailable → user is default
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

    std::optional<std::size_t> preview_wipe(const std::string& scope) override {
        auto s = memory::parse_scope(scope);
        if (!s || memory::path_for(*s).empty()) return std::nullopt;
        return memory::load_all(*s).size();
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
//   Backs search_docs. Runs agentty's full RAG pipeline (rag-cpp) and returns
//   flat passages. The funnel, as actually wired in src/rag/adapter.cpp:
//
//     sources:  docs folder + skills + memory, with MCP resources opt-in
//     retrieve: BM25 plus probed Ollama dense retrieval, weighted RRF fusion
//     optional: PRF, GraphRAG, CRAG, HyDE, and multi-query are explicit modes
//     rerank:   deterministic feature rerank + MMR + adjacent-hit stitch
//     compress: query-focused spans under one aggregate output budget
//     persist:  validated manifest + incremental docs/code refresh
//
//   The `mode` string carries the rich provenance (root path, mode,
//   reranked, +N variants, confidence) so no signal is lost when the
//   shell renders the result.
// A single process-wide Retriever backs both the search_docs tool and the
// proactive pre-turn path. Function-local static ⇒ constructed on first use
// (after the app has set cwd / env), destroyed at exit.
static ::agentty::rag::Retriever& shared_retriever() {
    static ::agentty::rag::Retriever r;
    return r;
}

class AgenttyDocRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        return run_(q, mode, err);
    }

private:
    static std::vector<mt::DocPassage>
    run_(const mt::DocQuery& q, std::string& mode, std::string& err) {
        std::vector<mt::DocPassage> out;
        auto res = shared_retriever().retrieve(q.query, q.k);
        if (!res.error.empty()) { err = res.error; return out; }
        mode = res.mode;
        out.reserve(res.passages.size());
        for (auto& p : res.passages) {
            mt::DocPassage d;
            d.source     = std::move(p.source);
            d.path       = std::move(p.path);
            d.line_start = p.line_start;
            d.line_end   = p.line_end;
            d.score      = p.score;
            d.text       = std::move(p.text);
            out.push_back(std::move(d));
        }
        return out;
    }
};


// ── CodeRetriever ──────────────────────────────────────────
//   definition-aware source chunks; BM25 is always available and a bounded
//   Ollama probe enables dense retrieval. A pruned 4,000-file manifest detects
//   drift, and small edit sets update only changed files.
class AgenttyCodeRetriever final : public mt::DocRetriever {
public:
    std::vector<mt::DocPassage>
    retrieve(const mt::DocQuery& q, std::string& mode, std::string& err) override {
        std::vector<mt::DocPassage> out;
        auto res = shared_retriever().retrieve_code(q.query, q.k);
        if (!res.error.empty()) { err = res.error; return out; }
        mode = res.mode;
        out.reserve(res.passages.size());
        for (auto& p : res.passages) {
            mt::DocPassage d;
            d.source     = std::move(p.source);
            d.path       = std::move(p.path);
            d.line_start = p.line_start;
            d.line_end   = p.line_end;
            d.score      = p.score;
            d.text       = std::move(p.text);
            out.push_back(std::move(d));
        }
        return out;
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
    std::string base = provider::anthropic::default_system_prompt(/*lean=*/true);
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

provider::StreamResult run_one_completion(Thread& thread,
                              const subagent::Config& cfg,
                              const AgentType& type,
                              std::string& log) {
    // Provider-agnostic request — the generic shape every transport accepts.
    // fresh_auth_header refreshes the ANTHROPIC OAuth token from disk; on any
    // other backend it would CLOBBER the provider's key with Anthropic
    // credentials, so gate it on the active provider kind.
    provider::Request req;
    // Model routing: read-only roles (explorer/reviewer) do grunt work —
    // read/grep/map/summarise — a small model handles as well as a flagship
    // for a fraction of the cost, so route them to the cheapest capable model
    // the ACTIVE provider offers. Write-capable roles (coder/general) keep the
    // parent model — their edits must match the parent's quality. The router
    // never routes up and never crosses providers, so a single-model or
    // Opus-only provider sees no change (returns cfg.model unchanged).
    req.model         = type.read_only
                          ? agentty::cheapest_capable_model(cfg.model, cfg.candidates)
                          : cfg.model;
    req.system_prompt = subagent_system_prompt(type);
    req.auth          = provider::active().kind == provider::Kind::Anthropic
                      ? auth::fresh_auth_header(cfg.auth)
                      : cfg.auth;
    // A subagent's job is to investigate and return a CONCISE standalone
    // report — not to emit a 32k-token essay. The parent's default is 16k;
    // 8k is ample for a report yet caps the per-turn output cost of a
    // fan-out of parallel subagents (each turn otherwise billed at the full
    // ceiling). The wrap-up nudge already forces a tight final report.
    req.max_tokens    = 8192;
    req.messages      = thread.messages;
    req.cancel        = std::make_shared<http::CancelToken>();
    // Stable per-subagent conversation identity so the shared prefix
    // (heavy system prompt + tool schemas + accumulated tool results) is
    // PROMPT-CACHED across this subagent's up-to-24 turns instead of being
    // re-encoded from scratch every turn. Keyed on the agent role + the task
    // prompt so each spawned subagent gets its own stable cache lane and a
    // fan-out of parallel explorers doesn't collide. Without this the single
    // biggest subagent cost — re-sending the same large prefix every turn —
    // pays full price on every one of those turns.
    {
        std::string key = "subagent:";
        key += type.name;
        key += ':';
        // A short stable digest of the task prompt (FNV-1a) keeps the key
        // bounded and distinct per delegated task.
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : thread.messages.empty()
                                   ? std::string_view{}
                                   : std::string_view{thread.messages.front().text}) {
            h ^= c; h *= 0x00000100000001B3ULL;
        }
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(h));
        key += buf;
        req.session_key = std::move(key);
    }

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
    std::string_view newest_user;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
        if (it->role == Role::User && !it->proactive_context) {
            newest_user = it->text;
            break;
        }
    }
    for (const auto* tool : tools::select_wire_tools(newest_user)) {
        const auto& t = *tool;
        if (!allowed(t)) continue;
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }

    Message asst;
    asst.role = Role::Assistant;
    StopReason stop = StopReason::Unspecified;
    std::unordered_map<std::string, std::string> tool_json;

    auto find_tool = [&](const ToolCallId& id) -> ToolUse* {
        auto it = std::find_if(asst.tool_calls.begin(), asst.tool_calls.end(),
            [&](const ToolUse& tc) { return tc.id == id; });
        return it == asst.tool_calls.end() ? nullptr : &*it;
    };

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
                    tool_json[e.id.value].clear();
                } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                    tool_json[e.id.value] += e.partial_json;
                } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                    ToolUse* tc = find_tool(e.id);
                    auto json_it = tool_json.find(e.id.value);
                    const std::string partial = json_it == tool_json.end()
                                              ? std::string{} : json_it->second;
                    if (tc && !partial.empty()) {
                        try {
                            tc->args = json::parse(partial);
                        } catch (...) {
                            // Truncated/unbalanced args JSON (stream cut, weak
                            // model). Salvage by synthesising the missing
                            // closers — but NEVER when the cut landed inside a
                            // string VALUE: the repaired JSON would parse fine
                            // and silently run a tool with a half-written body.
                            if (!util::ended_inside_string(partial)) {
                                try {
                                    tc->args = json::parse(
                                        util::close_partial_json(partial));
                                    ::agentty::util::dbglog("subagent.tool_args.repaired",
                                                 partial);
                                } catch (...) {
                                    ::agentty::util::dbglog("subagent.tool_args.parse",
                                                 partial);
                                }
                            } else {
                                ::agentty::util::dbglog("subagent.tool_args.mid_string",
                                             partial);
                            }
                        }
                    }
                    tool_json.erase(e.id.value);
                    if (tc) {
                        log += "\n  ⚙ ";
                        log += summarize_call(*tc);
                        pump(/*force=*/true);
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    // StreamResult is authoritative. The event remains useful
                    // for legacy/scripted seams that cannot stamp a result.
                }
            }, *sm);
        };

    // Route through the SAME provider dispatch the parent uses (installed
    // at startup); fall back to the Anthropic transport when no seam is
    // wired (tests that install only auth+model).
    provider::StreamResult result;
    auto cancel = req.cancel;
    auto parent_cancel = cancellation::current();
    std::jthread cancel_bridge;
    if (parent_cancel) {
        cancel_bridge = std::jthread([parent_cancel, cancel](std::stop_token st) {
            while (!st.stop_requested() && !cancel->is_cancelled()) {
                if (parent_cancel()) { cancel->cancel(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    if (cancellation::requested()) cancel->cancel();
    if (cfg.stream) {
        result = cfg.stream(std::move(req), sink);
    } else {
        provider::anthropic::AnthropicProvider p;
        result = p.stream(std::move(req), sink);
    }
    cancel_bridge.request_stop();
    pump(/*force=*/true);   // flush the throttled tail

    // An empty successful close is a transient transport failure. A max-token
    // turn is also incomplete: partial prose/tool JSON is not a final report.
    if (result.ok() && asst.text.empty() && asst.tool_calls.empty())
        result = provider::StreamResult::failed(
            "provider returned an empty completion");
    if (result.ok() && (result.stop == StopReason::MaxTokens
                        || stop == StopReason::MaxTokens)) {
        result = provider::StreamResult::failed(
            "provider hit the max-token limit before completing the subagent turn");
        result.stop = StopReason::MaxTokens;
    }

    thread.messages.push_back(std::move(asst));
    return result;
}

class AgenttySubagentRunner final : public mt::SubagentRunner {
public:
    std::string unavailable_reason() const override {
        auto cfg = subagent::current();
        if (!cfg.installed)
            return "subagent runtime was not installed; restart agentty with the current executable";
        if (cfg.model.empty())
            return "no model is selected for the active provider";
        // Runtime dispatch is credential-aware itself: ChatGPT resolves its
        // native OAuth store per request and local providers need no auth at
        // all. Only the legacy direct-Anthropic fallback requires a non-empty
        // header here.
        if (!cfg.stream && auth::is_empty(cfg.auth))
            return "no provider stream or fallback Anthropic credential is configured";
        if (subagent::current_depth() >= subagent::kMaxDepth)
            return "subagent nesting depth limit reached (maximum "
                 + std::to_string(subagent::kMaxDepth) + ")";
        if (cancellation::requested())
            return "cancelled before the subagent started";
        return {};
    }

    std::string run(const mt::SubagentRequest& sreq, bool& is_error) override {
        is_error = false;
        auto cfg = subagent::current();
        if (!cfg.installed || cfg.model.empty()
            || (!cfg.stream && auth::is_empty(cfg.auth))) {
            is_error = true;
            return "subagents are unavailable in this context (no model/stream wired)";
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
        // Set once we've told the model it's out of budget and must write its
        // final report NOW. Prevents a thorough agent from exploring straight
        // into the turn cap and never emitting a report (the loop below then
        // salvages a stale early narration line instead of a real answer).
        bool wrapup_nudged = false;

        while (turns < subagent::kMaxTurns && !doomed) {
            ++turns;

            // FINAL-TURN NUDGE: when only a couple of turns of budget remain,
            // inject a synthetic user message ordering the model to stop
            // running tools and write its report. Without this the agent
            // spends its last completion on yet another tool call, hits the
            // cap mid-investigation, and returns no final text at all.
            if (!wrapup_nudged && turns >= subagent::kMaxTurns - 1
                && !thread.messages.empty()
                && thread.messages.back().role != Role::User) {
                Message nudge;
                nudge.role = Role::User;
                nudge.text =
                    "You are almost out of turn budget. Do NOT run any more "
                    "tools. Write your FINAL report now as a plain text "
                    "message: a complete, self-contained answer to the task "
                    "using everything you have gathered so far.";
                thread.messages.push_back(std::move(nudge));
                wrapup_nudged = true;
            }

            provider::StreamResult stream_result =
                run_one_completion(thread, cfg, type, log);

            if (!stream_result.ok()) {
                const std::string err = stream_result.error.value_or("stream failed");
                last_error = err;
                const auto error_class = stream_result.cancelled()
                    ? provider::ErrorClass::Cancelled
                    : provider::classify_stream_error(err,
                                                       stream_result.http_status);
                const bool empty_completion =
                    err == "provider returned an empty completion";
                const bool retryable = !stream_result.non_replayable
                    && (empty_completion
                        || error_class == provider::ErrorClass::Transient
                        || error_class == provider::ErrorClass::RateLimit);
                // A partial assistant message can contain unpaired tool calls
                // and must never be replayed on retry or reported as complete.
                if (!thread.messages.empty()
                    && thread.messages.back().role == Role::Assistant)
                    thread.messages.pop_back();
                if (!retryable || error_class == provider::ErrorClass::Cancelled
                    || cancellation::requested()) {
                    is_error = true;
                    if (error_class == provider::ErrorClass::Cancelled
                        || cancellation::requested())
                        last_error = "subagent cancelled: " + err;
                    break;
                }
                ++stream_failures;
                if (stream_failures > kMaxStreamRetries) {
                    log += "\n  ⚠ stream failed "
                         + std::to_string(stream_failures) + "× — giving up: "
                         + err;
                    progress::emit(log);
                    break;
                }
                // Honor server guidance, but cap it so a hostile/mistaken
                // Retry-After cannot pin a task worker indefinitely.
                constexpr auto kMaxRetryAfter = std::chrono::seconds{30};
                auto wait = stream_result.retry_after
                    ? std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::min(*stream_result.retry_after, kMaxRetryAfter))
                    : provider::backoff_with_jitter(error_class,
                                                    stream_failures - 1);
                log += "\n  ↻ retry " + std::to_string(stream_failures)
                     + "/" + std::to_string(kMaxStreamRetries)
                     + " in " + std::to_string(wait.count()) + "ms (" + err + ")";
                progress::emit(log);
                const auto retry_until = std::chrono::steady_clock::now() + wait;
                while (std::chrono::steady_clock::now() < retry_until) {
                    if (cancellation::requested()) {
                        is_error = true;
                        return "subagent cancelled while waiting to retry: " + err;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                --turns;
                continue;
            }
            stream_failures = 0;
            last_error.clear();

            Message& asst = thread.messages.back();
            bool ran_a_tool = false;
            if (!asst.tool_calls.empty()) {
                const auto now = std::chrono::steady_clock::now();
                for (auto& tc : asst.tool_calls) {
                    if (cancellation::requested()) {
                        is_error = true;
                        last_error = "subagent cancelled before local tool execution";
                        doomed = true;
                        break;
                    }
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
                        // ECONOMY: a subagent is a focused, read-heavy burst
                        // (explorer/reviewer call read/grep/repo_map, whose
                        // outputs run to tens of KiB each). Those results
                        // accumulate in the subagent's OWN thread and replay
                        // on every one of its up-to-24 turns. The parent's
                        // 64 KiB newest-result budget is tuned for a long
                        // interactive chat; for a subagent it's the dominant
                        // cost (8 live results × 64 KiB replayed per turn).
                        // Cap each result to a tight head+tail the instant we
                        // store it, so the working set the model reasons over
                        // stays lean without losing the WHAT of any result.
                        // (Transport aging still applies on top for old ones.)
                        constexpr std::size_t kSubagentToolBudget = 8u * 1024u;
                        std::string capped = provider::wire::cap_tool_result(
                            res->text, kSubagentToolBudget);
                        tc.status = ToolUse::Done{now, now, std::move(capped)};
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

        // Extract the report. The AUTHORITATIVE report is the text of the
        // FINAL assistant turn — the message the model wrote last, after all
        // its tool work. We do NOT walk backward to the first non-empty text:
        // an agent that explored straight into the turn cap has its last few
        // turns as pure tool calls (empty .text), and a naive reverse-walk
        // would resurrect its turn-1 narration ("I'll start by mapping...")
        // and pass it off as a finished report. That was the real bug: a
        // 24-turn run returning its opening sentence instead of an answer.
        std::string report;
        bool salvaged_stale = false;
        if (!thread.messages.empty()
            && thread.messages.back().role == Role::Assistant
            && !thread.messages.back().text.empty()) {
            // Clean finish: the model's last act was to write prose.
            report = thread.messages.back().text;
        } else {
            // No final text (ran out of budget mid-tool, or last turn was
            // tool-only). Salvage the most recent EARLIER assistant text as a
            // partial, but mark it stale so the banner below tells the caller
            // this is not a proper final report.
            for (auto it = thread.messages.rbegin(); it != thread.messages.rend(); ++it) {
                if (it->role == Role::Assistant && !it->text.empty()) {
                    report = it->text;
                    salvaged_stale = true;
                    break;
                }
            }
        }

        if (report.empty() || salvaged_stale) {
            std::string why;
            if (!last_error.empty())
                why = "[subagent failed: " + last_error + "]";
            else if (doomed)
                why = "[subagent stopped: the same tool call failed 3\xc3\x97 "
                      "in a row without converging]";
            else if (turns >= subagent::kMaxTurns)
                why = "[subagent hit its turn budget without producing a final "
                      "report \xe2\x80\x94 the summary below is incomplete]";
            else
                why = "[subagent finished without a final text report]";
            if (salvaged_stale) {
                // We have partial prose from an earlier turn: surface the
                // banner, then the salvaged text, then the activity log.
                report = why + "\n\nLast text the subagent produced (may be an "
                             "early, incomplete note):\n" + report
                       + (log.empty() ? std::string{}
                                      : "\n\nActivity:\n" + log);
            } else {
                report = log.empty() ? why : (why + "\n\nActivity:\n" + log);
            }
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
namespace {

// TRUE when the memory record behind `mem_path` ("memory://<scope>/<id>")
// is ALREADY rendered in the system prompt's <learned-memory> block —
// injecting it again via <retrieved-context> would spend context tokens on
// bytes the model can already see. Mirrors the transport's selection
// exactly (load_recent_* → select_for_prompt) and caches the id set,
// invalidated by the same (size,mtime) stat the RAG epoch uses.
bool memory_fact_in_prompt_(const std::string& mem_path) {
    auto slash = mem_path.rfind('/');
    if (slash == std::string::npos || slash + 1 >= mem_path.size()) return false;
    const std::string id = mem_path.substr(slash + 1);

    static std::mutex mu;
    static std::unordered_set<std::string> prompt_ids;
    static std::uint64_t stamp = ~0ULL;

    std::uint64_t now_stamp = 1469598103934665603ULL;
    auto mix = [&now_stamp](std::uint64_t v) {
        now_stamp = (now_stamp ^ v) * 1099511628211ULL;
    };
    for (auto scope : {memory::Scope::User, memory::Scope::Project}) {
        std::error_code ec;
        auto p = memory::path_for(scope);
        if (p.empty()) continue;
        mix(static_cast<std::uint64_t>(
            fs::exists(p, ec) ? fs::file_size(p, ec) : 0));
        auto t = fs::last_write_time(p, ec);
        mix(ec ? 0ULL
               : static_cast<std::uint64_t>(t.time_since_epoch().count()));
    }

    std::lock_guard<std::mutex> lock(mu);
    if (stamp != now_stamp) {
        stamp = now_stamp;
        prompt_ids.clear();
        for (auto load : {&memory::load_recent_user, &memory::load_recent_project}) {
            auto picked = memory::select_for_prompt(load());
            for (const auto& r : picked.records) prompt_ids.insert(r.id);
        }
    }
    return prompt_ids.count(id) > 0;
}

// Cross-turn de-duplication for PROACTIVE injection. Without this, a stable
// high-confidence corpus re-injects the SAME passages into <retrieved-context>
// on every single turn of a thread — pure context-window spend for zero new
// information (the model already saw them last turn). We remember the keys
// (source:path:line) of recently-injected passages in a bounded FIFO and skip
// any we've surfaced before, so proactive injection only ever spends tokens on
// passages the model hasn't been shown yet this session.
//
// Bounded (kMax) so a long thread can't grow this without limit; once a key
// ages out of the window it MAY be re-injected, which is the correct behaviour
// (it's relevant again and long-since scrolled out of the model's attention).
//
// SPLIT into peek (proactive_seen_) and commit (proactive_mark_injected_):
// proactive_retrieve builds candidate blocks on a WORKER that the caller may
// ABANDON when it blows the latency budget. If the funnel both checked AND
// recorded keys, an abandoned worker would mark passages "injected" that the
// user never saw — permanently suppressing them. So the funnel only PEEKS,
// and the caller commits the surviving keys ONLY when it actually returns the
// hit to the wire. Both share one mutex/FIFO.
namespace {
std::mutex& proactive_dedup_mu_() { static std::mutex mu; return mu; }
std::unordered_set<std::string>& proactive_dedup_seen_() {
    static std::unordered_set<std::string> seen; return seen;
}
std::deque<std::string>& proactive_dedup_fifo_() {
    static std::deque<std::string> fifo; return fifo;
}
constexpr std::size_t kProactiveDedupMax = 256;
}

// PEEK: true if `key` was already injected this session. Does NOT record it.
bool proactive_seen_(const std::string& key) {
    std::lock_guard<std::mutex> lock(proactive_dedup_mu_());
    return proactive_dedup_seen_().count(key) > 0;
}

// COMMIT: record `key` as injected (bounded FIFO eviction). Idempotent.
void proactive_mark_injected_(const std::string& key) {
    std::lock_guard<std::mutex> lock(proactive_dedup_mu_());
    auto& seen = proactive_dedup_seen_();
    if (seen.count(key)) return;
    seen.insert(key);
    auto& fifo = proactive_dedup_fifo_();
    fifo.push_back(key);
    while (fifo.size() > kProactiveDedupMax) {
        seen.erase(fifo.front());
        fifo.pop_front();
    }
}

} // namespace

// Runs the SAME AgenttyDocRetriever the search_docs tool uses, but out of
// band — before the model sees the turn — and only surfaces its result when
// confidence clears a HIGH bar (higher than the tool's LOW floor: we're
// spending the user's context-window tokens unprompted, so the hit must be
// Read the confidence bar for UNPROMPTED injection. The tool's LOW floor is
// 0.25; we inject only well above it. Tunable via AGENTTY_RAG_PROACTIVE_MIN.
namespace {
double proactive_min_conf_() {
    // The confidence is now CRAG's CALIBRATED relevance grade (a real
    // model-free evaluator), not the old raw fusion score. CRAG grades a
    // genuinely-relevant retrieval in the ~0.3–0.6 band, so the unprompted
    // injection bar is 0.35 — high enough to skip noise, low enough that a
    // solidly-relevant hit reaches the model. Tunable via AGENTTY_RAG_PROACTIVE_MIN.
    double min_conf = 0.35;
    if (const char* mc = std::getenv("AGENTTY_RAG_PROACTIVE_MIN"); mc && mc[0]) {
        // Any non-negative value is honoured; a bar above 1.0 is a
        // legitimate "never inject" switch (confidence is clamped to [0,1]).
        try { double v = std::stod(mc); if (v >= 0) min_conf = v; }
        catch (...) { /* keep default */ }
    }
    return min_conf;
}

// The proactive retrieval funnel body: run the retriever, gate on confidence,
// build the fenced <retrieved-context> block, PEEK the cross-turn dedup and
// collect surviving keys on the hit. Does NOT commit the dedup keys — the
// caller commits them only when it actually returns/injects the block, so an
// abandoned or discarded run never suppresses a passage that was never shown.
// Never throws (best-effort).
std::optional<ProactiveHit>
run_proactive_funnel_(const std::string& query, int k, double min_conf) {
  try {
    AgenttyDocRetriever r;
    mt::DocQuery q;
    q.query = query;
    q.k     = k > 0 ? k : 3;
    std::string mode, err;
    // Proactive retrieval is explicit opt-in and already runs on an isolated
    // worker, so use the complete index once. The old cold-index shortcut could
    // silently search only skills/memory and miss the document that triggered
    // the knowledge-shaped query.
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
    std::vector<std::string> keys;
    for (const auto& p : passages) {
        // CONTEXT-ECONOMY: memory facts already rendered in the system
        // prompt's <learned-memory> block would be pure double-spend —
        // the model can see them. select_for_prompt() decides that
        // rendering; mirror it here and drop any memory passage whose
        // record made the prompt cut. (Docs/skills/MCP passages are
        // never in the system prompt — always kept.)
        if (p.source == "memory" && memory_fact_in_prompt_(p.path))
            continue;

        // CROSS-TURN DEDUP: don't re-inject a passage the model was
        // already shown earlier this session — that's context spend for
        // zero new signal. Key on source:path:line so distinct chunks of
        // the same file are treated separately. PEEK only here — the caller
        // commits the surviving keys once it actually injects the block.
        std::string key = (p.source.empty() ? std::string{"docs"} : p.source)
                        + ":" + p.path + ":" + std::to_string(p.line_start);
        if (proactive_seen_(key))
            continue;
        keys.push_back(key);

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

    if (n == 0) return std::nullopt;   // everything deduped away
    return ProactiveHit{std::move(block), conf, n, std::move(keys)};
  } catch (...) {
    return std::nullopt;   // proactive retrieval is best-effort, never fatal
  }
}
} // namespace

// Compatibility entry point. Proactive retrieval is now opt-in and the submit
// path always owns an isolated worker, so there is no reason to start a second
// detached hedge. Execute the funnel exactly once.
std::optional<ProactiveHit> proactive_retrieve(const std::string& query, int k) {
    return proactive_retrieve_blocking(query, k);
}

// Full funnel for the isolated worker owned by the app. Commits dedup keys only
// when a block is actually returned for injection.
std::optional<ProactiveHit>
proactive_retrieve_blocking(const std::string& query, int k) {
    auto hit = run_proactive_funnel_(query, k, proactive_min_conf_());
    if (hit)
        for (const auto& key : hit->dedup_keys)
            proactive_mark_injected_(key);
    return hit;
}

} // namespace agentty::tools
