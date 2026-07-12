// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index); mutation = per-hunk Accepted/Rejected
// status flips; AcceptAll / RejectAll fan over every pending change at
// once. Emits status toasts via set_status_toast on the no-change paths
// so empty-state Enter doesn't feel silent.

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

// FileChange.path is whatever the model passed the tool — often a
// workspace-relative path. The process cwd can differ from the workspace
// root (`-w` flag), so resolve relative paths against workspace_root()
// exactly like the fs tools did when they wrote the file.
std::filesystem::path resolve_change_path(const std::string& p) {
    std::filesystem::path fp{p};
    if (fp.is_absolute()) return fp;
    return tools::util::workspace_root() / fp;
}

// Materialise the user's hunk decisions for one FileChange onto disk.
// Tools write eagerly, so the file currently holds new_contents; this
// rewrites it to the accepted-only reconstruction. Reverting a file
// CREATION (empty baseline) removes the file rather than leaving an
// empty husk. Returns false when the write itself failed (the entry
// should then stay pending so the user can retry).
bool materialise_decisions(const FileChange& fc) {
    const std::string desired = diff::apply_accepted(fc);
    if (desired == fc.new_contents) return true;   // disk already matches
    namespace fsys = std::filesystem;
    const fsys::path target = resolve_change_path(fc.path);
    if (desired.empty() && fc.original_contents.empty()) {
        // Rejected creation — delete instead of truncating to zero bytes.
        std::error_code ec;
        fsys::remove(target, ec);
        return !ec;
    }
    return tools::util::write_file(target, desired).empty();
}

// Revert one FileChange wholesale (RejectAll): restore the baseline
// contents, deleting files the agent created.
bool revert_change(const FileChange& fc) {
    namespace fsys = std::filesystem;
    const fsys::path target = resolve_change_path(fc.path);
    if (fc.original_contents.empty()) {
        std::error_code ec;
        fsys::remove(target, ec);
        return !ec;
    }
    return tools::util::write_file(target, fc.original_contents).empty();
}

// A change is fully decided when no hunk is still Pending.
bool fully_decided(const FileChange& fc) {
    for (const auto& h : fc.hunks)
        if (h.status == Hunk::Status::Pending) return false;
    return !fc.hunks.empty();
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
            // Materialise every fully-decided file's hunk choices onto
            // disk and retire those entries. Files with undecided hunks
            // stay pending — closing the pane is "I'll finish later",
            // not "discard my decisions". Failed writes also stay so
            // the user can retry instead of silently losing the choice.
            int applied = 0;
            std::erase_if(m.d.pending_changes, [&](FileChange& fc) {
                if (!fully_decided(fc)) return false;
                if (!materialise_decisions(fc)) return false;
                ++applied;
                return true;
            });
            if (applied > 0) {
                auto cmd = set_status_toast(m,
                    "applied review decisions to " + std::to_string(applied)
                    + (applied == 1 ? " file" : " files"));
                return {std::move(m), std::move(cmd)};
            }
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
            // Tools already wrote the new contents to disk — accepting
            // means "keep what's there": no file I/O, just retire the
            // change set so the strip/pane show a clean slate.
            int hunks = 0;
            for (auto& fc : m.d.pending_changes)
                hunks += static_cast<int>(fc.hunks.size());
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
            // Reject = REVERT: restore every file's baseline contents on
            // disk (delete files the agent created). Entries whose revert
            // fails stay pending so the user can see + retry them.
            int reverted = 0, failed = 0;
            std::erase_if(m.d.pending_changes, [&](const FileChange& fc) {
                if (revert_change(fc)) { ++reverted; return true; }
                ++failed; return false;
            });
            if (m.d.pending_changes.empty())
                m.ui.diff_review = pick::Closed{};
            std::string msg = "reverted " + std::to_string(reverted)
                + (reverted == 1 ? " file" : " files");
            if (failed > 0)
                msg += " \xc2\xb7 " + std::to_string(failed) + " failed";
            auto cmd = set_status_toast(m, std::move(msg));
            return {std::move(m), std::move(cmd)};
        },
    }, dm);
}

} // namespace agentty::app::detail
