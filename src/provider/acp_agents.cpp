// agentty::provider — external ACP agent registry (config surface).
// See acp_agents.hpp for the format + resolution rules.

#include "agentty/provider/acp_agents.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace agentty::provider {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

[[nodiscard]] fs::path home_dir() noexcept {
    if (auto* h = std::getenv("HOME"); h && *h) return fs::path{h};
#if defined(_WIN32)
    if (auto* h = std::getenv("USERPROFILE"); h && *h) return fs::path{h};
#endif
    return {};
}

// External ACP agents are entirely config-driven (Zed's `agent_servers`
// model). ACP is the generic transport; agentty does not privilege or install
// a particular agent implementation. Users opt into any external agent by
// naming its command in acp-agents.json.

// Resolve the config file path + whether it's workspace-local (untrusted).
// Mirrors mcp::resolve_config: $AGENTTY_ACP_AGENTS > ~/.agentty > ./.agentty.
fs::path resolve_config(bool& out_project_local) {
    out_project_local = false;
    std::error_code ec;
    if (const char* e = std::getenv("AGENTTY_ACP_AGENTS"); e && e[0]) {
        fs::path p{e};
        return fs::is_regular_file(p, ec) ? p : fs::path{};
    }
    if (fs::path h = home_dir(); !h.empty()) {
        fs::path p = h / ".agentty" / "acp-agents.json";
        if (fs::is_regular_file(p, ec)) return p;
    }
    fs::path proj = fs::path{".agentty"} / "acp-agents.json";
    if (fs::is_regular_file(proj, ec)) { out_project_local = true; return proj; }
    return {};
}

[[nodiscard]] bool env_truthy(const char* name) noexcept {
    const char* e = std::getenv(name);
    return e && (e[0] == '1' || e[0] == 't' || e[0] == 'T' || e[0] == 'y' || e[0] == 'Y');
}

// Parse the whole config file into `out` keyed by id. Returns false (and leaves
// out untouched) on a missing / untrusted / malformed config — a built-in
// default can still satisfy the lookup.
bool load_config(std::vector<AcpAgentSpec>& out) {
    bool project_local = false;
    fs::path cfg = resolve_config(project_local);
    if (cfg.empty()) return false;

    // Untrusted-workspace spawn gate (same policy as .agentty/mcp.json): a
    // project-local config can ride in on a cloned repo and spawn arbitrary
    // commands, so require an explicit opt-in.
    if (project_local && !env_truthy("AGENTTY_ACP_ALLOW_PROJECT")) {
        std::fprintf(stderr,
            "acp: ignoring workspace-local %s (it can spawn arbitrary\n"
            "     commands). Set AGENTTY_ACP_ALLOW_PROJECT=1 to enable it, or\n"
            "     move trusted agents to ~/.agentty/acp-agents.json.\n",
            cfg.string().c_str());
        return false;
    }

    json doc;
    try {
        std::ifstream f(cfg);
        f >> doc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "acp: failed to parse %s: %s\n",
                     cfg.string().c_str(), e.what());
        return false;
    }

    const json* agents = nullptr;
    if (doc.contains("acpAgents") && doc["acpAgents"].is_object())
        agents = &doc["acpAgents"];
    else if (doc.contains("agents") && doc["agents"].is_object())
        agents = &doc["agents"];
    if (!agents) return false;

    auto as_str = [](const json& v) {
        return v.is_string() ? v.get<std::string>() : v.dump();
    };
    for (auto it = agents->begin(); it != agents->end(); ++it) {
        const json& spec = it.value();
        AcpAgentSpec a;
        a.id      = it.key();
        a.command = spec.value("command", std::string{});
        if (a.command.empty()) {
            std::fprintf(stderr, "acp: agent '%s' has no \"command\" — skipping\n",
                         a.id.c_str());
            continue;
        }
        if (spec.contains("args") && spec["args"].is_array())
            for (const auto& x : spec["args"]) a.args.push_back(as_str(x));
        if (spec.contains("env") && spec["env"].is_object())
            for (auto e = spec["env"].begin(); e != spec["env"].end(); ++e)
                a.env.emplace_back(e.key(), as_str(e.value()));
        a.cwd = spec.value("cwd", std::string{});
        out.push_back(std::move(a));
    }
    return true;
}

} // namespace

std::optional<AcpAgentSpec> resolve_acp_agent(std::string_view id) {
    std::vector<AcpAgentSpec> cfg;
    load_config(cfg);
    for (auto& a : cfg)
        if (a.id == id) return std::move(a);
    return std::nullopt;
}

bool is_acp_agent_id(std::string_view id) noexcept {
    std::vector<AcpAgentSpec> cfg;
    load_config(cfg);
    for (const auto& a : cfg)
        if (a.id == id) return true;
    return false;
}

std::vector<AcpAgentSpec> enumerate_acp_agents() {
    std::vector<AcpAgentSpec> out;
    load_config(out);
    return out;
}

} // namespace agentty::provider
