// model_picker_update + thread_list_update — reducers for the model and
// thread pickers (and the related async loads, ModelsLoaded / ThreadsLoaded).
// Both are list-modal pickers that the user opens with a key shortcut, moves
// through with Up/Down, and confirms with Enter; the underlying data comes
// from the store + provider so neither reducer is purely-local.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/skills.hpp"

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;
using maya::Cmd;

Step model_picker_update(Model m, msg::ModelPickerMsg pm) {
    return std::visit(overload{
        [&](OpenModelPicker) -> Step {
            int idx = 0;
            for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                if (m.d.available_models[i].id == m.d.model_id) idx = i;
            m.ui.model_picker = pick::OpenAt{idx};
            return {std::move(m), cmd::fetch_models()};
        },
        [&](ModelsLoaded& e) -> Step {
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            m.d.available_models.clear();
            for (auto& mi : e.models) {
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.d.available_models.push_back(std::move(mi));
            }
            if (auto* p = pick::opened(m.ui.model_picker)) {
                p->index = 0;
                for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                    if (m.d.available_models[i].id == m.d.model_id) p->index = i;
            }
            return done(std::move(m));
        },
        [&](CloseModelPicker) -> Step {
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerMove& e) -> Step {
            if (m.d.available_models.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.available_models.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ModelPickerJump& e) -> Step {
            if (m.d.available_models.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.available_models.size());
            using W = ModelPickerJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = sz - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(sz - 1, p->index + kPage); break;
            }
            return done(std::move(m));
        },
        [&](ModelPickerSelect) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p && !m.d.available_models.empty()) {
                m.d.model_id = m.d.available_models[p->index].id;
                // Update the per-model context cap so the status-bar ctx
                // % bar reflects the right denominator for the new model
                // (1 M for `[1m]` variants, 200 K otherwise).
                m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                persist_settings(m);
            }
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerToggleFavorite) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p && !m.d.available_models.empty()) {
                auto& mi = m.d.available_models[p->index];
                mi.favorite = !mi.favorite;
            }
            return done(std::move(m));
        },
    }, pm);
}

