// agentty::mcp — the bridge: config → mcp::cap providers → agentty ToolDefs.
//
// This is a THIN adapter. All protocol + capability machinery lives in the
// mcp-cpp submodule's capability layer (mcp::cap): spawning servers, driving
// the handshake, listing/calling tools + resources + prompts, namespacing,
// dispatch, and the *_list_changed notifications. agentty only: (1) reads its
// config, (2) builds a cap::Registry of providers, and (3) projects the
// registry's tools / resources / prompts onto agentty's own surfaces so the
// model sees MCP capabilities beside the local ones. The heavy mcp-cpp
// templates stay confined to this one TU.
//
// Protocol coverage (MCP 2025-11-25):
//   • tools/list + tools/call        — wrapped as ToolDefs (always)
//   • tool annotations               — readOnlyHint/destructiveHint → EffectSet
//   • structured + non-text content  — preserved in the rendered output
//   • resources/list + resources/read — `mcp_read_resource` tool + accessors
//   • prompts/list + prompts/get     — `mcp_get_prompt` tool + accessors
//   • tools/list_changed (+ res/prompts) — live snapshot rebuilt on notify
//   • pagination (nextCursor)        — followed in the cap layer
//
// Flow:
//   mcp_tools()
//     → resolve config (.agentty/mcp.json / $AGENTTY_MCP_CONFIG / ~)
//     → for each server: cap::StdioServerProvider (connects synchronously)
//     → cap::Registry fans them in + namespaces collisions
//     → project registry tools/resources/prompts onto agentty ToolDefs
//   The Registry (and its live server connections) live in a process-wide
//   ConnectionPool kept alive by a shared_ptr the execute() closures capture.

#include "agentty/mcp/client.hpp"
#include "agentty/mcp/http_server.hpp"

#include <mcp/cap/cap.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::mcp {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// The keep-alive pool the public PoolHandle points at: it owns the cap
// Registry (which owns the provider shared_ptrs, which own the spawned server
// processes + transports). A mutex guards dispatch since several tool workers
// may call into it concurrently; the Registry routes to per-server providers
// that already serialize their own transport.
struct ConnectionPool {
    ::mcp::cap::Registry registry;
    std::mutex           mu;          // guards registry dispatch + list rebuild
    std::atomic<unsigned long> generation{0};   // bumps on any *_list_changed
};

namespace {

// ── process-wide pool ─────────────────────────────────────────────────────
// mcp_tools() stores the pool here so the resource/prompt/live accessors can
// reach the same connections without threading a handle through every caller.
std::mutex&  g_pool_mu()  { static std::mutex m;            return m; }
PoolHandle&  g_pool_ref() { static PoolHandle p; return p; }

PoolHandle current_pool() {
    std::lock_guard<std::mutex> lk(g_pool_mu());
    return g_pool_ref();
}

std::chrono::milliseconds call_timeout() {
    long ms = 60'000;
    if (const char* e = std::getenv("AGENTTY_MCP_TIMEOUT_MS"); e && e[0]) {
        try { long v = std::stol(e); if (v > 0) ms = v; } catch (...) {}
    }
    return std::chrono::milliseconds{ms};
}

// Resolve the config path per the documented precedence. Empty if none.
fs::path resolve_config() {
    std::error_code ec;
    if (const char* e = std::getenv("AGENTTY_MCP_CONFIG"); e && e[0]) {
        fs::path p{e};
        return fs::is_regular_file(p, ec) ? p : fs::path{};
    }
    if (auto local = fs::path{".agentty"} / "mcp.json"; fs::is_regular_file(local, ec))
        return local;
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        auto user = fs::path{home} / ".agentty" / "mcp.json";
        if (fs::is_regular_file(user, ec)) return user;
    }
    return {};
}

