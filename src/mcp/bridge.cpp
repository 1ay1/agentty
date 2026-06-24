// agentty::mcp — the bridge: config → mcp::cap providers → agentty ToolDefs.
//
// This is now a THIN adapter. All the protocol + capability machinery lives in
// the mcp-cpp submodule's capability layer (mcp::cap): spawning servers,
// driving the handshake, listing/calling tools, namespacing, dispatch. agentty
// only: (1) reads its config, (2) builds a cap::Registry of providers, and
// (3) wraps each registry tool in a ToolDef so the model sees MCP tools beside
// the local ones. The heavy mcp-cpp templates stay confined to this one TU.
//
// Flow:
//   mcp_tools()
//     → resolve config (.agentty/mcp.json / $AGENTTY_MCP_CONFIG / ~)
//     → for each server: cap::StdioServerProvider (connects synchronously)
//     → cap::Registry fans them in + namespaces collisions
//     → for each registry tool: a ToolDef whose execute() calls registry.dispatch
//   The Registry (and its live server connections) live in a ConnectionPool
//   kept alive by a shared_ptr captured into every execute() closure.

#include "agentty/mcp/client.hpp"

#include <mcp/cap/cap.hpp>

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
    std::mutex           mu;
};

namespace {

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

    // A remote tool can do anything → always require permission.
    def.effects = tools::EffectSet{tools::Effect::Exec, tools::Effect::WriteFs,
                                   tools::Effect::Net,  tools::Effect::ReadFs};

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
            return tools::ToolOutput{r.text.empty() ? "(no output)" : r.text, std::nullopt};
        } catch (const std::exception& e) {
            return std::unexpected(tools::ToolError::subprocess(
                std::string{"MCP call failed: "} + e.what()));
        } catch (...) {
            return std::unexpected(tools::ToolError::subprocess("MCP call failed"));
        }
    };
    return def;
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
        std::fprintf(stderr, "mcp: failed to parse %s: %s\n", cfg.c_str(), e.what());
        return out;
    }

    const json* servers = nullptr;
    if (doc.contains("mcpServers") && doc["mcpServers"].is_object())
        servers = &doc["mcpServers"];
    else if (doc.contains("servers") && doc["servers"].is_object())
        servers = &doc["servers"];
    if (!servers) return out;

    auto pool = std::make_shared<ConnectionPool>();
    for (auto it = servers->begin(); it != servers->end(); ++it) {
        if (auto p = make_provider(it.key(), it.value())) {
            std::fprintf(stderr, "mcp: server '%s' connected (%zu tools)\n",
                         it.key().c_str(), p->list().size());
            pool->registry.add(std::move(p));
        }
    }
    if (pool->registry.provider_count() == 0) return out;   // nothing connected

    out_pool = pool;                       // keep providers alive
    for (auto& t : pool->registry.tools()) out.push_back(make_tool(pool, t));
    return out;
}

} // namespace agentty::mcp
