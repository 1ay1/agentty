#include "agentty/runtime/view/thread/turn/permission.hpp"

namespace agentty::ui {

maya::Permission::Config inline_permission_config(const PendingPermission& pp,
                                                  const ToolUse& tc) {
    std::string desc;
    if (!tc.args.is_object()) {
        desc = pp.reason;
    } else if (tc.name == "bash" || tc.name == "diagnostics") {
        desc = tc.args.value("command", "");
    } else if (tc.name == "read" || tc.name == "edit"
            || tc.name == "write" || tc.name == "list_dir") {
        desc = tc.args.value("path", "");
    } else if (tc.name == "web_fetch") {
        desc = tc.args.value("url", "");
    } else if (tc.name == "web_search") {
        desc = tc.args.value("query", "");
    } else if (tc.name == "git_commit") {
        desc = tc.args.value("message", "");
    } else if (tc.name == "find_definition") {
        desc = tc.args.value("symbol", "");
    } else {
        desc = tc.args_dump();
    }

    maya::Permission::Config cfg;
    cfg.tool_name         = tc.name.value;
    cfg.description       = desc.empty() ? pp.reason : desc;
    cfg.show_always_allow = true;
    return cfg;
}

} // namespace agentty::ui