// Build a cap provider from one server config entry. Returns nullptr (and logs)
// on any failure so the caller can skip it.
//
// STDIO transport is POSIX-only: mcp-cpp's StdioServerProvider needs a
// ChildProcess (fork/exec) and only exists when MCP_CAP_HAVE_PROCESS is set
// (__unix__/__APPLE__). On Windows the symbol is absent, so this whole
// function compiles to a graceful "unsupported" stub — HTTP MCP servers,
// which need no child process, still work everywhere.
#if MCP_CAP_HAVE_PROCESS
std::shared_ptr<::mcp::cap::CapabilityProvider>
make_provider(const std::string& name, const json& spec) {
    const std::string command = spec.value("command", std::string{});
    if (command.empty()) {
        std::fprintf(stderr, "mcp: server '%s' has no \"command\"\n", name.c_str());
        return nullptr;
    }
    ::mcp::cap::StdioServerProvider::Config cfg;
    cfg.name           = name;
    cfg.spawn.command  = command;
    if (spec.contains("args") && spec["args"].is_array())
        for (const auto& a : spec["args"]) cfg.spawn.args.push_back(a.get<std::string>());
    if (spec.contains("env") && spec["env"].is_object())
        for (auto it = spec["env"].begin(); it != spec["env"].end(); ++it)
            cfg.spawn.env_kv.push_back(it.key() + "=" + it.value().get<std::string>());
    cfg.client_info  = ::mcp::Implementation{"agentty", AGENTTY_VERSION};
    cfg.call_timeout = call_timeout();

    try {
        return std::make_shared<::mcp::cap::StdioServerProvider>(std::move(cfg));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mcp: server '%s' failed: %s\n", name.c_str(), e.what());
        return nullptr;
    } catch (...) {
        std::fprintf(stderr, "mcp: server '%s' failed (unknown)\n", name.c_str());
        return nullptr;
    }
}
#else
std::shared_ptr<::mcp::cap::CapabilityProvider>
make_provider(const std::string& name, const json& /*spec*/) {
    std::fprintf(stderr,
        "mcp: server '%s' uses stdio transport, which is unsupported on this "
        "platform (no child-process spawn); use an http/sse server instead\n",
        name.c_str());
    return nullptr;
}
#endif // MCP_CAP_HAVE_PROCESS

// ── effects from tool annotations ─────────────────────────────────────────
// A remote tool's annotations tell us how dangerous it is. The MCP spec
// (Tool.annotations) is explicit that these are UNTRUSTED HINTS from the
// server, so we are conservative: a tool is treated as read-only ONLY when it
// affirmatively says readOnlyHint:true AND does NOT also claim destructive.
// Everything else gets the full effect set (always asks permission) — the
// safe default that the original bridge applied unconditionally.
tools::EffectSet effects_for(const ::mcp::Tool& t) {
    using tools::Effect;
    const tools::EffectSet full{Effect::Exec, Effect::WriteFs, Effect::Net, Effect::ReadFs};
    if (!t.annotations.has_value()) return full;
    const auto& a = *t.annotations;
    const bool read_only   = a.readOnlyHint.has_value()    && *a.readOnlyHint;
    const bool destructive = a.destructiveHint.has_value() && *a.destructiveHint;
    if (read_only && !destructive) {
        // Read-only remote tool: it observes the world but does not mutate it.
        // Model it as ReadFs|Net (the two non-exclusive, permission-free
        // effects) so the permission policy treats it like `read`/`grep`.
        return tools::EffectSet{Effect::ReadFs, Effect::Net};
    }
    return full;
}

// ── render a cap::Result into agentty tool text ───────────────────────────
// The cap layer already flattened text blocks into r.text and preserved the
// original content blocks in r.raw + structuredContent in r.structured. We
// surface ALL of it: text verbatim, then a compact summary of any non-text
// blocks (image/audio/resource) the model can't otherwise see, then the
// structured JSON when present. This is what makes structured + multimodal
// MCP results usable instead of silently dropped.
std::string render_result(const ::mcp::cap::Result& r) {
    std::string out = r.text;

    // Non-text content blocks (image/audio/embedded resource). r.text already
    // included a one-line "[type] uri" stub for each via result_from_call, so
    // here we only append richer detail the model benefits from: mime + size.
    if (r.raw.is_array()) {
        for (const auto& b : r.raw) {
            if (!b.is_object()) continue;
            const auto type = b.value("type", std::string{});
            if (type == "image" || type == "audio") {
                const auto mime = b.value("mimeType", std::string{});
                std::size_t bytes = 0;
                if (auto it = b.find("data"); it != b.end() && it->is_string())
                    bytes = it->get<std::string>().size();   // base64 length
                if (out.empty() || out.back() != '\n') out += '\n';
                out += "[" + type + (mime.empty() ? "" : " " + mime) + ", ~" +
                       std::to_string(bytes) + "B base64]\n";
            }
        }
    }

    // Structured output (MCP structuredContent) — a typed payload alongside
    // the human text. Emit it as a fenced JSON block so the model can parse it.
    if (r.structured.is_object() && !r.structured.empty()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "```json\n" + r.structured.dump(2) + "\n```\n";
    } else if (!r.structured.is_object() && !r.structured.is_null()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "```json\n" + r.structured.dump(2) + "\n```\n";
    }
    return out;
}

