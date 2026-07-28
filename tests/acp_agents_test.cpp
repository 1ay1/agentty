// acp_agents_test — the external-ACP config surface + provider-selection
// routing. Pure, subprocess-free: exercises resolve_acp_agent() built-in
// defaults, JSON config override (via a temp $AGENTTY_ACP_AGENTS file), and
// that parse_selection() routes an agent id to Kind::ExternalAcp.

#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>   // getpid
#endif

namespace fs = std::filesystem;
namespace P = agentty::provider;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::cerr << "CHECK failed: " #cond " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
    ++failures; } } while (0)

// Built-in defaults resolve with zero config: the binary name on $PATH. There
// is exactly one built-in reference agent (claude-agent-acp); everything else
// (incl. codex-acp) is config-driven, Zed-style.
static void test_builtin_defaults() {
    // Ensure no config file interferes.
    ::unsetenv("AGENTTY_ACP_AGENTS");

    CHECK(P::is_acp_agent_id("claude-agent-acp"));
    // codex-acp is NOT a built-in — Codex is a native provider; a second
    // "Codex (ACP)" default would be redundant. It only resolves if the user
    // adds it to acp-agents.json by name.
    CHECK(!P::is_acp_agent_id("codex-acp"));
    CHECK(!P::is_acp_agent_id("anthropic"));
    CHECK(!P::is_acp_agent_id("definitely-not-an-agent"));

    auto claude = P::resolve_acp_agent("claude-agent-acp");
    CHECK(claude.has_value());
    if (claude) {
        CHECK(claude->id == "claude-agent-acp");
        CHECK(claude->command == "claude-agent-acp");
        auto argv = claude->argv();
        CHECK(!argv.empty());
        CHECK(argv[0] == "claude-agent-acp");
    }

    // codex-acp has no built-in default now.
    CHECK(!P::resolve_acp_agent("codex-acp").has_value());
    CHECK(!P::resolve_acp_agent("nope").has_value());

    // enumerate lists exactly the one built-in reference agent with no config.
    auto agents = P::enumerate_acp_agents();
    CHECK(agents.size() == 1);
    if (!agents.empty()) CHECK(agents[0].id == "claude-agent-acp");
}

// A config entry overrides the built-in default and adds a custom agent.
static void test_config_override() {
    fs::path tmp = fs::temp_directory_path() /
        ("acp-agents-" + std::to_string(::getpid()) + ".json");
    {
        std::ofstream f(tmp);
        f << R"({
          "acpAgents": {
            "claude-agent-acp": {
              "command": "/opt/claude/bin/claude-agent-acp",
              "args": ["--stdio", "--verbose"],
              "env": { "CLAUDE_LOG": "1" },
              "cwd": "/work"
            },
            "my-agent": { "command": "my-acp", "args": ["serve"] },
            "codex-acp": { "command": "codex-acp", "args": ["acp"] }
          }
        })";
    }
    ::setenv("AGENTTY_ACP_AGENTS", tmp.string().c_str(), /*overwrite*/ 1);

    // Built-in id now resolves to the config's overridden argv.
    auto claude = P::resolve_acp_agent("claude-agent-acp");
    CHECK(claude.has_value());
    if (claude) {
        CHECK(claude->command == "/opt/claude/bin/claude-agent-acp");
        CHECK(claude->args.size() == 2);
        CHECK(claude->cwd == "/work");
        CHECK(claude->env.size() == 1);
        auto argv = claude->argv();
        CHECK(argv.size() == 3);
        CHECK(argv[0] == "/opt/claude/bin/claude-agent-acp");
        CHECK(argv[1] == "--stdio");
    }

    // A config-only agent (no built-in) is now selectable.
    CHECK(P::is_acp_agent_id("my-agent"));
    auto mine = P::resolve_acp_agent("my-agent");
    CHECK(mine.has_value());
    if (mine) { CHECK(mine->command == "my-acp"); CHECK(mine->args.size() == 1); }

    // codex-acp is now selectable BECAUSE the config defines it (not a
    // built-in): Zed-style, the user named the binary.
    CHECK(P::is_acp_agent_id("codex-acp"));
    auto codex = P::resolve_acp_agent("codex-acp");
    CHECK(codex.has_value());
    if (codex) CHECK(codex->command == "codex-acp");

    // enumerate now lists the built-in reference agent + both config agents
    // (my-agent, codex-acp). claude-agent-acp appears once (config overrode
    // the built-in in place), so 3 rows total.
    auto agents = P::enumerate_acp_agents();
    CHECK(agents.size() == 3);
    {
        bool has_claude = false, has_mine = false, has_codex = false;
        for (const auto& a : agents) {
            if (a.id == "claude-agent-acp") {
                has_claude = true;
                // overridden in place: the config path, not the bare name.
                CHECK(a.command == "/opt/claude/bin/claude-agent-acp");
            }
            if (a.id == "my-agent")  has_mine = true;
            if (a.id == "codex-acp") has_codex = true;
        }
        CHECK(has_claude);
        CHECK(has_mine);
        CHECK(has_codex);
    }

    ::unsetenv("AGENTTY_ACP_AGENTS");
    std::error_code ec; fs::remove(tmp, ec);
}

// parse_selection routes an agent id to Kind::ExternalAcp and stamps the id.
static void test_selection_routing() {
    ::unsetenv("AGENTTY_ACP_AGENTS");

    auto s = P::parse_selection("claude-agent-acp");
    CHECK(s.kind == P::Kind::ExternalAcp);
    CHECK(s.acp_agent_id == "claude-agent-acp");

    // codex-acp is NOT a built-in, so with no config it does NOT route to an
    // ACP agent — it falls through to a custom/OpenAI-style spec.
    auto s2 = P::parse_selection("codex-acp");
    CHECK(s2.kind != P::Kind::ExternalAcp);

    // Non-ACP specs still route to their native kinds.
    CHECK(P::parse_selection("anthropic").kind == P::Kind::Anthropic);
    CHECK(P::parse_selection("").kind == P::Kind::Anthropic);
    CHECK(P::parse_selection("openai").kind == P::Kind::OpenAI);

    // Display name is the agent id (no hardcoded registry label anymore).
    CHECK(P::provider_display_name(s) == "claude-agent-acp");
}

int main() {
    test_builtin_defaults();
    test_config_override();
    test_selection_routing();

    if (failures == 0) { std::cout << "acp_agents_test OK\n"; return 0; }
    std::cerr << "acp_agents_test FAILED (" << failures << " check(s))\n";
    return 1;
}
