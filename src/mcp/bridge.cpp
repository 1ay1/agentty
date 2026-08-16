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
#include "agentty/tool/util/fs_helpers.hpp"
#include "agentty/util/dbglog.hpp"

#include <mcp/cap/cap.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
struct ServerPolicy {
    bool trust_annotations = false;
    int max_output_chars = 30'000;
    std::unordered_set<std::string> include;
    std::unordered_set<std::string> exclude;
    std::unordered_set<std::string> pin;
};

struct ConnectionPool {
    // External capabilities are always namespaced. Conditional namespacing
    // makes a tool's wire identity change when another server is enabled and
    // cannot detect collisions with agentty's native catalog.
    ::mcp::cap::Registry registry{true};
    std::mutex           mu;          // guards list/resource/prompt projection
    std::atomic<unsigned long> generation{0};   // bumps on any *_list_changed
    std::unordered_map<std::string, ServerPolicy> policies;
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
        try { long v = std::stol(e); if (v > 0) ms = v; }
        catch (const std::exception& ex) { util::dbglog("mcp.call_timeout.env", ex.what()); }
        catch (...) { util::dbglog("mcp.call_timeout.env", "non-std exception"); }
    }
    return std::chrono::milliseconds{ms};
}

// Resolve the config path per the documented precedence. Empty if none.
// `out_project_local` is set true when the config came from the WORKSPACE
// (./.agentty/mcp.json) — which can ride in on a cloned repo — vs. an
// explicitly-pointed ($AGENTTY_MCP_CONFIG) or user-global (~/.agentty)
// config the user themselves placed. Project-local stdio servers spawn
// arbitrary commands, so they're gated behind an opt-in (see mcp_tools).
fs::path resolve_config(bool& out_project_local) {
    out_project_local = false;
    std::error_code ec;
    if (const char* e = std::getenv("AGENTTY_MCP_CONFIG"); e && e[0]) {
        fs::path p{e};
        return fs::is_regular_file(p, ec) ? p : fs::path{};
    }
    if (auto local = fs::path{".agentty"} / "mcp.json"; fs::is_regular_file(local, ec)) {
        out_project_local = true;
        return local;
    }
    if (const char* home = std::getenv("HOME"); home && home[0]) {
        auto user = fs::path{home} / ".agentty" / "mcp.json";
        if (fs::is_regular_file(user, ec)) return user;
    }
    return {};
}

// Convenience overload for callers that don't care about the trust source.
fs::path resolve_config() {
    bool ignore = false;
    return resolve_config(ignore);
}

