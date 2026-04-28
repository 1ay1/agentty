#include "moha/runtime/view/view.hpp"

#include <maya/widget/app_layout.hpp>

#include "moha/runtime/login.hpp"
#include "moha/runtime/view/changes.hpp"
#include "moha/runtime/view/composer.hpp"
#include "moha/runtime/view/diff_review.hpp"
#include "moha/runtime/view/login.hpp"
#include "moha/runtime/view/pickers.hpp"
#include "moha/runtime/view/statusbar.hpp"
#include "moha/runtime/view/thread.hpp"

namespace moha::ui {

namespace {

// Pick the active overlay, if any. Login modal has highest priority —
// auth is the gating step, no other UI should appear over it.
struct OverlayPick {
    maya::Element element{maya::TextElement{}};
    bool          present = false;
};

OverlayPick pick_overlay(const Model& m) {
    if (login::is_open(m.ui.login))        return {login_modal(m),     true};
    if (pick::is_open(m.ui.model_picker))  return {model_picker(m),    true};
    if (pick::is_open(m.ui.thread_list))   return {thread_list(m),     true};
    if (is_open(m.ui.command_palette))     return {command_palette(m), true};
    if (pick::is_open(m.ui.diff_review))   return {diff_review(m),     true};
    if (pick::is_open(m.ui.todo.open))     return {todo_modal(m),      true};
    return {};
}

} // namespace

maya::Element view(const Model& m) {
    auto ov = pick_overlay(m);
    return maya::AppLayout{{
        .thread_panel    = thread_panel(m),
        .changes_strip   = changes_strip(m),
        .composer        = composer(m),
        .status_bar      = status_bar(m),
        .overlay         = std::move(ov.element),
        .overlay_present = ov.present,
    }}.build();
}

} // namespace moha::ui
