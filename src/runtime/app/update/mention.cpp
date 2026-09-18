// mention_update — reducer for `msg::MentionMsg`. The @file
// picker reads a snapshot of the workspace's files and filters it
// per-keystroke. On select, an Attachment
// of kind FileRef is appended to composer.attachments and the inline
// SOH placeholder is inserted at the cursor; the file's bytes are
// loaded later, at submit time (modal.cpp), so an edited file is read
// with its latest contents.
//
// The snapshot/memo/cursor mechanics are the shared FilteredPicker's
// (panel/filtered_picker.hpp). Notably the COLD-OPEN REFILL: it used to live
// only in the `MentionInput` arm behind an `o->files.empty()` guard, so a
// picker opened before the index published and then arrowed (rather than
// typed into) never refilled — and once the query was non-empty the guard
// could never fire again, pinning the picker to whatever partial list it
// caught on keystroke one. The refill now happens on every READ of the
// snapshot, which is total by construction.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/panel/mention.hpp"
#include "agentty/workspace/files.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

using maya::overload;

Step mention_update(Model m, msg::MentionMsg mm) {
    return std::visit(overload{
        // (No OpenMention arm: the panel opens from the COMPOSER's `@`
        // handler, which builds pn::Mention directly with the file listing
        // — see composer.cpp. A message nobody sent sat here for months.)
        [&](CloseMention) -> Step {
            ascend(m);   // usually → thread (opened by typing @), or a parent
            return done(std::move(m));
        },
        [&](MentionInput& e) -> Step {
            if (auto* o = m.ui.panel.get<pn::Mention>()) o->picker.type(e.ch);
            return done(std::move(m));
        },
        [&](MentionBackspace) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (!o) return done(std::move(m));
            // Backspace on an empty query closes the picker — same
            // affordance as command palette + most chat apps. The picker
            // answers "was there anything to erase", so the test and the
            // mutation cannot drift apart.
            if (!o->picker.backspace()) m.ui.panel.close<pn::Mention>();
            return done(std::move(m));
        },
        [&](MentionMove& e) -> Step {
            if (auto* o = m.ui.panel.get<pn::Mention>()) o->picker.move(e.delta);
            return done(std::move(m));
        },
        [&](MentionSelect) -> Step {
            auto* o = m.ui.panel.get<pn::Mention>();
            if (!o) return done(std::move(m));
            const auto* hit = o->picker.selected();
            if (!hit) {
                m.ui.panel.close<pn::Mention>();
                return done(std::move(m));
            }
            // COPY, not move: the snapshot is shared and immutable now, so
            // stealing the string out of it would corrupt the cached index
            // every other reader (and the next `@`) sees. The old move was
            // only safe because each picker owned a private deep copy of the
            // whole file list — which is precisely the copy we removed.
            std::string path = *hit;
            m.ui.panel.close<pn::Mention>();

            // Frecency: this path just got referenced — rank it near the
            // top of the next `@`. Cheap, bounded recency window.
            note_file_referenced(path);

            // Append a FileRef attachment + insert its placeholder at
            // the composer cursor. Body is filled at submit time
            // (modal.cpp) so a file edited between selection and send
            // reaches the model with its current bytes.
            Attachment att;
            att.kind = Attachment::Kind::FileRef;
            att.path = std::move(path);
            std::size_t idx = m.ui.composer.attachments.size();
            m.ui.composer.attachments.push_back(std::move(att));

            auto placeholder = attachment::make_placeholder(idx);
            m.ui.composer.text.insert(m.ui.composer.cursor, placeholder);
            m.ui.composer.cursor += static_cast<int>(placeholder.size());
            return done(std::move(m));
        },
    }, mm);
}

} // namespace agentty::app::detail
