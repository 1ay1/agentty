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
            // Frozen-prefix gate: a tool whose enclosing message has
            // already settled into m.ui.frozen has its expanded
            // state baked into the snapshotted Element — flipping
            // tc.expanded on a frozen Message would change model
            // state without ever reaching the canvas (the Element's
            // hash_id is stamped at freeze time and never
            // recomputed). with_live_tool refuses the mutation; the
            // expand key simply no-ops on scrollback turns, which
            // matches the user's reasonable expectation that
            // "scrollback is archaeology".
            with_live_tool(m, e.id, [](ToolUse& tc) {
                tc.expanded = !tc.expanded;
            });
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
            //
            // Frame-rate-independent: the reveal rate is bytes/second,
            // not bytes/tick. On a 30 fps tick we drip ~kBytesPerSec/30
            // per tick; on a 10 fps tick (non-DEC-2026 terminals) we
            // drip 3× as much per tick. The user-perceived fill speed
            // is identical across terminal capabilities — previously,
            // Apple Terminal / plain xterm / tmux-without-sync paid a
            // 3× latency tax because the 100 ms tick AND the per-tick
            // cap compounded.
            //
            // Burst-flush: when more than kBurstFlushBytes are pending,
            // the smoother is no longer hiding chunky paints — it's
            // making the user stare at a stalled cursor. Drain the
            // whole backlog in one tick. The fill animation is only
            // worth preserving while the wire is keeping up; once it
            // gets ahead by a paragraph or more, the latency cost
            // dominates the aesthetic benefit.
            // Wire-quiescence flush. The smoothing pacer only earns its
            // keep while bytes are ACTIVELY flowing — it hides the visual
            // jump of a chunky multi-KB content_block_delta by revealing
            // it over a few frames. The moment the wire goes quiet (model
            // paused between bursts, finished a paragraph and is thinking,
            // or the render path was briefly backpressured and is now
            // catching up) there is nothing left to smooth AGAINST:
            // continuing to dribble the buffered tail at kBytesPerSec is
            // exactly the "it pauses, then slowly scrolls in the text it
            // already had" symptom.
            // last_event_at is bumped on every SSE event (text/json
            // delta AND heartbeats), so a gap here means the wire is
            // genuinely idle — reveal the whole backlog now.
            //
            // Threshold tuning: 90 ms was too tight. A model that
            // batches tokens routinely pauses 90–200 ms between
            // content_block_deltas, so a 90 ms quiet window tripped a
            // full backlog flush on nearly every inter-delta gap — the
            // smoother never engaged and the user saw "freeze, then a
            // chunk dumps, freeze, chunk" (the stuck-then-burst
            // symptom). 250 ms only trips on a GENUINE pause (model
            // thinking / finished a paragraph), where flushing is
            // correct; ordinary token batching now flows through the
            // pacer and reveals smoothly.
            bool wire_quiet = false;
            if (const auto* a = active_ctx(m.s.phase)) {
                if (a->last_event_at.time_since_epoch().count() != 0)
                    wire_quiet = (now - a->last_event_at)
                               > std::chrono::milliseconds(250);
            }

            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant)
            {
                auto& msg = m.d.current.messages.back();
                if (!msg.pending_stream.empty()) {
                    // Base reveal rate ~32 KB/s. At 30 fps that's ~1 KB
                    // per tick on a steady fill, well above any plausible
                    // sustained model emission rate, so the pacer only
                    // engages on bursts. At 10 fps it's ~3 KB per tick,
                    // same wall-clock fill speed.
                    constexpr float kBytesPerSec     = 32768.0f;
                    constexpr std::size_t kDripMin   = 64;
                    // Burst-flush threshold. Anthropic batches code
                    // blocks heavily — a small function body arrives
                    // in a single content_block_delta of 2–4 KB. 8 KB
                    // bounds the visual stall — at the adaptive reveal
                    // rate that's well under a quarter-second of
                    // catch-up — while letting routine code-block deltas
                    // flow through the smoother instead of dumping.
                    constexpr std::size_t kBurstFlushBytes = 8192;
                    // Bound the catch-up: whatever the backlog, drain it
                    // within this window so the pacer never lags the wire
                    // by more than a fraction of a second. This is what
                    // turns a fixed-rate dribble (which falls behind a
                    // fast model, accumulates, then bursts on the next
                    // quiet gap — the stuck-then-fast symptom) into a
                    // smooth accelerate-to-catch-up.
                    constexpr float kMaxDrainSeconds = 0.20f;

                    std::size_t drip;
                    if (msg.pending_stream.size() >= kBurstFlushBytes
                        || wire_quiet) {
                        // Backlog dominates, OR the wire has gone quiet so
                        // there's nothing left to smooth against — flush
                        // it all this tick.
                        drip = msg.pending_stream.size();
                    } else {
                        // Reveal at the GREATER of the base rate and the
                        // rate needed to drain the current backlog within
                        // kMaxDrainSeconds. A small backlog reveals at the
                        // gentle base rate; a growing one accelerates so it
                        // stays close to the wire instead of saving up a
                        // burst.
                        auto base_target = kBytesPerSec * dt;
                        auto drain_target =
                            static_cast<float>(msg.pending_stream.size())
                            * (dt / kMaxDrainSeconds);
                        auto target = static_cast<std::size_t>(
                            std::max(base_target, drain_target));
                        drip = std::max(target, kDripMin);
                        drip = std::min(drip, msg.pending_stream.size());
                    }
                    // UTF-8 safety: don't split a multi-byte codepoint
                    // at the drip boundary. Walk forward to the next
                    // leading byte; bounded by the 4-byte max UTF-8
                    // sequence length so a buffer of malformed
                    // continuation bytes can't drag us through the
                    // whole pending_stream.
                    {
                        std::size_t walked = 0;
                        while (drip < msg.pending_stream.size()
                               && walked < 3
                               && (static_cast<unsigned char>(msg.pending_stream[drip]) & 0xC0) == 0x80) {
                            ++drip;
                            ++walked;
                        }
                    }
                    msg.streaming_text.append(msg.pending_stream, 0, drip);
                    msg.pending_stream.erase(0, drip);
                }
            }

            // ── Per-tick canvas bound (the SPEED path) ───────────
            // Tick is the dominant render driver during a stream. Without
            // bounding here, a long single turn (a big streaming response,
            // or one auto-pilot turn with many write/edit cards) grows the
            // live canvas unboundedly: every settled-but-unfrozen sub-turn
            // re-lays-out + re-paints each tick, and the shadow verify +
            // canvas.clear() walk the whole oversized canvas. Even AFTER a
            // sub-turn freezes, layout + per-cell blit still scale with
            // frozen_row_total — so we must both freeze settled prefixes
            // AND trim the part of the frozen prefix above the viewport.
            // Cost otherwise climbs with turn length (the mid-stream lag).
            //
            // Freeze any sub-turns that became terminal (into the
            // zero-copy, hash-keyed frozen prefix maya blits), THEN trim
            // the part of that prefix that has scrolled above the
            // viewport. Freezing alone does NOT bound per-frame cost: a
            // hash-keyed frozen entry skips the body REBUILD but still
            // pays layout + per-cell blit + canvas.clear + shadow verify
            // every tick, all O(frozen_row_total). A long single turn
            // grows that prefix without limit, so per-frame cost climbs
            // with turn length — the progressive mid-stream slowdown.
            // trim_frozen_above_viewport drops only entries PROVABLY
            // above the viewport (keeps ~3 screens, robust to the
            // byte-wrap over-count) and commits EXACTLY the dropped rows,
            // so the bound holds mid-stream without stranding a duplicate.
            maya::Cmd<Msg> midrun_trim = maya::Cmd<Msg>::none();
            if (m.s.active()) {
                // Bound a long PURE-TEXT answer first: split its committed
                // markdown prefix into a settled sub-turn so the next
                // freeze_settled_subturns call can freeze it. Without
                // this a 5k-line prose reply re-lays-out every frame
                // (~13 ms/frame); with it only the ~2KB live tail does.
                freeze_streaming_text_prefix(m);
                freeze_settled_subturns(m);
                midrun_trim = trim_frozen_above_viewport(m);
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
            if (!midrun_trim.is_none())
                return {std::move(m), std::move(midrun_trim)};
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
