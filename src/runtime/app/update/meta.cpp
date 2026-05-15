// meta_update — reducer for `msg::MetaMsg`. Session-level events that
// don't belong to any single domain: the Tick clock + stream-stall
// watchdog + token-rate sampler, profile cycling, conversation
// compaction kickoff, scroll/toggle, status-toast cleanup, and Quit.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"

namespace agentty::app::detail {

using maya::overload;
using maya::Cmd;

Step meta_update(Model m, msg::MetaMsg mm) {
    return std::visit(overload{
        [&](CompactContext) -> Step {
            // Refuse if a turn is already in flight or compaction is
            // already running — the next CompactContext lands cleanly
            // on Idle. Refuse on an empty thread (nothing to compact).
            if (!m.s.is_idle() || m.s.compacting) return done(std::move(m));
            if (m.d.current.messages.empty()) return done(std::move(m));

            // Compaction is wire-only: we never mutate the transcript.
            // The summarisation prompt is built and trimmed on the fly
            // by `cmd_factory::wire_messages_for_compaction` (which
            // consults `m.s.context_max` and drops oldest turns until
            // the request fits ~65% of the window). The streamed reply
            // lands in `m.s.compaction_buffer`, not in `messages`.
            // The user keeps seeing every turn they ever had — the
            // status banner is the only UI signal that compaction is
            // happening.
            //
            // `compaction_target_index` is the boundary the resulting
            // CompactionRecord will cover: "replace [0, this) with the
            // summary on future wire payloads." Captured at kickoff so
            // turns submitted by the user during compaction (queued
            // via the queue-on-compact path) don't get retroactively
            // folded into a summary they weren't part of.
            m.s.compaction_target_index = m.d.current.messages.size();
            m.s.compaction_buffer.clear();

            auto now = std::chrono::steady_clock::now();
            phase::Active ctx;
            ctx.started       = now;
            ctx.last_event_at = now;
            m.s.phase     = phase::Streaming{std::move(ctx)};
            m.s.compacting = true;
            m.s.status      = "compacting context\xe2\x80\xa6";   // …
            m.s.status_until = {};   // sticky until compaction completes
            return {std::move(m), cmd::launch_stream(m)};
        },

        [&](CycleProfile) -> Step {
            m.d.profile = m.d.profile == Profile::Write   ? Profile::Ask
                      : m.d.profile == Profile::Ask     ? Profile::Minimal
                                                       : Profile::Write;
            persist_settings(m);
            return done(std::move(m));
        },
        [&](RestoreCheckpoint&) -> Step {
            m.s.status = "checkpoint restore not implemented yet";
            return done(std::move(m));
        },
        [&](ScrollThread& e) -> Step {
            m.ui.thread_scroll = std::max(0, m.ui.thread_scroll + e.delta);
            return done(std::move(m));
        },
        [&](ToggleToolExpanded& e) -> Step {
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id) tc.expanded = !tc.expanded;
            return done(std::move(m));
        },
        [&](Tick) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (m.s.last_tick.time_since_epoch().count() == 0) m.s.last_tick = now;
            auto tick_gap = now - m.s.last_tick;
            float dt = std::chrono::duration<float>(tick_gap).count();
            m.s.last_tick = now;
            if (m.s.active()) m.s.spinner.advance(dt);

            // ── Streaming-text smoothing pacer ────────────────────────
            // Drip from pending_stream → streaming_text on every tick so
            // big server bursts (Anthropic's content_block_delta can carry
            // 50-100+ chars at once) reveal smoothly at cursor pace
            // instead of jumping in.
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant)
            {
                auto& msg = m.d.current.messages.back();
                if (!msg.pending_stream.empty()) {
                    constexpr std::size_t kDripMin = 32;
                    constexpr std::size_t kDripMax = 256;
                    std::size_t drip = std::clamp(
                        msg.pending_stream.size() / 8, kDripMin, kDripMax);
                    drip = std::min(drip, msg.pending_stream.size());
                    while (drip < msg.pending_stream.size() &&
                           (static_cast<unsigned char>(msg.pending_stream[drip]) & 0xC0) == 0x80) {
                        ++drip;
                    }
                    msg.streaming_text.append(msg.pending_stream, 0, drip);
                    msg.pending_stream.erase(0, drip);
                }
            }

            // ── Stream-stall watchdog ──────────────────────────────────
            // 120 s of total silence is overwhelmingly likely to be a
            // wedged transport rather than legitimate model behaviour.
            // Clock-skew guard: if a render pass took > 2 s we rebase
            // last_event_at forward so one slow frame can't synthesize
            // a spurious stream-stalled error.
            constexpr auto kTickRebaseThreshold = std::chrono::seconds(2);
            if (auto* a = active_ctx(m.s.phase);
                a && tick_gap >= kTickRebaseThreshold
                && a->last_event_at.time_since_epoch().count() != 0) {
                a->last_event_at += tick_gap;
            }
            constexpr auto kStallSecs = std::chrono::seconds(120);
            if (auto* a = active_ctx(m.s.phase);
                a && m.s.is_streaming()
                && std::holds_alternative<retry::Fresh>(a->retry)
                && a->last_event_at.time_since_epoch().count() != 0
                && now - a->last_event_at >= kStallSecs) {
                a->retry = retry::StallFired{};
                if (a->cancel) a->cancel->cancel();
                auto since = std::chrono::duration_cast<std::chrono::seconds>(
                                 now - a->last_event_at).count();
                std::string msg = "stream stalled — no events for "
                                + std::to_string(since) + "s";
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::milliseconds(0),
                    Msg{StreamError{std::move(msg)}})};
            }

            // Sample tok/s into the sparkline ring every ~500 ms while
            // the stream is actively producing bytes.
            if (auto* a = active_ctx(m.s.phase);
                a && m.s.is_streaming()
                && a->first_delta_at.time_since_epoch().count() != 0) {
                constexpr auto kSampleInterval = std::chrono::milliseconds{500};
                if (a->rate_last_sample_at.time_since_epoch().count() == 0) {
                    a->rate_last_sample_at    = now;
                    a->rate_last_sample_bytes = a->live_delta_bytes;
                } else if (now - a->rate_last_sample_at >= kSampleInterval) {
                    auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a->rate_last_sample_at).count();
                    auto bytes_delta = (a->live_delta_bytes >= a->rate_last_sample_bytes)
                                       ? (a->live_delta_bytes - a->rate_last_sample_bytes)
                                       : 0;
                    float rate = window_ms > 0
                               ? (static_cast<float>(bytes_delta) / 4.0f)
                                 * (1000.0f / static_cast<float>(window_ms))
                               : 0.0f;
                    m.s.rate_history[m.s.rate_history_pos] = rate;
                    m.s.rate_history_pos =
                        (m.s.rate_history_pos + 1) % StreamState::kRateSamples;
                    if (m.s.rate_history_pos == 0) m.s.rate_history_full = true;
                    a->rate_last_sample_at    = now;
                    a->rate_last_sample_bytes = a->live_delta_bytes;
                }
            }
            return done(std::move(m));
        },
        [&](Quit) -> Step {
            if (!m.d.current.messages.empty()) deps().save_thread(m.d.current);
            return {std::move(m), Cmd<Msg>::quit()};
        },
        [&](NoOp) -> Step { return done(std::move(m)); },
        [&](RedrawScreen) -> Step {
            // Ctrl-L → viewport-only soft redraw.
            //
            // Drops maya's renderer cell cache (force_redraw zeroes
            // prev_rows on the active InlineFrameState) and on the
            // next compose triggers serialize.cpp's case-(B) path:
            // cursor_up + re-serialize the last `term_h` rows +
            // \x1b[J. Net effect: every cell inside the live viewport
            // is rewritten from the canvas, wiping any ghost glyphs
            // a half-written frame or stray subprocess output left
            // behind. Cheap; preserves scrollback; preserves cursor
            // position relative to the viewport (no "composer jumps
            // to the bottom" jolt).
            //
            // ── SCOPE ─────────────────────────────────────────────────
            //
            // This hotkey fixes viewport corruption ONLY. Scrollback
            // rows that have already overflowed the terminal viewport
            // are owned by the terminal emulator and cannot be
            // repainted by an inline-mode application without also
            // overwriting any non-agentty content above the frame.
            // The hard contract lives at
            // maya/src/render/serialize.cpp’s case-(B) emit — see
            // the SCOPE CONTRACT comment there for the full rationale.
            //
            // If the user's scrollback is mangled (a stray subprocess
            // wrote past agentty, a tmux pane swap got confused,
            // bytes were dropped during a resize), Ctrl-L will not
            // help. The right recoveries in that situation are:
            //   • The terminal emulator's own redraw hotkey — most
            //     terminals bind their Ctrl-L to a full repaint of
            //     the emulator's local cell grid, which IS able to
            //     reach scrollback rows.
            //   • Resizing the terminal window by even one cell —
            //     maya treats a resize as a coherence-collapse,
            //     routing the next render through the Divergent
            //     path which DOES emit \x1b[2J\x1b[3J\x1b[H. That
            //     sequence wipes the host's scrollback too, so it's
            //     deliberately gated on a resize and not bound to a
            //     keystroke.
            return {std::move(m), Cmd<Msg>::force_redraw()};
        },
        [&](ClearStatus& e) -> Step {
            // No-op if the user (or another handler) wrote a newer
            // status since this cleaner was scheduled — stamps won't
            // match, so the current banner stays.
            if (m.s.status_until == e.stamp) {
                m.s.status.clear();
                m.s.status_until = {};
            }
            return done(std::move(m));
        },
    }, mm);
}

} // namespace agentty::app::detail
