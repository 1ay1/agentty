#pragma once
// agentty::ui::overlay — THE single source of truth for overlay routing.
//
// Before this header existed, "which overlay is open?" was answered
// independently by THREE hand-synchronized code sites:
//
//   • subscribe.cpp — 18 `is_open` bools + a 20-deep `if (in_X) return
//     on_X(ev)` priority chain (keyboard routing)
//   • view.cpp pick_overlay() — a SECOND 19-deep chain deciding what
//     renders — in a DIFFERENT order than subscribe's
//   • the paste handler — a third, partial copy of the same question
//
// Exclusivity (only one overlay open at a time) was a convention enforced
// by ~17 scattered `m.ui.X = pick::Closed{}` writes in the open-reducers.
// If any two overlays ever ended up open together, subscribe routed keys
// to one while the view rendered the OTHER — the worst modal-UI bug class
// there is, latent behind comments like "both check model_picker BEFORE
// provider_picker".
//
// Now: `overlay::top(m)` computes the active overlay from the model in ONE
// canonical priority order. subscribe.cpp routes keys to top(m); view.cpp
// renders top(m). They consume the same function, so they CANNOT disagree —
// even if two overlays are simultaneously open (a reducer bug), keys and
// pixels still go to the same surface, degrading the failure from
// "keyboard controls an invisible window" to "one overlay temporarily
// shadows another".
//
// Adding an overlay is now: one Kind, one arm in top(), one routing arm,
// one view arm — and the compiler's -Wswitch on the Kind enum points at
// every site that needs the new arm.
//
// The navigation VOCABULARY is likewise centralised: nav.hpp's NavSpec
// gives every list overlay the same Esc/Enter/arrows/vim/paging grammar
// from one table, so a new picker inherits consistent navigation instead
// of hand-copying a switch (see subscribe.cpp).

#include <optional>

#include "agentty/runtime/model.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/picker.hpp"

namespace agentty::ui::overlay {

// Every overlay surface, in CANONICAL PRIORITY ORDER (highest first).
// This ordering is the one previously implied — divergently — by the
// subscribe/view chains:
//   1. login        — auth gates everything; owns the whole keyboard.
//   2. permission   — a tool is blocked awaiting y/n; must be answerable
//                     over any picker.
//   3. palettes     — transient input-companions (command/mention/symbol)
//                     that open OVER the composer.
//   4. viewers      — code blocks, tool output, checkpoints, settings.
//   5. pickers      — model/fused/provider/thread/smart.
//   6. panes        — diff review, todo (todo deliberately LAST: it is
//                     ambient — most keys fall through to global).
enum class Kind {
    None,
    Login,
    Permission,
    CommandPalette,
    Mention,
    Symbol,
    CodeBlocks,
    CodeBlockResult,
    ToolViewer,
    Checkpoints,
    RagSettings,
    SettingsList,
    Fork,
    ModelPicker,
    FusedPicker,
    ProviderPicker,
    ThreadList,
    SmartMode,
    DiffReview,
    Todo,
};

// The active (topmost) overlay. THE routing function: subscribe.cpp sends
// keys to it, view.cpp renders it, the paste handler consults it. O(#kinds)
// of cheap predicate calls, run per key event / per frame — negligible.
[[nodiscard]] inline Kind top(const Model& m) noexcept {
    if (login::is_open(m.ui.login))                    return Kind::Login;
    if (m.d.pending_permission.has_value())            return Kind::Permission;
    if (agentty::is_open(m.ui.command_palette))        return Kind::CommandPalette;
    if (agentty::mention_is_open(m.ui.mention_palette))
        return Kind::Mention;
    if (agentty::symbol_palette_is_open(m.ui.symbol_palette))
        return Kind::Symbol;
    if (agentty::code_block_picker_is_open(m.ui.code_blocks))
        return Kind::CodeBlocks;
    if (agentty::code_block_result_is_open(m.ui.code_blocks))
        return Kind::CodeBlockResult;
    if (agentty::tool_viewer_is_open(m.ui.tool_viewer))
        return Kind::ToolViewer;
    if (agentty::checkpoint_picker_is_open(m.ui.checkpoints))
        return Kind::Checkpoints;
    if (agentty::rag_settings_is_open(m.ui.rag_settings))
        return Kind::RagSettings;
    if (agentty::settings_list_is_open(m.ui.settings_list))
        return Kind::SettingsList;
    if (agentty::fork_picker_is_open(m.ui.fork_picker))
        return Kind::Fork;
    if (pick::is_open(m.ui.model_picker))              return Kind::ModelPicker;
    if (pick::is_open(m.ui.fused_picker))              return Kind::FusedPicker;
    if (pick::is_open(m.ui.provider_picker))           return Kind::ProviderPicker;
    if (pick::is_open(m.ui.thread_list))               return Kind::ThreadList;
    if (pick::is_open(m.ui.smart_mode))                return Kind::SmartMode;
    if (pick::is_open(m.ui.diff_review))               return Kind::DiffReview;
    if (pick::is_open(m.ui.todo.open))                 return Kind::Todo;
    return Kind::None;
}

} // namespace agentty::ui::overlay
