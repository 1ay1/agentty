#include "moha/view/view.hpp"

#include <vector>

#include "moha/view/changes.hpp"
#include "moha/view/composer.hpp"
#include "moha/view/diff_review.hpp"
#include "moha/view/header.hpp"
#include "moha/view/pickers.hpp"
#include "moha/view/statusbar.hpp"
#include "moha/view/thread.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

Element view(const Model& m) {
    std::vector<Element> rows;
    rows.push_back(header(m));
    rows.push_back(sep);
    rows.push_back((v(thread_panel(m)) | grow_<1>).build());
    rows.push_back(changes_strip(m));
    rows.push_back(composer(m));
    rows.push_back(status_bar(m));

    auto base = (v(std::move(rows)) | pad<1>).build();

    // Overlays — we render at most one at a time.
    if (m.model_picker.open)    return (v(base, model_picker(m))).build();
    if (m.thread_list.open)     return (v(base, thread_list(m))).build();
    if (m.command_palette.open) return (v(base, command_palette(m))).build();
    if (m.diff_review.open)     return (v(base, diff_review(m))).build();
    return base;
}

} // namespace moha::ui
