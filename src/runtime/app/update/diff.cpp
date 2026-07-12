// diff_review_update — reducer for `msg::DiffReviewMsg`. Two-axis modal
// over (file_index, hunk_index) plus an in-hunk body scroll; mutation =
// per-hunk Accepted/Rejected/Pending flips with auto-advance to the next
// undecided hunk (the review "flow" — decide, land on the next decision,
// never hunt for it), per-file mass decisions, and AcceptAll / RejectAll
// over the whole set. `DiffReviewFinish` (Enter) is the happy-path exit:
// remaining Pending hunks count as Accepted (the disk already holds the
// new bytes — keeping them is the do-nothing choice) and every file
// materialises. `ToggleDiffReview` flips the persisted review_enabled
// setting; disabling mid-session auto-accepts whatever is pending.
//
// Every handler re-clamps the cursor against the CURRENT change set
// before using it: entries can vanish underneath an open pane (an agent
// edit re-coalesced back to baseline drops its entry; CheckpointRestored
// clears the whole set) and an unclamped file_index is a straight OOB.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/diff/diff.hpp"
#include "agentty/runtime/app/deps.hpp"
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

// Re-clamp the open cursor against the live change set. Returns nullptr
// when the pane is closed OR there is nothing left to review (in which
// case it also closes the pane so the overlay doesn't render a husk).
pick::OpenAtCell* clamped_cursor(Model& m) {
    auto* c = pick::opened(m.ui.diff_review);
    if (!c) return nullptr;
    if (m.d.pending_changes.empty()) {
        m.ui.diff_review = pick::Closed{};
        return nullptr;
    }
    const int files = static_cast<int>(m.d.pending_changes.size());
    c->file_index = std::clamp(c->file_index, 0, files - 1);
    const int hunks = static_cast<int>(
        m.d.pending_changes[static_cast<std::size_t>(c->file_index)]
            .hunks.size());
    c->hunk_index = hunks > 0 ? std::clamp(c->hunk_index, 0, hunks - 1) : 0;
    if (c->body_scroll < 0) c->body_scroll = 0;
    return c;
}

// Move the cursor to the next PENDING hunk strictly after the current
// position, scanning forward across files and wrapping around once. If
// every hunk is decided, fall back to a plain next-hunk step within the
// file so the key still visibly does something. Resets body_scroll.
void advance_to_next_pending(Model& m, pick::OpenAtCell& c) {
    const int files = static_cast<int>(m.d.pending_changes.size());
    int fi = c.file_index, hi = c.hunk_index;
    for (int step = 0; step < 2 * files + 2; /* advanced inside */) {
        // Step one hunk forward (cross-file, wrapping).
        const auto& fc = m.d.pending_changes[static_cast<std::size_t>(fi)];
        if (hi + 1 < static_cast<int>(fc.hunks.size())) {
            ++hi;
        } else {
            fi = (fi + 1) % files;
            hi = 0;
            ++step;
        }
        if (fi == c.file_index && hi == c.hunk_index) break;  // full loop
        const auto& cand = m.d.pending_changes[static_cast<std::size_t>(fi)];
        if (!cand.hunks.empty()
            && cand.hunks[static_cast<std::size_t>(hi)].status
                   == Hunk::Status::Pending) {
            c.file_index = fi;
            c.hunk_index = hi;
            c.body_scroll = 0;
            return;
        }
    }
    // Nothing pending anywhere — step to the next hunk in this file (or
    // stay put on the last one) so repeated decide-keys don't feel dead.
    const auto& fc =
        m.d.pending_changes[static_cast<std::size_t>(c.file_index)];
    if (c.hunk_index + 1 < static_cast<int>(fc.hunks.size())) {
        ++c.hunk_index;
        c.body_scroll = 0;
    }
}

// Set the focused hunk's status (Accept/Reject/Pending). Returns true
// when a hunk actually existed to mutate.
bool set_focused_hunk(Model& m, pick::OpenAtCell& c, Hunk::Status s) {
    auto& fc = m.d.pending_changes[static_cast<std::size_t>(c.file_index)];
    if (fc.hunks.empty()) return false;
    fc.hunks[static_cast<std::size_t>(c.hunk_index)].status = s;
    return true;
}

