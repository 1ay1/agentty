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

std::optional<Expanded> expand_checked(Io io, std::string_view text, const Catalog& cat,
                                       bool skill_already_active) {
    const Resolved hit = resolve(text, cat);
    switch (hit.what) {
        case Resolved::What::Command:
            if (auto t = tools::commands::try_expand(io, text)) return Expanded{std::move(*t)};
            return std::nullopt;
        case Resolved::What::Prompt:
#if AGENTTY_MCP
        {
            bool usage = false;
            if (auto t = mcp::mcp_expand_prompt(text, &usage)) return Expanded{std::move(*t), usage};
            return std::nullopt;
        }
#else
            return std::nullopt;
#endif
        case Resolved::What::Skill: {
            const auto sk = tools::skills::find(io, hit.entry->name);
            if (!sk) return std::nullopt;
            const std::string& rest = hit.args;
            if (skill_already_active)
                return Expanded{"Apply the already-loaded '" + sk->name + "' skill" +
                                (rest.empty() ? "." : ": " + rest)};
            std::string out = tools::skills::activation_payload(io, *sk);
            out += "\n\nFollow the skill instructions above";
            out += rest.empty() ? "." : " for this task: " + rest;
            return Expanded{std::move(out)};
        }
        case Resolved::What::Action:
        case Resolved::What::Plain:
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> expand(Io io, std::string_view text, const Catalog& cat,
                                  bool skill_already_active) {
    auto e = expand_checked(io, text, cat, skill_already_active);
    if (!e) return std::nullopt;
    // A usage note is for the user. Callers that can't show one send the
    // line as typed; the model then sees what the user wrote, not our note.
    if (e->usage) return std::nullopt;
    return std::move(e->text);
}

}  // namespace agentty::slash
