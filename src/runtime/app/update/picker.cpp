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
#include "agentty/provider/registry.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/runtime/view/helpers.hpp"
#include "agentty/tool/skills.hpp"
#include "agentty/tool/subagent.hpp"

namespace agentty::app::detail {

namespace pick = agentty::ui::pick;
using maya::overload;

namespace {
// Indices into m.d.available_models that match the picker's live query
// (case-insensitive substring over the display name). An empty query
// yields every index in order, so the un-filtered picker is unchanged.
// The picker cursor (OpenAt::index) indexes INTO this filtered list, so
// every nav/select site resolves through here to reach the real model.
std::vector<int> model_filtered(const std::vector<ModelInfo>& models,
                                std::string_view query) {
    std::vector<int> out;
    out.reserve(models.size());
    for (int i = 0; i < static_cast<int>(models.size()); ++i)
        if (pick::fuzzy_contains(models[static_cast<std::size_t>(i)].display_name,
                                 query))
            out.push_back(i);
    return out;
}
} // namespace
using maya::Cmd;

Step model_picker_update(Model m, msg::ModelPickerMsg pm) {
    return std::visit(overload{
        [&](OpenModelPicker) -> Step {
            int idx = 0;
            for (int i = 0; i < static_cast<int>(m.d.available_models.size()); ++i)
                if (m.d.available_models[i].id == m.d.model_id) idx = i;
            m.ui.model_picker = pick::OpenAt{idx};
            m.s.models_loading = true;
            return {std::move(m), cmd::fetch_models()};
        },
        [&](ModelsLoaded& e) -> Step {
            // The fetch finished (success OR failure) — always clear the
            // in-flight flag so the picker leaves "Loading models…".
            m.s.models_loading = false;
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            m.d.available_models.clear();
            for (auto& mi : e.models) {
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.d.available_models.push_back(std::move(mi));
            }
            // If the active model isn't offered by this provider (e.g. just
            // switched to Ollama with no recall, or a stale saved id), fall
            // back to the first available model so the user is never pointed
            // at a model that 400s on the first prompt. Persist the pick so
            // it sticks as this provider's recall.
            bool active_present = false;
            for (const auto& mi : m.d.available_models)
                if (mi.id == m.d.model_id) { active_present = true; break; }
            if (!active_present && !m.d.available_models.empty()) {
                m.d.model_id = m.d.available_models.front().id;
                m.s.context_max =
                    ui::context_max_for_model(m.d.model_id.value);
                tools::subagent::set_model(m.d.model_id.value);
                persist_settings(m);
            }
            if (auto* p = pick::opened(m.ui.model_picker)) {
                // Cursor indexes the FILTERED list; a fetch can land while a
                // query is active. Find the active model's position within
                // the current filter (fall back to row 0).
                const auto vis = model_filtered(m.d.available_models, p->query);
                p->index = 0;
                for (int i = 0; i < static_cast<int>(vis.size()); ++i)
                    if (m.d.available_models[static_cast<std::size_t>(vis[static_cast<std::size_t>(i)])].id
                        == m.d.model_id) { p->index = i; break; }
            }
            return done(std::move(m));
        },
        [&](CloseModelPicker) -> Step {
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerMove& e) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            const auto vis = model_filtered(m.d.available_models, p->query);
            if (vis.empty()) return done(std::move(m));
            int sz = static_cast<int>(vis.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ModelPickerJump& e) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            const auto vis = model_filtered(m.d.available_models, p->query);
            if (vis.empty()) return done(std::move(m));
            int sz = static_cast<int>(vis.size());
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
        [&](ModelPickerFilterInput& e) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            // Append the codepoint (UTF-8). Narrowing the list can leave
            // the cursor past the new end — clamp so it always points at a
            // visible row (the Picker widget auto-scrolls to it).
            char buf[4];
            const char32_t c = e.ch;
            if (c < 0x80) p->query.push_back(static_cast<char>(c));
            else {
                int n = 0;
                if (c < 0x800) { buf[n++] = static_cast<char>(0xC0 | (c >> 6)); }
                else if (c < 0x10000) {
                    buf[n++] = static_cast<char>(0xE0 | (c >> 12));
                    buf[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                } else {
                    buf[n++] = static_cast<char>(0xF0 | (c >> 18));
                    buf[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                    buf[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                }
                if (c >= 0x80) buf[n++] = static_cast<char>(0x80 | (c & 0x3F));
                p->query.append(buf, static_cast<std::size_t>(n));
            }
            const int sz = static_cast<int>(
                model_filtered(m.d.available_models, p->query).size());
            p->index = sz == 0 ? 0 : std::clamp(p->index, 0, sz - 1);
            return done(std::move(m));
        },
        [&](ModelPickerFilterBackspace) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (!p || p->query.empty()) return done(std::move(m));
            // Drop the last UTF-8 codepoint (walk back over continuation
            // bytes 0x80..0xBF).
            std::size_t n = p->query.size();
            do { --n; } while (n > 0
                && (static_cast<unsigned char>(p->query[n]) & 0xC0) == 0x80);
            p->query.resize(n);
            const int sz = static_cast<int>(
                model_filtered(m.d.available_models, p->query).size());
            p->index = sz == 0 ? 0 : std::clamp(p->index, 0, sz - 1);
            return done(std::move(m));
        },
        [&](ModelPickerSelect) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p) {
                const auto vis = model_filtered(m.d.available_models, p->query);
                if (!vis.empty() && p->index >= 0
                    && p->index < static_cast<int>(vis.size())) {
                    const int real = vis[static_cast<std::size_t>(p->index)];
                    m.d.model_id = m.d.available_models[static_cast<std::size_t>(real)].id;
                    // Update the per-model context cap so the status-bar ctx
                    // % bar reflects the right denominator for the new model
                    // (1 M for `[1m]` variants, 200 K otherwise).
                    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                    // Keep subagents on the live model: the startup config
                    // captured whatever was saved at launch, which can be a
                    // stale/invalid id (every subagent request 400s and the
                    // tool returns no report). Track the picker selection.
                    tools::subagent::set_model(m.d.model_id.value);
                    persist_settings(m);
                }
            }
            m.ui.model_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ModelPickerToggleFavorite) -> Step {
            auto* p = pick::opened(m.ui.model_picker);
            if (p) {
                const auto vis = model_filtered(m.d.available_models, p->query);
                if (!vis.empty() && p->index >= 0
                    && p->index < static_cast<int>(vis.size())) {
                    auto& mi = m.d.available_models[
                        static_cast<std::size_t>(vis[static_cast<std::size_t>(p->index)])];
                    mi.favorite = !mi.favorite;
                }
            }
            return done(std::move(m));
        },
        [&](ModelPickerCycleEffort& e) -> Step {
            // Step the reasoning-effort tier within what the highlighted
            // model supports (cycle_effort wraps and returns None for a
            // model that can't reason). Persist immediately so the pick
            // survives a restart; the request path re-clamps at send time.
            auto* p = pick::opened(m.ui.model_picker);
            if (p) {
                const auto vis = model_filtered(m.d.available_models, p->query);
                if (!vis.empty() && p->index >= 0
                    && p->index < static_cast<int>(vis.size())) {
                    const auto caps = ModelCapabilities::from_id(
                        m.d.available_models[
                            static_cast<std::size_t>(vis[static_cast<std::size_t>(p->index)])]
                            .id.value);
                    m.d.effort = cycle_effort(m.d.effort, e.delta, caps);
                    persist_settings(m);
                }
            }
            return done(std::move(m));
        },
    }, pm);
}