// Move to the next file that still has a pending hunk (after a per-file
// mass decision); if none, stay on the current file.
void advance_to_next_pending_file(Model& m, pick::OpenAtCell& c) {
    const int files = static_cast<int>(m.d.pending_changes.size());
    for (int k = 1; k <= files; ++k) {
        const int fi = (c.file_index + k) % files;
        const auto& fc = m.d.pending_changes[static_cast<std::size_t>(fi)];
        if (!fully_decided(fc)) {
            c.file_index = fi;
            // Land on the file's first pending hunk, not hunk 0.
            c.hunk_index = 0;
            for (std::size_t i = 0; i < fc.hunks.size(); ++i)
                if (fc.hunks[i].status == Hunk::Status::Pending) {
                    c.hunk_index = static_cast<int>(i);
                    break;
                }
            c.body_scroll = 0;
            return;
        }
    }
}

// Persist the review_enabled flag without clobbering unrelated settings.
void persist_review_enabled(bool enabled) {
    auto s = deps().load_settings();
    s.review_enabled = enabled;
    deps().save_settings(s);
}

} // namespace

Step diff_review_update(Model m, msg::DiffReviewMsg dm) {
    return std::visit(overload{
        [&](OpenDiffReview) -> Step {
            // Tell the user when there's nothing to review instead of
            // silently doing nothing — opening an empty pane would just
            // flicker the screen and leave them confused about whether
            // their keystroke registered.
            if (!m.d.review_enabled) {
                auto cmd = set_status_toast(m,
                    "diff review is disabled — enable it via "
                    "Ctrl+K → \"Toggle diff review\"");
                return {std::move(m), std::move(cmd)};
            }
            if (m.d.pending_changes.empty()) {
                auto cmd = set_status_toast(m, "no pending changes to review");
                return {std::move(m), std::move(cmd)};
            }
            // Open on the FIRST undecided hunk — reopening mid-review
            // resumes where work remains, not at a hunk already decided.
            pick::OpenAtCell at{0, 0, 0};
            bool found = false;
            for (std::size_t f = 0;
                 !found && f < m.d.pending_changes.size(); ++f)
                for (std::size_t h = 0;
                     h < m.d.pending_changes[f].hunks.size(); ++h)
                    if (m.d.pending_changes[f].hunks[h].status
                            == Hunk::Status::Pending) {
                        at.file_index = static_cast<int>(f);
                        at.hunk_index = static_cast<int>(h);
                        found = true;
                        break;
                    }
            m.ui.diff_review = at;
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
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            // Walk hunks ACROSS file boundaries: ↓ past a file's last
            // hunk lands on the next file's first, ↑ mirrors — the whole
            // change set reads as one continuous list (wraps at the ends).
            const int files = static_cast<int>(m.d.pending_changes.size());
            const auto hunks_of = [&](int fi) {
                return static_cast<int>(
                    m.d.pending_changes[static_cast<std::size_t>(fi)]
                        .hunks.size());
            };
            if (e.delta > 0) {
                for (int n = 0; n < e.delta; ++n) {
                    if (c->hunk_index + 1 < hunks_of(c->file_index)) {
                        ++c->hunk_index;
                    } else {
                        c->file_index = (c->file_index + 1) % files;
                        c->hunk_index = 0;
                    }
                }
            } else {
                for (int n = 0; n < -e.delta; ++n) {
                    if (c->hunk_index > 0) {
                        --c->hunk_index;
                    } else {
                        c->file_index = (c->file_index - 1 + files) % files;
                        c->hunk_index = std::max(0, hunks_of(c->file_index) - 1);
                    }
                }
            }
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](DiffReviewNextFile) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index + 1) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](DiffReviewPrevFile) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            int sz = static_cast<int>(m.d.pending_changes.size());
            c->file_index = (c->file_index - 1 + sz) % sz;
            c->hunk_index = 0;
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](DiffReviewJump& e) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            const auto& fc =
                m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
            c->hunk_index = e.to_end
                ? std::max(0, static_cast<int>(fc.hunks.size()) - 1)
                : 0;
            c->body_scroll = 0;
            return done(std::move(m));
        },
        [&](DiffReviewBodyScroll& e) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            // Clamp against the focused hunk's patch line count so PgDn
            // never runs the offset far past the body (which would make
            // PgUp feel dead for several presses on the way back). The
            // view additionally clamps against its live window height.
            const auto& fc =
                m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
            int lines = 0;
            if (!fc.hunks.empty()) {
                const auto& patch =
                    fc.hunks[static_cast<std::size_t>(c->hunk_index)].patch;
                lines = static_cast<int>(
                    std::count(patch.begin(), patch.end(), '\n'));
            }
            c->body_scroll = std::clamp(c->body_scroll + e.delta, 0,
                                        std::max(0, lines - 1));
            return done(std::move(m));
        },
        [&](AcceptHunk) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            if (set_focused_hunk(m, *c, Hunk::Status::Accepted))
                advance_to_next_pending(m, *c);
            return done(std::move(m));
        },
        [&](RejectHunk) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            if (set_focused_hunk(m, *c, Hunk::Status::Rejected))
                advance_to_next_pending(m, *c);
            return done(std::move(m));
        },
        [&](DiffReviewUndoHunk) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            // Back to Pending, no auto-advance — undo means the user is
            // revisiting THIS hunk, don't yank the cursor away from it.
            set_focused_hunk(m, *c, Hunk::Status::Pending);
            return done(std::move(m));
        },
        [&](DiffReviewAcceptFile) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            auto& fc =
                m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
            for (auto& h : fc.hunks) h.status = Hunk::Status::Accepted;
            advance_to_next_pending_file(m, *c);
            return done(std::move(m));
        },
        [&](DiffReviewRejectFile) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            auto& fc =
                m.d.pending_changes[static_cast<std::size_t>(c->file_index)];
            for (auto& h : fc.hunks) h.status = Hunk::Status::Rejected;
            advance_to_next_pending_file(m, *c);
            return done(std::move(m));
        },
        [&](DiffReviewFinish) -> Step {
            auto* c = clamped_cursor(m);
            if (!c) return done(std::move(m));
            // Enter = "done reviewing". Undecided hunks default to
            // Accepted: the tool already wrote those bytes, so keeping
            // them is the no-op choice — Enter must never destroy work
            // the user didn't explicitly reject.
            int rejected = 0;
            for (auto& fc : m.d.pending_changes)
                for (auto& h : fc.hunks) {
                    if (h.status == Hunk::Status::Pending)
                        h.status = Hunk::Status::Accepted;
                    if (h.status == Hunk::Status::Rejected) ++rejected;
                }
            int applied = 0, failed = 0;
            std::erase_if(m.d.pending_changes, [&](const FileChange& fc) {
                if (materialise_decisions(fc)) { ++applied; return true; }
                ++failed; return false;
            });
            if (m.d.pending_changes.empty())
                m.ui.diff_review = pick::Closed{};
            std::string msg = "review done · " + std::to_string(applied)
                + (applied == 1 ? " file" : " files");
            if (rejected > 0)
                msg += " · " + std::to_string(rejected)
                     + (rejected == 1 ? " hunk reverted" : " hunks reverted");
            if (failed > 0)
                msg += " · " + std::to_string(failed) + " failed (still pending)";
            auto cmd = set_status_toast(m, std::move(msg));
            return {std::move(m), std::move(cmd)};
        },
        [&](ToggleDiffReview) -> Step {
            m.d.review_enabled = !m.d.review_enabled;
            persist_review_enabled(m.d.review_enabled);
            if (m.d.review_enabled) {
                auto cmd = set_status_toast(m,
                    "diff review enabled — new edits will queue for review "
                    "(Ctrl+R)");
                return {std::move(m), std::move(cmd)};
            }
            // Disabling with a live review: everything already sitting in
            // the set counts as accepted (disk holds the new bytes — no
            // I/O needed), and the pane closes if it was open.
            const auto n = m.d.pending_changes.size();
            m.d.pending_changes.clear();
            m.ui.diff_review = pick::Closed{};
            std::string msg = "diff review disabled — edits auto-accept";
            if (n > 0)
                msg += " · " + std::to_string(n)
                     + (n == 1 ? " pending file accepted"
                               : " pending files accepted");
            auto cmd = set_status_toast(m, std::move(msg));
            return {std::move(m), std::move(cmd)};
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
