#include "agentty/runtime/view/thread/turn/permission.hpp"

#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"

namespace agentty::ui {

maya::Permission::Config inline_permission_config(const PendingPermission& pp,
                                                  const ToolUse& tc) {
    std::string desc;
    if (!tc.args.is_object()) {
        desc = pp.reason;
    } else if (tc.name == "shell" || tc.name == "diagnostics") {
        desc = tc.args.value("command", "");
        // `command` is not the whole of what runs. `cd` moves it, and `env`
        // can hand the child LD_PRELOAD / BASH_ENV / GIT_SSH_COMMAND -- so a
        // card showing a harmless-looking `ls -la` could be approving
        // arbitrary code from a path the user never saw. Consent has to show
        // everything that decides what executes, which is the whole point of
        // the card.
        if (const auto cd = pick_arg(tc.args, {"cd", "cwd", "directory"}); !cd.empty())
            desc += "  \xc2\xb7 in " + cd;
        if (auto it = tc.args.find("env");
            it != tc.args.end() && it->is_object() && !it->empty()) {
            desc += "  \xc2\xb7 env ";
            bool first = true;
            for (auto e = it->begin(); e != it->end(); ++e) {
                if (!first) desc += " ";
                first = false;
                desc += e.key() + "=";
                // The VALUE is the payload (a path to a .so, a command for
                // git to run), so it has to be visible -- but bounded, or a
                // long value pushes the command itself off the card.
                std::string v = e.value().is_string()
                    ? e.value().get<std::string>() : e.value().dump();
                if (v.size() > 48) { v.resize(47); v += "\xe2\x80\xa6"; }
                desc += v;
            }
        }
    } else if (tc.name == "read" || tc.name == "edit"
            || tc.name == "write" || tc.name == "list_dir") {
        // Alias-aware: write's canonical key is `file_path`, and models
        // routinely pick filepath/filename for the others. Reading only
        // `path` left the prompt description BLANK for a write — the
        // user was asked to approve a file mutation without seeing
        // which file.
        desc = pick_arg(tc.args, {"path", "file_path", "filepath", "filename"});
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
