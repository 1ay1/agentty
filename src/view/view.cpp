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
    std::vector<Element> rows;
    rows.push_back(thread_panel(m));
    rows.push_back(changes_strip(m));
    rows.push_back(composer(m));
    rows.push_back(status_bar(m));

    auto base = (v(std::move(rows)) | pad<1>).build();

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
                vstack().bg(Color::black())(std::move(overlay)))});

    return base;
}

} // namespace moha::ui
