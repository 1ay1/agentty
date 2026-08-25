// model_picker_update + thread_list_update — reducers for the model and
// thread pickers (and the related async loads, ModelsLoaded / ThreadsLoaded).
// Both are list-modal pickers that the user opens with a key shortcut, moves
// through with Up/Down, and confirms with Enter; the underlying data comes
// from the store + provider so neither reducer is purely-local.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/platform/io.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/provider/chatgpt/responses.hpp"
#include "agentty/provider/copilot/copilot_oauth.hpp"
#include "agentty/provider/kimi/kimi_oauth.hpp"
#include "agentty/provider/registry.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/picker.hpp"
#include "agentty/runtime/provider_rows.hpp"
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

[[nodiscard]] bool is_chatgpt_active() {
    return provider::active().is_chatgpt();
}

// Codex exposes a reasoning ladder through its live model catalogue. Agentty
// currently has no `Ultra` enum value, so `max` is the highest selectable
// level; all other CLI-supported levels map one-for-one.
[[nodiscard]] Effort cycle_codex_effort(Effort current, int delta) {
    static constexpr std::array<Effort, 6> levels{
        Effort::None, Effort::Low, Effort::Medium,
        Effort::High, Effort::Xhigh, Effort::Max,
    };
    int index = 0;
    for (int i = 0; i < static_cast<int>(levels.size()); ++i)
        if (levels[static_cast<std::size_t>(i)] == current) { index = i; break; }
    const int n = static_cast<int>(levels.size());
    index = ((index + delta) % n + n) % n;
    return levels[static_cast<std::size_t>(index)];
}
} // namespace
using maya::Cmd;

