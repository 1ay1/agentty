// plugins_in_model_test — the Plugins panel is a pure function of the Model.
//
// This locks the architectural contract that replaced the recurring
// "stuck on connecting… / laggy panel" bugs:
//
//   1. Opening the Plugins panel sets plugins_loading and returns a Cmd
//      (the connect is DRIVEN BY THE UPDATE LOOP, not a lazy side effect).
//   2. PluginsUpdated{snapshot} stores the snapshot in m.ui.plugins and
//      clears the loading flag — the reducer, not a render-time global,
//      owns the truth.
//   3. items_for(Plugins) renders m.ui.plugins verbatim (a pure projection):
//      an empty model + loading → "connecting…"; a populated model → the
//      servers/tools; nothing calls the live pool.
//
// Because the snapshot lives IN the Model, visual_hash covers it (see
// visual_hash_coverage_test) and the panel repaints on every change with no
// nonce hack — that invariant is what makes the old bug class impossible.

#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/settings_items.hpp"
#include "agentty/mcp/plugin_model.hpp"

#include <cstdio>
#include <string>

using namespace agentty;

static int g_fails = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fails;
}

int main() {
    // ── 1: opening Plugins arms loading + returns a connect Cmd ──
    {
        Model m;
        auto [m2, cmd] = app::update(
            std::move(m), Msg{OpenSettingsList{settings::Category::Plugins}});
        check(settings_list_is_open(m2.ui.settings_list),
              "open: Plugins panel is open");
        check(m2.ui.plugins_loading,
              "open: plugins_loading armed (connect is loop-driven)");
        check(!cmd.is_none(),
              "open: a connect Cmd was returned (not a lazy side effect)");
    }

    // ── 2: PluginsUpdated stores the snapshot + clears loading ──
    {
        Model m;
        m.ui.settings_list = settings::ListOpen{settings::Category::Plugins, 0};
        m.ui.plugins_loading = true;

        mcp::PluginModel snap;
        mcp::ServerState s;
        s.name = "date";
        s.connected = true;
        s.tools.push_back({"current_date", "today's date", true, false});
        s.tools.push_back({"days_between", "day math", true, false});
        snap.servers.push_back(std::move(s));

        auto [m2, cmd] = app::update(std::move(m),
                                     Msg{PluginsUpdated{std::move(snap)}});
        check(!m2.ui.plugins_loading, "updated: loading flag cleared");
        check(m2.ui.plugins.servers.size() == 1,
              "updated: snapshot stored in m.ui.plugins");
        check(m2.ui.plugins.servers[0].connected,
              "updated: connected state preserved");
        check(m2.ui.plugins.servers[0].tools.size() == 2,
              "updated: advertised tools preserved");
    }

    // ── 3: items_for is a pure projection of m.ui.plugins ──
    {
        // Empty + loading → a "connecting…" row.
        Model loading;
        loading.ui.plugins_loading = true;
        auto rows_loading = settings::items_for(loading, settings::Category::Plugins);
        check(rows_loading.size() == 1
              && rows_loading[0].primary.find("connecting") != std::string::npos,
              "projection: empty+loading renders 'connecting…'");

        // Empty + not loading → the "no plugins" hint.
        Model empty;
        auto rows_empty = settings::items_for(empty, settings::Category::Plugins);
        check(rows_empty.size() == 1
              && rows_empty[0].primary.find("no plugins") != std::string::npos,
              "projection: empty+idle renders the 'no plugins' hint");

        // Populated → a server header + its tools; nothing reads the live pool.
        Model populated;
        mcp::ServerState s;
        s.name = "date";
        s.connected = true;
        s.tools.push_back({"current_date", "d", true, false});
        populated.ui.plugins.servers.push_back(std::move(s));
        auto rows = settings::items_for(populated, settings::Category::Plugins);
        bool saw_date = false, saw_tool = false;
        for (const auto& r : rows) {
            if (r.primary.find("date") != std::string::npos) saw_date = true;
            if (r.primary.find("current_date") != std::string::npos) saw_tool = true;
        }
        check(saw_date, "projection: populated model renders the server");
        check(saw_tool, "projection: populated model renders its tools");

        // A CONNECTED server row must read as healthy (Status::Ok), NOT wear
        // the remove-action badge that looked like an error.
        for (const auto& r : rows) {
            if (r.primary == "date") {
                check(r.status == settings::Item::Status::Ok,
                      "badge: connected server is Status::Ok (healthy, not a red ✕)");
                // Enter TOGGLES the whole plugin on/off (reversible), it does
                // NOT remove — destructive delete is the deliberate `d` key.
                check(r.action == settings::Action::TogglePlugin,
                      "interaction: Enter on a plugin toggles it on/off");
                check(r.on && r.hint == "disable",
                      "interaction: an enabled plugin shows on + 'disable'");
            }
        }

        // A DISABLED server reads as off (Neutral, not an error) and offers
        // to re-enable.
        Model off;
        mcp::ServerState d;
        d.name = "date";
        d.disabled = true;
        d.tools.push_back({"current_date", "", true, false});
        off.ui.plugins.servers.push_back(std::move(d));
        for (const auto& r : settings::items_for(off, settings::Category::Plugins)) {
            if (r.primary == "date") {
                check(!r.on && r.hint == "enable",
                      "interaction: a disabled plugin shows off + 'enable'");
                check(r.status == settings::Item::Status::Neutral,
                      "badge: a disabled plugin is Neutral (off on purpose, not an error)");
            }
        }

        // A FAILED server reads as Bad.
        Model failed;
        mcp::ServerState bad;
        bad.name = "broken";
        bad.connected = false;
        bad.error = "spawn failed: no such file";
        failed.ui.plugins.servers.push_back(std::move(bad));
        for (const auto& r : settings::items_for(failed, settings::Category::Plugins)) {
            if (r.primary == "broken")
                check(r.status == settings::Item::Status::Bad,
                      "badge: a failed server is Status::Bad (⚠)");
        }
    }

    if (g_fails == 0) { std::printf("\nAll plugins-in-model tests passed.\n"); return 0; }
    std::printf("\n%d plugins-in-model test(s) FAILED.\n", g_fails);
    return 1;
}
