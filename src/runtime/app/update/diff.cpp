// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.

#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "moha/runtime/picker.hpp"

namespace moha::app::detail {

namespace pick = moha::ui::pick;
using maya::overload;

Step diff_review_update(Model m, msg::DiffReviewMsg dm) {
    return std::visit(overload{
        [&](OpenDiffReview) -> Step {
            // Tell the user when there's nothing to review instead of
            // silently doing nothing — opening an empty pane would just
            // flicker the screen and leave them confused about whether
            // their keystroke registered.
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to review");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.diff_review = ui::pick::TwoAxis{pick::OpenAtCell{0, 0}};
            return done(std::move(m));
        },
        [&](CloseDiffReview) -> Step {
            m.ui.diff_review = pick::Closed{};
            return done(std::move(m));
        },
        [&](DiffReviewMove& e) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.d.pending_changes[c->file_index];
            int sz = static_cast<int>(fc.hunks.size());
            if (sz == 0) return done(std::move(m));
            c->hunk_index = (c->hunk_index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index + 1) % sz;
            c->hunk_index = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index - 1 + sz) % sz;
            c->hunk_index = 0;
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (c && !m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[c->file_index];
                if (!fc.hunks.empty())
                    fc.hunks[c->hunk_index].status = Hunk::Status::Accepted;
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (c && !m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[c->file_index];
                if (!fc.hunks.empty())
                    fc.hunks[c->hunk_index].status = Hunk::Status::Rejected;
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to accept");
                return {std::move(m), std::move(cmd)};
            }
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Accepted;
                    ++hunks;
                }
            auto cmd = set_status_toast(m,
                "accepted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return {std::move(m), std::move(cmd)};
        },
        [&](RejectAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to reject");
                return {std::move(m), std::move(cmd)};
            }
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Rejected;
                    ++hunks;
                }
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
            auto cmd = set_status_toast(m,
                "rejected " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return {std::move(m), std::move(cmd)};
        },
    }, dm);
}

} // namespace moha::app::detail
