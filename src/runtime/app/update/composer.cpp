// composer_update — reducer for `msg::ComposerMsg`. The composer is purely
// local UI state (text, cursor, expanded flag, queued items); arms here
// don't reach into network/streaming/tools. ComposerEnter / ComposerSubmit
// route through detail::submit_message which handles the broader "kick a
// new turn" flow.

#include "moha/runtime/app/update.hpp"

#include <maya/core/overload.hpp>

#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/view/helpers.hpp"

namespace moha::app::detail {

using maya::overload;

Step composer_update(Model m, msg::ComposerMsg cm) {
    return std::visit(overload{
        [&](ComposerCharInput e) -> Step {
            auto utf8 = ui::utf8_encode(e.ch);
            m.ui.composer.text.insert(m.ui.composer.cursor, utf8);
            m.ui.composer.cursor += static_cast<int>(utf8.size());
            return done(std::move(m));
        },
        [&](ComposerBackspace) -> Step {
            if (m.ui.composer.cursor > 0 && !m.ui.composer.text.empty()) {
                int p = ui::utf8_prev(m.ui.composer.text, m.ui.composer.cursor);
                m.ui.composer.text.erase(p, m.ui.composer.cursor - p);
                m.ui.composer.cursor = p;
            }
            return done(std::move(m));
        },
        [&](ComposerEnter)  { return submit_message(std::move(m)); },
        [&](ComposerSubmit) { return submit_message(std::move(m)); },
        [&](ComposerNewline) -> Step {
            m.ui.composer.text.insert(m.ui.composer.cursor, "\n");
            m.ui.composer.cursor += 1;
            m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerToggleExpand) -> Step {
            m.ui.composer.expanded = !m.ui.composer.expanded;
            return done(std::move(m));
        },
        [&](ComposerCursorLeft) -> Step {
            m.ui.composer.cursor = ui::utf8_prev(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorRight) -> Step {
            m.ui.composer.cursor = ui::utf8_next(m.ui.composer.text, m.ui.composer.cursor);
            return done(std::move(m));
        },
        [&](ComposerCursorHome) -> Step {
            m.ui.composer.cursor = 0;
            return done(std::move(m));
        },
        [&](ComposerCursorEnd) -> Step {
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            return done(std::move(m));
        },
        [&](ComposerPaste& e) -> Step {
            m.ui.composer.text.insert(m.ui.composer.cursor, e.text);
            m.ui.composer.cursor += static_cast<int>(e.text.size());
            if (e.text.find('\n') != std::string::npos) m.ui.composer.expanded = true;
            return done(std::move(m));
        },
        [&](ComposerRecallQueued) -> Step {
            // No-op when there's nothing to recall — the caller (the
            // Up-arrow keymap) only emits this when the queue is
            // non-empty, but the predicate is racy across frames so
            // be defensive.
            if (m.ui.composer.queued.empty()) return done(std::move(m));

            // Drain the queue into the composer, joined by '\n', and
            // append any pre-existing composer text after another
            // '\n'. Mirrors Claude Code's `Lc_` (binary offset
            // 76303220): a single ↑ press drains the WHOLE editable
            // queue into one composer load — no per-item cycling.
            // Multi-line queued items keep their newlines so a paste
            // that became a queued message survives the recall
            // round-trip.
            std::string recalled;
            for (std::size_t i = 0; i < m.ui.composer.queued.size(); ++i) {
                if (i > 0) recalled.push_back('\n');
                recalled += m.ui.composer.queued[i];
            }
            // Cursor lands at the boundary between recalled text and
            // the user's pre-existing composer input — exactly where
            // they'd want to start editing or appending. (Claude
            // Code's seam is `O.join("\n").length + 1 + _`; we use
            // the same idea: end-of-recalled + 1 if there's anything
            // after, else end-of-recalled.)
            int boundary = static_cast<int>(recalled.size());
            if (!m.ui.composer.text.empty()) {
                recalled.push_back('\n');
                ++boundary;
                recalled += m.ui.composer.text;
            }
            m.ui.composer.text   = std::move(recalled);
            m.ui.composer.cursor = boundary;
            // Multi-line content → flip expanded so the composer's
            // `expanded` cap (16 rows) takes effect, not the 8-row
            // unexpanded cap. Same trigger as ComposerPaste.
            if (m.ui.composer.text.find('\n') != std::string::npos)
                m.ui.composer.expanded = true;
            // Destructive recall: queued items now live ONLY in the
            // composer buffer. Re-submit re-queues at the tail (fresh
            // tail position). Clearing the composer drops them. Same
            // trade-off as Claude Code — keeps the data model simple
            // (no "soft-deleted, recallable" intermediate state).
            m.ui.composer.queued.clear();
            return done(std::move(m));
        },
    }, cm);
}

} // namespace moha::app::detail