// ── Fresh-thread reset ────────────────────────────────────────────────────
// Swap the model over to a brand-new empty thread and return the terminal
// reset that wipes the departing thread's rendered turns off-screen.
//
// This is the SHARED core behind two entry points: `NewThread` (^N / picker
// `N`) and `ThreadListDelete` when the row removed is the active thread.
// It is deliberately the part *after* the caller's save/delete decision —
// NewThread persists the outgoing thread first, delete has just destroyed
// it — so the caller owns that policy and this owns the reset. Keeping the
// two callers on one code path is what stops them drifting (they had, and
// the delete copy was missing the phase reset + kernel release + inline
// wipe, which is exactly the machinery that makes a mid-stream swap safe).
//
// Returns the reset_inline Cmd so the caller can batch it with its own
// commands (delete also kicks a thread-list refresh + a toast).
[[nodiscard]] Cmd<Msg> reset_to_fresh_thread(Model& m) {
    // Skill activations belong to the departing thread's context; the new
    // thread must be able to re-load any skill from scratch.
    tools::skills::reset_activations();
    // Drop the whole render cache: every (tid,msg) entry belongs to the
    // thread we're leaving, whose messages will never freeze again (freeze
    // is the only per-entry drop, and it only runs on the CURRENT thread).
    // Keys embed thread_id so there's no collision — this purely reclaims
    // the old thread's staged/pinned entries so they don't linger.
    m.ui.view_cache.clear();
    m.d.current = Thread{};
    m.d.current.id = deps().new_thread_id();
    m.d.current.created_at = m.d.current.updated_at =
        std::chrono::system_clock::now();
    clear_frozen(m);
    // Close every modal that framed the OLD thread: the picker we acted
    // from, plus the palette / code-block picker whose contents belonged to
    // the departing thread's last reply.
    m.ui.thread_list      = pick::Closed{};
    m.ui.command_palette  = palette::Closed{};
    m.ui.code_blocks      = code_block_picker::Closed{};
    // Wipe the whole composer draft — a pasted-but-unsent image (or any
    // chip / queued message) belongs to the thread we're leaving. Leaking
    // it once carried an empty-bytes image attachment into the new thread's
    // first submit and 400'd the request.
    reset_composer_draft(m.ui.composer);
    // A fresh empty thread has no live turn — drop any streaming phase and
    // hand the kernel back so a mid-stream swap can't leave the wire running
    // against a thread that no longer exists.
    m.s.phase = phase::Idle{};
    release_to_kernel();
    // Re-warm the active provider's TLS socket. The launch-time prewarm in
    // main() has usually aged out of the pool by now — a user reads a reply,
    // composes, then hits ^N, and the 90 s idle TTL has evicted the warm
    // connection. Without this the FIRST turn of every new thread re-pays
    // the full DNS+TCP+TLS handshake (~150-300 ms) before its first SSE byte,
    // which reads as a per-new-thread lag. Opening the socket now overlaps
    // that cost with the user typing their first prompt. Non-blocking: spawns
    // a tracked background dial and returns immediately; a no-op when the pool
    // is already warm enough to serve the next request.
    provider::prewarm_active_provider();
    // Per maya's contract this is the ONE allowed wiring of reset_inline: an
    // explicit, user-initiated content swap. `\x1b[3J` wipes saved-lines
    // (including pre-agentty shell history), acceptable precisely because
    // the user asked to switch threads. Do NOT extend it to per-turn paths.
    return Cmd<Msg>::reset_inline();
}

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
            // STALENESS GATE: only accept a payload fetched FOR the provider
            // that is active NOW. Two quick switches interleave their slow
            // fetches; without this, provider A's late catalog lands under
            // provider B and the picker offers models B cannot stream.
            // (Empty provider_id = legacy/synthetic dispatch — accept.)
            if (!e.provider_id.empty()
                && e.provider_id != active_provider_id()) {
                return done(std::move(m));   // keep models_loading: the
                                             // newer fetch is still in flight
            }
            // The fetch finished (success OR failure) — always clear the
            // in-flight flag so the picker leaves "Loading models…".
            m.s.models_loading = false;
            // A failed fetch surfaces its reason as a transient toast —
            // never as a StreamError, which would feed the live turn's
            // retry machinery (see the ModelsLoaded msg comment).
            if (!e.error.empty()) {
                auto toast = set_status_toast(m, std::move(e.error),
                                              std::chrono::seconds{6});
                return {std::move(m), std::move(toast)};
            }
            if (e.models.empty()) return done(std::move(m));
            auto settings = deps().load_settings();
            m.d.available_models.clear();
            for (auto& mi : e.models) {
                // DISCOVERED entitlement: this account already 400'd on the
                // context-1m beta ("long context beta is not available for
                // this subscription"), so offering the `[1m]` rows would
                // just sell a window the wire will reject. OAuth alone can't
                // tell us (the token carries no entitlement field) — the
                // flag is learned from the first rejection and cleared on
                // sign-out/account switch.
                if (settings.context_1m_blocked
                    && mi.id.value.find("[1m]") != std::string::npos)
                    continue;
                for (const auto& fav : settings.favorite_models)
                    if (mi.id == fav) mi.favorite = true;
                m.d.available_models.push_back(std::move(mi));
            }
            // Refresh the subagent router's candidate pool so read-only roles
            // route to the cheapest capable model THIS provider offers. Done
            // on every load (startup, provider switch, refetch) so routing
            // never uses a stale provider's list.
            tools::subagent::set_candidates(m.d.available_models);
            // Keep the subagent role-router in sync with Smart Mode (Layer 3b).
            tools::subagent::set_smart(m.d.smart);
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
                // The auto-selected model may not support the effort tier that
                // rode over from the previous provider — clamp it so the picker
                // chip and the wire agree (commit_provider_switch couldn't do
                // this yet: the model id was empty until this refetch landed).
                if (!is_chatgpt_active()) {
                    m.d.effort = clamp_effort(
                        m.d.effort, ModelCapabilities::from_id(m.d.model_id.value));
                }
                tools::subagent::set_model(m.d.model_id.value);
                persist_settings(m);
            }
            // The active model may have remained valid, in which case the
            // old branch did not refresh its context cap. Codex publishes a
            // 272K window (rather than Agentty's generic 200K fallback), and
            // the status-bar gauge must reflect that immediately.
            for (const auto& mi : m.d.available_models) {
                if (mi.id == m.d.model_id && mi.context_window > 0) {
                    m.s.context_max = mi.context_window;
                    break;
                }
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
            // Flush any effort-tier cycling to disk ONCE on close (see the
            // CycleEffort arm — persisting per arrow keystroke was a
            // synchronous load+fsync+rename on the UI thread per keypress).
            // Select persists on its own arm; Quit persists globally; this
            // covers the Esc path.
            if (m.ui.effort_dirty) {
                persist_settings(m);
                m.ui.effort_dirty = false;
            }
            m.ui.model_picker = pick::Closed{};
            // Slot-assign mode: Esc is BACK, not exit. Pop one level up the
            // picker stack — re-open Smart Mode at the slot row we descended
            // from — instead of closing every overlay. Navigating into a
            // setting and hitting Esc should return you to the parent picker.
            if (m.ui.smart_assign_slot >= 0) {
                const int slot = m.ui.smart_assign_slot;
                m.ui.smart_assign_slot = -1;
                m.ui.smart_mode = ui::pick::OpenAt{8 + slot};   // rows 8..10
                return done(std::move(m));
            }
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
                    const std::string chosen =
                        m.d.available_models[static_cast<std::size_t>(real)].id.value;

                    // Smart Mode slot-assign mode: write the chosen model into
                    // the target role slot instead of switching the active
                    // model. Enabling Smart Mode implicitly (pinning a slot
                    // means you want it on).
                    if (m.ui.smart_assign_slot >= 0) {
                        smart::SlotOverride* slot = nullptr;
                        switch (m.ui.smart_assign_slot) {
                            case 0: slot = &m.d.smart.strategic;      break;
                            case 1: slot = &m.d.smart.implementation; break;
                            case 2: slot = &m.d.smart.utility;        break;
                        }
                        if (slot) {
                            slot->model = chosen;
                            slot->set   = true;
                            m.d.smart.enabled = true;
                        }
                        const int assigned = m.ui.smart_assign_slot;
                        m.ui.smart_assign_slot = -1;
                        persist_settings(m);
                        m.ui.effort_dirty = false;
                        m.ui.model_picker = pick::Closed{};
                        // Pop back to the parent Smart Mode picker, cursor on
                        // the slot we just set — not out to the thread. You
                        // came from there and probably want to set the sibling
                        // slots too; forcing a re-open of Smart Mode after
                        // every slot is the exact tedium this fixes.
                        m.ui.smart_mode = ui::pick::OpenAt{8 + assigned};
                        auto toast = set_status_toast(m,
                            "Smart Mode slot set");
                        return {std::move(m), std::move(toast)};
                    }

                    m.d.model_id = m.d.available_models[static_cast<std::size_t>(real)].id;
                    // Update the per-model context cap so the status-bar ctx
                    // % bar (and the auto-compaction threshold) uses the right
                    // denominator. Prefer the window the provider actually
                    // advertised for this model (list_models stamps 1M for the
                    // Sonnet-4 line on OAuth; Ollama/OpenAI probe a real
                    // window) and only fall back to the auth-blind id guess
                    // when the loaded row carries no window.
                    m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
                    for (const auto& mi : m.d.available_models)
                        if (mi.id == m.d.model_id && mi.context_window > 0) {
                            m.s.context_max = mi.context_window;
                            break;
                        }
                    // Degrade the effort tier to what the newly-picked model
                    // supports — picking a non-reasoning (or lower-ceiling)
                    // model while effort=Xhigh must not leave a stale chip that
                    // the wire silently drops.
                    if (!is_chatgpt_active()) {
                        m.d.effort = clamp_effort(
                            m.d.effort, ModelCapabilities::from_id(m.d.model_id.value));
                    }
                    // Keep subagents on the live model: the startup config
                    // captured whatever was saved at launch, which can be a
                    // stale/invalid id (every subagent request 400s and the
                    // tool returns no report). Track the picker selection.
                    tools::subagent::set_model(m.d.model_id.value);
                    persist_settings(m);
                    m.ui.effort_dirty = false;
                    // Confirmation toast naming model AND provider — the
                    // same feedback the provider switch gives. Without it a
                    // pick is silent, and when a stale-catalog race (or a
                    // provider the user forgot they were on) is in play,
                    // "model changed but provider didn't" has no on-screen
                    // contradiction the user can catch.
                    m.ui.model_picker = pick::Closed{};
                    auto toast = set_status_toast(m,
                        ui::pretty_model_label(m.d.model_id.value) + " \xc2\xb7 "
                            + provider::provider_display_name(provider::active()),
                        std::chrono::seconds{3});
                    return {std::move(m), std::move(toast)};
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
                    // Persist NOW — a toggle that only reaches disk via some
                    // later select/switch/quit is lost on a crash or kill.
                    persist_settings(m);
                }
            }
            return done(std::move(m));
        },
        [&](ModelPickerCycleEffort& e) -> Step {
            // Step the reasoning-effort tier within what the highlighted
            // model supports (cycle_effort wraps and returns None for a
            // model that can't reason). The new tier takes effect in live
            // state immediately; the DISK persist is deferred to picker
            // close/select (effort_dirty) — persisting here meant a
            // synchronous load+fsync+rename (~5ms on btrfs) per arrow
            // keystroke, which is UI-thread jank under key repeat. The
            // request path re-clamps at send time.
            auto* p = pick::opened(m.ui.model_picker);
            if (p) {
                const auto vis = model_filtered(m.d.available_models, p->query);
                if (!vis.empty() && p->index >= 0
                    && p->index < static_cast<int>(vis.size())) {
                    if (is_chatgpt_active()) {
                        m.d.effort = cycle_codex_effort(m.d.effort, e.delta);
                    } else {
                        const auto caps = ModelCapabilities::from_id(
                            m.d.available_models[
                                static_cast<std::size_t>(vis[static_cast<std::size_t>(p->index)])]
                                .id.value);
                        m.d.effort = cycle_effort(m.d.effort, e.delta, caps);
                    }
                    m.ui.effort_dirty = true;
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
    // The picker's rows are ONE ordered list (presets + ACP agents + saved
    // custom hosts + "Custom host…" sentinel), built once from the current
    // search query. The cursor is an index into THIS list — no offset math,
    // and the same list the view renders (see build_provider_rows).
    const std::string query = [&] {
        const auto* p = pick::opened(m.ui.provider_picker);
        return p ? p->query : std::string{};
    }();
    auto settings = deps().load_settings();
    const std::vector<std::string> saved_custom_hosts =
        provider::saved_custom_hosts(settings.provider_keys);
    const auto rows = ui::build_provider_rows(saved_custom_hosts, query);
    const int n = static_cast<int>(rows.size());

    return std::visit(overload{
        [&](OpenProviderPicker) -> Step {
            // Open at the row matching the currently-active provider. Fresh
            // rows with an empty query (so every provider is present to match).
            const auto fresh = ui::build_provider_rows(saved_custom_hosts, "");
            const auto& sel = provider::active();
            const std::string active_label =
                sel.kind == provider::Kind::ExternalAcp ? sel.acp_agent_id
                : sel.kind == provider::Kind::OpenAI    ? sel.openai_endpoint.label
                : std::string{provider::default_provider_id()};
            int idx = 0;
            for (int i = 0; i < static_cast<int>(fresh.size()); ++i) {
                const auto& row = fresh[static_cast<std::size_t>(i)];
                if (const auto* pr = row.preset(); pr && pr->id == active_label) { idx = i; break; }
                if (const auto* ag = row.acp();    ag && ag->id == active_label) { idx = i; break; }
                if (const auto* ch = row.custom_host(); ch && *ch == active_label) { idx = i; break; }
            }
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
        [&](ProviderPickerFilterInput& e) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p) return done(std::move(m));
            // Append the typed codepoint (UTF-8) and reset the cursor to the
            // top of the freshly-narrowed list.
            char32_t cp = e.codepoint;
            if (cp < 0x80) { p->query.push_back(static_cast<char>(cp)); }
            else {
                // Minimal UTF-8 encode for multibyte input (rare in provider
                // names, but never corrupt the buffer).
                if (cp < 0x800) {
                    p->query.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    p->query.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    p->query.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    p->query.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            p->index = 0;
            return done(std::move(m));
        },
        [&](ProviderPickerFilterBackspace) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p || p->query.empty()) return done(std::move(m));
            // Pop one UTF-8 codepoint (trim continuation bytes then the lead).
            while (!p->query.empty()
                   && (static_cast<unsigned char>(p->query.back()) & 0xC0) == 0x80)
                p->query.pop_back();
            if (!p->query.empty()) p->query.pop_back();
            p->index = 0;
            return done(std::move(m));
        },
        [&](ProviderPickerSelect) -> Step {
            // Capture the cursor before closing: assigning Closed destroys the
            // OpenAt alternative, so keeping a pointer into it would dangle.
            const auto* p = pick::opened(m.ui.provider_picker);
            const int selected = p ? p->index : -1;
            m.ui.provider_picker = pick::Closed{};
            if (selected < 0 || selected >= n) return done(std::move(m));
            const ui::ProviderRow& chosen = rows[static_cast<std::size_t>(selected)];

            // "Custom host…" sentinel: hand off to the free-text endpoint modal.
            if (chosen.is_new_custom_host()) {
                m.ui.login = ui::login::CustomHostInput{};
                return done(std::move(m));
            }

            // An external ACP agent row: agentty drives the agent subprocess,
            // which does its OWN auth — no key resolution here.
            if (const provider::AcpAgentSpec* agent = chosen.acp()) {
                return commit_provider_switch(std::move(m), agent->id,
                                              auth::AuthHeader{}, agent->id);
            }

            // A saved custom OpenAI-compatible host row: the spec string is the
            // key into Settings.provider_keys. Resolve the saved key and commit
            // directly — no re-entry, because the key is already on disk.
            if (const std::string* spec_ptr = chosen.custom_host()) {
                const std::string spec = *spec_ptr;
                std::string saved_key;
                {
                    auto s = deps().load_settings();
                    if (auto it = s.provider_keys.find(spec);
                        it != s.provider_keys.end())
                        saved_key = it->second;
                }
                auth::AuthHeader anthropic_creds = deps().auth;
                if (auto saved = auth::load_credentials())
                    anthropic_creds = auth::make_auth_header(*saved);
                auth::AuthHeader new_auth = provider::resolve_auth_for(
                    spec, anthropic_creds, /*cli_key=*/{}, saved_key);
                return commit_provider_switch(std::move(m), spec,
                                              std::move(new_auth), spec);
            }

            const auto& preset = *chosen.preset();
            const auto& active = provider::active();
            const bool is_active_account_provider =
                (preset.id == "chatgpt" && active.is_chatgpt())
                || (preset.id == "copilot" && active.is_copilot())
                || (preset.id == "kimi" && active.is_kimi())
                || (preset.kind() == provider::Kind::Anthropic
                    && active.kind == provider::Kind::Anthropic);
            if (is_active_account_provider) {
                // Enter on the active OAuth provider drills into its accounts.
                return agentty::app::update(std::move(m), Msg{OpenAccounts{}});
            }

            const std::string spec{preset.id};

            // Resolve the new backend's credentials BEFORE committing so we can
            // refuse a switch that would land the user in a silently-broken
            // state. Pass Anthropic creds loaded FRESH from disk, not
            // deps().auth (which holds the currently-active provider's key).
            auth::AuthHeader anthropic_creds = deps().auth;
            if (auto saved = auth::load_credentials())
                anthropic_creds = auth::make_auth_header(*saved);
            std::string saved_provider_key;
            {
                auto s = deps().load_settings();
                if (auto it = s.provider_keys.find(spec);
                    it != s.provider_keys.end())
                    saved_provider_key = it->second;
            }
            auth::AuthHeader new_auth =
                provider::resolve_auth_for(spec, anthropic_creds,
                                           /*cli_key=*/{}, saved_provider_key);

            // A hosted (non-local) OpenAI-family provider with no resolvable
            // key can't stream. Open the in-app key-entry modal for THIS
            // provider instead of a dead-end error; login_submit commits the
            // switch once the key lands.
            const bool needs_key =
                preset.kind() == provider::Kind::OpenAI && !preset.is_local
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

            // Native device-flow OAuth providers (Copilot, Kimi): if not signed
            // in, launch the device login instead of switching to a backend
            // that would show "not signed in" on the first turn. One helper
            // path for both — see launch_device_login in login.cpp.
            if (spec == "copilot" && !provider::copilot::signed_in()) {
                const auto attempt_id = cmd::next_codex_login_attempt_id();
                auto cancel = std::make_shared<std::atomic_bool>(false);
                m.ui.login = ui::login::DeviceWaiting{
                    .provider = "copilot", .provider_label = "GitHub Copilot",
                    .attempt_id = attempt_id, .cancel = cancel,
                };
                return {std::move(m),
                        cmd::device_login_async("copilot", "GitHub Copilot",
                                                attempt_id, std::move(cancel))};
            }
            if (spec == "kimi" && !provider::kimi::signed_in()) {
                const auto attempt_id = cmd::next_codex_login_attempt_id();
                auto cancel = std::make_shared<std::atomic_bool>(false);
                m.ui.login = ui::login::DeviceWaiting{
                    .provider = "kimi", .provider_label = "Kimi",
                    .attempt_id = attempt_id, .cancel = cancel,
                };
                return {std::move(m),
                        cmd::device_login_async("kimi", "Kimi",
                                                attempt_id, std::move(cancel))};
            }

            // codex-cli / chatgpt authenticates via native ChatGPT OAuth
            // (loopback or device). Launch it if not signed in.
            if ((spec == "chatgpt" || spec == "codex-cli")
                && !provider::chatgpt::responses_available()) {
                const auto attempt_id = cmd::next_codex_login_attempt_id();
                auto cancel = std::make_shared<std::atomic_bool>(false);
                m.ui.login = ui::login::ChatGptWaiting{
                    .attempt_id = attempt_id,
                    .cancel = cancel,
                    .device_auth = provider::chatgpt::codex_device_auth_preferred(),
                };
                return {std::move(m),
                        cmd::codex_login_async(attempt_id, std::move(cancel))};
            }

            // Every entry point funnels the actual switch through the ONE
            // helper so provider + per-provider model recall + effort clamp +
            // auth swap + refetch can never drift between call sites.
            return commit_provider_switch(std::move(m), spec, std::move(new_auth),
                                          std::string{preset.label});
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
            p->confirm_remove.clear();   // moving disarms a pending `d`
            int sz = static_cast<int>(m.d.threads.size());
            p->index = (p->index + e.delta + sz) % sz;
            return done(std::move(m));
        },
        [&](ThreadListJump& e) -> Step {
            if (m.d.threads.empty()) return done(std::move(m));
            auto* p = pick::opened(m.ui.thread_list);
            if (!p) return done(std::move(m));
            p->confirm_remove.clear();   // jumping disarms a pending `d`
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
            if (p) p->confirm_remove.clear();   // selecting disarms a pending `d`
            if (p && !m.d.threads.empty() && !m.s.thread_loading) {
                // Re-clamp: p->index can be stale if an async refresh shrank
                // the list since the last navigation (see ThreadListDelete).
                p->index = std::clamp(p->index, 0,
                                      static_cast<int>(m.d.threads.size()) - 1);
                const Thread& meta = m.d.threads[static_cast<std::size_t>(p->index)];
                // Same-thread re-select — closing the picker is the
                // only useful action. No async load: would just
                // reparse the same bytes and flash.
                if (meta.id == m.d.current.id) {
                    m.ui.thread_list = pick::Closed{};
                    return done(std::move(m));
                }
                m.s.thread_loading = true;
                // Warm the socket now so the first turn in the thread the user
                // is switching INTO doesn't re-pay the handshake (the pool's
                // idle TTL has usually evicted it during composer breathing
                // room). Non-blocking; no-op if already warm.
                provider::prewarm_active_provider();
                cmd = cmd::load_thread_async(meta.id);
            }
            m.ui.thread_list = pick::Closed{};
            return {std::move(m), std::move(cmd)};
        },
        [&](ThreadListDelete) -> Step {
            // `d` / `D` in the thread picker — two-press delete with
            // confirm_remove, mirroring SettingsListRemove / AccountRemove.
            // First press on a row marks it pending (⚠ badge in the view);
            // second press on the SAME row commits via deps().delete_thread().
            // Any move/jump/select/new/close disarms the pending state.
            auto* p = pick::opened(m.ui.thread_list);
            if (!p || m.d.threads.empty()) return done(std::move(m));
            // Bounds-guard the cursor before indexing. Navigation handlers
            // clamp p->index on every move, but the thread list can be
            // mutated out from under the picker by an async refresh (or a
            // prior delete) that shrinks it, leaving a stale index that
            // points past the new end. Reading m.d.threads[idx] then is an
            // out-of-bounds access; the erase(begin()+idx) below would
            // compound it. Re-clamp into range instead of trusting p->index.
            const int sz_now = static_cast<int>(m.d.threads.size());
            const int idx = std::clamp(p->index, 0, sz_now - 1);
            p->index = idx;
            const Thread& target = m.d.threads[static_cast<std::size_t>(idx)];
            // Use the thread id as the confirm key — stable across title edits.
            const std::string key = target.id.value;
            if (p->confirm_remove != key) {
                p->confirm_remove = key;
                return done(std::move(m));
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
            deps().delete_thread(target_id);
            m.d.threads.erase(m.d.threads.begin() + idx);
            // Clamp the cursor so it stays valid after removal.
            const int sz = static_cast<int>(m.d.threads.size());
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
                return {std::move(m),
                        Cmd<Msg>::batch(cmd::load_threads_async(),
                                        std::move(reset), std::move(toast))};
            }
            return {std::move(m), std::move(toast)};
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
            // Warm the socket for the switched-into thread's first turn.
            provider::prewarm_active_provider();
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
            // Persist the outgoing thread before we drop it (delete's
            // active-row path does the opposite — it just removed the
            // thread, so it must NOT save). The shared reset below owns
            // everything after this policy decision.
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            auto reset = reset_to_fresh_thread(m);
            return {std::move(m), std::move(reset)};
        },
        [&](ThreadsLoaded& e) -> Step {
            m.d.threads = std::move(e.threads);
            m.s.threads_loading = false;
            // If the thread picker is open, its cursor may now point past the
            // end of the freshly-loaded (possibly shorter) list. Re-clamp so
            // the view and every ThreadList* handler index safely.
            if (auto* p = pick::opened(m.ui.thread_list)) {
                const int sz = static_cast<int>(m.d.threads.size());
                p->index = sz > 0 ? std::clamp(p->index, 0, sz - 1) : 0;
            }
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
            // Arm the one-shot post-paint trim: the rehydrate budget used
            // ESTIMATED heights; the first paint records real ones into the
            // ledger, and the Tick arm re-trims against those. The Tick
            // subscription gates on this flag (subscribe.cpp) until it fires.
            m.ui.pending_rehydrate_trim = !m.ui.frozen.empty();
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
