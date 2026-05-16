// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.
//
// A reject decision is the only path that touches the filesystem: the
// edit tool already wrote the *full* new_contents on success, so accept
// is a no-op (file is already what the user wants); rejecting one or
// more hunks calls `diff::apply_accepted` to recompose the file from
// `original_contents` + the still-Accepted hunks and writes the result
// back atomically via `tools::util::write_file`. Accept-side toasts
// reassure the user nothing was rewritten; reject-side toasts confirm
// what the rewrite did (or surface a write_file error).

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <filesystem>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/diff/diff.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/tool/util/fs_helpers.hpp"

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

namespace {

// Rewrite `fc.path` to reflect the current Accepted/Rejected mix of
// hunks. Returns an empty string on success, or a write_file error
// detail on failure. Skips the FS call entirely if every hunk is
// still Accepted (the file on disk already reflects new_contents —
// the edit tool wrote it before we ever ran).
std::string apply_review_to_disk(FileChange& fc) {
    bool any_rejected = false;
    for (const auto& h : fc.hunks)
        if (h.status == Hunk::Status::Rejected) { any_rejected = true; break; }
    if (!any_rejected) return {};

    std::string recomposed = diff::apply_accepted(fc);
    auto err = tools::util::write_file(std::filesystem::path{fc.path}, recomposed);
    if (err.empty()) {
        // Update the FileChange's snapshot of "what's on disk now"
        // so a subsequent flip-back-to-accept doesn't try to rewrite
        // with a stale baseline.
        fc.new_contents = std::move(recomposed);
    }
    return err;
}

} // namespace

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
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.d.pending_changes[c->file_index];
            if (fc.hunks.empty()) return done(std::move(m));
            fc.hunks[c->hunk_index].status = Hunk::Status::Accepted;
            // Rewrite is only needed if a SIBLING hunk in this file is
            // still rejected — otherwise the file already matches
            // new_contents from the original tool run.
            auto err = apply_review_to_disk(fc);
            if (!err.empty()) {
                auto cmd = set_status_toast(m,
                    "accept hunk: write failed — " + err);
                return {std::move(m), std::move(cmd)};
            }
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = pick::opened(m.ui.diff_review);
            if (!c || m.d.pending_changes.empty()) return done(std::move(m));
            auto& fc = m.d.pending_changes[c->file_index];
            if (fc.hunks.empty()) return done(std::move(m));
            fc.hunks[c->hunk_index].status = Hunk::Status::Rejected;
            auto err = apply_review_to_disk(fc);
            if (!err.empty()) {
                auto cmd = set_status_toast(m,
                    "reject hunk: write failed — " + err);
                return {std::move(m), std::move(cmd)};
            }
            auto cmd = set_status_toast(m,
                "hunk reverted in " + fc.path);
            return {std::move(m), std::move(cmd)};
        },
        [&](AcceptAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to accept");
                return {std::move(m), std::move(cmd)};
            }
            int hunks = 0;
            // Pure status flip — file on disk already matches
            // new_contents (the edit tool wrote it on success), no FS
            // call needed. But if the user had earlier rejected some
            // hunks and is now accepting-all, we DO need to rewrite
            // each affected file with the full new_contents.
            std::string first_err;
            int rewritten = 0;
            for (auto& fc : m.d.pending_changes) {
                bool had_rejection = false;
                for (const auto& h : fc.hunks)
                    if (h.status == Hunk::Status::Rejected)
                        { had_rejection = true; break; }
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Accepted;
                    ++hunks;
                }
                if (had_rejection) {
                    auto err = tools::util::write_file(
                        std::filesystem::path{fc.path}, fc.new_contents);
                    if (err.empty()) ++rewritten;
                    else if (first_err.empty()) first_err = std::move(err);
                }
            }
            std::string msg_;
            if (!first_err.empty()) {
                msg_ = "accept all: write failed — " + first_err;
            } else if (rewritten > 0) {
                msg_ = "accepted " + std::to_string(hunks)
                     + (hunks == 1 ? " hunk (" : " hunks (")
                     + std::to_string(rewritten) + " file"
                     + (rewritten == 1 ? "" : "s") + " restored)";
            } else {
                msg_ = "accepted " + std::to_string(hunks)
                     + (hunks == 1 ? " hunk" : " hunks");
            }
            auto cmd = set_status_toast(m, std::move(msg_));
            return {std::move(m), std::move(cmd)};
        },
        [&](RejectAllChanges) -> Step {
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to reject");
                return {std::move(m), std::move(cmd)};
            }
            // Mark every hunk Rejected and rewrite each file to its
            // pre-edit content. We rewrite BEFORE clearing the queue
            // because apply_review_to_disk needs the FileChange data.
            int hunks = 0;
            std::string first_err;
            for (auto& fc : m.d.pending_changes) {
                for (auto& h : fc.hunks) {
                    h.status = Hunk::Status::Rejected;
                    ++hunks;
                }
                auto err = apply_review_to_disk(fc);
                if (!err.empty() && first_err.empty())
                    first_err = std::move(err);
            }
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
            std::string msg_;
            if (!first_err.empty()) {
                msg_ = "reject all: some writes failed — " + first_err;
            } else {
                msg_ = "reverted " + std::to_string(hunks)
                     + (hunks == 1 ? " hunk" : " hunks");
            }
            auto cmd = set_status_toast(m, std::move(msg_));
            return {std::move(m), std::move(cmd)};
        },
    }, dm);
}

} // namespace agentty::app::detail
