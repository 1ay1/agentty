// settings_add_effect_test — the settings pane's add-mode create is an effect.
//
// Submitting "name command" in the Plugins (or Commands/Agents) add prompt
// writes the disk: an entry in mcp.json, or a starter file. That used to run
// INSIDE the reducer. Now the reducer only describes it (cmd::settings_add)
// and folds the answer (SettingsAddDone). Pinned here:
//   1. Submit returns a Cmd and writes NOTHING to disk during the fold.
//   2. SettingsAddDone{ok} for Plugins toasts "… — connecting…" and starts
//      a plugin reload; a failure toasts the message and reloads nothing.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/panel/settings/list.hpp"

namespace fs = std::filesystem;
using namespace agentty;
namespace pn = agentty::ui::panel;

namespace {  // fold: TU-local (bundled into agentty_standalone_tests)
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

void type(Model& m, const std::string& s) {
    for (char c : s)
        (void)app::update(m, msg::SettingsListMsg{SettingsListChar{static_cast<char32_t>(c)}});
}
}  // namespace

int main() {
    std::printf("=== settings_add_effect_test ===\n");
    const fs::path home = fs::temp_directory_path() / "agentty_settings_add_effect";
    fs::remove_all(home);
    fs::create_directories(home);
#if defined(_WIN32)
    _putenv_s("AGENTTY_HOME", (home / ".agentty").string().c_str());
#else
    ::setenv("AGENTTY_HOME", (home / ".agentty").c_str(), 1);
    ::setenv("HOME", home.c_str(), 1);
#endif
    const fs::path mcp = home / ".agentty" / "mcp.json";

    Model m;
    pn::SettingsList o;
    o.concern = settings::Category::Plugins;
    m.ui.panel.descend(std::move(o));

    (void)app::update(m, msg::SettingsListMsg{SettingsListAddStart{}});
    type(m, "demo /bin/true");
    auto cmd = app::update(m, msg::SettingsListMsg{SettingsListSubmitInput{}});
    check(!cmd.is_none(), "submit returns an effect");
    check(!fs::exists(mcp), "submit wrote nothing during the fold");
    const auto* after = m.ui.panel.get<pn::SettingsList>();
    check(after && !after->input_active && after->input.empty(),
          "submit closes the add prompt");

    // The effect's answer, folded.
    m.ui.plugins_loading = false;
    auto ok_cmd = app::update(m, msg::SettingsListMsg{
        SettingsAddDone{settings::Category::Plugins, true, "added demo"}});
    check(!ok_cmd.is_none(), "a successful plugin add starts a reload");
    check(m.ui.plugins_loading, "the pane shows the reload in flight");
    check(m.s.status.find("added demo") != std::string::npos
              && m.s.status.find("connecting") != std::string::npos,
          "the toast says what happened and that it is connecting");

    m.ui.plugins_loading = false;
    (void)app::update(m, msg::SettingsListMsg{
        SettingsAddDone{settings::Category::Plugins, false, "no such script: x.py"}});
    check(!m.ui.plugins_loading, "a failed add reloads nothing");
    check(m.s.status.find("no such script") != std::string::npos,
          "a failed add toasts its reason");

    fs::remove_all(home);
    std::printf(g_fail ? "FAILED (%d)\n" : "PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
