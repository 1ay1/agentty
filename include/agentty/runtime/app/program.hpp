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
    //
    // ENFORCED CONTRACT — this is NOT a place to rely on care alone.
    // tests/visual_hash_coverage_test.cpp holds two declarative
    // tables: every view-affecting model axis must advance this hash,
    // and every non-visual axis (last_tick, token counters) must NOT.
    // If you add a `mix()` for a new field, add a matching row to that
    // test's kVisualAxes; if you add ephemeral state the view ignores,
    // add it to kInvariantAxes. Forgetting to mix a view-axis is a
    // SILENT dead region in production — the test converts it to a
    // loud CI failure.
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

        // ── Modal / picker state.
        //
        // CRITICAL: every modal's open/closed state AND its in-modal
        // cursor/query must feed the hash. These pickers are
        // selection-driven — pressing ↑/↓ only mutates an index (or a
        // query string) inside the variant; nothing else in the model
        // moves. If that index isn't hashed, ModelPickerMove produces a
        // model the gate considers visually identical, skip_render fires,
        // and the cursor doesn't repaint until some OTHER hashed axis
        // (caret-blink parity, status text) happens to flip ~265 ms later.
        // Symptom: "press 4-5 times, registers once."
        //
        // variant::index() captures Closed-vs-Open (and login's six
        // sub-states); the OpenAt index / palette query+index capture the
        // cursor movement within an open picker.
        mix(static_cast<std::uint64_t>(m.ui.model_picker.index()));
        mix(static_cast<std::uint64_t>(ui::pick::index_or(m.ui.model_picker)));
        mix(static_cast<std::uint64_t>(m.ui.provider_picker.index()));
        mix(static_cast<std::uint64_t>(ui::pick::index_or(m.ui.provider_picker)));
        mix(static_cast<std::uint64_t>(m.ui.thread_list.index()));
        mix(static_cast<std::uint64_t>(ui::pick::index_or(m.ui.thread_list)));
        mix(static_cast<std::uint64_t>(m.ui.diff_review.index()));
        if (auto* c = ui::pick::opened(m.ui.diff_review)) {
            mix(static_cast<std::uint64_t>(c->file_index));
            mix(static_cast<std::uint64_t>(c->hunk_index));
        }
        mix(static_cast<std::uint64_t>(m.ui.command_palette.index()));
        if (auto* o = opened(m.ui.command_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.mention_palette.index()));
        if (auto* o = mention_opened(m.ui.mention_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.symbol_palette.index()));
        if (auto* o = symbol_palette_opened(m.ui.symbol_palette)) {
            mix_str(o->query);
            mix(static_cast<std::uint64_t>(o->index));
        }
        mix(static_cast<std::uint64_t>(m.ui.todo.open.index()));
        mix(static_cast<std::uint64_t>(m.ui.login.index()));

        // Time-driven animation buckets. Each bucket flip forces a
        // render via hash advance. The bucket size is the FLOOR on
        // how often we'll re-render purely for animation; the actual
        // wake-up cadence is driven by the widget frame-request
        // scheduler (maya request_animation_frame() — the single
        // animation clock), so a tight bucket here costs nothing when
        // no animation is live.
        //
        // CRITICAL: the hash bucket must be PHASE-LOCKED to whatever
        // animation is actually on screen, otherwise the two beat
        // against each other and frames get skipped — the symptom is a
        // caret that blinks smoothly sometimes and freezes-until-
        // keypress other times. The render gate (skip_render when the
        // hash is unchanged) means: if the hash doesn't advance on the
        // exact frame an animation toggles, the widget's build() never
        // runs, so its request_animation_frame() is never re-armed, the
        // frame request is never re-issued, and the loop sleeps the
        // full idle timeout until the next keypress. So the bucket has
        // to step once per visible animation transition, no faster, no
        // slower.
        //
        // Three regimes:
        //   (a) fine animation live (spinner / streaming caret / welcome
        //       bob / queued-chip pulse): step at the SAME cadence the Tick
        //       subscription wakes the loop. On DEC-2026 terminals that's
        //       33 ms (~30 fps) and the synchronized-output wrapper makes
        //       each frame swap atomically — smooth, no tearing. On
        //       terminals WITHOUT mode 2026 (Apple Terminal, plain xterm,
        //       tmux without sync passthrough) every multi-row repaint
        //       paints progressively, so the chrome at the bottom of the
        //       frame (spinner / sparkline / status bar) visibly tears on
        //       each render. There the Tick already drops to 100 ms
        //       (subscribe.cpp), but this bucket must MATCH it: a 33 ms
        //       bucket against a 100 ms tick advances ~3x per wake, so the
        //       skip-render gate fires a torn repaint on every tick anyway
        //       and the 100 ms throttle buys nothing. Locking the bucket
        //       to the tick period means exactly one render per tick —
        //       ~10 fps of torn frames instead of ~10 redundant ones, and
        //       phase-locked so renders don't double up. The spinner is
        //       already capped at 10 fps by the tick on those terminals,
        //       so perceived smoothness is unchanged; we only stop the
        //       extra tearing repaints. On sync terminals the period is
        //       still 33 ms — UX identical to before.
        //   (b) idle with the composer caret blinking: lock to the blink
        //       HALF-period (265 ms = 530 ms / 2) so every hash step is
        //       exactly one caret toggle. This keeps the frame-request
        //       loop self-sustaining (each render re-issues the
        //       composer's frame request) and the blink perfectly
        //       regular, at ~4 renders/sec.
        //   (c) nothing animating at all (no caret — e.g. a modal owns
        //       focus): no time bucket, so a settled screen does ZERO
        //       idle renders until an event arrives.
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool fine_anim_live =
            m.s.active()                              // spinner / streaming caret
            || m.d.current.messages.empty()           // welcome wordmark bob
            || !m.ui.composer.queued.empty();         // queued-chip pulse
        // The composer caret blinks (maya composer.hpp, U+2588 toggled
        // via style every 265 ms) whenever the composer is idle. maya's
        // own gate is simply `!active`, so match it exactly — if a modal
        // happens to cover the composer the extra ~4 renders/sec are
        // harmless, whereas UNDER-animating would reintroduce the
        // freeze-until-keypress bug. Bias toward animating.
        constexpr std::int64_t kBlinkHalfMs = 265;
        const bool caret_blinking = !m.s.active();
        // Fine-animation bucket period. PHASE-LOCKED to the Tick
        // subscription by construction: it is the SAME value
        // streaming_tick_period() hands subscribe.cpp for the
        // `Sub::every(period, Tick{})` interval, so the hash advances
        // exactly once per loop wake (33 ms sync / 100 ms non-sync /
        // ≥ 80 ms SSH). Sharing the one function eliminates the old
        // hand-maintained duplicate heuristic here — which silently
        // omitted the SSH floor and could beat against the real tick.
        const std::int64_t kFineAnimMs = streaming_tick_period().count();

        // Streaming-text render bucket. While an assistant message is
        // actively streaming, the live edge runs maya's reveal_fx
        // (scramble→resolve + gradient + caret), a ~60 fps animation that
        // wants a render on every RAF wake (16 ms). The host also re-feeds
        // the full arrived source each frame, so newly arrived bytes show
        // up promptly. If we bucketed this at the tick period (100 ms on
        // non-sync terminals) the render gate would SKIP the intervening
        // RAF wakes — view() wouldn't run, the live-edge FX would freeze,
        // and a chunk of newly arrived text would pop in at once on the
        // next 100 ms flip (the "stuck then burst" symptom). Stepping the
        // bucket at the RAF interval renders each 16 ms wake. The
        // spinner-only case (tool running, no streaming_text) keeps the
        // calmer tick-period bucket so non-sync terminals don't tear the
        // chrome at 60 fps.
        const bool revealing_text =
            m.s.active()
            && !m.d.current.messages.empty()
            && m.d.current.messages.back().role == Role::Assistant
            && (!m.d.current.messages.back().streaming_text.empty()
                || !m.d.current.messages.back().pending_stream.empty());
        const std::int64_t kRevealBucketMs = 16;   // == kAnimationFrameInterval

        // POST-STREAM REVEAL DRAIN bucket. After StreamFinished the phase
        // goes Idle and streaming_text drains into `text`, so BOTH
        // m.s.active() and `revealing_text` go false — but the reveal
        // widget is STILL live: finalize_turn armed request_finalize(200)
        // so the typewriter glides the last codepoints to the edge and
        // flips live_ off ON ITS OWN over ~200 ms. That glide advances
        // ONLY inside build(), which runs ONLY when the visual hash moves.
        // Without a fast time term here the hash falls to the
        // caret-blink PARITY bucket (one flip / 265 ms) — far too coarse
        // for a 16 ms glide — so the render gate skips frames, the ramp
        // cursor never advances, live_ never flips off, and the
        // scramble/gradient overlay is STRANDED on screen at idle (and
        // worse, gets frozen INTO scrollback when the settle-freeze fires
        // — the frozen-garbage-glyph bug). pending_settle_freeze is set
        // for exactly this drain window (finalize_turn sets it; meta.cpp's
        // Tick clears it once live_tail_reveal_settled()), so key the fast
        // bucket on it: keep ticking the 16 ms reveal clock until the
        // glide drains and the freeze handoff lands.
        // The reveal-drain time bucket must keep ticking through BOTH the
        // pre-freeze settle (pending_settle_freeze) AND the post-freeze
        // settle cooldown (settle_cooldown_ticks). The cooldown frames are
        // where maya reconciles the live-tail→frozen collapse (its
        // detect→commit→demote→repaint shrink chain). request_animation_
        // frame() alone CANNOT drive them: the run loop's visual_hash gate
        // skips any frame whose hash didn't move (maya/app/app.hpp), so
        // without advancing the hash here those frames are gated away and
        // the collapse never finishes reconciling — leaving a stranded
        // duplicate turn in scrollback. Advancing the hash while the
        // cooldown counts down makes each armed Tick actually render.
        const bool draining_reveal =
            m.ui.pending_settle_freeze || m.ui.settle_cooldown_ticks > 0;

        if (revealing_text || draining_reveal) {
            mix(static_cast<std::uint64_t>(now_ms / kRevealBucketMs));
        } else if (fine_anim_live) {
            mix(static_cast<std::uint64_t>(now_ms / kFineAnimMs));
        } else if (caret_blinking) {
            // Phase-locked: feed the blink PARITY, not a time bucket, so
            // the hash advances on exactly the same boundary maya uses
            // to flip the caret cell. Beat-free by construction.
            mix(static_cast<std::uint64_t>((now_ms / kBlinkHalfMs) & 1));
        }
        // else: nothing animating — contribute no time term at all.

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
