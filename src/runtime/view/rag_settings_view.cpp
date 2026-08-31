// rag_settings_view.cpp — the RAG mode picker overlay.
//
// Three rows: On / First turn only / Off — how proactive (pre-turn) retrieval
// behaves. The current mode is marked; Enter/Space selects. A deliberately
// tiny pane: one decision, not a wall of toggles.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/rag_settings.hpp"

#include <maya/widget/picker.hpp>
#include <maya/platform/io.hpp>

#include <algorithm>
#include <string>

namespace ov = agentty::ui::overlay;

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace rs = agentty::rag_settings;

Element rag_settings_picker(const Model& m) {
    const auto* o = m.ui.overlay.get<ov::RagSettings>();
    if (!o) return nothing();

    // Which mode is currently persisted (to mark the active row with a dot).
    const store::RagMode active = o->active;

    Picker::Config cfg;
    cfg.title      = " RAG ";
    cfg.accent     = info;
    cfg.min_width  = 46;
    cfg.viewport_h = rs::kModeCount + 2;
    cfg.scroll     = nullptr;
    cfg.selected   = o->index;

    for (int i = 0; i < rs::kModeCount; ++i) {
        const store::RagMode mode = rs::kModes[i];
        const bool is_active = (mode == active);

        Picker::Config::Row row;
        row.badge       = is_active ? "\xe2\x97\x8f" : " ";   // ● active marker
        row.badge_style = fg_of(success);
        row.leading       = std::string{store::to_string(mode)};
        row.leading_style = fg_of(fg);
        row.trailing      = std::string{store::describe(mode)};
        row.trailing_style = fg_dim(muted);
        row.selected      = (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(h(
        text("\xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)),   text("move   ", fg_dim(muted)),
        text("Enter ", fg_of(fg)), text("select   ", fg_dim(muted)),
        text("Esc ", fg_of(fg)),   text("close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
