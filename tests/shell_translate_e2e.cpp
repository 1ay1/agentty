// shell_translate_e2e — a `shell` call that is pure file inspection is
// answered by the REAL native tools (read / grep / list_dir / glob), not a
// shell: the ToolExecOutput carries the native output plus the translation
// record the view renders as native cards. Anything that can't be answered
// faithfully falls back to the shell with the old output.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/tool/mcp_tools_bridge.hpp"
#include "agentty/tool/registry.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace fs = std::filesystem;
using namespace agentty;

namespace {
int g_fail = 0, g_n = 0;
void check(bool ok, const std::string& what) {
    ++g_n;
    std::printf("%s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) ++g_fail;
}

// Run the run_tool Cmd synchronously and capture the ToolExecOutput.
ToolExecOutput run_shell(const std::string& command, const std::string& cwd) {
    auto cmd = app::cmd::run_tool(ToolCallId{"t1"}, ToolName{"shell"},
                                  nlohmann::json{{"command", command}, {"cd", cwd}}, {}, 7);
    ToolExecOutput got{ToolCallId{""}, std::string{}};
    bool have = false;
    auto sink = [&](Msg m) {
        std::visit([&](auto& sub) {
            using S = std::decay_t<decltype(sub)>;
            if constexpr (std::is_same_v<S, msg::ToolMsg>)
                if (auto* o = std::get_if<ToolExecOutput>(&sub)) { got = std::move(*o); have = true; }
        }, m);
    };
    std::visit([&](auto& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, maya::Cmd<Msg>::IsolatedTask> || std::is_same_v<V, maya::Cmd<Msg>::Task>)
            v.run(sink);
    }, cmd.inner);
    if (!have) std::printf("  (no ToolExecOutput for: %s)\n", command.c_str());
    return got;
}
std::string text(const ToolExecOutput& o) { return o.result ? *o.result : "ERR " + o.result.error().render(); }
bool has(const ToolExecOutput& o, std::string_view s) { return text(o).find(s) != std::string::npos; }
} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("agentty_xlate_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    auto put = [&](const fs::path& p, const std::string& s) { std::ofstream(p) << s; };
    std::string lines;
    for (int i = 1; i <= 60; ++i) lines += "line " + std::to_string(i) + (i == 30 ? " needle here\n" : "\n");
    put(root / "src" / "a.cpp", lines);
    put(root / "src" / "b.hpp", "struct Widget {};\nstruct Gadget {};\n");
    put(root / "notes.txt", "alpha\nbeta\n");
    ::setenv("HOME", root.c_str(), 1);
    tools::util::set_workspace_root(root);
    tools::wire_mcp_runtime("off");
    const std::string cwd = root.string();

    // 1. sed -n range → a single native read, native output
    {
        auto o = run_shell("sed -n '28,31p' src/a.cpp", cwd);
        check(o.translated.size() == 1 && o.translated[0].tool == "read", "sed -n → read");
        check(has(o, "answered natively") && has(o, "needle here"), "read output reaches the model");
        check(!o.translated.empty() && o.translated[0].args.value("start_line", 0) == 28
              && o.translated[0].args.value("end_line", 0) == 31, "range carried into read args");
        check(!o.translated.empty() && o.translated[0].fragment.starts_with("sed -n"), "fragment kept for the card");
    }
    // 2. grep with a GNU BRE alternation → native grep with ERE
    {
        auto o = run_shell("grep -rn 'Widget\\|Gadget' src | head -5", cwd);
        check(o.translated.size() == 1 && o.translated[0].tool == "grep", "grep -rn BRE → grep");
        check(!o.translated.empty() && o.translated[0].args.value("pattern", "") == "Widget|Gadget",
              "BRE \\| converted to ERE |");
        check(has(o, "Widget") && has(o, "Gadget"), "both alternatives found natively");
    }
    // 3. chain: grep && sed → two native calls, both outputs
    {
        auto o = run_shell("grep -n needle src/a.cpp && sed -n 1,2p notes.txt", cwd);
        check(o.translated.size() == 2, "chain → 2 native calls");
        check(has(o, "needle") && has(o, "alpha"), "both outputs present");
    }
    // 4. find -name → glob; ls → list_dir
    {
        auto f = run_shell("find src -name '*.hpp'", cwd);
        check(f.translated.size() == 1 && f.translated[0].tool == "glob" && has(f, "b.hpp"), "find -name → glob");
        auto l = run_shell("ls src", cwd);
        check(l.translated.size() == 1 && l.translated[0].tool == "list_dir" && has(l, "a.cpp"), "ls → list_dir");
    }
    // 5. native can't answer faithfully → shell fallback, no translation
    {
        auto nomatch = run_shell("grep -rn zzqqnomatch src", cwd);
        check(nomatch.translated.empty() && !has(nomatch, "answered natively"),
              "no native match → shell decides (fallback)");
        auto missing = run_shell("sed -n 1,2p nope.txt", cwd);
        check(missing.translated.empty(), "missing file → shell (its own error text)");
        put(root / "scratch.txt", "a\n");
        for (const char* c : {"grep -rn x src | sort", "cat a; echo --; cat b", "grep -c needle src/a.cpp",
                              "echo hi", "cmake --version", "sed -i 's/a/b/' scratch.txt"}) {
            auto o = run_shell(c, cwd);
            check(o.translated.empty(), std::string{"stays shell: "} + c);
        }
    }
    // 6. kill switch
    {
        ::setenv("AGENTTY_NO_SHELL_TRANSLATE", "1", 1);
        auto o = run_shell("sed -n 1,2p notes.txt", cwd);
        check(o.translated.empty() && has(o, "alpha") && !has(o, "answered natively"),
              "AGENTTY_NO_SHELL_TRANSLATE=1 → shell");
        ::unsetenv("AGENTTY_NO_SHELL_TRANSLATE");
    }
    fs::remove_all(root);
    std::printf("%d checks, %d failures\n", g_n, g_fail);
    return g_fail == 0 ? 0 : 1;
}
