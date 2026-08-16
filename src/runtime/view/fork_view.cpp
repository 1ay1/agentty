// fork_view.cpp — the fork picker overlay.
//
// Three rows: how proactive RAG behaves in the fork. A fork always starts
// FRESH (near-zero context; the parent transcript is readable on demand),
// so the only choice is RAG behaviour. Enter forks with the highlighted
// row. Own TU, matching rag_settings_view.

#include "agentty/runtime/view/pickers.hpp"

#include "agentty/runtime/view/helpers.hpp"
#include "agentty/runtime/view/palette.hpp"
#include "agentty/runtime/fork_picker.hpp"

#include <maya/widget/picker.hpp>

#include <string>

namespace agentty::ui {

using namespace maya;
using namespace maya::dsl;
namespace fp = agentty::fork_picker;

namespace {

struct RowSpec { const char* label; const char* help; };

const RowSpec kRows[fp::kChoiceCount] = {
    {"RAG per turn",   "fresh thread · retrieve context before every turn"},
    {"First-turn RAG", "fresh thread · retrieve once, up front"},
    {"RAG off",        "fresh thread · no retrieval"},
};

} // namespace

Element fork_picker_view(const Model& m) {
    const auto* o = fork_picker_opened(m.ui.fork_picker);
    if (!o) return nothing();

    Picker::Config cfg;
    cfg.title      = " Fork thread ";
    cfg.accent     = info;
    cfg.min_width  = 52;
    cfg.viewport_h = fp::kChoiceCount + 2;
    cfg.scroll     = nullptr;
    cfg.selected   = o->index;

    for (int i = 0; i < fp::kChoiceCount; ++i) {
        Picker::Config::Row row;
        row.leading       = kRows[i].label;
        row.leading_style = fg_of(fg);
        row.trailing      = kRows[i].help;
        row.trailing_style = fg_dim(muted);
        row.selected      = (i == o->index);
        cfg.rows.push_back(std::move(row));
    }

    cfg.footer.push_back(text(""));
    cfg.footer.push_back(text(
        "  A fork starts FRESH (near-zero context) · the old transcript is "
        "readable on demand.",
        fg_dim(muted)));
    cfg.footer.push_back(h(
        text("\xe2\x86\x91\xe2\x86\x93 ", fg_of(fg)),  text("choose   ", fg_dim(muted)),
        text("Enter ", fg_of(fg)), text("fork   ", fg_dim(muted)),
        text("Esc ", fg_of(fg)),   text("close", fg_dim(muted))
    ).build());

    return Picker{std::move(cfg)}.build();
}

} // namespace agentty::ui