// ── Provider picker ────────────────────────────────────────────────────────
// Selecting a row live-switches the active backend: parse the preset id
// into a Selection, install it (process-global), persist it, swap the
// Deps auth to the new provider's resolved credentials, and kick a fresh
// model fetch so the model list reflects the new backend. No restart.
Step provider_picker_update(Model m, msg::ProviderPickerMsg pm) {
    const auto presets = provider::providers();
    // One extra virtual row after the presets: "Custom host…", which opens
    // a free-text endpoint entry (llama.cpp / vLLM / remote host:port).
    const int n = static_cast<int>(presets.size()) + 1;
    const int custom_row = n - 1;   // index of the sentinel row
    return std::visit(overload{
        [&](OpenProviderPicker) -> Step {
            // Open at the row matching the currently-active provider.
            int idx = 0;
            const auto& active_label = provider::active().kind
                                       == provider::Kind::OpenAI
                ? provider::active().openai_endpoint.label
                : std::string{provider::default_provider_id()};
            for (int i = 0; i < n; ++i)
                if (presets[static_cast<std::size_t>(i)].id == active_label) idx = i;
            m.ui.provider_picker = pick::OpenAt{idx};
            return done(std::move(m));
        },
        [&](CloseProviderPicker) -> Step {
            m.ui.provider_picker = pick::Closed{};
            return done(std::move(m));
        },
        [&](ProviderPickerMove& e) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p || n == 0) return done(std::move(m));
            p->index = (p->index + e.delta + n) % n;
            return done(std::move(m));
        },
        [&](ProviderPickerJump& e) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p || n == 0) return done(std::move(m));
            using W = ProviderPickerJump::Where;
            constexpr int kPage = 14;  // matches kViewportH in pickers.cpp
            switch (e.where) {
                case W::Home:     p->index = 0; break;
                case W::End:      p->index = n - 1; break;
                case W::PageUp:   p->index = std::max(0, p->index - kPage); break;
                case W::PageDown: p->index = std::min(n - 1, p->index + kPage); break;
            }
            return done(std::move(m));
        },
        [&](ProviderPickerSelect) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            m.ui.provider_picker = pick::Closed{};
            if (!p || p->index < 0 || p->index >= n) return done(std::move(m));

            // "Custom host…" row: hand off to the free-text endpoint modal
            // instead of selecting a preset.
            if (p->index == custom_row) {
                m.ui.login = ui::login::CustomHostInput{};
                return done(std::move(m));
            }

            const auto& preset = presets[static_cast<std::size_t>(p->index)];
            const std::string spec{preset.id};

            // Capture the OUTGOING provider id before provider::select swaps
            // active() — needed to file the current model under it.
            const std::string active_provider_id_before = active_provider_id();

            // Resolve the new backend's credentials BEFORE committing the
            // switch so we can refuse a switch that would land the user in a
            // silently-broken state (every request 401s with no key). For
            // Anthropic we reuse the session creds; for OpenAI-family we
            // resolve from the registry's env-var chain; local needs none.
            //
            // CRITICAL: pass the Anthropic creds loaded FRESH from disk, NOT
            // deps().auth. deps().auth holds whatever provider is currently
            // active — if the user is on Ollama (empty key) and switches BACK
            // to Anthropic, resolve_auth_for would echo that empty key as the
            // "anthropic creds" and every Anthropic request (incl. the model
            // list fetch) would see is_empty(auth) and silently no-op. The
            // real login creds live on disk and survive provider hops.
            auth::AuthHeader anthropic_creds = deps().auth;
            if (auto saved = auth::load_credentials())
                anthropic_creds = auth::make_auth_header(*saved);
            // Consult the in-app key store (Settings.provider_keys) so a key
            // the user pasted on a PRIOR switch to this provider is reused —
            // otherwise resolve_auth_for only sees env vars and re-prompts on
            // every switch even though the key is persisted on disk.
            std::string saved_provider_key;
            {
                auto settings = deps().load_settings();
                if (auto it = settings.provider_keys.find(spec);
                    it != settings.provider_keys.end())
                    saved_provider_key = it->second;
            }
            auth::AuthHeader new_auth =
                provider::resolve_auth_for(spec, anthropic_creds,
                                           /*cli_key=*/{}, saved_provider_key);

            // A hosted (non-local) OpenAI-family provider with no resolvable
            // key can't stream. Instead of a dead-end error, open the in-app
            // key-entry modal targeted at THIS provider: the user pastes a
            // key, it's saved to Settings.provider_keys, and login_submit
            // commits the switch (see login.cpp). The selection isn't
            // installed until the key lands.
            const bool needs_key =
                preset.kind == provider::Kind::OpenAI && !preset.is_local
                && preset.auth != provider::AuthStyle::None;
            if (needs_key && auth::is_empty(new_auth)) {
                m.ui.login = ui::login::ApiKeyInput{
                    .key_input      = {},
                    .cursor         = 0,
                    .provider       = spec,
                    .provider_label = std::string{preset.label},
                };
                return done(std::move(m));
            }

            // Install + persist the new selection.
            provider::select(provider::parse_selection(spec));
            {
                auto settings = deps().load_settings();
                // Remember the model we were using on the OUTGOING provider
                // so a later switch back restores it.
                if (!m.d.model_id.empty())
                    settings.provider_models[active_provider_id_before] =
                        m.d.model_id.value;
                settings.provider = spec;
                deps().save_settings(settings);
            }

            // Make a valid model active for the NEW provider: the model last
            // used there, else a built-in default. For local backends with
            // no recall this is empty and ModelsLoaded auto-selects the first
            // available model once the refetch lands.
            if (auto next = model_for_provider(spec); !next.empty()) {
                m.d.model_id = ModelId{next};
                m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                tools::subagent::set_model(m.d.model_id.value);
            }

            // Swap the Deps auth to the new backend's credentials. The stream
            // seam reads provider::active() at call time so the next request
            // targets the new backend.
            app::switch_provider(new_auth);

            // Models differ per backend — drop the stale list and refetch.
            m.d.available_models.clear();
            m.s.models_loading = true;

            // Confirmation toast + refetch the new backend's model list.
            auto toast = set_status_toast(
                m, "provider → " + std::string{preset.label});
            return {std::move(m),
                    Cmd<Msg>::batch(std::move(toast), cmd::fetch_models())};
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
            // Open AT the current thread, not row 0 — the user's mental
            // anchor is "where am I", and cycling from there (↑ newer /
            // ↓ older) mirrors the Alt+←/→ quick-cycle order.
            int at = 0;
            for (int i = 0; i < static_cast<int>(m.d.threads.size()); ++i)
                if (m.d.threads[static_cast<std::size_t>(i)].id == m.d.current.id) {
                    at = i;
                    break;
                }
            m.ui.thread_list = pick::OpenAt{at};
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
        [&](ThreadCycle& e) -> Step {
            // Alt+←/→ — jump to the adjacent thread without the picker.
            // Recency order (same as ^J): index 0 = newest; +1 = older,
            // -1 = newer, wrapping at both ends. Gated on an idle
            // session — swapping m.d.current under an active stream
            // would strand the in-flight ctx's writes.
            if (m.s.active()) {
                auto cmd = set_status_toast(m,
                    "wait for the reply to finish before switching threads");
                return {std::move(m), std::move(cmd)};
            }
            if (m.s.thread_loading) return done(std::move(m));
            const int sz = static_cast<int>(m.d.threads.size());
            if (sz == 0) {
                // History not loaded yet (or genuinely empty) — kick a
                // refresh so the NEXT press works, and say so.
                Cmd<Msg> cmd = Cmd<Msg>::none();
                if (!m.s.threads_loading) {
                    m.s.threads_loading = true;
                    cmd = cmd::load_threads_async();
                }
                auto toast = set_status_toast(m, "no other threads yet");
                return {std::move(m),
                        Cmd<Msg>::batch(std::move(cmd), std::move(toast))};
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
                    return {std::move(m), std::move(toast)};
                }
                target = ((cur + e.delta) % sz + sz) % sz;
            }
            const Thread& meta = m.d.threads[static_cast<std::size_t>(target)];
            if (meta.id == m.d.current.id) return done(std::move(m));
            // Preserve the thread being left — same courtesy NewThread
            // extends. finalize_turn saves per turn, but a title edit or
            // an un-persisted tail shouldn't be lost to a quick cycle.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            m.s.thread_loading = true;
            // "thread k/N · title" — the positional readout that makes
            // repeated Alt+←/→ presses feel like flipping through a
            // deck rather than teleporting blind. Survives the swap
            // because ThreadLoaded doesn't touch m.s.status.
            auto toast = set_status_toast(m,
                "thread " + std::to_string(target + 1) + "/"
                    + std::to_string(sz) + " \xc2\xb7 "
                    + (meta.title.empty() ? "(untitled)" : meta.title));
            return {std::move(m),
                    Cmd<Msg>::batch(cmd::load_thread_async(meta.id),
                                    std::move(toast))};
        },
        [&](NewThread) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            // Skill activations belong to the old thread's context;
            // the new thread must be able to re-load any skill.
            tools::skills::reset_activations();
            // Drop the whole render cache: every (tid,msg) entry belongs
            // to the thread we're leaving, whose messages will never
            // freeze again (freeze is the only per-entry drop, and it
            // only runs on the CURRENT thread). Keys embed thread_id so
            // there's no collision — this is purely reclaiming the old
            // thread's staged/pinned entries so they don't linger for the
            // session. The empty new thread repopulates from scratch.
            m.ui.view_cache.clear();
            m.d.current = Thread{};
            m.d.current.id = deps().new_thread_id();
            m.d.current.created_at = m.d.current.updated_at = std::chrono::system_clock::now();
            clear_frozen(m);
            m.ui.thread_list = pick::Closed{};
            m.ui.command_palette = palette::Closed{};
            // Blocks belong to the OLD thread's last reply — running one
            // against a fresh empty thread would be confusing.
            m.ui.code_blocks = code_block_picker::Closed{};
            // Wipe the whole composer draft — a pasted-but-unsent image (or
            // any chip / queued message) belongs to the thread we're
            // leaving. Leaking it carried an empty-bytes image attachment
            // into the new thread's first submit and 400'd the request.
            reset_composer_draft(m.ui.composer);
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
                    m.ui.frozen.row_total(),
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