// Synthesize a ToolDef that routes through the shared registry by EXPOSED name.
tools::ToolDef make_tool(PoolHandle pool, const ::mcp::Tool& t) {
    tools::ToolDef def;
    def.name = ToolName{t.name};   // already namespaced by the registry

    std::string desc = t.description.has_value() ? *t.description : std::string{};
    def.description = "[MCP] " +
        (desc.empty() ? ("Remote MCP tool '" + t.name + "'.") : desc);

    json schema = ::mcp::to_json(t.inputSchema);
    if (!schema.is_object()) schema = json::object();
    if (!schema.contains("type")) schema["type"] = "object";
    if (!schema.contains("properties")) schema["properties"] = json::object();
    def.input_schema = std::move(schema);

    def.effects = effects_for(t);

    const std::string exposed = t.name;
    def.execute = [pool, exposed](const json& args) -> tools::ExecResult {
        try {
            ::mcp::cap::Result r;
            {
                std::lock_guard<std::mutex> lk(pool->mu);
                r = pool->registry.dispatch(exposed, args);
            }
            if (r.is_error)
                return std::unexpected(tools::ToolError::subprocess(
                    r.text.empty() ? "MCP tool reported an error" : r.text));
            std::string text = render_result(r);
            return tools::ToolOutput{text.empty() ? "(no output)" : text, std::nullopt};
        } catch (const std::exception& e) {
            return std::unexpected(tools::ToolError::subprocess(
                std::string{"MCP call failed: "} + e.what()));
        } catch (...) {
            return std::unexpected(tools::ToolError::subprocess("MCP call failed"));
        }
    };
    return def;
}

// ── built-in resource/prompt access tools ─────────────────────────────────
// When any connected server exposes resources or prompts, we add ONE generic
// tool for each so the model can list+read resources / render prompts without
// us needing a tool per URI. They route through the same pool.

tools::ToolDef make_read_resource_tool(PoolHandle pool) {
    tools::ToolDef def;
    def.name        = ToolName{"mcp_read_resource"};
    def.description =
        "[MCP] Read the contents of an MCP resource by URI. Resources are "
        "server-provided documents/data (files, DB rows, API docs). Call with "
        "no args (or {\"list\":true}) to LIST every available resource and its "
        "URI; call with {\"uri\":\"...\"} to read one. Read-only.";
    def.input_schema = json{
        {"type", "object"},
        {"properties", json{
            {"uri",  json{{"type", "string"}, {"description", "Resource URI to read. Omit to list all resources."}}},
            {"list", json{{"type", "boolean"}, {"description", "List all available resources instead of reading."}}},
        }},
    };
    def.effects = tools::EffectSet{tools::Effect::ReadFs, tools::Effect::Net};
    def.execute = [pool](const json& args) -> tools::ExecResult {
        const std::string uri = args.is_object() ? args.value("uri", std::string{}) : std::string{};
        const bool want_list  = uri.empty() || (args.is_object() && args.value("list", false));
        try {
            if (want_list) {
                std::lock_guard<std::mutex> lk(pool->mu);
                auto res = pool->registry.resources();
                auto tpls = pool->registry.resource_templates();
                if (res.empty() && tpls.empty())
                    return tools::ToolOutput{"(no resources advertised)", std::nullopt};
                std::string out = "Available MCP resources:\n";
                for (const auto& r : res) {
                    out += "  " + r.uri;
                    if (r.title.has_value() && !r.title->empty()) out += "  — " + *r.title;
                    else if (!r.name.empty())                     out += "  — " + r.name;
                    if (r.mimeType.has_value() && !r.mimeType->empty()) out += " [" + *r.mimeType + "]";
                    out += '\n';
                }
                for (const auto& tp : tpls) {
                    out += "  " + tp.uriTemplate + "  (template";
                    if (tp.name.empty()) out += ")"; else out += ": " + tp.name + ")";
                    out += '\n';
                }
                return tools::ToolOutput{out, std::nullopt};
            }
            std::vector<::mcp::ResourceContents> contents;
            std::string err;
            bool ok;
            {
                std::lock_guard<std::mutex> lk(pool->mu);
                ok = pool->registry.read_resource(uri, contents, err);
            }
            if (!ok)
                return std::unexpected(tools::ToolError::subprocess(
                    err.empty() ? "resources/read failed" : err));
            std::string out;
            for (const auto& c : contents) {
                std::visit([&](const auto& rc) {
                    using T = std::decay_t<decltype(rc)>;
                    if constexpr (std::is_same_v<T, ::mcp::TextResourceContents>) {
                        out += rc.text;
                        if (!out.empty() && out.back() != '\n') out += '\n';
                    } else {
                        out += "[blob " +
                               (rc.mimeType.has_value() ? *rc.mimeType : std::string{"application/octet-stream"}) +
                               ", ~" + std::to_string(rc.blob.size()) + "B base64]\n";
                    }
                }, c);
            }
            return tools::ToolOutput{out.empty() ? "(empty resource)" : out, std::nullopt};
        } catch (const std::exception& e) {
            return std::unexpected(tools::ToolError::subprocess(
                std::string{"resource read failed: "} + e.what()));
        } catch (...) {
            return std::unexpected(tools::ToolError::subprocess("resource read failed"));
        }
    };
    return def;
}