// Build a cap provider from one server config entry. Returns nullptr (and logs)
// on any failure so the caller can skip it.
//
// STDIO transport spawns the server as a child process. This is supported on
// every platform: POSIX via fork/exec, Windows via CreateProcess (mcp-cpp's
// ChildProcess, MCP_CAP_HAVE_PROCESS). HTTP/SSE servers take the
// make_http_provider path instead.
std::shared_ptr<::mcp::cap::CapabilityProvider>
make_provider(const std::string& name, const json& spec) {
    const std::string command = spec.value("command", std::string{});
    if (command.empty()) {
        std::fprintf(stderr, "mcp: server '%s' has no \"command\"\n", name.c_str());
        return nullptr;
    }
    // ── Pre-flight: resolve the command BEFORE forking + exec ────────────
    // A missing binary otherwise fails deep in the child
    // (execvp -> "mcp::cap: exec '…' failed: No such file or directory"),
    // whose stderr can bleed onto the TUI and whose failure recurs every
    // connect. Resolve it up front and skip the server with ONE clear line
    // if it doesn't exist — no fork, no exec, no terminal corruption.
    {
        const bool has_slash = command.find('/') != std::string::npos
#ifdef _WIN32
                            || command.find('\\') != std::string::npos
#endif
            ;
        bool resolvable = false;
        std::error_code ec;
        if (has_slash) {
            // Explicit path (absolute or relative): must exist + be a file.
            resolvable = fs::exists(command, ec) && !fs::is_directory(command, ec);
        } else {
            // Bare name: search PATH, like execvp will.
            if (const char* path = std::getenv("PATH"); path && *path) {
                std::string p(path);
                std::size_t start = 0;
                while (start <= p.size() && !resolvable) {
                    std::size_t sep = p.find(':', start);
                    std::string dir = p.substr(start,
                        sep == std::string::npos ? std::string::npos : sep - start);
                    if (!dir.empty()) {
                        fs::path cand = fs::path{dir} / command;
                        if (fs::exists(cand, ec) && !fs::is_directory(cand, ec))
                            resolvable = true;
                    }
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
            }
        }
        if (!resolvable) {
            std::fprintf(stderr,
                "mcp: server '%s' skipped — command not found: %s\n"
                "     (check the path in mcp.json; use an ABSOLUTE path for a "
                "local build)\n",
                name.c_str(), command.c_str());
            return nullptr;
        }
    }
    ::mcp::cap::StdioServerProvider::Config cfg;
    cfg.name           = name;
    cfg.spawn.command  = command;
    // Coerce non-string args/env elements instead of throwing: a single
    // numeric/bool element used to make `.get<std::string>()` throw
    // json::type_error and skip the WHOLE server. Strings pass through
    // verbatim; anything else is stringified via dump() so e.g.
    // `"args": ["--port", 8080]` works as the user clearly intended.
    auto as_str = [](const json& v) {
        return v.is_string() ? v.get<std::string>() : v.dump();
    };
    if (spec.contains("args") && spec["args"].is_array())
        for (const auto& a : spec["args"]) cfg.spawn.args.push_back(as_str(a));
    if (spec.contains("env") && spec["env"].is_object())
        for (auto it = spec["env"].begin(); it != spec["env"].end(); ++it)
            cfg.spawn.env_kv.push_back(it.key() + "=" + as_str(it.value()));
    cfg.client_info  = ::mcp::Implementation{"agentty", AGENTTY_VERSION};
    cfg.call_timeout = call_timeout();
    if (const auto ms = spec.value("timeoutMs", 0L); ms > 0)
        cfg.call_timeout = std::chrono::milliseconds{ms};
    if (const auto ms = spec.value("connectTimeoutMs", 0L); ms > 0)
        cfg.handshake_timeout = std::chrono::milliseconds{ms};

    const std::string root = tools::util::workspace_root().generic_string();
    if (!root.empty()) {
        std::string uri = "file://" + root;
#ifdef _WIN32
        if (root.size() > 1 && root[1] == ':') uri = "file:///" + root;
#endif
        cfg.integration.roots.push_back(
            ::mcp::Root{std::move(uri), std::string{"workspace"}, json::object()});
    }

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

// ── effects from tool annotations ─────────────────────────────────────────
// A remote tool's annotations tell us how dangerous it is. The MCP spec
// (Tool.annotations) is explicit that these are UNTRUSTED HINTS from the
// server, so we are conservative: a tool is treated as read-only ONLY when it
// affirmatively says readOnlyHint:true AND does NOT also claim destructive.
// Everything else gets the full effect set (always asks permission) — the
// safe default that the original bridge applied unconditionally.
tools::EffectSet effects_for(const ::mcp::Tool& t, bool trust_annotations) {
    using tools::Effect;
    const tools::EffectSet full{Effect::Exec, Effect::WriteFs, Effect::Net, Effect::ReadFs};
    // An annotation is an untrusted server hint. Only an explicitly trusted
    // server may use it to reduce permission/scheduling effects.
    if (!trust_annotations || !t.annotations.has_value()) return full;
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

std::string canonical_mcp_name(std::string_view exposed) {
    // Registry names are `mcp:<server>__<tool>` when always_namespace=true.
    // Provider APIs accept only a conservative identifier alphabet, so map
    // punctuation to underscores and retain the raw route only in execute().
    if (exposed.starts_with("mcp:")) exposed.remove_prefix(4);
    std::string out = "mcp__";
    out.reserve(out.size() + exposed.size());
    for (unsigned char c : exposed)
        out.push_back((std::isalnum(c) || c == '_' || c == '-')
                          ? static_cast<char>(c)
                          : '_');
    constexpr std::size_t kMaxToolName = 128;
    if (out.size() > kMaxToolName) out.resize(kMaxToolName);
    return out;
}

std::string mcp_origin_id(std::string_view exposed) {
    if (exposed.starts_with("mcp:")) exposed.remove_prefix(4);
    if (auto split = exposed.find("__"); split != std::string_view::npos)
        exposed = exposed.substr(0, split);
    return std::string{exposed};
}

std::string mcp_bare_name(std::string_view exposed) {
    if (exposed.starts_with("mcp:")) exposed.remove_prefix(4);
    if (auto split = exposed.find("__"); split != std::string_view::npos)
        exposed.remove_prefix(split + 2);
    return std::string{exposed};
}

// Synthesize a ToolDef that routes through the shared registry by EXPOSED name.
tools::ToolDef make_tool(PoolHandle pool, const ::mcp::Tool& t,
                         const ServerPolicy& policy) {
    tools::ToolDef def;
    const std::string exposed = t.name;
    def.name = ToolName{canonical_mcp_name(exposed)};
    def.origin = tools::ToolOrigin::Mcp;
    def.origin_id = mcp_origin_id(exposed);

    std::string desc = t.description.has_value() ? *t.description : std::string{};
    def.description = "[MCP " + def.origin_id + "] " +
        (desc.empty() ? ("Remote tool '" + def.name.value + "'.") : desc);

    json schema = ::mcp::to_json(t.inputSchema);
    if (!schema.is_object()) schema = json::object();
    if (!schema.contains("type")) schema["type"] = "object";
    if (!schema.contains("properties")) schema["properties"] = json::object();
    def.input_schema = std::move(schema);

    def.effects = effects_for(t, policy.trust_annotations);
    def.scheduling_effects = def.effects;
    def.max_output_chars = std::clamp(policy.max_output_chars, 2'000, 100'000);
    def.always_expose = policy.pin.contains(mcp_bare_name(exposed));
    def.output_truncation = tools::OutputTruncation::HeadTail;

    def.execute = [pool, exposed](const json& args) -> tools::ExecResult {
        try {
            ::mcp::cap::Result r;
            // Registry dispatch resolves a provider through its own
            // generation-safe route snapshot; independent servers may run in
            // parallel and each provider serializes only its own transport.
            r = pool->registry.dispatch(::mcp::cap::Request{
                exposed, args, tools::progress::current(),
                tools::cancellation::current()});
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
    def.origin = tools::ToolOrigin::Mcp;
    def.origin_id = "resources";
    def.scheduling_effects = def.effects;
    def.max_output_chars = 30'000;
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

std::optional<std::string> resolve_prompt_route(
    const ::mcp::cap::Registry& registry, std::string_view requested) {
    std::optional<std::string> match;
    for (const auto& prompt : registry.prompts()) {
        const std::string canonical = canonical_mcp_name(prompt.name);
        const bool bare_match = prompt.name.size() > requested.size() + 2
            && prompt.name.ends_with("__" + std::string{requested});
        if (prompt.name == requested || canonical == requested || bare_match) {
            if (match && *match != prompt.name) return std::nullopt;
            match = prompt.name;
        }
    }
    return match;
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
    def.origin = tools::ToolOrigin::Mcp;
    def.origin_id = "prompts";
    def.scheduling_effects = def.effects;
    def.max_output_chars = 30'000;
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
                    out += "  " + canonical_mcp_name(p.name);
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
                auto route = resolve_prompt_route(pool->registry, name);
                if (!route)
                    return std::unexpected(tools::ToolError::not_found(
                        "MCP prompt is missing or ambiguous: " + name));
                ok = pool->registry.get_prompt(*route, kv, res, err);
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

std::optional<std::string> resolve_tool_route(
    const ::mcp::cap::Registry& registry, std::string_view requested) {
    std::optional<std::string> match;
    for (const auto& tool : registry.tools()) {
        const std::string canonical = canonical_mcp_name(tool.name);
        const bool bare_match = tool.name.size() > requested.size() + 2
            && tool.name.ends_with("__" + std::string{requested});
        if (tool.name == requested || canonical == requested || bare_match) {
            if (match && *match != tool.name) return std::nullopt;
            match = tool.name;
        }
    }
    return match;
}

tools::ToolDef make_search_tools_tool(PoolHandle pool) {
    tools::ToolDef def;
    def.name = ToolName{"mcp_search_tools"};
    def.description =
        "Search connected MCP capability catalogs by meaning. Use this when "
        "the needed integration is not already visible as a direct mcp__ tool; "
        "then invoke it through mcp_call.";
    def.input_schema = json{
        {"type", "object"},
        {"properties", json{
            {"query", json{{"type", "string"}, {"description", "Capability or action to find."}}},
            {"k", json{{"type", "integer"}, {"minimum", 1}, {"maximum", 20}, {"default", 8}}},
        }},
        {"required", json::array({"query"})},
    };
    def.origin = tools::ToolOrigin::Mcp;
    def.origin_id = "catalog";
    def.effects = tools::EffectSet{};
    def.scheduling_effects = tools::EffectSet{};
    def.max_output_chars = 12'000;
    def.always_expose = true;
    def.execute = [pool](const json& args) -> tools::ExecResult {
        const std::string query = args.value("query", std::string{});
        if (query.empty())
            return std::unexpected(tools::ToolError::invalid_args("query is required"));
        const int k = std::clamp(args.value("k", 8), 1, 20);
        std::string needle = query;
        std::ranges::transform(needle, needle.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::vector<std::pair<int, ::mcp::Tool>> ranked;
        for (auto tool : pool->registry.tools()) {
            std::string text = tool.name + " " + tool.description.value_or("");
            std::ranges::transform(text, text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            int score = text.find(needle) != std::string::npos ? 20 : 0;
            std::size_t pos = 0;
            while (pos < needle.size()) {
                while (pos < needle.size() && !std::isalnum(static_cast<unsigned char>(needle[pos]))) ++pos;
                auto begin = pos;
                while (pos < needle.size() && std::isalnum(static_cast<unsigned char>(needle[pos]))) ++pos;
                if (pos - begin > 1 && text.find(needle.substr(begin, pos - begin)) != std::string::npos)
                    score += 3;
            }
            ranked.emplace_back(score, std::move(tool));
        }
        std::stable_sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string out;
        for (int i = 0; i < k && i < static_cast<int>(ranked.size()); ++i) {
            const auto& tool = ranked[static_cast<std::size_t>(i)].second;
            out += "- `" + canonical_mcp_name(tool.name) + "`";
            if (tool.description && !tool.description->empty()) out += " — " + *tool.description;
            out += '\n';
        }
        return tools::ToolOutput{out.empty() ? "(no MCP tools available)" : out, std::nullopt};
    };
    return def;
}

tools::ToolDef make_call_tool(PoolHandle pool) {
    tools::ToolDef def;
    def.name = ToolName{"mcp_call"};
    def.description =
        "Call a connected MCP tool discovered with mcp_search_tools. Direct "
        "mcp__ tools are preferred when visible because their typed schema is safer.";
    def.input_schema = json{
        {"type", "object"},
        {"properties", json{
            {"name", json{{"type", "string"}, {"description", "Canonical mcp__server__tool name."}}},
            {"arguments", json{{"type", "object"}, {"description", "Arguments for the target tool."}}},
        }},
        {"required", json::array({"name", "arguments"})},
    };
    def.origin = tools::ToolOrigin::Mcp;
    def.origin_id = "catalog";
    def.effects = tools::EffectSet{tools::Effect::Exec, tools::Effect::WriteFs,
                                   tools::Effect::Net, tools::Effect::ReadFs};
    def.scheduling_effects = tools::EffectSet{tools::Effect::Exec};
    def.max_output_chars = 30'000;
    def.always_expose = true;
    def.execute = [pool](const json& args) -> tools::ExecResult {
        const std::string name = args.value("name", std::string{});
        const json call_args = args.contains("arguments") && args["arguments"].is_object()
            ? args["arguments"] : json::object();
        auto route = resolve_tool_route(pool->registry, name);
        if (!route)
            return std::unexpected(tools::ToolError::not_found(
                "MCP tool is missing or ambiguous: " + name));
        auto result = pool->registry.dispatch(::mcp::cap::Request{
            *route, call_args, tools::progress::current(),
            tools::cancellation::current()});
        if (result.is_error)
            return std::unexpected(tools::ToolError::subprocess(
                result.text.empty() ? "MCP tool reported an error" : result.text));
        auto text = render_result(result);
        return tools::ToolOutput{text.empty() ? "(no output)" : text, std::nullopt};
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
        for (auto& t : pool->registry.tools()) {
            const auto origin = mcp_origin_id(t.name);
            const auto bare = mcp_bare_name(t.name);
            const auto policy_it = pool->policies.find(origin);
            const ServerPolicy fallback;
            const auto& policy = policy_it == pool->policies.end()
                ? fallback : policy_it->second;
            if ((!policy.include.empty() && !policy.include.contains(bare))
                || policy.exclude.contains(bare))
                continue;
            out.push_back(make_tool(pool, t, policy));
        }
        any_resources = !pool->registry.resources().empty() ||
                        !pool->registry.resource_templates().empty();
        any_prompts   = !pool->registry.prompts().empty();
    }
    // mcp_search_tools / mcp_call are the on-demand discovery layer: they let
    // the model find MCP tools that AREN'T shipped inline. They only earn
    // their place when there are MORE direct MCP tools than the wire's inline
    // budget (select_wire_tools caps external tools at kInlineBudget) —
    // otherwise every MCP tool already ships inline and the meta-tools are
    // pure overhead.
    //
    // They also actively BREAK the Anthropic OAuth (first-party) path: that
    // endpoint validates the tool set against Claude Code's and rejects a
    // client-defined tool-search capability with HTTP 400 ("third-party apps
    // now draw from your extra usage") — taking down EVERY turn while any MCP
    // server is configured. Isolated empirically: dropping just these two
    // tools flips the request 400→200 with the direct MCP tools still
    // present (see the acp A/B in the plugins investigation). So: expose
    // them only when they're genuinely needed (direct count over budget),
    // which is also exactly when the small-setup 400 can't happen.
    constexpr std::size_t kInlineBudget = 16;
    const std::size_t direct_mcp = out.size();
    const bool need_search = direct_mcp > kInlineBudget;
    if (need_search) {
        out.push_back(make_search_tools_tool(pool));
        out.push_back(make_call_tool(pool));
    }
    if (any_resources) out.push_back(make_read_resource_tool(pool));
    if (any_prompts)   out.push_back(make_get_prompt_tool(pool));
    return out;
}

} // namespace

bool mcp_config_present() { return !resolve_config().empty(); }

std::vector<ServerLaunch> configured_servers_for_delegation() {
    bool project_local = false;
    const fs::path path = resolve_config(project_local);
    if (path.empty()) return {};
    const char* allow = std::getenv("AGENTTY_MCP_ALLOW_PROJECT");
    const bool project_allowed = allow && (allow[0] == '1' || allow[0] == 't'
        || allow[0] == 'T' || allow[0] == 'y' || allow[0] == 'Y');
    if (project_local && !project_allowed) return {};

    json document;
    try { std::ifstream input(path); input >> document; }
    catch (...) { return {}; }
    const json* servers = nullptr;
    if (document.contains("mcpServers") && document["mcpServers"].is_object())
        servers = &document["mcpServers"];
    else if (document.contains("servers") && document["servers"].is_object())
        servers = &document["servers"];
    if (!servers) return {};

    auto string_value = [](const json& value) {
        return value.is_string() ? value.get<std::string>() : value.dump();
    };
    std::vector<ServerLaunch> out;
    for (auto it = servers->begin(); it != servers->end(); ++it) {
        if (!it.value().is_object() || it.value().value("disabled", false)) continue;
        const auto& spec = it.value();
        ServerLaunch launch;
        launch.name = it.key();
        launch.command = spec.value("command", std::string{});
        launch.url = spec.value("url", std::string{});
        const auto type = spec.value("type", std::string{});
        if (!launch.url.empty() || type == "http" || type == "streamable-http")
            launch.transport = ServerLaunch::Transport::Http;
        else if (type == "sse")
            launch.transport = ServerLaunch::Transport::Sse;
        if (auto args = spec.find("args"); args != spec.end() && args->is_array())
            for (const auto& value : *args) launch.args.push_back(string_value(value));
        if (auto env = spec.find("env"); env != spec.end() && env->is_object())
            for (auto entry = env->begin(); entry != env->end(); ++entry)
                launch.env.emplace_back(entry.key(), string_value(entry.value()));
        if (auto headers = spec.find("headers"); headers != spec.end() && headers->is_object())
            for (auto entry = headers->begin(); entry != headers->end(); ++entry)
                launch.headers.emplace_back(entry.key(), string_value(entry.value()));
        if ((launch.transport == ServerLaunch::Transport::Stdio && launch.command.empty())
            || (launch.transport != ServerLaunch::Transport::Stdio && launch.url.empty()))
            continue;
        out.push_back(std::move(launch));
    }
    return out;
}

std::vector<tools::ToolDef> mcp_tools(PoolHandle& out_pool) {
    std::vector<tools::ToolDef> out;
    bool project_local = false;
    fs::path cfg = resolve_config(project_local);
    if (cfg.empty()) return out;          // no config → zero work, zero tools

    // ── Untrusted-workspace spawn gate (security) ────────────────────────
    // A project-local ./.agentty/mcp.json can ride in on a cloned repo, and
    // its stdio servers spawn ARBITRARY commands at registry-build time with
    // no per-tool permission prompt — bypassing the Exec gate every other
    // code path honors. So when the config is workspace-local we require an
    // explicit opt-in (AGENTTY_MCP_ALLOW_PROJECT=1). $AGENTTY_MCP_CONFIG and
    // ~/.agentty/mcp.json are trusted (the user placed them) and never gated.
    bool allow_project = false;
    if (const char* e = std::getenv("AGENTTY_MCP_ALLOW_PROJECT");
        e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y'))
        allow_project = true;
    if (project_local && !allow_project) {
        std::fprintf(stderr,
            "mcp: ignoring workspace-local %s (it can spawn arbitrary\n"
            "     commands). Set AGENTTY_MCP_ALLOW_PROJECT=1 to enable it, or\n"
            "     move trusted servers to ~/.agentty/mcp.json.\n",
            cfg.string().c_str());
        return out;
    }

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

    // ── Connect every server IN PARALLEL with a global deadline ──────────
    // Each provider's constructor blocks on a handshake (up to the SDK's
    // per-server timeout). Connecting serially made first registry() access
    // O(N × handshake) on the cold-start path, so one slow/hung server
    // stalled the entire tool surface. Fan the connects out across threads
    // and bound the whole batch with AGENTTY_MCP_CONNECT_TIMEOUT_MS (default
    // 15 s); any server still handshaking past the deadline is skipped this
    // session (its eventual result is dropped when the future detaches).
    long connect_deadline_ms = 15'000;
    if (const char* e = std::getenv("AGENTTY_MCP_CONNECT_TIMEOUT_MS"); e && e[0]) {
        try { long v = std::stol(e); if (v > 0) connect_deadline_ms = v; }
        catch (const std::exception& ex) { util::dbglog("mcp.connect_timeout.env", ex.what()); }
        catch (...) { util::dbglog("mcp.connect_timeout.env", "non-std exception"); }
    }

    struct Pending {
        std::string name;
        std::shared_future<std::shared_ptr<::mcp::cap::CapabilityProvider>> fut;
    };
    std::vector<Pending> pending;
    for (auto it = servers->begin(); it != servers->end(); ++it) {
        const std::string sname = it.key();
        const json spec = it.value();   // copy: detached worker outlives `doc`
        if (spec.value("disabled", false)) continue;

        ServerPolicy policy;
        policy.trust_annotations = spec.value("trustAnnotations", false);
        policy.max_output_chars = spec.value("maxOutputChars", 30'000);
        auto add_names = [&](const json& values, std::unordered_set<std::string>& out) {
            if (!values.is_array()) return;
            for (const auto& value : values)
                if (value.is_string()) out.insert(value.get<std::string>());
        };
        if (auto tools_it = spec.find("tools"); tools_it != spec.end() && tools_it->is_object()) {
            add_names(tools_it->value("include", json::array()), policy.include);
            add_names(tools_it->value("exclude", json::array()), policy.exclude);
            add_names(tools_it->value("pin", json::array()), policy.pin);
        }
        pool->policies[sname] = std::move(policy);
        // A server entry with a "url" (or type:"http"/"sse") is a remote
        // Streamable HTTP server; anything with a "command" is a spawned
        // stdio server. URL wins when both are present.
        pending.push_back(Pending{sname, std::async(std::launch::async,
            [sname, spec]() -> std::shared_ptr<::mcp::cap::CapabilityProvider> {
                const std::string url  = spec.value("url", std::string{});
                const std::string type = spec.value("type", std::string{});
                const bool is_http = !url.empty() || type == "http" || type == "sse"
                                     || type == "streamable-http";
                if (is_http) {
                    HttpConfig hc;
                    hc.url = url;
                    if (spec.contains("headers") && spec["headers"].is_object())
                        for (auto h = spec["headers"].begin(); h != spec["headers"].end(); ++h)
                            hc.headers.emplace_back(h.key(), h.value().is_string()
                                                                 ? h.value().get<std::string>()
                                                                 : h.value().dump());
                    hc.call_timeout = call_timeout();
                    hc.workspace_root = tools::util::workspace_root().generic_string();
                    if (const auto ms = spec.value("timeoutMs", 0L); ms > 0)
                        hc.call_timeout = std::chrono::milliseconds{ms};
                    std::string err;
                    auto p = make_http_provider(sname, hc, err);
                    if (!p) std::fprintf(stderr, "mcp: %s\n", err.c_str());
                    return p;
                }
                return make_provider(sname, spec);
            }).share()});
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds{connect_deadline_ms};
    for (auto& pend : pending) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining < std::chrono::milliseconds{0})
            remaining = std::chrono::milliseconds{0};
        if (pend.fut.wait_for(remaining) != std::future_status::ready) {
            std::fprintf(stderr,
                "mcp: server '%s' did not connect within the deadline — skipping\n",
                pend.name.c_str());
            // Detach so the still-handshaking worker can finish and clean up
            // without blocking startup; its result is dropped.
            std::thread([f = pend.fut]() mutable { f.wait(); }).detach();
            continue;
        }
        auto p = pend.fut.get();
        if (p) {
            std::fprintf(stderr,
                "mcp: server '%s' connected (%zu tools, %zu resources, %zu prompts)\n",
                pend.name.c_str(), p->list().size(), p->resources().size(), p->prompts().size());
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

std::size_t mcp_reload() {
    // Rebuild the pool from the CURRENT config. mcp_tools() reads the
    // config fresh, spawns every listed server, and installs the new pool
    // as g_pool_ref (replacing the old one). We throw away the returned
    // ToolDefs here — the registry re-projects them from the pool on its
    // next refresh; what matters is that g_pool_ref now points at the
    // reloaded pool and its generation is non-zero.
    //
    // Generation: a fresh pool starts at 0, but refresh_wire_cache_locked
    // treats generation==0 as "use the cached initial_mcp snapshot" — so a
    // reloaded pool MUST report a non-zero generation to route through the
    // live projection. Seed it to 1 before mcp_tools() connects (any
    // subsequent list_changed bumps from there).
    PoolHandle throwaway;
    (void)mcp_tools(throwaway);
    if (auto p = current_pool()) {
        unsigned long g = p->generation.load(std::memory_order_relaxed);
        if (g == 0) p->generation.store(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(p->mu);
        return p->registry.provider_count();
    }
    return 0;
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
        pi.name        = canonical_mcp_name(p.name);
        pi.server      = mcp_origin_id(p.name);
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
        auto route = resolve_prompt_route(pool->registry, name);
        if (!route) { err = "prompt is missing or ambiguous: '" + name + "'"; return std::nullopt; }
        if (!pool->registry.get_prompt(*route, args, res, err)) return std::nullopt;
    }
    return render_prompt(res);
}

} // namespace agentty::mcp
