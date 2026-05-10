// model_picker_update + thread_list_update — reducers for the model and
// thread pickers (and the related async loads, ModelsLoaded / ThreadsLoaded).
// Both are list-modal pickers that the user opens with a key shortcut, moves
// through with Up/Down, and confirms with Enter; the underlying data comes
// from the store + provider so neither reducer is purely-local.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"

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
        [&](ThreadListSelect) -> Step {
            auto* p = pick::opened(m.ui.thread_list);
            if (p && !m.d.threads.empty()) {
                // Picker entries are metadata-only skeletons (see
                // load_all_threads); full message bodies are read off
                // disk now. A failed load (file gone, corrupted) leaves
                // m.d.current untouched and closes the picker.
                const Thread& meta = m.d.threads[p->index];
                auto loaded = deps().load_thread(meta.id);
                if (!loaded) {
                    m.ui.thread_list = pick::Closed{};
                    return done(std::move(m));
                }
                // Cache invalidation is implicit: the new thread's
                // (thread_id, message_id) keys differ from the old
                // thread's, so view-side cache lookups against the new
                // thread's messages all miss and rebuild. Old thread's
                // entries linger until LRU pushes them out — bounded
                // by cap (32) × per-entry size, so transient memory
                // overhead is on the order of tens of MiB until the
                // new thread's accesses reclaim those slots. The
                // reducer no longer reaches into the view cache; this
                // arm is purely a Model state transition.
                m.d.current = std::move(*loaded);
                // Hand the freed pages back to the kernel. glibc malloc
                // will otherwise hold the released arenas indefinitely;
                // mimalloc is more eager but still benefits from the
                // explicit collect at this known free-point.
                release_to_kernel();
            }
            m.ui.thread_list = pick::Closed{};
            // A freshly-loaded thread has no prev-frame correspondence in
            // the inline buffer — render everything that fits in the window.
            int total = static_cast<int>(m.d.current.messages.size());
            m.ui.thread_view_start = std::max(0, total - kViewWindow);
            return done(std::move(m));
        },
        [&](NewThread) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            // No cache eviction needed — the freshly-minted Thread
            // has a different ThreadId, and a freshly-appended Message
            // has a fresh MessageId, so the old (tid, mid) keys never
            // collide with new lookups. LRU drains the previous
            // thread's entries as the new thread fills the cap.
            m.d.current = Thread{};
            m.d.current.id = deps().new_thread_id();
            m.d.current.created_at = m.d.current.updated_at = std::chrono::system_clock::now();
            m.ui.thread_list = pick::Closed{};
            m.ui.command_palette = palette::Closed{};
            m.ui.composer.text.clear();
            m.ui.composer.cursor = 0;
            // any → Idle. Discards the active ctx if any was present
            // (NewThread can fire mid-stream; the user-visible Esc
            // wasn't pressed but the request is conceptually
            // abandoned along with the thread).
            m.s.phase = phase::Idle{};
            m.ui.thread_view_start = 0;
            release_to_kernel();
            return done(std::move(m));
        },
        [&](ThreadsLoaded& e) -> Step {
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            return done(std::move(m));
        },
    }, tm);
}

} // namespace agentty::app::detail
