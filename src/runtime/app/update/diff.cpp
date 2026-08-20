// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/app/deps.hpp"

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

Step diff_review_update(Model m, msg::DiffReviewMsg dm) {
    // Persist one file's REVIEW DECISION to disk. The tool already wrote the
    // file when it ran, so `new_contents` is what's on disk now. Accept = keep;
    // Reject = revert. diff::apply_accepted() reconstructs the file with only
    // the accepted hunks applied on top of the original — so a file with any
    // rejected hunk gets rewritten, and an all-accepted file is left as-is.
    // Pending (undecided) hunks are treated as accepted on close (the change
    // is already live; not touching it keeps it).
    auto persist = [&](const FileChange& fc) {
        bool any_reject = false;
        for (const auto& hk : fc.hunks)
            if (hk.status == Hunk::Status::Rejected) { any_reject = true; break; }
        if (!any_reject) return;                 // nothing to revert; disk is correct
        if (deps().write_file)
            deps().write_file(fc.path, diff::apply_accepted(fc));
    };
    // Advance the cursor to the next still-PENDING hunk in the current file so
    // a decision flows the reviewer forward (like accepting a git add -p). If
    // none remain in this file, hop to the next file with pending hunks; wraps.
    auto advance = [&](pick::OpenAtCell* c) {
        const int nfiles = static_cast<int>(m.d.pending_changes.size());
        for (int fo = 0; fo < nfiles; ++fo) {
            int fi = (c->file_index + fo) % nfiles;
            const auto& hunks = m.d.pending_changes[static_cast<std::size_t>(fi)].hunks;
            int start = (fo == 0) ? c->hunk_index : 0;
            for (int ho = 0; ho < static_cast<int>(hunks.size()); ++ho) {
                int hi = (start + ho) % static_cast<int>(hunks.size());
                if (hunks[static_cast<std::size_t>(hi)].status == Hunk::Status::Pending) {
                    c->file_index = fi; c->hunk_index = hi; return;
                }
            }
        }
        // Nothing pending anywhere — leave the cursor where it is.
    };

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
            // Persist every file's decision on the way out, then clear the
            // queue — closing the pane commits the review.
            for (const auto& fc : m.d.pending_changes) persist(fc);
            m.d.pending_changes.clear();
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
                advance(c);
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (c && !m.d.pending_changes.empty()) {
                auto& fc = m.d.pending_changes[c->file_index];
                if (!fc.hunks.empty())
                    fc.hunks[c->hunk_index].status = Hunk::Status::Rejected;
                advance(c);
            }
            return done(std::move(m));
        },
        [&](AcceptAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to accept");
                return {std::move(m), std::move(cmd)};
            }
            // Accept = keep what the tools already wrote; nothing to persist.
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Accepted; ++hunks; }
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
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
            // Reject ALL = revert every touched file to its original contents
            // on disk (the tools already wrote the new version, so this undoes
            // them). apply_accepted() with every hunk Rejected yields exactly
            // original_contents.
            int hunks = 0, files = 0;
            for (auto& fc : m.d.pending_changes) {
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Rejected; ++hunks; }
                if (deps().write_file) {
                    deps().write_file(fc.path, fc.original_contents);
                    ++files;
                }
            }
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
            auto cmd = set_status_toast(m,
                "reverted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks")
                + " across " + std::to_string(files)
                + (files == 1 ? " file" : " files"));
            return {std::move(m), std::move(cmd)};
        },
    }, dm);
}

} // namespace agentty::app::detail
