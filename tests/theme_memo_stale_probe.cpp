// theme_memo_stale_probe — does a theme switch actually recolour a
// SETTLED tool panel?
//
// agent_timeline.cpp holds three thread_local Element memos:
//   g_body_cache          key: tool id | status | output size | grep_sig
//   g_panel_cache         key: tool ids/status/sizes/render_keys | tool_output pref
//   g_panel_render_memo   key: msg_id | render_key
//
// None of those keys names the THEME, and nothing clears them when the
// theme changes (restyle_sealed_turns clears view_cache + rehydrates the
// frozen ledger, but never these). If a cached panel Element bakes any
// RESOLVED colour at build time, the memo will keep serving the old
// palette for as long as the tool call's content is unchanged — which,
// for a settled tool call, is forever.
//
// The probe renders the same settled tool panel under two very different
// schemes and compares the emitted SGR bytes. Warm vs cold tells us
// whether the difference (if any) came from the theme or from the memo.

#include <cstdio>
#include <string>
#include <vector>

#include <maya/maya.hpp>
#include <maya/style/schemes.hpp>
#include <maya/style/theme.hpp>

#include "agentty/domain/conversation.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/agent_timeline.hpp"

using namespace agentty;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// One settled (terminal) tool call — the memoizable shape.
std::vector<ToolUse> settled_batch() {
    ToolUse tc;
    tc.id     = ToolCallId{"toolu_probe_1"};
    tc.name   = ToolName{"edit"};
    tc.args   = nlohmann::json{{"path", "src/main.cpp"},
                               {"display_description", "fix the thing"}};
    tc.status = ToolUse::Done{.output = "applied 1 edit to src/main.cpp"};
    return {tc};
}

std::string render_panel(const maya::Theme& t, std::string_view msg_id,
                         std::uint64_t render_key) {
    ui_prefs::publish_theme(t);
    auto batch = settled_batch();
    maya::Element el = ui::agent_timeline_element_memoized(
        msg_id, render_key, batch, /*spinner_frame=*/0,
        maya::Color::slot(maya::ThemeSlot::Accent));
    return maya::render_to_string_ansi(el, 100);
}

} // namespace

int main() {
    std::printf("=== theme_memo_stale_probe ===\n\n");

    const maya::Theme* dracula = ui_prefs::find_scheme("Dracula");
    const maya::Theme* gruv    = ui_prefs::find_scheme("Gruvbox Light");
    if (!dracula || !gruv) {
        std::printf("could not find both schemes — abort\n");
        return 2;
    }

    // ── A. COLD: a different msg_id each time, so the memo cannot hit.
    // This is the ground truth: what the panel SHOULD look like under
    // each theme.
    const std::string cold_a = render_panel(*dracula, "cold_msg_a", 1);
    const std::string cold_b = render_panel(*gruv,    "cold_msg_b", 2);

    std::printf("cold (memo bypassed via distinct keys)\n");
    check(cold_a != cold_b,
          "the two schemes DO paint a settled panel differently");
    std::printf("    dracula bytes=%zu  gruvbox bytes=%zu\n\n",
                cold_a.size(), cold_b.size());

    // ── B. WARM: the SAME msg_id + render_key across a theme switch.
    // This is what a real theme switch does to an already-settled
    // tool panel: the content did not change, so the key is identical.
    const std::string warm_a = render_panel(*dracula, "warm_msg", 7);
    const std::string warm_b = render_panel(*gruv,    "warm_msg", 7);

    std::printf("warm (same msg_id + render_key, theme swapped between)\n");
    check(warm_a != warm_b,
          "a theme switch recolours the SETTLED panel");

    if (warm_a == warm_b) {
        std::printf("\n  >>> STALE: the memo served the Dracula-built\n");
        std::printf("      Element after the theme became Gruvbox Light.\n");
        std::printf("      Byte-identical output across a real palette change.\n");
    }

    // ── C. Is the stale copy the OLD theme's paint?
    if (warm_b == warm_a && cold_a != cold_b) {
        std::printf("\n  the warm Gruvbox render equals the Dracula paint: %s\n",
                    warm_b == warm_a ? "yes" : "no");
    }

    std::printf("\n");
    if (failures == 0) {
        std::printf("PASS — theme switches reach settled tool panels\n");
        return 0;
    }
    std::printf("FAILED: %d check(s) — stale palette reproduced\n", failures);
    return 1;
}
