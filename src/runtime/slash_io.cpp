// slash_io.cpp — the disk + MCP side of slash.hpp, for callers with no
// Model (ACP, `agentty run`). The TUI builds its catalog from the Model.
#include "agentty/runtime/slash.hpp"

#include "agentty/tool/commands.hpp"
#include "agentty/tool/skills.hpp"
#if AGENTTY_MCP
#include "agentty/mcp/client.hpp"
#endif

namespace agentty::slash {

Catalog discover(Io io, bool with_actions) {
    std::vector<Source> cmds, skills, prompts;
    for (const auto& c : tools::commands::all(io))
        cmds.push_back({c.name, c.description, c.argument_hint});
    for (const auto& s : tools::skills::all(io))
        if (!s.name.empty()) skills.push_back({s.name, s.description, {}});
#if AGENTTY_MCP
    try {
        for (const auto& p : mcp::mcp_prompts()) {
            std::string hint;
            for (const auto& a : p.arguments) {
                if (!hint.empty()) hint += ' ';
                hint += a.required ? "<" + a.name + ">" : "[" + a.name + "]";
            }
            prompts.push_back({p.name, p.description, std::move(hint)});
        }
    } catch (...) {}
#endif
    return make_catalog(cmds, skills, prompts, with_actions);
}

std::optional<std::string> expand(Io io, std::string_view text, const Catalog& cat,
                                  bool skill_already_active) {
    const Resolved hit = resolve(text, cat);
    switch (hit.what) {
        case Resolved::What::Command:
            return tools::commands::try_expand(io, text);
        case Resolved::What::Prompt:
#if AGENTTY_MCP
            return mcp::mcp_expand_prompt(text);
#else
            return std::nullopt;
#endif
        case Resolved::What::Skill: {
            const auto sk = tools::skills::find(io, hit.entry->name);
            if (!sk) return std::nullopt;
            const std::string& rest = hit.args;
            if (skill_already_active)
                return "Apply the already-loaded '" + sk->name + "' skill" +
                       (rest.empty() ? "." : ": " + rest);
            std::string out = tools::skills::activation_payload(io, *sk);
            out += "\n\nFollow the skill instructions above";
            out += rest.empty() ? "." : " for this task: " + rest;
            return out;
        }
        case Resolved::What::Action:
        case Resolved::What::Plain:
            return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace agentty::slash
