#pragma once
// agentty::Library — what is installed on disk: skills, their approvals,
// slash commands and the hooks file, as a value.
//
// Read on a worker (cmd::load_library) and held on the Model, so reducers
// and panels look things up here instead of walking the disk on the UI
// thread. Reloaded at startup and after each turn.

#include <string>
#include <vector>

#include "agentty/scope/scope.hpp"
#include "agentty/tool/commands.hpp"
#include "agentty/tool/skills.hpp"

namespace agentty {

struct Library {
    bool                                  loaded = false;
    std::vector<tools::skills::Skill>     skills;
    std::vector<tools::skills::Shadowed>  shadowed;
    scope::Approvals                      skill_approvals;
    std::vector<tools::commands::Command> commands;
    std::string                           hooks_file;      // "" when none
    bool                                  hooks_pending = false;

    [[nodiscard]] const tools::skills::Skill* skill(std::string_view name) const noexcept {
        for (const auto& s : skills)
            if (s.name == name) return &s;
        return nullptr;
    }
    // Same scope, same name, two directories: worth flagging.
    [[nodiscard]] bool shadowed_within_scope(std::string_view name) const noexcept {
        const auto* w = skill(name);
        if (!w) return false;
        for (const auto& sh : shadowed)
            if (sh.name == name && sh.source == w->source) return true;
        return false;
    }
};

// Read everything above from disk. Worker-side only.
[[nodiscard]] Library read_library();

}  // namespace agentty
