// read_library — the on-disk half of agentty::Library. Runs on a worker.

#include "agentty/runtime/library.hpp"

#include "agentty/tool/hooks.hpp"
#include "agentty/tool/mcp_tools_backends.hpp"   // project_agent_names

namespace agentty {

Library read_library(Io io) {
    Library l;
    l.skills          = tools::skills::all(io);
    l.shadowed        = tools::skills::shadowed(io);
    l.skill_approvals = tools::skills::load_approvals(io);
    l.commands        = tools::commands::all(io);
    l.hooks_file      = tools::hooks::active_file(io);
    l.hooks_pending   = !l.hooks_file.empty() && tools::hooks::pending_approval(io);
    try { l.project_agents = tools::project_agent_names(); } catch (...) {}
    l.loaded          = true;
    return l;
}

}  // namespace agentty
