// thread_list.cpp - the thread switcher's reducer (open/pick/delete/rename
// + the async ThreadsLoaded). Split from panels.cpp; shares only
// reset_to_fresh_thread (defined here, its sole user).

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/util/logx.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/io/persistence.hpp"   // new_id: a pure function, called directly
#include "agentty/provider/selection.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/panel/common.hpp"
#include "agentty/runtime/panel/thread_filter.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/util/utf8.hpp"   // safe_utf8_cut: the rename draft cap

namespace pn = agentty::ui::panel;

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

namespace {

// The thread under the picker cursor, after the filter. -1 when none.
[[nodiscard]] int picked_thread(const Model& m, const pn::ThreadList& p) {
    const auto vis = ui::visible_threads(m.d.threads, p.query);
    if (vis.empty()) return -1;
    return vis[static_cast<std::size_t>(
        std::clamp(p.index, 0, static_cast<int>(vis.size()) - 1))];
}

[[nodiscard]] int visible_count(const Model& m, const pn::ThreadList& p) {
    return static_cast<int>(ui::visible_threads(m.d.threads, p.query).size());
}

// A pasted or typed piece of a title, flattened to one line: line breaks
// and tabs become spaces, other control bytes go. The draft is cleaned
// again on submit; this only keeps what is on screen sane meanwhile.
[[nodiscard]] std::string one_line(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else if (u >= 0x20 && u != 0x7f)         out.push_back(c);
    }
    return out;
}

}  // namespace
// ── Fresh-thread reset ────────────────────────────────────────────────────
// Swap the model over to a brand-new empty thread and return the terminal
// reset that wipes the departing thread's rendered turns off-screen.
//
// This is the SHARED core behind two entry points: `NewThread` (^N, in the
// picker too) and `ThreadListDelete` when the row removed is the active thread.
// It is deliberately the part *after* the caller's save/delete decision —
// NewThread persists the outgoing thread first, delete has just destroyed
// it — so the caller owns that policy and this owns the reset. Keeping the
// two callers on one code path is what stops them drifting (they had, and
// the delete copy was missing the phase reset + kernel release + inline
// wipe, which is exactly the machinery that makes a mid-stream swap safe).
//
// Returns the reset_inline Cmd so the caller can batch it with its own
// commands (delete also kicks a thread-list refresh + a toast).
[[nodiscard]] Cmd reset_to_fresh_thread(Model& m) {
    // Drop the whole render cache: every (tid,msg) entry belongs to the
    // thread we're leaving, whose messages will never freeze again (freeze
    // is the only per-entry drop, and it only runs on the CURRENT thread).
    // Keys embed thread_id so there's no collision — this purely reclaims
    // the old thread's staged/pinned entries so they don't linger.
    m.ui.view_cache.clear();
    // A thread load still in flight is for the transcript we're replacing;
    // forget it so its result doesn't overwrite the fresh thread.
    if (!m.s.loading_thread.value.empty()) {
        m.s.loading_thread = {};
        m.s.thread_loading = false;
    }
    m.d.current = Thread{};
    m.d.current.id = persistence::new_id();
    m.d.current.created_at = m.d.current.updated_at =
        m.wall_now();
    m.s.reset_usage_for_thread(0);
    clear_frozen(m);
    // Close every modal that framed the OLD thread: the picker we acted
    // from, plus the palette / code-block picker whose contents belonged to
    // the departing thread's last reply.
    m.ui.panel.close<pn::ThreadList>();
    m.ui.panel.close<pn::Palette>();
    m.ui.panel.close<pn::CodeBlocks>(); m.ui.panel.close<pn::CodeBlockResult>();
    // Wipe the whole composer draft — a pasted-but-unsent image (or any
    // chip / queued message) belongs to the thread we're leaving. Leaking
    // it once carried an empty-bytes image attachment into the new thread's
    // first submit and 400'd the request.
    reset_composer_draft(m.ui.composer);
    // A fresh empty thread has no live turn — drop any streaming phase and
    // hand the kernel back so a mid-stream swap can't leave the wire running
    // against a thread that no longer exists.
    end_turn(m.s.phase);
    // Smart-Mode per-THREAD routing state must not leak into the new thread:
    // complexity momentum (classify_score_with_context inherits a tier from
    // the PREVIOUS turn), the session cascade bias, and the last turn's
    // signature (outcome feedback would otherwise attribute the new thread's
    // first reply to the OLD thread's route). The learned per-workspace
    // priors (RoutingMemory) survive by design — they are cross-thread.
    m.s.smart_turn_complexity  = smart::Complexity::Standard;
    m.s.smart_effort_bias      = 0;
    release_to_kernel();
    // Re-warm the active provider's TLS socket. The launch-time prewarm has
    // usually aged out of the pool by now — a user reads a reply, composes,
    // then hits ^N, and the 90 s idle TTL has evicted the warm connection.
    // Without this the FIRST turn of every new thread re-pays the full
    // DNS+TCP+TLS handshake (~150-300 ms) before its first SSE byte. Returned
    // as an effect, so the dial overlaps the user typing their first prompt.
    //
    // Per maya's contract reset_inline is the ONE allowed wiring of
    // `\x1b[3J`: an explicit, user-initiated content swap. It wipes
    // saved-lines (including pre-agentty shell history), acceptable precisely
    // because the user asked to switch threads. Do NOT extend it to per-turn
    // paths.
    return Cmd::batch(cmd::prewarm_provider(m), cmd::reset_inline());
}

