// settings_list_view.cpp — the settings pickers (Ctrl+K →
// Plugins/Commands/Agents/Hooks). One list overlay, built on the house
// Picker widget so it frames/scrolls exactly like every other picker.
//
// Two modes, both rendered here:
//   • list  — rows from settings::items_for(concern), each with a
//             colour-coded action badge; footer names what Enter does and
//             offers `a add`.
//   • add   — a one-line prompt (header) with a live caret; footer shows
//             the format hint + submit/cancel keys.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/settings_list.hpp"
#include "agentty/runtime/settings_items.hpp"

#include <maya/widget/picker.hpp>

#include <string>

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace se = agentty::settings;

namespace {

struct Badge { std::string glyph; Color color; };
Badge action_badge(se::Action a) {
    switch (a) {
        case se::Action::CycleProfile: return {"\xe2\x86\xbb", info};      // ↻
        case se::Action::OpenRag:
        case se::Action::OpenSmart:    return {"\xe2\x86\x92", highlight};  // →
        case se::Action::RemovePlugin: return {"\xe2\x9c\x95", danger};     // ✕
        case se::Action::ApproveHooks: return {"\xe2\x9c\x93", warn};       // ✓
        case se::Action::None:
        default:                       return {" ", muted};
    }
}

// Whether this concern supports the inline `a`dd flow.
bool can_add(se::Category c) {
    return c == se::Category::Plugins
        || c == se::Category::Commands
        || c == se::Category::Agents;
}

// The prompt shown while typing a new entry, per concern.
const char* add_prompt(se::Category c) {
    switch (c) {
        case se::Category::Plugins:
            return "name command args\xe2\x80\xa6  (or: name --python f.py / --uvx pkg / --npx pkg)";
        case se::Category::Commands: return "new command name (creates .agentty/commands/<name>.md)";
        case se::Category::Agents:   return "new agent name (creates .agentty/agents/<name>.md)";
        default:                     return "";
    }
}

} // namespace

Element settings_list_picker(const Model& m) {
    const auto* o = settings_list_opened(m.ui.settings_list);
    if (!o) return nothing();

    const bool adding = o->input_active;
    auto rows = se::items_for(m, o->concern);

    Picker::Config cfg;
    cfg.title      = std::string{" "} + se::label(o->concern) + " ";
    cfg.accent     = highlight;   // cyan, matching the command palette
    cfg.min_width  = 64;
    cfg.viewport_h = 14;
    cfg.scroll     = nullptr;
    cfg.selected   = adding ? -1 : o->index;

    // ── Header ───────────────────────────────────────────────────
    if (adding) {
        // The live add prompt: label, then the typed buffer + block caret.
        cfg.header.push_back(h(
            text("  "), text(add_prompt(o->concern), fg_dim(muted))
        ).build());
        cfg.header.push_back(h(
            text("  \xe2\x9d\xaf ", fg_of(highlight)),          // ❯
            text(o->input, fg_of(fg)),
            text("\xe2\x96\x8a", fg_of(highlight))              // ▊ caret
        ).build());
    } else {
        cfg.header.push_back(h(
            text("  "), text(se::subtitle(o->concern), fg_dim(muted))
        ).build());
    }
    cfg.header.push_back(text(""));

    // ── Rows (dimmed while adding, to focus the prompt) ──────────
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const auto& it = rows[static_cast<std::size_t>(i)];
        const Badge b = action_badge(it.action);

        Picker::Config::Row row;
        row.badge       = b.glyph;
        row.badge_style = fg_of(adding ? muted : b.color);
        row.leading       = it.primary;
        row.leading_style = fg_of(adding ? muted : fg);
        row.trailing       = it.secondary;
        row.trailing_style = fg_dim(muted);
        row.selected       = !adding && (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    // ── Footer ───────────────────────────────────────────────────
    cfg.footer.push_back(text(""));
    if (adding) {
        cfg.footer.push_back(h(
            text("  \xe2\x86\xb5 ", fg_of(highlight)), text("create   ", fg_dim(muted)),
            text("esc ", fg_of(fg)), text("cancel", fg_dim(muted))
        ).build());
    } else {
        // Action hint for the focused row, then keys (+ `a add`).
        std::string act_hint;
        if (o->index >= 0 && o->index < static_cast<int>(rows.size()))
            act_hint = rows[static_cast<std::size_t>(o->index)].hint;
        if (!act_hint.empty())
            cfg.footer.push_back(h(
                text("  \xe2\x86\xb5 ", fg_of(highlight)),
                text(act_hint, fg_of(fg))
            ).build());

        std::vector<Element> keys;
        keys.push_back(text("  \xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)));
        keys.push_back(text("move   ", fg_dim(muted)));
        keys.push_back(text("\xe2\x86\xb5 ", fg_of(fg)));
        keys.push_back(text("act   ", fg_dim(muted)));
        if (can_add(o->concern)) {
            keys.push_back(text("a ", fg_of(success)));
            keys.push_back(text("add   ", fg_dim(muted)));
        }
        keys.push_back(text("esc ", fg_of(fg)));
        keys.push_back(text("close", fg_dim(muted)));
        cfg.footer.push_back(h(std::move(keys)).build());
    }

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