Step thread_list_update(Model m, msg::ThreadListMsg tm) {
    return std::visit(overload{
        [&](OpenThreadList) -> Step {
            // Refresh in the background if no load is in flight — the
            // walk + parse is too slow (seconds, with hundreds of
            // multi-MB thread files) to do synchronously here. The
            // picker opens immediately against the cached list; new
            // entries fade in when ThreadsLoaded lands.
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (!m.s.threads_loading) {
                m.s.threads_loading = true;
                cmd = cmd::load_threads_async();
            }
            m.ui.thread_list = pick::OpenAt{0};
            return {std::move(m), std::move(cmd)};
        },
        [&](CloseThreadList) -> Step {
            m.ui.thread_list = pick::Closed{};
            return done(std::move(m));
        },
        [&](ThreadListMove& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.thread_list);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.threads.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListJump& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.thread_list);
            if (!p) return done(std::move(m));
            int sz = static_cast<int>(m.d.threads.size());
            using W = ThreadListJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = sz - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(sz - 1, p->index + kPage); break;
            }
            return done(std::move(m));
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
        //   Cmd::force_redraw demotes Synced → Stale, routing the
        //   next render through compose case (B). Case (B)'s
        //   scroll-to-fit branch (scroll_n > 0) emits \n at the
        //   viewport bottom when the new frame is taller than the
        //   old cursor's offset from viewport top — each \n there
        //   scrolls a row of whatever was on screen (old thread
        //   tail + host shell history above it) up into
        //   terminal-owned scrollback, permanently. History: commit
        //   8becb88 did exactly that and reverted in 0b24148.
        [&](ThreadListSelect) -> Step {
            auto* p = pick::opened(m.ui.thread_list);
            Cmd<Msg> cmd = Cmd<Msg>::none();
            if (p && !m.d.threads.empty() && !m.s.thread_loading) {
                const Thread& meta = m.d.threads[p->index];
                // Same-thread re-select — closing the picker is the
                // only useful action. No async load: would just
                // reparse the same bytes and flash.
                if (meta.id == m.d.current.id) {
                    m.ui.thread_list = pick::Closed{};
                    return done(std::move(m));
                }
                m.s.thread_loading = true;
                cmd = cmd::load_thread_async(meta.id);
            }
            m.ui.thread_list = pick::Closed{};
            return {std::move(m), std::move(cmd)};
        },
        [&](NewThread) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            // Skill activations belong to the old thread's context;
            // the new thread must be able to re-load any skill.
            tools::skills::reset_activations();
            // No cache eviction needed — the freshly-minted Thread
            // has a different ThreadId, and a freshly-appended Message
            // has a fresh MessageId, so the old (tid, mid) keys never
            // collide with new lookups. LRU drains the previous
            // thread's entries as the new thread fills the cap.
            m.d.current = Thread{};
            m.d.current.id = deps().new_thread_id();
            m.d.current.created_at = m.d.current.updated_at = std::chrono::system_clock::now();
            clear_frozen(m);
            m.ui.thread_list = pick::Closed{};
            m.ui.command_palette = palette::Closed{};
            m.ui.composer.text.clear();
            m.ui.composer.cursor = 0;
            // any → Idle. Discards the active ctx if any was present
            // (NewThread can fire mid-stream; the user-visible Esc
            // wasn't pressed but the request is conceptually
            // abandoned along with the thread).
            m.s.phase = phase::Idle{};
            release_to_kernel();
            // Wholesale model swap into a fresh (empty) thread. The old
            // thread typically overflowed the viewport, committing many
            // rows to the terminal's native scrollback. Those rows are
            // off-viewport and OWNED by the terminal emulator — neither
            // force_redraw (viewport-only case-B) nor
            // commit_scrollback_overflow (advances prev_rows but leaves
            // physical off-viewport rows on the wire) can erase them.
            // Result without reset_inline: the previous thread's tail
            // turns sit stranded above the new welcome screen, visible
            // as a fake "continuation" of the new thread above it.
            //
            // reset_inline emits `\x1b[2J\x1b[3J\x1b[H` — the ONLY path
            // that reaches native scrollback. Per maya/app/app.hpp:
            // "the correct recovery for a WHOLESALE CONTENT SWAP into
            // shorter content (thread switch / new thread)."
            //
            // Cost: `\x1b[3J` wipes the terminal's saved-lines, including
            // the user's pre-agentty shell history. This is an explicit,
            // user-initiated content swap (^N / picker select) — wiping
            // scrollback is acceptable here precisely because the user
            // asked for it. Per maya's contract this is the ONE allowed
            // wiring of reset_inline; do NOT extend it to per-turn paths.
            return {std::move(m), Cmd<Msg>::reset_inline()};
        },
        [&](ThreadsLoaded& e) -> Step {
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            return done(std::move(m));
        },
        [&](ThreadLoaded& e) -> Step {
            // Result of the async single-thread load kicked off by
            // ThreadListSelect. Empty Thread (default ThreadId) means
            // the disk read or parse failed; just clear the spinner
            // and leave the current thread in place.
            m.s.thread_loading = false;
            if (e.thread.id.value.empty()) return done(std::move(m));
            // Old thread's skill activations leave context with it.
            tools::skills::reset_activations();
            // Optional timing probe. AGENTTY_LOAD_PROF=1 keeps surfacing
            // the synchronous portion of the load (rehydrate +
            // release_to_kernel) that still lives on the UI thread.
            const bool prof = []{
                static const bool on = [] {
                    const char* e = std::getenv("AGENTTY_LOAD_PROF");
                    return e && *e && *e != '0';
                }();
                return on;
            }();
            std::FILE* prof_out = nullptr;
            if (prof) prof_out = std::fopen("/tmp/agentty-load-prof.log", "a");
            auto stamp = [&](const char* tag, auto t0) {
                if (!prof_out) return;
                auto dt = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                std::fprintf(prof_out, "[load-async] %s: %.2f ms\n", tag, dt);
                std::fflush(prof_out);
            };
            m.d.current = std::move(e.thread);
            auto t1 = std::chrono::steady_clock::now();
            rehydrate_frozen(m);
            stamp("rehydrate_frozen", t1);
            // Frozen scrollback was just built from cold; the very
            // first render() would otherwise pay full layout+paint
            // over every frozen Turn. Flip the warmup flag so maya's
            // run loop pre-warms the component cache before the
            // wire-bound render — see Program::needs_warmup hook.
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            auto t2 = std::chrono::steady_clock::now();
            release_to_kernel();
            stamp("release_to_kernel", t2);
            if (prof_out) {
                const auto _ts = maya::platform::query_terminal_size(
                    maya::platform::stdout_handle());
                std::fprintf(prof_out,
                    "[load-async] msgs=%zu frozen=%zu frozen_rows=%zu "
                    "frozen_through=%zu term_h=%d\n",
                    m.d.current.messages.size(),
                    m.ui.frozen.size(),
                    m.ui.frozen_row_total,
                    m.ui.frozen_through,
                    _ts.height.value);
                std::fflush(prof_out);
                std::fclose(prof_out);
            }
            // Wholesale model swap into the loaded thread. Same
            // rationale as NewThread above: the previous thread's
            // overflow rows are committed to native scrollback and only
            // reset_inline (which emits `\x1b[2J\x1b[3J\x1b[H`) can
            // erase them. Without it the previous thread's tail turns
            // are visible above the rehydrated thread's first turn.
            //
            // Per maya/app/app.hpp reset_inline() docs: this is the
            // sanctioned recovery for thread switch / new thread. The
            // `\x1b[3J` cost (wipes the user's pre-agentty shell
            // scrollback) is acceptable because the user explicitly
            // asked for the content swap (picker select).
            return {std::move(m), Cmd<Msg>::reset_inline()};
        },
    }, tm);
}

} // namespace agentty::app::detail