Cmd thread_list_update(Model& m, msg::ThreadListMsg tm) {
    return std::visit(overload{
        [&](OpenThreadList) -> Cmd {
            // Refresh in the background if no load is in flight — the
            // walk + parse is too slow (seconds, with hundreds of
            // multi-MB thread files) to do synchronously here. The
            // picker opens immediately against the cached list; new
            // entries fade in when ThreadsLoaded lands.
            Cmd cmd = Cmd::none();
            if (!m.s.threads_loading) {
                m.s.threads_loading = true;
                cmd = cmd::load_threads_async();
            }
            // Open AT the current thread, not row 0 — the user's mental
            // anchor is "where am I", and cycling from there (↑ newer /
            // ↓ older) mirrors the Alt+←/→ quick-cycle order.
            int at = 0;
            for (int i = 0; i < static_cast<int>(m.d.threads.size()); ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    at = i;
                    break;
                }
            m.ui.panel.descend(pn::ThreadList{{at}});
            return cmd;
        },
        [&](CloseThreadList) -> Cmd {
            // Esc with a filter clears it first, so a typo never closes the list.
            if (auto* p = m.ui.panel.get<pn::ThreadList>(); p && !p->query.empty()) {
                const int was = picked_thread(m, *p);
                p->query.clear();
                p->index = std::max(0, was);   // stay on the same thread
                return Cmd::none();
            }
            ascend(m);   // Esc: back to whatever opened this, or close
            return Cmd::none();
        },
        [&](ThreadListFilterInput& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return Cmd::none();
            p->confirm_remove.clear();
            p->query += ui::utf8_encode(e.ch);
            p->index = 0;                    // best (most recent) match first
            m.ui.thread_list_scroll.y = 0;
            return Cmd::none();
        },
        [&](ThreadListFilterBackspace) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->query.empty()) return Cmd::none();
            p->confirm_remove.clear();
            p->query.erase(static_cast<std::size_t>(
                ui::utf8_prev(p->query, static_cast<int>(p->query.size()))));
            p->index = 0;
            m.ui.thread_list_scroll.y = 0;
            return Cmd::none();
        },
        [&](ThreadListMove& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return Cmd::none();
            p->confirm_remove.clear();   // moving disarms a pending ^D
            p->index = ui::step(p->index, e.delta, visible_count(m, *p));
            return Cmd::none();
        },
        [&](ThreadListJump& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return Cmd::none();
            const int sz = visible_count(m, *p);
            if (sz == 0) return Cmd::none();
            p->confirm_remove.clear();   // jumping disarms a pending ^D
            using W = ThreadListJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = sz - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(sz - 1, p->index + kPage); break;
            }
            return Cmd::none();
        },
        // ── Model swap: commit overflow before swapping ──────────────
        //
        // ThreadListSelect and NewThread replace m.d.current wholesale.
        // Before the swap we dispatch Cmd::commit_scrollback_overflow()
        // — NOT force_redraw (see history below).
        //
        // Why commit-overflow is required:
        //   maya's inline diff treats rows [0, prev_rows - term_h) as
        //   committed scrollback ("updatable_start" in serialize.cpp).
        //   When the old thread overflowed (prev_rows > term_h) those
        //   rows are skipped by the diff scan and per-row emit. After
        //   a wholesale model swap the new thread's canvas rows at
        //   those Y positions are entirely different content — but
        //   the diff still considers them "scrollback, untouchable"
        //   and never emits them. Result: visible seam mid-viewport
        //   where the wire still holds old-thread bytes against the
        //   new-thread canvas, manifesting as two unrelated text
        //   fragments on adjacent rows.
        //
        //   commit_scrollback_overflow() calls into maya's
        //   commit_inline_overflow which advances prev_cells by
        //   max(0, prev_rows - term_h) rows. After it runs,
        //   prev_rows ≤ term_h, updatable_start drops to 0, and the
        //   diff scans the full common range — every visible row
        //   gets correctly emitted against the new thread.
        //
        //   The rows that scroll out of prev_cells are bytes the
        //   terminal already committed to its native scrollback
        //   anyway (they were emitted via bottom-edge \r\n's during
        //   streaming). commit just acknowledges that fact — zero
        //   wire effect.
        //
        // Why NOT force_redraw:
        //   cmd::force_redraw demotes Synced → Stale, routing the
        //   next render through compose case (B). Case (B)'s
        //   scroll-to-fit branch (scroll_n > 0) emits \n at the
        //   viewport bottom when the new frame is taller than the
        //   old cursor's offset from viewport top — each \n there
        //   scrolls a row of whatever was on screen (old thread
        //   tail + host shell history above it) up into
        //   terminal-owned scrollback, permanently. History: commit
        //   8becb88 did exactly that and reverted in 0b24148.
        [&](ThreadListSelect) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            Cmd cmd = Cmd::none();
            if (p) p->confirm_remove.clear();   // selecting disarms a pending ^D
            if (p && !m.d.threads.empty() && !m.s.thread_loading) {
                const int at = picked_thread(m, *p);
                if (at < 0) return Cmd::none();   // the filter matches nothing
                const Thread& meta = m.d.threads[static_cast<std::size_t>(at)];
                // Same-thread re-select — closing the picker is the
                // only useful action. No async load: would just
                // reparse the same bytes and flash.
                if (meta.id == m.d.current.id) {
                    m.ui.panel.close<pn::ThreadList>();
                    return Cmd::none();
                }
                m.s.thread_loading = true;
                m.s.loading_thread = meta.id;
                // Warm the socket so the first turn in the thread the user
                // is switching INTO doesn't re-pay the handshake (the pool's
                // idle TTL has usually evicted it during composer breathing
                // room). Returned, not performed.
                cmd = Cmd::batch(cmd::prewarm_provider(m),
                                 cmd::load_thread_async(meta.id));
            }
            m.ui.panel.close<pn::ThreadList>();
            return cmd;
        },
        [&](ThreadListDelete) -> Cmd {
            // ^D in the thread picker: two-press delete with confirm_remove,
            // mirroring SettingsListRemove / AccountRemove. First press on a
            // row marks it pending (warning badge in the view); second press
            // on the SAME row returns a DeleteThread effect. Any
            // move/jump/select/new/close disarms the pending state.
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || m.d.threads.empty()) return Cmd::none();
            // The cursor is a position in the FILTERED list; picked_thread
            // maps it back and clamps it, so a list shrunk by an async
            // refresh can't index past the end.
            const int idx = picked_thread(m, *p);
            if (idx < 0) return Cmd::none();
            const Thread& target = m.d.threads[static_cast<std::size_t>(idx)];
            // Use the thread id as the confirm key — stable across title edits.
            const std::string key = target.id.value;
            if (p->confirm_remove != key) {
                p->confirm_remove = key;
                return Cmd::none();
            }
            // Second press — commit. Snapshot everything we need OUT of the
            // vector element BEFORE erase(): the erase invalidates `target`,
            // so reading target.title / target.id afterward is a
            // use-after-free. Copy them here while the reference is live.
            const ThreadId  target_id = target.id;
            const bool      was_current = (target_id == m.d.current.id);
            const std::string label =
                target.title.empty() ? "(untitled)" : target.title;

            p->confirm_remove.clear();
            // The delete is a VALUE, batched into both of this arm's
            // returns below — taken here, where target_id is still in hand.
            Cmd del = Cmd(DeleteThread{target_id});
            m.d.threads.erase(m.d.threads.begin() + idx);
            // Clamp the cursor so it stays valid after removal.
            const int sz = visible_count(m, *p);
            if (sz == 0) {
                p->index = 0;
            } else if (p->index >= sz) {
                p->index = sz - 1;
            }
            std::string msg = "deleted \"" + label + "\"";
            if (was_current) msg += " \xe2\x80\x94 started a new thread";
            auto toast = set_status_toast(m, std::move(msg));
            // Deleting the ACTIVE thread leaves m.d.current pointing at a
            // thread whose file no longer exists — swap to a fresh empty
            // thread through the SAME core NewThread uses. That single code
            // path is what guarantees the phase reset + kernel release (so a
            // mid-stream delete can't leave the wire running against a dead
            // thread), the modal/skill/cache teardown, and the reset_inline
            // that wipes the deleted thread's rendered turns off-screen.
            if (was_current) {
                auto reset = reset_to_fresh_thread(m);
                return Cmd::batch(std::move(del), cmd::load_threads_async(),
                                  std::move(reset), std::move(toast));
            }
            return Cmd::batch(std::move(del), std::move(toast));
        },
        [&](ThreadListRenameStart) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return Cmd::none();
            const int idx = picked_thread(m, *p);
            if (idx < 0) return Cmd::none();
            const Thread& target = m.d.threads[static_cast<std::size_t>(idx)];
            p->confirm_remove.clear();
            p->renaming     = target.id.value;
            p->rename_input = target.title;   // pre-filled: edit, don't retype
            p->rename_cursor = static_cast<int>(p->rename_input.size());
            return Cmd::none();
        },
        [&](ThreadListRenameChar& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty()) return Cmd::none();
            const std::string add = one_line(ui::utf8_encode(e.ch));
            // Refuse past the cap rather than cut on submit, so what you see
            // is what gets stored.
            if (p->rename_input.size() + add.size() > persistence::kTitleMaxBytes)
                return Cmd::none();
            p->rename_input.insert(static_cast<std::size_t>(p->rename_cursor), add);
            p->rename_cursor += static_cast<int>(add.size());
            return Cmd::none();
        },
        [&](ThreadListRenameBackspace) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty() || p->rename_cursor <= 0) return Cmd::none();
            const int at = ui::utf8_prev(p->rename_input, p->rename_cursor);
            p->rename_input.erase(static_cast<std::size_t>(at),
                                  static_cast<std::size_t>(p->rename_cursor - at));
            p->rename_cursor = at;
            return Cmd::none();
        },
        [&](ThreadListRenameDelete) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty()) return Cmd::none();
            const int end = ui::utf8_next(p->rename_input, p->rename_cursor);
            p->rename_input.erase(static_cast<std::size_t>(p->rename_cursor),
                                  static_cast<std::size_t>(end - p->rename_cursor));
            return Cmd::none();
        },
        [&](ThreadListRenameCursor& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty()) return Cmd::none();
            using To = ThreadListRenameCursor::To;
            switch (e.to) {
                case To::Left:  p->rename_cursor = ui::utf8_prev(p->rename_input, p->rename_cursor); break;
                case To::Right: p->rename_cursor = ui::utf8_next(p->rename_input, p->rename_cursor); break;
                case To::Home:  p->rename_cursor = 0; break;
                case To::End:   p->rename_cursor = static_cast<int>(p->rename_input.size()); break;
            }
            return Cmd::none();
        },
        [&](ThreadListRenamePaste& e) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty()) return Cmd::none();
            // Only what fits goes in, so a long paste mid-title cuts the
            // paste, never the text after the caret.
            std::string add = one_line(e.text);
            const std::size_t room = persistence::kTitleMaxBytes
                                   - std::min(persistence::kTitleMaxBytes, p->rename_input.size());
            add.resize(tools::util::safe_utf8_cut(add, room));
            p->rename_input.insert(static_cast<std::size_t>(p->rename_cursor), add);
            p->rename_cursor += static_cast<int>(add.size());
            return Cmd::none();
        },
        [&](ThreadListRenameCancel) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p) return Cmd::none();
            p->renaming.clear();
            p->rename_input.clear();
            p->rename_cursor = 0;
            return Cmd::none();
        },
        [&](ThreadListRenameSubmit) -> Cmd {
            auto* p = m.ui.panel.get<pn::ThreadList>();
            if (!p || p->renaming.empty()) return Cmd::none();
            const ThreadId id{p->renaming};
            std::string title = persistence::clean_title(p->rename_input);
            p->renaming.clear();
            p->rename_input.clear();
            p->rename_cursor = 0;
            // Found by id: a refresh that landed mid-edit may have reordered
            // the list, or dropped the thread (deleted elsewhere): then there
            // is nothing to rename.
            const auto it = std::ranges::find(m.d.threads, id, &Thread::id);
            if (it == m.d.threads.end()) return Cmd::none();
            // Empty or unchanged is a cancel: Enter on an untouched draft
            // must not cost a disk write.
            if (title.empty() || title == it->title) return Cmd::none();
            const std::int64_t at = std::chrono::duration_cast<std::chrono::seconds>(
                m.wall_now().time_since_epoch()).count();
            it->title      = title;
            it->renamed_at = at;
            // The open thread is re-saved every turn from m.d.current, which
            // would write the old title back over the rename.
            if (m.d.current.id == id) {
                m.d.current.title      = title;
                m.d.current.renamed_at = at;
            }
            // The renamed row may no longer match the filter.
            const int sz = visible_count(m, *p);
            p->index = sz > 0 ? std::clamp(p->index, 0, sz - 1) : 0;
            return Cmd(RenameThread{id, std::move(title), at});
        },
        [&](ThreadCycle& e) -> Cmd {
            // Alt+←/→ — jump to the adjacent thread without the picker.
            // Recency order (same as ^J): index 0 = newest; +1 = older,
            // -1 = newer, wrapping at both ends. Gated on an idle
            // session — swapping m.d.current under an active stream
            // would strand the in-flight ctx's writes.
            if (m.s.active()) {
                auto cmd = set_status_toast(m,
                    "wait for the reply to finish before switching threads");
                return cmd;
            }
            if (m.s.thread_loading) return Cmd::none();
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                // History not loaded yet (or genuinely empty) — kick a
                // refresh so the NEXT press works, and say so.
                Cmd cmd = Cmd::none();
                if (!m.s.threads_loading) {
                    m.s.threads_loading = true;
                    cmd = cmd::load_threads_async();
                }
                auto toast = set_status_toast(m, "no other threads yet");
                return Cmd::batch(std::move(cmd), std::move(toast));
            }
            // Locate the current thread in the recency list. A fresh
            // unsaved thread isn't in it — treat "newest" as the anchor
            // so the first press lands on the most recent saved thread.
            int cur = -1;
            for (int i = 0; i < sz; ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    cur = i;
                    break;
                }
            int target;
            if (cur < 0) {
                target = (e.delta >= 0) ? 0 : sz - 1;
            } else {
                if (sz == 1) {
                    auto toast = set_status_toast(m, "only one thread");
                    return toast;
                }
                target = ((cur + e.delta) % sz + sz) % sz;
            }
            const Thread& meta = m.d.threads[static_cast<std::size_t>(target)];
            if (meta.id == m.d.current.id) return Cmd::none();
            // Preserve the thread being left — same courtesy NewThread
            // extends. finalize_turn saves per turn, but a title edit or
            // an un-persisted tail shouldn't be lost to a quick cycle.
            Cmd save = m.d.current.messages.empty()
                         ? Cmd::none() : Cmd(SaveThread{m.d.current});
            m.s.thread_loading = true;
            m.s.loading_thread = meta.id;
            // "thread k/N · title" — the positional readout that makes
            // repeated Alt+←/→ presses feel like flipping through a
            // deck rather than teleporting blind. Survives the swap
            // because ThreadLoaded doesn't touch m.s.status.
            auto toast = set_status_toast(m,
                "thread " + std::to_string(target + 1) + "/"
                    + std::to_string(sz) + " \xc2\xb7 "
                    + (meta.title.empty() ? "(untitled)" : meta.title));
            // The prewarm warms the socket for the switched-into thread's
            // first turn.
            return Cmd::batch(std::move(save),
                              cmd::prewarm_provider(m),
                              cmd::load_thread_async(meta.id),
                              std::move(toast));
        },
        [&](NewThread) -> Cmd {
            // Persist the outgoing thread before we drop it (delete's
            // active-row path does the opposite — it just removed the
            // thread, so it must NOT save). The shared reset below owns
            // everything after this policy decision.
            Cmd save = m.d.current.messages.empty()
                         ? Cmd::none() : Cmd(SaveThread{m.d.current});
            auto reset = reset_to_fresh_thread(m);
            return Cmd::batch(std::move(save), std::move(reset));
        },
        [&](ThreadTitleGenerated& e) -> Cmd {
            if (m.s.titling == e.id) m.s.titling = {};
            // The model's reply, cleaned the same way a typed name is. Quotes
            // and a trailing period are the usual chat-model habits.
            std::string title = persistence::clean_title(e.title);
            while (!title.empty() && (title.front() == '"' || title.front() == '\''))
                title.erase(0, 1);
            while (!title.empty() && (title.back() == '"' || title.back() == '\''
                                      || title.back() == '.'))
                title.pop_back();
            if (title.empty()) return Cmd::none();   // failed: keep what it has

            // Only the open thread. If you switched away while the title was
            // being made, the thread keeps its first-line name, the same as a
            // failed request.
            if (m.d.current.id != e.id || m.d.current.renamed_at > 0) return Cmd::none();
            if (const auto it = std::ranges::find(m.d.threads, e.id, &Thread::id);
                it != m.d.threads.end()) {
                if (it->renamed_at > 0) return Cmd::none();   // never replace a chosen name
                it->title       = title;
                it->auto_titled = true;
            }
            m.d.current.title       = title;
            m.d.current.auto_titled = true;
            return Cmd(SaveThread{m.d.current});
        },
        [&](ThreadsLoaded& e) -> Cmd {
            // A reload can be read from disk before a rename made a moment
            // ago has been written. Keep any row's newer rename.
            for (auto& t : e.threads)
                if (const auto it = std::ranges::find(m.d.threads, t.id, &Thread::id);
                    it != m.d.threads.end() && it->renamed_at > t.renamed_at) {
                    t.title      = it->title;
                    t.renamed_at = it->renamed_at;
                }
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            // If the thread picker is open, its cursor may now point past the
            // end of the freshly-loaded (possibly shorter) list. Re-clamp so
            // the view and every ThreadList* handler index safely.
            if (auto* p = m.ui.panel.get<pn::ThreadList>()) {
                const int sz = visible_count(m, *p);
                p->index = sz > 0 ? std::clamp(p->index, 0, sz - 1) : 0;
            }
            return Cmd::none();
        },
        [&](ThreadLoaded& e) -> Cmd {
            // Result of the async single-thread load kicked off by
            // ThreadListSelect. Empty Thread (default ThreadId) means
            // the disk read or parse failed; just clear the spinner
            // and leave the current thread in place.
            // No pending load means it was superseded (^N): drop it.
            if (m.s.loading_thread.value.empty()) return Cmd::none();
            const bool wanted = e.thread.id == m.s.loading_thread;
            m.s.loading_thread = {};
            m.s.thread_loading = false;
            if (!wanted || e.thread.id.value.empty()) return Cmd::none();
            // Smart-Mode per-thread routing state belongs to the departing
            // thread too — same reset as reset_to_fresh_thread (momentum,
            // cascade bias, outcome-feedback signature). Without it the
            // loaded thread's first turn inherits the OLD thread's tier
            // momentum and its first follow-up trains the old signature.
            m.s.smart_turn_complexity  = smart::Complexity::Standard;
            m.s.smart_effort_bias      = 0;
            // Timing probe for the synchronous portion of the load
            // (rehydrate + release_to_kernel) that still lives on the UI
            // thread. AGENTTY_LOG=perf=debug surfaces it.
            //
            // A STOPWATCH: it reads a real clock twice to time work inside
            // this fold, which the fold's single instant (m.now) can't do.
            // Log-only; never reaches the Model, so replay is unaffected.
            auto stamp = [](const char* tag, auto t0) {
                auto dt = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                AGT_LOG(Perf, Debug, "thread.load", "stage={} ms={:.2f}",
                        tag, dt);
            };
            m.d.current = std::move(e.thread);
            // A rename is stored by the writer, which may still be busy with
            // an earlier save, so a load right after one can read the old
            // title from disk. Keep whichever title was renamed more recently.
            if (const auto it = std::ranges::find(m.d.threads, m.d.current.id, &Thread::id);
                it != m.d.threads.end() && !it->title.empty()
                && it->renamed_at > m.d.current.renamed_at) {
                m.d.current.title      = it->title;
                m.d.current.renamed_at = it->renamed_at;
            }
            m.s.reset_usage_for_thread(last_reported_prompt(m.d.current));
            // The loaded thread has no live turn, so any run state still
            // standing belongs to the thread we just left. Dropping it is
            // not housekeeping — it is a crash fix.
            //
            // Switching threads mid-stream (the load is async, so the wire
            // can very much still be running) left `phase` non-Idle and
            // `pending_permission` set, both pointing at a transcript that
            // no longer exists. The view then walks the NEW thread's
            // messages looking for the old thread's ToolUse id, and the
            // in-flight row reads active_ctx() for a run whose messages are
            // gone. NewThread has always done this via reset_to_fresh_thread
            // (see "a fresh empty thread has no live turn"); ThreadLoaded is
            // the same situation and was simply missing it.
            //
            // Order matters: hand the kernel back BEFORE the transcript is
            // rehydrated so a late delta can't land against a half-swapped
            // model.
            end_turn(m.s.phase);
            m.d.pending_permission.reset();
            // Modals framed the OLD thread's content (the palette and
            // code-block pickers hold indices into the departing thread's
            // last reply), so they'd index into the wrong transcript.
            m.ui.panel.close<pn::Palette>();
            m.ui.panel.close<pn::CodeBlocks>();
            m.ui.panel.close<pn::CodeBlockResult>();
            // Drop the whole render cache — same rationale as NewThread:
            // the entries belong to the thread being left, which won't
            // freeze again. The loaded thread rebuilds its frozen prefix
            // via rehydrate_frozen below and repopulates the cache lazily.
            m.ui.view_cache.clear();
            // Wipe the composer draft — same rationale as NewThread: a
            // pasted-but-unsent image / chip / queued message belongs to
            // the thread being left, and the leftover image Attachment has
            // empty bytes (drained into a prior Message), which serializes
            // an empty image block and 400s the next submit.
            reset_composer_draft(m.ui.composer);
            auto t1 = std::chrono::steady_clock::now();
            rehydrate_frozen(m);
            stamp("rehydrate_frozen", t1);
            // Frozen scrollback was just built from cold; the very
            // first render() would otherwise pay full layout+paint
            // over every frozen Turn. Flip the warmup flag so maya's
            // run loop pre-warms the component cache before the
            // wire-bound render — see Program::needs_warmup hook.
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            // Arm the one-shot post-paint trim: the rehydrate budget used
            // ESTIMATED heights; the first paint records real ones into the
            // ledger, and the Tick arm re-trims against those. The Tick
            // subscription gates on this flag (subscribe.cpp) until it fires.
            m.ui.pending_rehydrate_trim = !m.ui.frozen.empty();
            auto t2 = std::chrono::steady_clock::now();
            release_to_kernel();
            stamp("release_to_kernel", t2);
            AGT_LOG(Perf, Debug, "thread.load_done",
                    "msgs={} frozen={} frozen_rows={} "
                    "frozen_through={} term_h={}",
                    m.d.current.messages.size(),
                    m.ui.frozen.size(),
                    m.ui.frozen.row_total(),
                    m.ui.frozen_through,
                    m.ui.term_rows);
            // Wholesale model swap into the loaded thread. Same
            // rationale as NewThread above: the previous thread's
            // overflow rows are committed to native scrollback and only
            // reset_inline (which emits `\x1b[2J\x1b[3J\x1b[H`) can
            // erase them. Without it the previous thread's tail turns
            // are visible above the rehydrated thread's first turn.
            //
            // Per maya/device.hpp reset_inline() docs: this is the
            // sanctioned recovery for thread switch / new thread. The
            // `\x1b[3J` cost (wipes the user's pre-agentty shell
            // scrollback) is acceptable because the user explicitly
            // asked for the content swap (picker select).
            return cmd::reset_inline();
        },
    }, tm);
}

} // namespace agentty::app::detail