std::string render_prompt(const ::mcp::GetPromptResult& r) {
    std::string out;
    if (r.description.has_value() && !r.description->empty())
        out += "# " + *r.description + "\n\n";
    for (const auto& m : r.messages) {
        const auto role = ::mcp::to_json(m.role).get<std::string>();
        json cb = ::mcp::to_json(m.content);
        std::string body;
        if (cb.is_object() && cb.value("type", std::string{}) == "text")
            body = cb.value("text", std::string{});
        else
            body = cb.dump();
        out += role + ": " + body + "\n\n";
    }
    return out;
}

tools::ToolDef make_get_prompt_tool(PoolHandle pool) {
    tools::ToolDef def;
    def.name        = ToolName{"mcp_get_prompt"};
    def.description =
        "[MCP] Render an MCP prompt template provided by a server. Call with no "
        "args (or {\"list\":true}) to LIST every prompt, its name, and its "
        "arguments; call with {\"name\":\"...\",\"arguments\":{...}} to render "
        "one into ready-to-use messages. Read-only.";
    def.input_schema = json{
        {"type", "object"},
        {"properties", json{
            {"name",      json{{"type", "string"}, {"description", "Prompt name to render. Omit to list all prompts."}}},
            {"arguments", json{{"type", "object"}, {"description", "name→value map for the prompt's template arguments."}}},
            {"list",      json{{"type", "boolean"}, {"description", "List all available prompts instead of rendering."}}},
        }},
    };
    def.effects = tools::EffectSet{tools::Effect::ReadFs, tools::Effect::Net};
    def.execute = [pool](const json& args) -> tools::ExecResult {
        const std::string name = args.is_object() ? args.value("name", std::string{}) : std::string{};
        const bool want_list   = name.empty() || (args.is_object() && args.value("list", false));
        try {
            if (want_list) {
                std::lock_guard<std::mutex> lk(pool->mu);
                auto prompts = pool->registry.prompts();
                if (prompts.empty())
                    return tools::ToolOutput{"(no prompts advertised)", std::nullopt};
                std::string out = "Available MCP prompts:\n";
                for (const auto& p : prompts) {
                    out += "  " + p.name;
                    if (p.description.has_value() && !p.description->empty())
                        out += "  — " + *p.description;
                    out += '\n';
                    if (p.arguments.has_value())
                        for (const auto& a : *p.arguments) {
                            out += "      - " + a.name;
                            if (a.required.has_value() && *a.required) out += " (required)";
                            if (a.description.has_value() && !a.description->empty())
                                out += ": " + *a.description;
                            out += '\n';
                        }
                }
                return tools::ToolOutput{out, std::nullopt};
            }
            std::vector<std::pair<std::string, std::string>> kv;
            if (args.is_object() && args.contains("arguments") && args["arguments"].is_object())
                for (auto it = args["arguments"].begin(); it != args["arguments"].end(); ++it)
                    kv.emplace_back(it.key(), it.value().is_string()
                                                  ? it.value().get<std::string>()
                                                  : it.value().dump());
            ::mcp::GetPromptResult res;
            std::string err;
            bool ok;
            {
                std::lock_guard<std::mutex> lk(pool->mu);
                ok = pool->registry.get_prompt(name, kv, res, err);
            }
            if (!ok)
                return std::unexpected(tools::ToolError::subprocess(
                    err.empty() ? "prompts/get failed" : err));
            std::string out = render_prompt(res);
            return tools::ToolOutput{out.empty() ? "(empty prompt)" : out, std::nullopt};
        } catch (const std::exception& e) {
            return std::unexpected(tools::ToolError::subprocess(
                std::string{"prompt render failed: "} + e.what()));
        } catch (...) {
            return std::unexpected(tools::ToolError::subprocess("prompt render failed"));
        }
    };
    return def;
}

