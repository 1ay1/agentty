#include "agentty/runtime/view/thread/welcome_screen.hpp"

#include <maya/core/render_context.hpp>
#include <maya/widget/model_badge.hpp>

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"

namespace agentty::ui {

maya::WelcomeScreen::Config welcome_screen_config(const Model& m) {
    maya::ModelBadge mb{m.d.model_id.value};
    mb.set_compact(true);

    maya::WelcomeScreen::Config cfg;
    cfg.sigil_color = role_brand_alt;                   // bright_magenta flagship sigil
    cfg.tagline     = "a calm middleware between you and the model";
    cfg.model_badge    = mb.build();
    cfg.profile_label  = std::string{profile_label(m.d.profile)};
    cfg.profile_color  = profile_color(m.d.profile);
    // Starters card — shown ONLY on a genuine first run: thread history has
    // finished loading (`!threads_loading`) and there are no saved
    // conversations yet (`threads.empty()`). A returning user with history
    // never sees it, so the welcome stays clean; the placeholder
    // suggestions read as landing-page filler once you know the tool. But a
    // brand-new user staring at an empty composer has no idea what to type
    // first — these three concrete, real prompts teach the three things
    // agentty is for (understand a codebase, make a change, run/fix), so the
    // very first message is a copy of something useful rather than a blank.
    const bool first_run = !m.s.threads_loading && m.d.threads.empty();
    if (first_run) {
        cfg.starters_title = "New here? Try one of these";
        cfg.starters = {
            "Explain what this project does and how it's structured",
            "Find and fix the bug in <file> — it <symptom>",
            "Add a <feature> and run the tests",
        };
    } else {
        cfg.starters_title = {};
        cfg.starters       = {};
    }
    cfg.hint_intro     = "type to begin";
    // Full keybinding map (was previously in the bottom status bar's
    // ShortcutRow, which has been retired — the status row now serves
    // as a high-visibility toast slot instead). Per-key colors signal
    // what each binding does at a glance: pickers (palette/threads/
    // todo/models) share their action-surface hue, profile/new/quit
    // get distinct semantic colors.
    cfg.hints          = {{"^K",    " palette", code_path},      // cyan — actions
                          {"^J",    " threads", role_info},      // blue — navigation
                          {"^\xe2\x86\x90\xe2\x86\x92", " cycle", role_info},   // blue — navigation (^←→)
                          {"^T",    " todo",    warn},           // yellow — planning
                          {"S-Tab", " profile", role_brand},     // magenta — identity
                          {"^/",    " models",  role_brand_alt}, // bright magenta
                          {"^P",    " provider",code_path},      // cyan — actions
                          {"^N",    " new",     success},        // green — create
                          {"^C",    " quit",    danger}};        // red — destructive
    cfg.accent_color   = role_brand;
    cfg.text_color     = fg;
    // Viewport clamp. The idle frame around the welcome is ~11 rows of
    // chrome (AppLayout outer padding 2 + composer 6 + status bar 3);
    // cap the welcome at term_rows - 11 so the WHOLE idle frame fits
    // the terminal. maya tier-drops blank separators first, then
    // collapses the pixel sigil to a one-row wordmark. An overflowing
    // welcome commits its top rows to native scrollback at startup and
    // the first welcome→conversation swap strands them there — the
    // startup flavor of the wordmark-duplication corruption.
    // available_height() reads the sized RenderContext ui::view installs
    // for the whole build phase (real ioctl dims, or the harness's
    // simulated dims) — do NOT ioctl here, it would override test
    // harnesses that pin a simulated geometry.
    constexpr int kIdleChromeRows = 11;
    const int term_rows = maya::available_height();
    if (term_rows > 0)
        cfg.max_rows = std::max(4, term_rows - kIdleChromeRows);
    return cfg;
}

} // namespace agentty::ui
