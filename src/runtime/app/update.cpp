// agentty::app::update — pure (Model, Msg) -> (Model, Cmd<Msg>) reducer.
//
// Top-level orchestrator: a single 10-arm std::visit that dispatches on
// the domain (msg::ComposerMsg / msg::StreamMsg / …) and forwards to
// the matching per-domain reducer in update/<domain>.cpp.
//
// The previous version of this file inlined all 79 leaf arms in one
// overload{} — sizeof(Msg) was pinned by the heaviest leaf no matter
// which path was active, the std::visit instantiated a 79×N dispatch
// table, and any leaf change forced this whole TU to rebuild (~19 s on
// modest hardware). v2 splits the work: leaves are grouped into 10
// domain sub-variants in msg.hpp; each domain has its own visit in its
// own TU, so:
//
//   • this file's std::visit is 10 arms, one tiny dispatch table.
//   • update/<domain>.cpp recompiles only when its own leaves change.
//   • call sites still construct Msg via `Msg{ComposerEnter{}}` /
//     `dispatch(StreamTextDelta{...})` — std::variant's converting
//     constructor walks each domain alternative; only the matching
//     domain accepts a given leaf, so the wrap is unambiguous.

#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/update/internal.hpp"

namespace agentty::app {

using maya::Cmd;
using maya::overload;

// Returns true when `msg` is a USER-INPUT-driven message — composer
// keystrokes, picker/palette navigation, login flow, diff-review
// actions, todo edits. False for Tick (animation frame), StreamMsg
// (network events), and ToolMsg (background tool worker results).
// The needs_force_redraw consumer below uses this to fire the redraw
// only on the *first user input* after streaming settled, not on
// every Tick that lands between.
[[nodiscard]] static bool is_user_input(const Msg& m) noexcept {
    return std::holds_alternative<msg::ComposerMsg>(m)
        || std::holds_alternative<msg::ModelPickerMsg>(m)
        || std::holds_alternative<msg::ThreadListMsg>(m)
        || std::holds_alternative<msg::CommandPaletteMsg>(m)
        || std::holds_alternative<msg::MentionPaletteMsg>(m)
        || std::holds_alternative<msg::SymbolPaletteMsg>(m)
        || std::holds_alternative<msg::TodoMsg>(m)
        || std::holds_alternative<msg::LoginMsg>(m)
        || std::holds_alternative<msg::DiffReviewMsg>(m);
}

std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
    // Consume the needs_force_redraw flag: when streaming settled and
    // this Msg is the first user input since, batch a Cmd::force_redraw
    // alongside the normal Cmd. The redraw fires AFTER the regular
    // reducer's effects so any model changes from the input land in
    // the full repaint. Tick / Stream / Tool messages don't consume
    // the flag — those are background events the user didn't trigger.
    const bool fire_redraw = m.ui.needs_force_redraw && is_user_input(msg);
    if (fire_redraw) m.ui.needs_force_redraw = false;

    auto step = std::visit(overload{
        [&](msg::ComposerMsg cm)       { return detail::composer_update     (std::move(m), std::move(cm)); },
        [&](msg::StreamMsg sm)         { return detail::stream_update       (std::move(m), std::move(sm)); },
        [&](msg::ToolMsg tm)           { return detail::tool_update         (std::move(m), std::move(tm)); },
        [&](msg::ModelPickerMsg pm)    { return detail::model_picker_update (std::move(m), std::move(pm)); },
        [&](msg::ThreadListMsg tm)     { return detail::thread_list_update  (std::move(m), std::move(tm)); },
        [&](msg::CommandPaletteMsg pm) { return detail::palette_update      (std::move(m), std::move(pm)); },
        [&](msg::MentionPaletteMsg mm) { return detail::mention_update      (std::move(m), std::move(mm)); },
        [&](msg::SymbolPaletteMsg sm)  { return detail::symbol_update       (std::move(m), std::move(sm)); },
        [&](msg::TodoMsg tm)           { return detail::todo_update         (std::move(m), std::move(tm)); },
        [&](msg::LoginMsg lm)          { return detail::login_update        (std::move(m), std::move(lm)); },
        [&](msg::DiffReviewMsg dm)     { return detail::diff_review_update  (std::move(m), std::move(dm)); },
        [&](msg::MetaMsg mm)           { return detail::meta_update         (std::move(m), std::move(mm)); },
    }, msg);

    if (fire_redraw) {
        std::vector<Cmd<Msg>> batched;
        batched.reserve(2);
        batched.push_back(std::move(step.second));
        batched.push_back(Cmd<Msg>::force_redraw());
        step.second = Cmd<Msg>::batch(std::move(batched));
    }
    return step;
}

} // namespace agentty::app
