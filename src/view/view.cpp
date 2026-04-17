#include "moha/view/view.hpp"

#include <vector>

#include "moha/view/changes.hpp"
#include "moha/view/composer.hpp"
#include "moha/view/diff_review.hpp"
#include "moha/view/pickers.hpp"
#include "moha/view/statusbar.hpp"
#include "moha/view/thread.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element view(const Model& m) {
    // Use the runtime BoxBuilder so we can set explicit width=percent(100)
    // and align_items=Stretch on the chrome — these don't reliably propagate
    // through nested DSL pipe wrappers, which leaves the composer/statusbar
    // hugging their intrinsic content size instead of spanning the terminal.
    auto thread_row = vstack()
        .width(Dimension::percent(100))
        .grow(1.0f)
        .align_items(Align::Stretch)
        (thread_panel(m));

    auto base = vstack()
        .padding(1)
        .width(Dimension::percent(100))
        .grow(1.0f)
        .align_items(Align::Stretch)
        (std::move(thread_row),
         changes_strip(m),
         composer(m),
         status_bar(m));

    Element overlay;
    bool has_overlay = false;

    if (m.model_picker.open)        { overlay = model_picker(m);  has_overlay = true; }
    else if (m.thread_list.open)    { overlay = thread_list(m);   has_overlay = true; }
    else if (m.command_palette.open){ overlay = command_palette(m);has_overlay = true; }
    else if (m.diff_review.open)    { overlay = diff_review(m);   has_overlay = true; }
    else if (m.todo.open)           { overlay = todo_modal(m);    has_overlay = true; }

    if (has_overlay)
        return zstack({std::move(base),
            vstack().align_items(Align::Center).justify(Justify::End)(
                vstack().bg(Color::default_color())(std::move(overlay)))});

    return base;
}

} // namespace moha::ui
