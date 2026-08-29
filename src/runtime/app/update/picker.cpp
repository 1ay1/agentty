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
#include "agentty/provider/auth_state.hpp"
#include "agentty/provider/acp_agents.hpp"
#include "agentty/provider/selection.hpp"
#include "agentty/auth/auth.hpp"
#include "agentty/runtime/login.hpp"
#include "agentty/runtime/fused_models.hpp"
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
    // Smart-Mode per-THREAD routing state must not leak into the new thread:
    // complexity momentum (classify_score_with_context inherits a tier from
    // the PREVIOUS turn), the session cascade bias, and the last turn's
    // signature (outcome feedback would otherwise attribute the new thread's
    // first reply to the OLD thread's route). The learned per-workspace
    // priors (RoutingMemory) survive by design — they are cross-thread.
    m.s.smart_turn_complexity  = smart::Complexity::Standard;
    m.s.smart_effort_bias      = 0;
    m.s.smart_turn_signature.clear();
    m.s.smart_turn_had_failure = false;
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
            // Close the provider picker if the user cross-hopped here from it
            // (^/ in the provider picker). pick_overlay + the key dispatcher
            // both check model_picker BEFORE provider_picker, so a lingering
            // open provider_picker would render/eat keys under this one.
            m.ui.provider_picker = pick::Closed{};
            // Same for the fused picker: ^/ cycles fused → this classic
            // (single-provider) picker, so tear the fused one down cleanly
            // — release its row cache and flush any pending effort edit.
            if (pick::is_open(m.ui.fused_picker)) {
                m.ui.fused_picker = pick::Closed{};
                m.d.fused_rows.clear();
                if (m.ui.effort_dirty) { persist_settings(m); m.ui.effort_dirty = false; }
            }
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
            // PERSIST-ON-SUCCESS: a custom --provider spec registered at
            // startup as unproven becomes sticky NOW — the host answered a
            // non-empty model fetch, so it's a real endpoint, not a typo.
            // Presets persisted at parse time as always; this only fires
            // for raw host/URL specs, at most once per process.
            if (auto proven = provider::take_unproven_spec(
                    active_provider_id())) {
                settings.provider = proven->first;
                if (!proven->second.empty())
                    settings.provider_models[proven->first] = proven->second;
                deps().save_settings(settings);
            }
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
                        m.d.effort, resolved_caps(m.d.model_id.value));
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
                            m.d.effort, resolved_caps(m.d.model_id.value));
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
            // model supports. The ladder is ONE thing: cycle_effort walks the
            // model's catalog-declared levels (resolved_caps → supports_effort
            // / _xhigh / _max), wrapping and returning None for a model that
            // can't reason. This is uniform across EVERY provider — including
            // ChatGPT/Codex, whose gpt-5* models decode to Family::Gpt with the
            // correct low..xhigh(..max) ladder — so the picker never offers a
            // level the model won't accept, and the chip/footer/wire all read
            // the same source. The new tier takes effect in live state
            // immediately; the DISK persist is deferred to picker close/select
            // (effort_dirty) — persisting per keystroke was UI-thread jank under
            // key repeat. The request path re-clamps at send time.
            auto* p = pick::opened(m.ui.model_picker);
            if (p) {
                const auto vis = model_filtered(m.d.available_models, p->query);
                if (!vis.empty() && p->index >= 0
                    && p->index < static_cast<int>(vis.size())) {
                    const auto caps = resolved_caps(
                        m.d.available_models[
                            static_cast<std::size_t>(vis[static_cast<std::size_t>(p->index)])]
                            .id.value);
                    m.d.effort = cycle_effort(m.d.effort, e.delta, caps);
                    m.ui.effort_dirty = true;
                }
            }
            return done(std::move(m));
        },
        [&](ModelPickerToggleReasoning&) -> Step {
            // Cycle the highlighted model's per-model reasoning-effort
            // override: inference → force-on → force-off → inference. Claude/
            // GPT are family-gated (their effort ladder isn't user-editable),
            // so this is a no-op with a hint for them. Persisted immediately
            // (an explicit config action, not a hot keystroke) and pushed to
            // the catalog registry so resolved_caps() honors it live.
            auto* p = pick::opened(m.ui.model_picker);
            if (!p) return done(std::move(m));
            const auto vis = model_filtered(m.d.available_models, p->query);
            if (vis.empty() || p->index < 0
                || p->index >= static_cast<int>(vis.size()))
                return done(std::move(m));
            const std::string id =
                m.d.available_models[
                    static_cast<std::size_t>(vis[static_cast<std::size_t>(p->index)])]
                    .id.value;
            const auto base = ModelCapabilities::from_id(id);
            if (base.is_known_family()
                || base.family == ModelCapabilities::Family::Gpt) {
                auto toast = set_status_toast(m,
                    "reasoning effort is model-managed here (←/→ to set the tier)");
                return {std::move(m), std::move(toast)};
            }
            // Determine the next state from the CURRENT override (tri-state).
            const int cur = reasoning_override_for(id);   // -1 none, 0 off, 1 on
            auto s = deps().load_settings();
            const char* label = nullptr;
            if (cur < 0) {                 // inference → force ON
                s.reasoning_effort_overrides[id] = true;
                set_reasoning_override(id, true);
                label = "reasoning effort: forced ON for this model";
            } else if (cur == 1) {         // ON → force OFF
                s.reasoning_effort_overrides[id] = false;
                set_reasoning_override(id, false);
                label = "reasoning effort: forced OFF for this model";
            } else {                       // OFF → back to inference (clear)
                s.reasoning_effort_overrides.erase(id);
                clear_reasoning_override(id);
                label = "reasoning effort: auto (catalog default)";
            }
            deps().save_settings(s);
            // If the model just lost effort capability, drop any live tier so
            // the chip doesn't linger; re-clamp against the new caps.
            m.d.effort = clamp_effort(m.d.effort, resolved_caps(id));
            auto toast = set_status_toast(m, label);
            return {std::move(m), std::move(toast)};
        },
        [&](ModelPickerToggleShowReasoning&) -> Step {
            // Flip whether the model's reasoning/thinking is SHOWN. Global (all
            // providers): renders the transcript reasoning block AND makes the
            // Anthropic transport request VISIBLE thinking. Persisted so it
            // survives restarts. Mirrors the ToggleChangesStrip pattern.
            m.d.show_reasoning = !m.d.show_reasoning;
            auto s = deps().load_settings();
            s.show_reasoning = m.d.show_reasoning;
            deps().save_settings(s);
            // Anthropic caveat: visible thinking is only REQUESTED when an
            // effort tier is active (the transport gates thinking mode on
            // req.effort). With effort off, ^R would silently show nothing —
            // tell the user what to flip instead of leaving a dead toggle.
            const auto caps = resolved_caps(m.d.model_id.value);
            const bool claude_no_effort =
                caps.family != ModelCapabilities::Family::Unknown
                && caps.family != ModelCapabilities::Family::Gpt
                && !caps.reasoning_compat
                && m.d.effort == Effort::None;
            auto toast = set_status_toast(m, !m.d.show_reasoning
                ? "reasoning: hidden (existing blocks fold away too)"
                : claude_no_effort
                    ? "reasoning: shown — needs an effort tier on this model "
                      "(\xe2\x86\x90/\xe2\x86\x92 in the picker)"
                    : "reasoning: shown (live thinking + \xe2\x9c\xa6 summary)");
            return {std::move(m), std::move(toast)};
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
            // Close the model picker if the user cross-hopped here from it
            // (^P in the model picker). Without this the model picker stays
            // open and wins pick_overlay's priority order (checked first), so
            // the hop would render nothing new. Flush any pending effort-tier
            // change first — the same persist CloseModelPicker does on Esc,
            // so a hop doesn't silently drop it.
            if (m.ui.effort_dirty) {
                persist_settings(m);
                m.ui.effort_dirty = false;
            }
            // Abandon a pending Smart-Mode slot assignment: hopping away from
            // the model picker mid-assign must not leave the armed slot
            // behind, or the NEXT regular model pick silently lands in the
            // smart slot instead of switching the model.
            m.ui.smart_assign_slot = -1;
            m.ui.model_picker = pick::Closed{};
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
            p->confirm_remove.clear();   // navigating disarms a pending delete
            p->index = (p->index + e.delta + n) % n;
            return done(std::move(m));
        },
        [&](ProviderPickerJump& e) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p || n == 0) return done(std::move(m));
            p->confirm_remove.clear();   // navigating disarms a pending delete
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
        [&](ProviderPickerDelete) -> Step {
            auto* p = pick::opened(m.ui.provider_picker);
            if (!p || p->index < 0 || p->index >= n)
                return done(std::move(m));
            const ui::ProviderRow& row =
                rows[static_cast<std::size_t>(p->index)];
            // Only SAVED CUSTOM HOSTS are user-created and removable. Presets,
            // ACP agents, and the "Custom host…" sentinel are not.
            const std::string* spec = row.custom_host();
            if (!spec) {
                p->confirm_remove.clear();
                return done(std::move(m));
            }
            // Two-press: first press ARMS (marks confirm_remove on this spec),
            // second press on the SAME row COMMITS. Mirrors ThreadListDelete /
            // AccountRemove.
            if (p->confirm_remove != *spec) {
                p->confirm_remove = *spec;
                return done(std::move(m));
            }
            const std::string removed = *spec;
            {
                auto s = deps().load_settings();
                s.provider_keys.erase(removed);    // the saved host lives here
                s.provider_models.erase(removed);  // its remembered model
                deps().save_settings(s);
            }
            p->confirm_remove.clear();
            // Rebuild the row list so the removed host is gone; clamp cursor.
            auto s2 = deps().load_settings();
            const auto fresh = ui::build_provider_rows(
                provider::saved_custom_hosts(s2.provider_keys), p->query);
            if (!fresh.empty() && p->index >= static_cast<int>(fresh.size()))
                p->index = static_cast<int>(fresh.size()) - 1;
            auto toast = set_status_toast(m, "removed custom host: " + removed);
            return {std::move(m), std::move(toast)};
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
                ui::login::CustomHostInput ch;
                ch.back = ui::login::Back::ProviderPicker;  // Esc = one step back
                m.ui.login = std::move(ch);
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
                    .back           = ui::login::Back::ProviderPicker,
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

// ── Fused cross-provider model picker ────────────────────────────────────
namespace {

constexpr int kRecentCap = 6;

// Record (provider,model) at the FRONT of the MRU, deduped, capped. Persists
// to Settings.recent_models so RECENT + ^Tab survive restart. Mirrors into
// m.d.recent_models for the live picker build.
void record_recent(Model& m, const std::string& provider_id,
                   const std::string& model_id) {
    if (provider_id.empty() || model_id.empty()) return;
    ModelRef ref{provider_id, model_id};
    auto& mru = m.d.recent_models;
    std::erase(mru, ref);
    mru.insert(mru.begin(), ref);
    if (static_cast<int>(mru.size()) > kRecentCap) mru.resize(kRecentCap);

    auto s = deps().load_settings();
    s.recent_models.clear();
    for (const auto& r : mru)
        s.recent_models.push_back(r.provider_id + "\t" + r.model_id);
    deps().save_settings(s);
}

// Hydrate m.d.recent_models from Settings ("<provider>\t<model>" per entry).
void hydrate_recents(Model& m) {
    if (!m.d.recent_models.empty()) return;
    auto s = deps().load_settings();
    for (const auto& e : s.recent_models) {
        auto tab = e.find('\t');
        if (tab == std::string::npos) continue;
        m.d.recent_models.push_back(
            ModelRef{e.substr(0, tab), e.substr(tab + 1)});
    }
}

// Refresh the picker's SOURCES into the Model — a CHEAP, in-memory-only pass
// (one settings read + provider enumeration + stat-cached auth checks), run
// when the picker opens. It does NOT touch the network or build any provider's
// model list: the active provider is seeded from the catalog already in hand
// (available_models), every other authed provider gets an empty Loading entry
// that the async fetch (fetch_models_for) fills in a frame or two later. This
// is what keeps open INSTANT even with slow backends (Ollama / custom hosts
// whose list probe would otherwise block the UI thread for seconds).
void refresh_fused_sources(Model& m) {
    const auto settings = deps().load_settings();
    const std::string active_pid = active_provider_id();

    auto find_cat = [&](std::string_view id) -> ProviderCatalog* {
        for (auto& c : m.d.provider_catalogs)
            if (c.provider_id == id) return &c;
        return nullptr;
    };
    m.d.fused_offers.clear();

    for (const auto& p : provider::providers()) {
        const std::string id{p.id};
        if (!provider::provider_is_authed(p, settings)) {
            m.d.fused_offers.push_back(SigninOffer{id, std::string{p.label}});
            continue;
        }
        ProviderCatalog* c = find_cat(id);
        if (!c) {
            m.d.provider_catalogs.push_back(ProviderCatalog{
                id, std::string{p.label}, ProviderCatalog::State::Idle, {}, {}});
            c = &m.d.provider_catalogs.back();
        }
        // Active provider: MIRROR the live catalog we already hold
        // (available_models is the SSOT for the active provider). Re-seed on
        // EVERY refresh, not just when empty — otherwise the fused catalog
        // freezes on whatever available_models was at the FIRST open (often
        // the bundled seed, before the live /v1/models fetch landed), and
        // then diverges from the old model picker as available_models grows
        // (e.g. a newly-listed flagship never appears in the fused list).
        // Everyone else stays empty + Idle so Open fires a background fetch;
        // the row list simply grows as each resolves.
        if (id == active_pid && !m.d.available_models.empty()) {
            if (c->models != m.d.available_models) {
                c->models = m.d.available_models;
                c->search_keys.clear();     // model set changed — keys stale
            }
            c->state = ProviderCatalog::State::Ready;
        }
    }
}

} // namespace

// Shared builder used by BOTH this reducer and the fused_picker view, so the
// row list they act on can never disagree (SSOT). Declared in internal.hpp.
// PURE: builds only from the already-refreshed sources (provider_catalogs +
// fused_offers) + the live query — no disk, no enumeration, so it is cheap
// enough to run on every keystroke.
std::vector<FusedRow> fused_rows_for_model(const Model& m) {
    ui::FusedInputs in;
    in.catalogs   = &m.d.provider_catalogs;
    in.offers     = &m.d.fused_offers;
    in.recents    = &m.d.recent_models;
    in.active     = ModelRef{active_provider_id(), m.d.model_id.value};
    in.recent_cap = kRecentCap;
    if (auto* c = pick::opened(m.ui.fused_picker)) in.query = c->query;
    return ui::build_fused_rows(in);
}

namespace {

// Rebuild the cached row list into m.d.fused_rows. Called by the reducer ONLY
// at the points its inputs change (open / filter / catalog-loaded / favorite)
// so the view + cursor math never re-enumerate providers or re-read
// settings.json per frame or per keystroke.
void rebuild_fused_rows(Model& m) {
    // Keep each catalog's precomputed, lowercased fuzzy keys in sync with its
    // model set. This is the SINGLE place the filter consumes catalogs, so
    // keys built here are reused across every keystroke — the per-key filter
    // never re-allocates or re-lowercases a haystack per model. A model-set
    // change clears search_keys at the mutation site (size mismatch here), so
    // this rebuild runs O(models) only when a catalog actually changed.
    for (auto& c : m.d.provider_catalogs) {
        if (c.search_keys.size() == c.models.size()) continue;
        c.search_keys.clear();
        c.search_keys.reserve(c.models.size());
        for (const auto& mi : c.models) {
            std::string key = ui::detail::fused_haystack(c.label, mi);
            for (char& ch : key)
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            c.search_keys.push_back(std::move(key));
        }
    }
    m.d.fused_rows = fused_rows_for_model(m);
}

// Resolve the AuthHeader for switching to `spec` (an ALREADY-AUTHED provider,
// since the fused picker only surfaces authed rows). Delegates to the same
// resolver the provider picker uses: Anthropic OAuth/key from disk, hosted
// key from env or saved provider_keys; oauth-native providers ignore it.
auth::AuthHeader resolve_switch_auth(const std::string& spec) {
    auth::AuthHeader anthropic_creds = deps().auth;
    if (auto saved = auth::load_credentials())
        anthropic_creds = auth::make_auth_header(*saved);
    std::string saved_key;
    {
        auto s = deps().load_settings();
        if (auto it = s.provider_keys.find(spec); it != s.provider_keys.end())
            saved_key = it->second;
    }
    return provider::resolve_auth_for(spec, anthropic_creds,
                                      /*cli_key=*/{}, saved_key);
}

// THE atomic switch: make (provider, model) active.
//   • same provider  → change the model in place (effort re-clamp, persist,
//     refetch), no provider hop.
//   • cross provider  → commit_provider_switch with the model PRE-STASHED so
//     the funnel installs exactly it (atomic provider+model+auth).
// Records the target in the MRU either way (unless `record` is false, e.g. a
// ^Tab ring-walk which must not reorder the MRU mid-cycle), and fires the
// switch toast.
Step switch_to_model_ref(Model m, const ModelRef& ref, bool record = true) {
    const std::string cur_pid = active_provider_id();

    if (ref.provider_id == cur_pid) {
        // Same provider — pure model change.
        m.d.model_id    = ModelId{ref.model_id};
        m.s.context_max = ui::context_max_for_model(m.d.model_id.value);
        for (const auto& mi : m.d.available_models)
            if (mi.id == m.d.model_id && mi.context_window > 0) {
                m.s.context_max = mi.context_window; break;
            }
        if (!is_chatgpt_active())
            m.d.effort = clamp_effort(m.d.effort,
                                      resolved_caps(m.d.model_id.value));
        tools::subagent::set_model(m.d.model_id.value);
        persist_settings(m);
        if (record) record_recent(m, ref.provider_id, ref.model_id);
        auto toast = set_status_toast(m,
            ui::pretty_model_label(m.d.model_id.value) + " \xc2\xb7 "
                + provider::provider_display_name(provider::active()),
            std::chrono::seconds{3});
        return {std::move(m), std::move(toast)};
    }

    // Cross-provider — atomic switch through the ONE funnel, model pre-stashed.
    const provider::ProviderPreset* p = provider::preset_for(ref.provider_id);
    const std::string label = p ? std::string{p->label} : ref.provider_id;
    auth::AuthHeader auth = resolve_switch_auth(ref.provider_id);
    if (record) record_recent(m, ref.provider_id, ref.model_id);
    return commit_provider_switch(std::move(m), ref.provider_id,
                                  std::move(auth), label, ref.model_id);
}

// Route to the login flow for `provider_id`, returning to `back` after auth.
// Used by a fused sign-in offer (un-authed provider row). Mirrors the entry
// points ProviderPickerSelect uses for each auth style.
Step open_login_for(Model m, const std::string& provider_id,
                    const std::string& label, ui::login::Back back) {
    const provider::ProviderPreset* p = provider::preset_for(provider_id);
    if (p && p->oauth_native) {
        // ChatGPT/Copilot/Kimi: OAuth device/browser flow via the method menu
        // scoped to this provider.
        ui::login::Picking pk;
        pk.provider = provider_id;
        pk.back = back;
        m.ui.login = std::move(pk);
        return {std::move(m), maya::Cmd<Msg>::none()};
    }
    // Hosted API-key (or Anthropic key): the API-key input, returning to the
    // fused picker on success.
    m.ui.login = ui::login::ApiKeyInput{
        .provider       = provider_id,
        .provider_label = label,
        .back           = back,
    };
    return {std::move(m), maya::Cmd<Msg>::none()};
}

} // namespace

Step fused_picker_update(Model m, msg::FusedPickerMsg pm) {
    using namespace agentty::msg;
    auto done = [](Model mm) -> Step { return {std::move(mm), maya::Cmd<Msg>::none()}; };

    // Clamp the cursor to the current row count after any list change.
    auto clamp_cursor = [](Model& mm) {
        if (auto* c = pick::opened(mm.ui.fused_picker)) {
            const int n = static_cast<int>(mm.d.fused_rows.size());
            if (n == 0) { c->index = 0; return; }
            if (c->index < 0)  c->index = 0;
            if (c->index >= n) c->index = n - 1;
        }
    };

    return std::visit(overload{
        [&](OpenFusedPicker) -> Step {
            hydrate_recents(m);
            // ^/ TOGGLES from the classic single-provider picker back to this
            // one. Tear the classic picker down cleanly — flush a pending
            // effort edit and abandon any Smart Mode slot-assign arming (the
            // fused picker doesn't assign slots).
            if (pick::is_open(m.ui.model_picker)) {
                m.ui.model_picker = pick::Closed{};
                m.ui.smart_assign_slot = -1;
                if (m.ui.effort_dirty) { persist_settings(m); m.ui.effort_dirty = false; }
            }
            m.ui.fused_picker = pick::OpenAt{0, ""};
            // ONE expensive pass: enumerate providers, read settings, seed
            // every authed provider's catalog from its bundled list so the
            // picker opens instantly full.
            refresh_fused_sources(m);
            // Background-refresh each authed provider with its LIVE catalog
            // (concurrent, non-blocking); the seeded list shows meanwhile.
            std::vector<maya::Cmd<Msg>> fetches;
            for (auto& c : m.d.provider_catalogs) {
                if (c.state == ProviderCatalog::State::Ready) continue;
                c.state = ProviderCatalog::State::Loading;
                fetches.push_back(cmd::fetch_models_for(c.provider_id));
            }
            rebuild_fused_rows(m);       // seed the cache the view reads
            return {std::move(m), maya::Cmd<Msg>::batch(std::move(fetches))};
        },
        [&](CloseFusedPicker) -> Step {
            m.ui.fused_picker = pick::Closed{};
            m.d.fused_rows.clear();       // release the cache while closed
            if (m.ui.effort_dirty) { persist_settings(m); m.ui.effort_dirty = false; }
            return done(std::move(m));
        },
        [&](FusedPickerMove e) -> Step {
            if (auto* c = pick::opened(m.ui.fused_picker)) {
                c->index += e.delta;
                c->staged_effort = -1;   // staged tier belonged to the old row
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](FusedPickerJump e) -> Step {
            if (auto* c = pick::opened(m.ui.fused_picker)) {
                const int n = static_cast<int>(m.d.fused_rows.size());
                // Page by a full viewport so PageUp/Down lands a screen away
                // (matches the classic pickers' kPage), not a fixed 10 that
                // under-shoots the ~14-row viewport.
                constexpr int page = 14;  // matches kViewportH in pickers.cpp
                using W = FusedPickerJump::Where;
                switch (e.where) {
                    case W::Home:     c->index = 0; break;
                    case W::End:      c->index = n - 1; break;
                    case W::PageUp:   c->index -= page; break;
                    case W::PageDown: c->index += page; break;
                }
                c->staged_effort = -1;
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](FusedPickerFilterInput e) -> Step {
            if (auto* c = pick::opened(m.ui.fused_picker)) {
                // Quick-select: on the UNFILTERED list (empty query), a digit
                // 1-9 jumps straight to the Nth row — the top rows are RECENT,
                // so "2" hits the 2nd model you alternate with, no arrowing.
                // Gated on an empty query so typing an id with digits ("o3",
                // "gpt5") still searches normally.
                if (c->query.empty() && e.ch >= '1' && e.ch <= '9') {
                    const int n = static_cast<int>(m.d.fused_rows.size());
                    const int want = e.ch - '1';
                    if (want < n) {
                        c->index = want;
                        c->staged_effort = -1;
                        clamp_cursor(m);
                    }
                    return done(std::move(m));
                }
                // ASCII only — model/provider ids are ASCII in practice.
                if (e.ch >= 0x20 && e.ch < 0x7f) {
                    c->query.push_back(static_cast<char>(e.ch));
                    c->index = 0;
                    c->staged_effort = -1;
                    rebuild_fused_rows(m);   // query changed → rows changed
                    clamp_cursor(m);
                }
            }
            return done(std::move(m));
        },
        [&](FusedPickerFilterBackspace) -> Step {
            if (auto* c = pick::opened(m.ui.fused_picker); c && !c->query.empty()) {
                c->query.pop_back();
                c->index = 0;
                c->staged_effort = -1;
                rebuild_fused_rows(m);
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](FusedCatalogLoaded e) -> Step {
            // Merge in place, guarded by provider_id (a provider signed out
            // mid-fetch is simply not in the list anymore).
            for (auto& c : m.d.provider_catalogs) {
                if (c.provider_id != e.provider_id) continue;
                if (e.ok && !e.models.empty()) {
                    c.models = std::move(e.models);
                    c.search_keys.clear();   // stale — rebuilt on next filter
                    c.state  = ProviderCatalog::State::Ready;
                } else {
                    c.state  = e.ok ? ProviderCatalog::State::Ready
                                    : ProviderCatalog::State::Failed;
                }
                break;
            }
            // Only the fused picker's own list depends on the merged catalogs;
            // rebuild it (cheap, once per resolving provider) if it's open.
            if (pick::is_open(m.ui.fused_picker)) {
                rebuild_fused_rows(m);
                clamp_cursor(m);
            }
            return done(std::move(m));
        },
        [&](FusedPickerToggleFavorite) -> Step {
            auto* c = pick::opened(m.ui.fused_picker);
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            auto s = deps().load_settings();
            ModelId mid = row.model.id;
            auto it = std::find(s.favorite_models.begin(),
                                s.favorite_models.end(), mid);
            const bool now_fav = (it == s.favorite_models.end());
            if (now_fav) s.favorite_models.push_back(mid);
            else         s.favorite_models.erase(it);
            deps().save_settings(s);
            // Live feedback: flip the star on every cached row for this model
            // (no re-sort — keep the cursor where it is).
            for (auto& r : m.d.fused_rows)
                if (r.model.id == mid) r.model.favorite = now_fav;
            return done(std::move(m));
        },
        [&](FusedPickerCycleEffort e) -> Step {
            // ←/→ walks the reasoning-effort ladder of the HIGHLIGHTED model
            // (off → low → medium → high … within what its caps allow). The
            // edit is STAGED on the picker row (c->staged_effort), NOT written
            // to the global m.d.effort — mutating the global mid-browse leaks
            // onto the currently-active model (a different provider/model you
            // never selected) and reverts nothing on Esc. The staged tier is
            // applied to m.d.effort only on select (FusedPickerSelect).
            auto* c = pick::opened(m.ui.fused_picker);
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            const auto caps = resolved_caps(row.model.id.value);
            if (!effort_capable(caps)) return done(std::move(m));
            // Base = the tier already staged for this row, else the highlighted
            // model's EFFECTIVE tier (global, clamped to its caps) — so the
            // first ←/→ steps from what the chip currently shows.
            const Effort base = c->staged_effort >= 0
                ? static_cast<Effort>(c->staged_effort)
                : clamp_effort(m.d.effort, caps);
            c->staged_effort =
                static_cast<int>(cycle_effort(base, e.delta, caps));
            return done(std::move(m));
        },
        [&](FusedPickerToggleReasoning) -> Step {
            // ^E flips the highlighted model's per-model reasoning OVERRIDE
            // through its tri-state (auto → ON → OFF → auto). Mirrors the
            // model picker so tuning survives the move to the fused surface.
            auto* c = pick::opened(m.ui.fused_picker);
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const auto& row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            if (row.is_signin_offer()) return done(std::move(m));
            const std::string id = row.model.id.value;
            const int cur = reasoning_override_for(id);   // -1 auto, 0 off, 1 on
            auto s = deps().load_settings();
            const char* label = nullptr;
            if (cur < 0) {
                s.reasoning_effort_overrides[id] = true;
                set_reasoning_override(id, true);
                label = "reasoning: forced ON for this model";
            } else if (cur == 1) {
                s.reasoning_effort_overrides[id] = false;
                set_reasoning_override(id, false);
                label = "reasoning: forced OFF for this model";
            } else {
                s.reasoning_effort_overrides.erase(id);
                clear_reasoning_override(id);
                label = "reasoning: auto (catalog default)";
            }
            deps().save_settings(s);
            m.d.effort = clamp_effort(m.d.effort, resolved_caps(id));
            auto toast = set_status_toast(m, label);
            return {std::move(m), std::move(toast)};
        },
        [&](SwitchToPreviousModel) -> Step {
            // ^Tab MRU cycle: walk the recent ring to progressively OLDER
            // models — A → B → C → D → A — not a single A↔B toggle. The switch
            // does NOT reorder the ring (record=false), so finding the active
            // model's index each press naturally advances the walk one step;
            // a full lap returns you home. (A TUI can't see Ctrl release, and
            // there's no idle tick, so a stable no-reorder walk is the robust
            // way to do Alt-Tab semantics here — no commit deadline needed.)
            hydrate_recents(m);
            const auto& ring = m.d.recent_models;
            if (ring.size() < 2) return done(std::move(m));  // nothing to cycle
            const ModelRef active{active_provider_id(), m.d.model_id.value};
            int cur = 0;
            for (int i = 0; i < static_cast<int>(ring.size()); ++i)
                if (ring[static_cast<std::size_t>(i)] == active) { cur = i; break; }
            const ModelRef target =
                ring[static_cast<std::size_t>((cur + 1) % static_cast<int>(ring.size()))];
            if (target.empty() || target == active) return done(std::move(m));
            return switch_to_model_ref(std::move(m), target, /*record=*/false);
        },
        [&](FusedPickerSelect) -> Step {
            auto* c = pick::opened(m.ui.fused_picker);
            if (!c || c->index < 0
                || c->index >= static_cast<int>(m.d.fused_rows.size()))
                return done(std::move(m));
            const FusedRow row = m.d.fused_rows[static_cast<std::size_t>(c->index)];
            const int staged = c->staged_effort;   // capture before the reset
            m.ui.fused_picker = pick::Closed{};
            m.d.fused_rows.clear();
            m.ui.effort_dirty = false;   // the switch below persists settings

            if (row.is_signin_offer()) {
                // Route to login for that provider, returning here after.
                return open_login_for(std::move(m), row.provider_id,
                                      row.label, ui::login::Back::FusedPicker);
            }
            // Commit the row's STAGED effort (←/→) now, clamped to the target
            // model's caps. Staging kept it off the active model during
            // browse; the switch below persists settings, so it sticks.
            if (staged >= 0) {
                m.d.effort = clamp_effort(static_cast<Effort>(staged),
                                          resolved_caps(row.model.id.value));
            }
            return switch_to_model_ref(std::move(m), row.ref());
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
            // Smart-Mode per-thread routing state belongs to the departing
            // thread too — same reset as reset_to_fresh_thread (momentum,
            // cascade bias, outcome-feedback signature). Without it the
            // loaded thread's first turn inherits the OLD thread's tier
            // momentum and its first follow-up trains the old signature.
            m.s.smart_turn_complexity  = smart::Complexity::Standard;
            m.s.smart_effort_bias      = 0;
            m.s.smart_turn_signature.clear();
            m.s.smart_turn_had_failure = false;
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