// Build the full ToolDef vector for a pool: every server tool + the generic
// resource/prompt access tools (only when the union exposes any).
std::vector<tools::ToolDef> project_tools(PoolHandle pool) {
    std::vector<tools::ToolDef> out;
    bool any_resources = false, any_prompts = false;
    {
        std::lock_guard<std::mutex> lk(pool->mu);
        for (auto& t : pool->registry.tools()) out.push_back(make_tool(pool, t));
        any_resources = !pool->registry.resources().empty() ||
                        !pool->registry.resource_templates().empty();
        any_prompts   = !pool->registry.prompts().empty();
    }
    if (any_resources) out.push_back(make_read_resource_tool(pool));
    if (any_prompts)   out.push_back(make_get_prompt_tool(pool));
    return out;
}

} // namespace

bool mcp_config_present() { return !resolve_config().empty(); }

std::vector<tools::ToolDef> mcp_tools(PoolHandle& out_pool) {
    std::vector<tools::ToolDef> out;
    fs::path cfg = resolve_config();
    if (cfg.empty()) return out;          // no config → zero work, zero tools

    json doc;
    try {
        std::ifstream f(cfg);
        f >> doc;
    } catch (const std::exception& e) {
        // cfg.c_str() is wchar_t* on Windows — narrow it for %s.
        std::fprintf(stderr, "mcp: failed to parse %s: %s\n",
                     cfg.string().c_str(), e.what());
        return out;
    }

    const json* servers = nullptr;
    if (doc.contains("mcpServers") && doc["mcpServers"].is_object())
        servers = &doc["mcpServers"];
    else if (doc.contains("servers") && doc["servers"].is_object())
        servers = &doc["servers"];
    if (!servers) return out;

    auto pool = std::make_shared<ConnectionPool>();
    // tools/list_changed (+ resources/prompts) from any server bumps the
    // pool generation. Callers compare mcp_generation() to know the snapshot
    // moved; the dispatch path keeps working regardless (it routes by name).
    pool->registry.set_on_list_changed([wp = std::weak_ptr<ConnectionPool>(pool)] {
        if (auto p = wp.lock())
            p->generation.fetch_add(1, std::memory_order_relaxed);
    });

    for (auto it = servers->begin(); it != servers->end(); ++it) {
        const std::string& sname = it.key();
        const json& spec = it.value();
        // A server entry with a "url" (or type:"http"/"sse") is a remote
        // Streamable HTTP server; anything with a "command" is a spawned
        // stdio server. URL wins when both are present.
        const std::string url  = spec.value("url", std::string{});
        const std::string type = spec.value("type", std::string{});
        const bool is_http = !url.empty() || type == "http" || type == "sse"
                             || type == "streamable-http";
        std::shared_ptr<::mcp::cap::CapabilityProvider> p;
        if (is_http) {
            HttpConfig hc;
            hc.url = url;
            if (spec.contains("headers") && spec["headers"].is_object())
                for (auto h = spec["headers"].begin(); h != spec["headers"].end(); ++h)
                    hc.headers.emplace_back(h.key(), h.value().is_string()
                                                         ? h.value().get<std::string>()
                                                         : h.value().dump());
            hc.call_timeout = call_timeout();
            std::string err;
            p = make_http_provider(sname, hc, err);
            if (!p) std::fprintf(stderr, "mcp: %s\n", err.c_str());
        } else {
            p = make_provider(sname, spec);
        }
        if (p) {
            std::fprintf(stderr,
                "mcp: server '%s' connected (%zu tools, %zu resources, %zu prompts)\n",
                sname.c_str(), p->list().size(), p->resources().size(), p->prompts().size());
            pool->registry.add(std::move(p));
        }
    }
    if (pool->registry.provider_count() == 0) return out;   // nothing connected

    out_pool = pool;                       // keep providers alive (caller)
    {
        std::lock_guard<std::mutex> lk(g_pool_mu());
        g_pool_ref() = pool;               // process-wide handle for accessors
    }
    return project_tools(pool);
}

