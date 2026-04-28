#include "moha/runtime/view/changes.hpp"

#include <maya/widget/changes_strip.hpp>

#include "moha/runtime/view/palette.hpp"

namespace moha::ui {

using namespace maya;

Element changes_strip(const Model& m) {
    if (m.d.pending_changes.empty()) return Element{TextElement{}};

    std::vector<maya::FileChange> changes;
    changes.reserve(m.d.pending_changes.size());
    for (const auto& c : m.d.pending_changes) {
        changes.push_back({
            .path          = c.path,
            .kind          = c.original_contents.empty()
                               ? maya::FileChangeKind::Created
                               : maya::FileChangeKind::Modified,
            .lines_added   = c.added,
            .lines_removed = c.removed,
        });
    }

    return maya::ChangesStrip{{
        .changes      = std::move(changes),
        .border_color = warn,
        .text_color   = fg,
        .accept_color = success,
        .reject_color = danger,
    }}.build();
}

} // namespace moha::ui
