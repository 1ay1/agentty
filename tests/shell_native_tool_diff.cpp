// Tool-level differential: the SAME shell tool call with the native path on
// and off must return the identical ToolOutput text. The env switch is read
// once per process, so run this binary twice (NATIVE=on/off) and diff.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <nlohmann/json.hpp>
#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

using namespace agentty;
int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    tools::util::set_workspace_root(root);
    tools::wire_mcp_runtime("off");
    const auto* sh = tools::find("shell");
    const char* cmds[] = {
        "sed -n '1,20p' README.md", "sed -n '5p' CMakeLists.txt", "cat LICENSE",
        "head -3 README.md", "tail -5 README.md", "tail -n +3 LICENSE | head -2",
        "cat LICENSE | wc -l", "sed -n '10,40p' src/runtime/app/deps.cpp | tail -3",
        "head -1 LICENSE && tail -1 LICENSE", "cat LICENSE 2>/dev/null | head -2",
        "sed -n '99999p' LICENSE", "head -0 LICENSE",
        "grep -n MIT LICENSE", "grep -c the LICENSE", "grep -ni 'permission' LICENSE",
        "grep -n 'zzqqnomatch' LICENSE", "grep -n 'Software\\|warranty' LICENSE | head -3",
        "grep -nw 'the' README.md | wc -l",
    };
    for (const char* c : cmds) {
        auto r = sh->execute(nlohmann::json{{"command", c}, {"cd", root}});
        std::printf("=== %s\n%s\n", c, r ? r->text.c_str() : ("ERR " + r.error().render()).c_str());
    }
    return 0;
}