// ── live / dynamic accessors ──────────────────────────────────────────────

std::vector<tools::ToolDef> mcp_tools_live() {
    auto pool = current_pool();
    if (!pool) return {};
    return project_tools(pool);
}

unsigned long mcp_generation() noexcept {
    auto pool = current_pool();
    if (!pool) return 0;
    return pool->generation.load(std::memory_order_relaxed);
}

std::vector<ResourceInfo> mcp_resources() {
    auto pool = current_pool();
    if (!pool) return {};
    std::vector<ResourceInfo> out;
    std::lock_guard<std::mutex> lk(pool->mu);
    for (const auto& r : pool->registry.resources()) {
        ResourceInfo ri;
        ri.uri         = r.uri;
        ri.name        = r.name;
        ri.title       = r.title.has_value() ? *r.title : r.name;
        ri.description = r.description.has_value() ? *r.description : std::string{};
        ri.mime_type   = r.mimeType.has_value() ? *r.mimeType : std::string{};
        out.push_back(std::move(ri));
    }
    return out;
}

std::optional<std::string> mcp_read_resource(const std::string& uri, std::string& err) {
    auto pool = current_pool();
    if (!pool) { err = "MCP not configured"; return std::nullopt; }
    std::vector<::mcp::ResourceContents> contents;
    {
        std::lock_guard<std::mutex> lk(pool->mu);
        if (!pool->registry.read_resource(uri, contents, err)) return std::nullopt;
    }
    std::string out;
    for (const auto& c : contents) {
        std::visit([&](const auto& rc) {
            using T = std::decay_t<decltype(rc)>;
            if constexpr (std::is_same_v<T, ::mcp::TextResourceContents>) {
                out += rc.text;
                if (!out.empty() && out.back() != '\n') out += '\n';
            } else {
                out += "[blob " +
                       (rc.mimeType.has_value() ? *rc.mimeType : std::string{"application/octet-stream"}) +
                       ", ~" + std::to_string(rc.blob.size()) + "B base64]\n";
            }
        }, c);
    }
    return out;
}

std::vector<PromptInfo> mcp_prompts() {
    auto pool = current_pool();
    if (!pool) return {};
    std::vector<PromptInfo> out;
    std::lock_guard<std::mutex> lk(pool->mu);
    for (const auto& p : pool->registry.prompts()) {
        PromptInfo pi;
        pi.name        = p.name;
        pi.title       = p.title.has_value() ? *p.title : p.name;
        pi.description = p.description.has_value() ? *p.description : std::string{};
        if (p.arguments.has_value())
            for (const auto& a : *p.arguments)
                pi.arguments.push_back(PromptArgInfo{
                    a.name,
                    a.description.has_value() ? *a.description : std::string{},
                    a.required.has_value() && *a.required});
        out.push_back(std::move(pi));
    }
    return out;
}

std::optional<std::string> mcp_get_prompt(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& args,
    std::string& err) {
    auto pool = current_pool();
    if (!pool) { err = "MCP not configured"; return std::nullopt; }
    ::mcp::GetPromptResult res;
    {
        std::lock_guard<std::mutex> lk(pool->mu);
        if (!pool->registry.get_prompt(name, args, res, err)) return std::nullopt;
    }
    return render_prompt(res);
}

} // namespace agentty::mcp
