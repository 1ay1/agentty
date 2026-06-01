#pragma once
// agentty::app::Program — the maya runtime binding.
//
// Forwards to the per-domain reducer / view / subscribe.  The init function
// reads settings + recent threads through the Store seam.

#include <chrono>
#include <cstdint>

#include <maya/maya.hpp>

#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/app/subscribe.hpp"
#include "agentty/runtime/app/update.hpp"
#include "agentty/runtime/model.hpp"
#include "agentty/runtime/msg.hpp"
#include "agentty/runtime/view/view.hpp"

namespace agentty::app {

[[nodiscard]] std::pair<Model, maya::Cmd<Msg>> init();

struct AgenttyApp {
    using Model = ::agentty::Model;
    using Msg   = ::agentty::Msg;

    static std::pair<Model, maya::Cmd<Msg>> init() { return ::agentty::app::init(); }

    static auto update(Model m, Msg msg) -> std::pair<Model, maya::Cmd<Msg>> {
        return ::agentty::app::update(std::move(m), std::move(msg));
    }

    static maya::Element view(const Model& m) {
        return ::agentty::ui::view(m);
    }

    static auto subscribe(const Model& m) -> maya::Sub<Msg> {
        return ::agentty::app::subscribe(m);
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasVisualHash).
    // The runtime calls this just before view(); when the hash is
    // unchanged from the previous render, view() + render() are skipped
    // entirely. Captures the axes that affect what the user can see;
    // intentionally omits things like Session::last_tick that change
    // every Tick without changing pixels.
    //
    // Time-driven animations (cursor blink, streaming caret pulse,
    // spinner) are bucketed at coarse intervals so each visible step
    // advances the hash. The bucket size is the upper bound on how
    // often we'll render purely for animation.
    static std::uint64_t visual_hash(const Model& m) {
        std::uint64_t k = 1469598103934665603ULL;
        auto mix = [&](std::uint64_t v) {
            k = (k ^ v) * 1099511628211ULL;
        };
        auto mix_str = [&](std::string_view s) {
            mix(s.size());
            // Don't hash every byte for long strings — a 50 KB tool
            // output would dominate the per-frame budget. Sample a
            // few stable offsets; combined with size + render_keys
            // computed elsewhere it's enough to detect change.
            if (!s.empty()) {
                mix(static_cast<std::uint8_t>(s.front()));
                mix(static_cast<std::uint8_t>(s.back()));
                if (s.size() >= 16)
                    mix(static_cast<std::uint8_t>(s[s.size() / 2]));
            }
        };

        // ── Domain.
        //
        // The frozen prefix — messages[0 .. ui.frozen_through) — is
        // an immutable archaeology layer (see m.ui.frozen). Its
        // contribution to the visual is fully captured by the
        // structural pair (frozen.size(), frozen_turn) which advance
        // only at freeze instants. Per-message render_keys for the
        // frozen range cannot change — the with_live_tool gate
        // refuses to mutate them — so iterating them every frame
        // would burn CPU for zero signal. Hash only the live tail.
        mix(m.d.current.messages.size());
        mix(static_cast<std::uint64_t>(m.ui.frozen.size()));
        mix(static_cast<std::uint64_t>(m.ui.frozen_turn));
        for (std::size_t i = m.ui.frozen_through;
             i < m.d.current.messages.size(); ++i) {
            mix(m.d.current.messages[i].compute_render_key());
        }
        mix(static_cast<std::uint64_t>(m.d.profile));
        mix_str(m.d.model_id.value);
        mix(m.d.pending_permission ? 1ULL : 0ULL);

        // ── Session / phase.
        mix(static_cast<std::uint64_t>(m.s.phase.index()));
        mix_str(m.s.status);
        // Status expiry: bucket at 100 ms so the gauge’s rolling
        // counter doesn't force a render every microsecond.
        if (m.s.status_until.time_since_epoch().count() != 0) {
            const auto until_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    m.s.status_until.time_since_epoch()).count();
            mix(static_cast<std::uint64_t>(until_ms / 100));
        }
        // Spinner frame bucketed at 10 (its cycle length). Same bucket
        // size the turn-level AgentTimeline cache uses, so a hash
        // advance here corresponds to a new visual.
        if (m.s.active()) {
            mix(static_cast<std::uint64_t>(m.s.spinner.frame_index() % 10));
        }

        // ── Composer / UI.
        mix_str(m.ui.composer.text);
        mix(static_cast<std::uint64_t>(m.ui.composer.cursor));
        mix(m.ui.composer.attachments.size());
        mix(m.ui.composer.queued.size());
        mix(m.ui.composer.expanded ? 1ULL : 0ULL);

        // Time-driven animation buckets. Each bucket flip forces a
        // render via hash advance. The bucket size is the FLOOR on
        // how often we'll re-render purely for animation; the actual
        // wake-up cadence is driven by request_animation_frame()
        // deadlines, so a tight bucket here costs nothing when no
        // animation is live.
        //
        // CRITICAL for long-thread idle cost: the time bucket must be
        // gated on whether anything is ACTUALLY animating. The composer
        // requests an animation frame every ~33 ms while idle to drive
        // the cursor blink, so the run loop wakes ~30x/s even when the
        // user is just sitting there reading. If we mixed a 33 ms
        // bucket unconditionally, the visual-hash gate would NEVER skip
        // a frame at idle and we'd pay a full view() + render() (and
        // maya's O(rows x width) clear + witness) 30x/s forever — the
        // dominant idle cost on a long thread. Instead:
        //
        //   • Streaming / spinner / welcome wordmark are live, fast
        //     motion — bucket at 33 ms (30 fps) so each visible step
        //     advances the hash.
        //   • Otherwise the only idle animation is the 530 ms composer
        //     cursor blink. Bucket at 265 ms (half the blink period) so
        //     the two blink half-phases land in different buckets and
        //     the toggle is still captured — but idle re-renders drop
        //     from ~30/s to ~4/s, each one corresponding to a real
        //     cursor toggle rather than a no-op repaint.
        const bool fast_anim =
            m.s.active() || m.d.current.messages.empty();
        const std::uint64_t bucket_ms = fast_anim ? 33ULL : 265ULL;
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        mix(static_cast<std::uint64_t>(now_ms) / bucket_ms);

        return k;
    }

    // Optional Program hook (see maya/app/app.hpp — detail::HasNeedsWarmup).
    // Returns true when the next view() result contains a freshly
    // rehydrated frozen scrollback whose cells haven't been captured
    // into maya's component cache yet. The runtime fires a one-shot
    // off-wire warmup_render() before the wire-bound render, which
    // converts the user-visible first frame from O(content) to O(blit)
    // — typically 50–660 ms to <1 ms on tool-heavy thread resume.
    //
    // The flag is set in the reducer (e.g. ThreadLoaded handler in
    // picker.cpp). Maya's loop edge-detects it (rising-edge fires
    // warmup, falling-edge resets the latch), so it's safe to leave
    // the flag set across subsequent reducer steps; we just won't
    // re-warm until it goes false then true again.
    static bool needs_warmup(const Model& m) {
        return m.ui.needs_warmup_render;
    }
};

static_assert(maya::Program<AgenttyApp>);

} // namespace agentty::app
