// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/app/deps.hpp"

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

Cmd diff_review_update(Model& m, msg::DiffReviewMsg dm) {
    // Persist one file's REVIEW DECISION to disk. The tool already wrote the
    // file when it ran, so `new_contents` is what's on disk now. Accept = keep;
    // Reject = revert. diff::apply_accepted() reconstructs the file with only
    // the accepted hunks applied on top of the original — so a file with any
    // rejected hunk gets rewritten, and an all-accepted file is left as-is.
    // Pending (undecided) hunks are treated as accepted on close (the change
    // is already live; not touching it keeps it).
    // Returns the write as a VALUE; the caller batches it into its result
    // and the host performs it. none() when the file needs no rewrite.
    auto persist = [](const FileChange& fc) -> Cmd {
        bool any_reject = false;
        for (const auto& hk : fc.hunks)
            if (hk.status == Hunk::Status::Rejected) { any_reject = true; break; }
        if (!any_reject) return Cmd::none();     // nothing to revert; disk is correct
        return Cmd(WriteFile{fc.path, diff::apply_accepted(fc)});
    };
    // Advance the cursor to the next still-PENDING hunk in the current file so
    // a decision flows the reviewer forward (like accepting a git add -p). If
    // none remain in this file, hop to the next file with pending hunks; wraps.
    auto advance = [&](pick::OpenAtCell* c) -> bool {
        const int nfiles = static_cast<int>(m.d.pending_changes.size());
        for (int fo = 0; fo < nfiles; ++fo) {
            int fi = (c->file_index + fo) % nfiles;
            const auto& hunks = m.d.pending_changes[static_cast<std::size_t>(fi)].hunks;
            int start = (fo == 0) ? c->hunk_index : 0;
            for (int ho = 0; ho < static_cast<int>(hunks.size()); ++ho) {
                int hi = (start + ho) % static_cast<int>(hunks.size());
                if (hunks[static_cast<std::size_t>(hi)].status == Hunk::Status::Pending) {
                    c->file_index = fi; c->hunk_index = hi;
                    c->body_scroll = 0;   // fresh hunk → top
                    return true;
                }
            }
        }
        // Nothing pending anywhere — all hunks are decided. The caller
        // commits and closes rather than parking the cursor on an
        // already-decided hunk.
        return false;
    };

    // Commit every decision and close the pane. Shared by CloseDiffReview
    // (Esc/^R) and the per-hunk handlers' auto-close when the last pending
    // hunk gets decided — the flow must not depend on the user knowing
    // which keystroke happens to be the last one. `done` selects the toast
    // verb: "review complete" when every hunk was decided (auto-close) vs
    // "review closed" when the user bailed mid-review keeping the rest
    // (Esc/^R).
    auto commit_and_close = [&](bool done) -> Cmd {
        int reverted = 0, kept = 0;
        std::vector<Cmd> writes;
        for (const auto& fc : m.d.pending_changes) {
            for (const auto& hk : fc.hunks) {
                if (hk.status == Hunk::Status::Rejected) ++reverted;
                else ++kept;   // accepted OR pending — both stay live
            }
            writes.push_back(persist(fc));
        }
        m.d.pending_changes.clear();
        m.d.reject_all_armed = false;   // review resolved → guard consumed
        ascend(m);   // decision committed above; Esc lands where you came from
        auto cmd = set_status_toast(m,
            reverted == 0
                ? (done ? "review complete — all " : "review closed — all ")
                    + std::to_string(kept)
                    + (kept == 1 ? " change" : " changes") + " kept"
                : (done ? "review complete — " : "review closed — ")
                    + std::to_string(reverted)
                    + " reverted, " + std::to_string(kept) + " kept");
        writes.push_back(std::move(cmd));
        return Cmd::batch(std::move(writes));
    };

    // Clamp a possibly-stale cursor against the CURRENT changeset. Hunk
    // counts can shrink while the pane is open: a new tool edit to the same
    // path collapses into its existing review entry (*it = diff::compute(
    // original, latest)), which may yield fewer hunks than the cursor's
    // index. The view clamps defensively for render; the reducer must too or
    // fc.hunks[c->hunk_index] is out-of-bounds UB on the next y/n/Enter.
    auto clamp_cursor = [&](pick::OpenAtCell* c) -> FileChange* {
        if (!c || m.d.pending_changes.empty()) return nullptr;
        const int nfiles = static_cast<int>(m.d.pending_changes.size());
        if (c->file_index >= nfiles) c->file_index = nfiles - 1;
        if (c->file_index < 0)       c->file_index = 0;
        auto& fc = m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
        const int nh = static_cast<int>(fc.hunks.size());
        if (c->hunk_index >= nh) c->hunk_index = nh > 0 ? nh - 1 : 0;
        if (c->hunk_index < 0)   c->hunk_index = 0;
        return &fc;
    };

    // Disarm the two-press guard on ANY diff-review action other than the
    // confirming second press — a stray first press must not leave a live
    // "next X/^X nukes everything" trap behind a j/k or scroll. Two guards:
    // inside the pane (confirm_reject_all on the cell) and outside it
    // (reject_all_armed on the domain, for X when the pane is closed).
    const bool arming =
        std::holds_alternative<RejectAllChanges>(dm);
    if (!arming) {
        if (auto* c = m.ui.panel.get<pn::DiffReview>())
            c->confirm_reject_all = false;
        m.d.reject_all_armed = false;
    }

    return std::visit(overload{
        [&](OpenDiffReview) -> Cmd {
            // Tell the user when there's nothing to review instead of
            // silently doing nothing — opening an empty pane would just
            // flicker the screen and leave them confused about whether
            // their keystroke registered.
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to review");
                return cmd;
            }
            m.ui.panel.descend(pn::DiffReview{{0, 0}});
            return Cmd::none();
        },
        [&](CloseDiffReview) -> Cmd {
            // Persist every file's decision on the way out, then clear the
            // queue — closing the pane commits the review. Say WHAT closing
            // meant: undecided hunks are kept (the change is already live on
            // disk), which is invisible unless we announce it.
            return commit_and_close(/*done=*/false);
        },
        [&](DiffReviewMove& e) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            auto* fc = clamp_cursor(c);
            if (!fc) return Cmd::none();
            int sz = static_cast<int>(fc->hunks.size());
            if (sz == 0) return Cmd::none();
            c->hunk_index = (c->hunk_index + e.delta + sz) % sz;
            c->body_scroll = 0;   // a newly-focused hunk starts at its top
            return Cmd::none();
        },
        [&](DiffReviewScroll& e) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            auto* fc = clamp_cursor(c);
            if (!fc || fc->hunks.empty()) return Cmd::none();
            const auto& hk =
                fc->hunks[static_cast<std::size_t>(c->hunk_index)];
            // Upper bound on the hunk's body rows: its patch line count.
            // The view clamps precisely against the parsed row count; this
            // just keeps the offset from running away unboundedly.
            const int max_rows = static_cast<int>(
                std::count(hk.patch.begin(), hk.patch.end(), '\n')) + 1;
            c->body_scroll = std::clamp(c->body_scroll + e.delta,
                                        0, std::max(0, max_rows - 1));
            return Cmd::none();
        },
        [&](DiffReviewNextFile) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (!c || m.d.pending_changes.empty()) return Cmd::none();
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index + 1) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return Cmd::none();
        },
        [&](DiffReviewPrevFile) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (!c || m.d.pending_changes.empty()) return Cmd::none();
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index - 1 + sz) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return Cmd::none();
        },
        [&](AcceptHunk) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (auto* fc = clamp_cursor(c)) {
                if (!fc->hunks.empty())
                    fc->hunks[static_cast<std::size_t>(c->hunk_index)].status =
                        Hunk::Status::Accepted;
                // Last decision made → commit now and close the pane. There
                // is no next hunk to land on, so demanding an extra Esc (whose
                // other meaning on a non-final press is "reject the rest")
                // would train two opposed functions into one key.
                if (!advance(c)) return commit_and_close(/*done=*/true);
            }
            return Cmd::none();
        },
        [&](RejectHunk) -> Cmd {
            auto* c = m.ui.panel.get<pn::DiffReview>();
            if (auto* fc = clamp_cursor(c)) {
                if (!fc->hunks.empty())
                    fc->hunks[static_cast<std::size_t>(c->hunk_index)].status =
                        Hunk::Status::Rejected;
                if (!advance(c)) return commit_and_close(/*done=*/true);
            }
            return Cmd::none();
        },
        [&](AcceptAllChanges) -> Cmd {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to accept");
                return cmd;
            }
            // Accept = keep what the tools already wrote; nothing to persist.
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Accepted; ++hunks; }
            m.d.pending_changes.clear();
            m.d.reject_all_armed = false;   // any resolution consumes the guard
            ascend(m);   // decision committed above; Esc lands where you came from
            auto cmd = set_status_toast(m,
                "accepted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks"));
            return cmd;
        },
        [&](RejectAllChanges rm) -> Cmd {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to reject");
                return cmd;
            }
            // TWO-PRESS guard for the key chords. Inside the pane (^X) the
            // first press arms the cell flag and the second executes; from
            // the changes strip (^X with the pane closed) the same protocol
            // runs on m.d.reject_all_armed. Either way a single keypress must
            // never revert every touched file. Mirrors the thread picker's
            // two-press delete.
            //
            // `rm.confirmed` bypasses the guard: the palette's "Reject all"
            // row is already open → select → Enter, so asking for a second
            // press there would be ceremony. The intent rides on the MESSAGE
            // rather than being pre-written into the Model by the caller — the
            // reducer stays the only writer of its own guard state.
            if (!rm.confirmed) {
                if (auto* c = m.ui.panel.get<pn::DiffReview>()) {
                    // Inside the pane: arm the cell flag on the first press.
                    if (!c->confirm_reject_all) {
                        c->confirm_reject_all = true;
                        auto cmd = set_status_toast(m,
                            "press ^X again to revert ALL changes — any other key cancels");
                        return cmd;
                    }
                } else if (!m.d.reject_all_armed) {
                    // Outside the pane: arm the domain flag on the first
                    // press. The guard is consumed by any review resolution
                    // (^X, ^A, ^R, new-turn submit), so it can never survive
                    // into a later turn and fire on an unrelated key.
                    m.d.reject_all_armed = true;
                    auto cmd = set_status_toast(m,
                        "press ^X again to revert ALL changes");
                    return cmd;
                }
            }
            // Reject ALL = revert every touched file to its original contents
            // on disk (the tools already wrote the new version, so this undoes
            // them). apply_accepted() with every hunk Rejected yields exactly
            // original_contents.
            int hunks = 0, files = 0;
            std::vector<Cmd> writes;
            for (auto& fc : m.d.pending_changes) {
                for (auto& h : fc.hunks) { h.status = Hunk::Status::Rejected; ++hunks; }
                writes.push_back(Cmd(WriteFile{fc.path, fc.original_contents}));
                ++files;
            }
            m.d.pending_changes.clear();
            m.d.reject_all_armed = false;   // executed → guard consumed
            ascend(m);   // decision committed above; Esc lands where you came from
            auto cmd = set_status_toast(m,
                "reverted " + std::to_string(hunks)
                + (hunks == 1 ? " hunk" : " hunks")
                + " across " + std::to_string(files)
                + (files == 1 ? " file" : " files"));
            writes.push_back(std::move(cmd));
            return Cmd::batch(std::move(writes));
        },
    }, dm);
}

} // namespace agentty::app::detail
