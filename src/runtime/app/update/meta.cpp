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
#include <maya/app/app.hpp>   // maya::request_animation_frame

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/composer_attachment.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/store/store.hpp"
#include "agentty/workspace/checkpoint.hpp"

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
            // A profile switch re-establishes the trust baseline; drop
            // session "always allow" grants so tightening to Minimal
            // actually re-arms the prompts the user expects.
            m.d.session_grants.clear();
            persist_settings(m);
            return done(std::move(m));
        },
        [&](RestoreCheckpoint& e) -> Step {
            // Rewind = destructive double restore: worktree files AND the
            // transcript both return to the instant before the checkpointed
            // user turn was submitted (Zed's "restore checkpoint"). Only
            // from Idle — a running turn holds tool workers that write
            // files; racing them against checkout-index would interleave
            // snapshot bytes with fresh tool output.
            if (!m.s.is_idle() || m.s.compacting || m.s.thread_loading)
                return {std::move(m),
                        set_status_toast(m, "cannot rewind while the agent is working")};
            // The checkpointed message must still exist (compaction never
            // deletes messages, so this only fails on a stale id).
            const auto& msgs = m.d.current.messages;
            const bool found = std::any_of(msgs.begin(), msgs.end(),
                [&](const Message& msg) {
                    return msg.checkpoint_id && *msg.checkpoint_id == e.id;
                });
            if (!found)
                return {std::move(m),
                        set_status_toast(m, "checkpoint not found in this thread")};
            // thread_loading doubles as the "transcript is about to be
            // rewritten" guard: submit_message queues instead of firing,
            // so no message can land between the index computed at
            // CheckpointRestored time and the truncation.
            m.s.thread_loading = true;
            m.s.status         = "rewinding to checkpoint\xe2\x80\xa6";
            m.s.status_until   = {};
            // Isolated worker: git checkout-index on a large snapshot can
            // block for seconds; never on the shared BG pool.
            auto cmd = Cmd<Msg>::task_isolated(
                [id = e.id](std::function<void(Msg)> dispatch) {
                    CheckpointRestored done;
                    done.id = id;
                    std::string err;
                    done.ok = workspace::restore_checkpoint(id.value, &err);
                    if (!done.ok) done.error = err.empty() ? "restore failed" : err;
                    dispatch(Msg{std::move(done)});
                });
            return {std::move(m), std::move(cmd)};
        },
        [&](CheckpointRestored& e) -> Step {
            m.s.thread_loading = false;
            if (!e.ok) {
                return {std::move(m),
                        set_status_toast(m, "rewind failed: " + e.error,
                                         std::chrono::seconds{6})};
            }
            // Files are already byte-identical to the snapshot; now cut the
            // transcript back to just before the checkpointed user turn and
            // hand its text back to the composer for editing + resubmit.
            auto& msgs = m.d.current.messages;
            auto it = std::find_if(msgs.begin(), msgs.end(),
                [&](const Message& msg) {
                    return msg.checkpoint_id && *msg.checkpoint_id == e.id;
                });
            if (it == msgs.end()) {
                // Restored the files but the message vanished (shouldn't
                // happen — dispatch is gated on Idle + thread_loading).
                return {std::move(m),
                        set_status_toast(m, "files rewound (turn already gone)")};
            }
            // Composer refill: plain-text render of the old prompt — chip
            // placeholders become their visible labels. Raw placeholders
            // can't be resurrected safely (image bodies were drained onto
            // Message.images at submit; a dangling chip 400s the request).
            std::string refill;
            refill.reserve(it->text.size());
            for (std::size_t i = 0; i < it->text.size(); ) {
                if (static_cast<unsigned char>(it->text[i]) == attachment::kSentinel) {
                    auto len = attachment::placeholder_len_at(it->text, i);
                    if (len > 0) {
                        auto idx = attachment::placeholder_index(it->text, i);
                        if (idx < it->attachments.size()) {
                            refill.push_back('[');
                            refill.append(attachment::chip_label(it->attachments[idx]));
                            refill.push_back(']');
                        }
                        i += len;
                        continue;
                    }
                }
                refill.push_back(it->text[i++]);
            }
            const auto cut = static_cast<std::size_t>(it - msgs.begin());
            msgs.erase(msgs.begin() + static_cast<std::ptrdiff_t>(cut), msgs.end());
            // Compaction records covering now-deleted messages are
            // meaningless (up_to_index points past the end) — drop them.
            std::erase_if(m.d.current.compactions,
                          [&](const Thread::CompactionRecord& r) {
                              return r.up_to_index > cut;
                          });
            m.d.current.updated_at = std::chrono::system_clock::now();
            deps().save_thread(m.d.current);

            reset_composer_draft(m.ui.composer);
            m.ui.composer.text   = std::move(refill);
            m.ui.composer.cursor = static_cast<int>(m.ui.composer.text.size());
            if (m.ui.composer.text.find('\n') != std::string::npos)
                m.ui.composer.expanded = true;

            // Wholesale content swap into a SHORTER transcript — same
            // recovery as ThreadLoaded/NewThread: rebuild the frozen
            // prefix and reset the inline region (the truncated turns'
            // rows live in native scrollback and only reset_inline can
            // clear them; the user explicitly asked for the rewind).
            //
            // Drop the whole render cache too: the truncated tail's
            // messages are gone (their entries would never freeze again
            // — the only per-entry drop), and the surviving prefix is
            // about to be rebuilt into the frozen ledger. Both repopulate
            // lazily on the next render.
            m.ui.view_cache.clear();
            rehydrate_frozen(m);
            m.ui.needs_warmup_render = !m.ui.frozen.empty();
            release_to_kernel();
            auto toast = set_status_toast(
                m, "rewound \xc2\xb7 files restored, prompt back in composer",
                std::chrono::seconds{5});
            return {std::move(m),
                    Cmd<Msg>::batch(Cmd<Msg>::reset_inline(), std::move(toast))};
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
            // Wall-clock gap since the previous Tick. This is the ONE true dt
            // for everything the TICK subscription drives (the spinner, the
            // tok/s smoothing spring): those advance once per Tick, so their
            // per-step delta is exactly the inter-Tick interval.
            //
            // Do NOT route this through maya::anim::default_clock().dt(): that
            // clock is the VIEW-loop frame clock, and its `last_` is bumped by
            // every view() read (the reveal pulse, every Motion). During an
            // active turn view() reads it at ~60fps, so by the time the Tick
            // reducer reads dt() the "frame" gap has already been consumed —
            // the reducer sees a few stray ms instead of the real 33-100ms
            // Tick interval, and the spinner crawls / advances unevenly. That
            // coupling is exactly what made tools "feel slower". The Tick gap
            // is self-contained and correct; keep the two clocks separate.
            if (m.s.last_tick.time_since_epoch().count() == 0) m.s.last_tick = now;
            const auto tick_gap = now - m.s.last_tick;
            m.s.last_tick = now;

            // dt for Tick-driven animations: the real inter-Tick interval,
            // clamped so a slow-frame / stall recovery can't teleport the
            // spinner or spring on the first Tick back.
            float dt = std::chrono::duration<float>(tick_gap).count();
            if (dt < 0.0f)  dt = 0.0f;
            if (dt > 0.25f) dt = 0.25f;   // dropped-frame clamp (250ms)
            if (m.s.active()) m.s.spinner.advance(dt);

            // Glide the BIG tok/s readout. Retarget the smoothing spring at
            // the same raw instantaneous rate the status bar would show, then
            // advance it by dt. The view reads disp_rate_spring.value() so the
            // number eases between samples instead of strobing frame-to-frame.
            // When nothing is in flight, pull it to rest at 0.
            {
                double raw_target = 0.0;
                if (auto* ra = active_ctx(m.s.phase);
                    ra && m.s.is_streaming()
                    && ra->first_delta_at.time_since_epoch().count() != 0) {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - ra->first_delta_at).count();
                    if (ms >= 250) {
                        double sec = std::max(0.001, static_cast<double>(ms) / 1000.0);
                        raw_target = (static_cast<double>(ra->live_delta_bytes) / 4.0) / sec;
                    } else {
                        raw_target = m.s.disp_rate_spring.target();  // hold during warm-up
                    }
                }
                m.s.disp_rate_spring.set_target(raw_target);
                m.s.disp_rate_spring.tick(dt);
            }

            // Post-freeze settling window countdown. Keeps the tick
            // subscription armed (subscribe.cpp gates on it) for a few
            // frames after the settle-freeze so maya's renderer fully
            // reconciles the live-tail→frozen collapse before the clock
            // stops. Decrement here so each rendered frame burns one.
            if (m.ui.settle_cooldown_ticks > 0) --m.ui.settle_cooldown_ticks;

            // ── Deferred settle-freeze (post-stream redraw fix) ──────
            // finalize_turn settled the just-finished assistant message
            // (finish() on its StreamingMarkdown) but deferred the freeze
            // to AVOID the post-settle redraw: finish() changes the md
            // widget's build shape + prefix generation, so freezing in
            // the same tick would diff a post-finish frozen tree against
            // the last pre-finish live frame still in maya's prev_cells
            // (cache miss = re-emit the whole turn from the top). By now
            // one view() has painted the settled (post-finish) message
            // via the live tail, so prev_cells holds the post-finish
            // hash. Freezing the byte-and-hash-identical tree HERE is a
            // cache hit (no re-emit). Trim in the same step, exactly as
            // the old inline path did.
            // ── Deferred settle-freeze (post-stream redraw fix) ──────
            // finalize_turn armed the reveal finalize ramp on the
            // just-finished assistant message(s) and set
            // pending_settle_freeze WITHOUT calling finish(). We hold the
            // freeze until the reveal has FULLY drained
            // (live_tail_reveal_settled): the typewriter cursor reached
            // the live edge, the finalize ramp completed, the widget
            // flipped live_ off ON ITS OWN, and the live tail has painted
            // the settled (post-finish-shape) tree into maya's prev_cells
            // at least once. Only THEN do we run settle_message_md (a
            // now-shape-neutral finish() that just stamps the cache
            // fast-path) and freeze. Because the snapshot equals the
            // on-screen frame byte-for-byte, the live-tail→frozen handoff
            // is a maya component-cache HIT — zero re-emit, zero
            // scrollback duplication. This is the agent_session guarantee
            // (live height == settled height at the freeze instant) with
            // the reveal animation fully intact.
            //
            // While the reveal is still animating we keep the flag set
            // and arm an animation frame so the typewriter keeps gliding
            // and the gate is re-checked next frame — it never looks
            // stuck and the freeze never fires early.
            maya::Cmd<Msg> settle_freeze_trim = maya::Cmd<Msg>::none();
            if (m.ui.pending_settle_freeze && m.s.is_idle()) {
                if (live_tail_reveal_settled(m)) {
                    m.ui.pending_settle_freeze = false;
                    for (std::size_t i = m.ui.frozen_through;
                         i < m.d.current.messages.size(); ++i) {
                        auto& mm = m.d.current.messages[i];
                        if (mm.role != Role::Assistant || mm.text.empty())
                            continue;
                        settle_message_md(m, mm);
                    }
                    freeze_through(m, m.d.current.messages.size());
                    // Drop oldest frozen entries past the budget. The
                    // live-tail collapse and the front-trim are TWO
                    // distinct shrinks with OPPOSITE wire requirements,
                    // so they must not be lumped under one none():
                    //
                    //  • The collapse (live tail folds into the frozen
                    //    prefix) is a BOTTOM-shrink with an unchanged
                    //    overflowed prefix. maya's scrollback_prefix_
                    //    matches branch falls through to the append-only
                    //    per-row diff — corruption-free, NO host commit.
                    //
                    //  • The trim drops entries from the TOP of the
                    //    frozen prefix — a top-DELETION maya cannot
                    //    reconcile at render time (the shifted-up canvas
                    //    fails the prefix memcmp and maya commits the NEW
                    //    prefix over PHYSICAL old rows, stranding a
                    //    duplicate). It MUST host-commit exactly the
                    //    dropped rows. trim_frozen_if_oversized returns
                    //    that commit_scrollback(N) when (and only when)
                    //    it actually trimmed; none() when it didn't.
                    //
                    // Forward the trim's Cmd verbatim: none() on the
                    // common collapse-only Tick, commit_scrollback(N) on
                    // the rarer Tick that also trims the front.
                    settle_freeze_trim = trim_frozen_if_oversized(m);

                    // Keep the clock alive for a few frames so maya's
                    // shrink reconciliation (its own diff-path collapse)
                    // completes at fps=0, mirroring agent_session's
                    // always-on clock.
                    m.ui.settle_cooldown_ticks = 6;
                    ::maya::request_animation_frame();
                } else {
                    // Reveal still draining — keep the frame armed so the
                    // typewriter animates and we re-test the gate next
                    // tick. Never freeze a still-animating turn.
                    ::maya::request_animation_frame();
                }
            } else if (m.ui.pending_settle_freeze) {
                // Armed to freeze, but the FSM is not yet Idle (a trailing
                // sub-turn teardown, a retry ctx still parked in the phase,
                // or the off-thread markdown parse finishing a hair before
                // the phase reaches Idle). The freeze body above is gated on
                // is_idle(), so it did NOT run this tick — and it did NOT
                // re-arm a frame either. On an fps=0 event-driven screen the
                // periodic Tick can be coalesced away, so if nothing else
                // schedules a frame the reveal settles silently off-thread
                // and pending_settle_freeze strands set: the just-finished
                // markdown sits in the live tail until the next keystroke,
                // which then diffs against stale prev_cells (cache miss →
                // duplicated / late-committed turn). Self-perpetuate a frame
                // as long as the flag is pending so the gate is always
                // re-tested on a real frame once is_idle() flips true.
                ::maya::request_animation_frame();
            }

            // ── pending_stream → streaming_text ──────────────────────
            // Move buffered wire bytes into streaming_text every tick.
            // No host-side pacing: agentty does NOT animate the stream.
            // The view feeds text + streaming_text + pending_stream in
            // full to the StreamingMarkdown widget, and maya's reveal_fx
            // owns the live-edge animation. It is a plain append; the
            // old rate / backlog / burst-flush / wire-quiet smoother was
            // a second pacing clock that beat against the widget's and
            // produced the "stuck then burst" stutter — removed.
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant)
            {
                auto& msg = m.d.current.messages.back();
                if (!msg.pending_stream.empty()) {
                    msg.streaming_text.append(msg.pending_stream);
                    msg.pending_stream.clear();
                }
            }

            // No mid-stream freeze or trim on Tick. The single freeze
            // site is finalize_turn (via pending_settle_freeze, the
            // agent_session MessageStop analog). Carving during streaming
            // was the documented source of "redraws from top + scrollback
            // corruption": each mid-stream freeze stamped a frozen Turn
            // whose hash_id maya's component cache had not seen on the
            // previous live-tail frame, so the cache missed and re-emitted
            // those rows — sometimes over committed scrollback. The
            // claimed per-frame cost it was meant to bound ("5k-line
            // prose reply re-lays-out every frame") is in fact handled
            // by maya's component cache on a STABLE live-tail hash; the
            // mid-stream carves were the ones invalidating that cache.
            // agent_session never carves mid-stream and shows zero
            // corruption / zero slowdown on long runs.
            maya::Cmd<Msg> midrun_trim = maya::Cmd<Msg>::none();

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
                StreamError err{std::move(msg)};
                err.from_stall = true;   // carry stall intent on the message
                return {std::move(m), Cmd<Msg>::after(
                    std::chrono::milliseconds(0),
                    Msg{std::move(err)})};
            }

            // ── ExecutingTool wedge watchdog ───────────────────────────
            // The stream-stall watchdog above never covers ExecutingTool
            // (is_streaming() is false there) and no SSE events flow once
            // a tool runs, so a turn that strands in this phase has no
            // recovery — it just dies silently (spinner stops, no error,
            // only a new submit revives the app). Two distinct wedges:
            //
            //   (a) NO tool is actually is_running() on the back message,
            //       yet the phase is ExecutingTool. Nothing in flight will
            //       ever dispatch a terminal ToolExecOutput, so the FSM is
            //       permanently stuck. This is a true logic wedge — recover
            //       after a short grace measured from last_event_at (the
            //       phase clock, untouched during ExecutingTool).
            //
            //   (b) A tool IS running but its worker is hung in a blocking
            //       syscall (dead NFS/FUSE mount, frozen network). This is
            //       indistinguishable from a legitimately slow tool by time
            //       alone, so the net is intentionally LONG (kToolWedge) and
            //       measured from the tool's own started_at — never the
            //       stale last_event_at. It is a last-resort safety valve,
            //       NOT the per-tool timeout that was removed at user
            //       request; normal builds / `gh run watch` finish well
            //       under it.
            //
            // Recovery in both cases: fail the offending running tool(s)
            // (or, for (a), just re-fire the scheduler) and call
            // kick_pending_tools, which either continues the turn
            // (post-tool sub-turn) or drops cleanly to Idle.
            //
            // kToolWedge is kept just ABOVE bash's own max self-timeout
            // (300s, the `timeout` arg ceiling in mcp-cpp shell.cpp) so a
            // legitimately long user-set bash command finishes on its own
            // terms before this last-resort net ever trips. The real fix for
            // a SPINNING weak-model loop is the doom-loop breaker in
            // kick_pending_tools, not this hung-syscall net.
            constexpr auto kNoRunningGrace = std::chrono::seconds(30);
            constexpr auto kToolWedge      = std::chrono::seconds(330);
            if (m.s.is_executing_tool()
                && !m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& back = m.d.current.messages.back();
                bool any_running = false;
                bool wedged      = false;
                for (auto& tc : back.tool_calls) {
                    if (!tc.is_running()) continue;
                    any_running = true;
                    auto started = tc.started_at();
                    if (started.time_since_epoch().count() != 0
                        && now - started >= kToolWedge) {
                        auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - started).count();
                        tc.status = ToolUse::Failed{started, now,
                            "tool ran " + std::to_string(secs) + "s with no "
                            "result — worker likely hung on a blocking "
                            "syscall; failing it so the turn can recover. "
                            "The worker thread may continue in the "
                            "background; its result is discarded if it "
                            "ever returns."};
                        wedged = true;
                    }
                }
                if (!any_running) {
                    auto* a = active_ctx(m.s.phase);
                    if (a && a->last_event_at.time_since_epoch().count() != 0
                        && now - a->last_event_at >= kNoRunningGrace) {
                        wedged = true;   // FSM stranded with nothing in flight
                    }
                }
                if (wedged) {
                    auto kick = cmd::kick_pending_tools(m);
                    return {std::move(m), std::move(kick)};
                }
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
            if (!midrun_trim.is_none() && !settle_freeze_trim.is_none())
                return {std::move(m), Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                    std::move(midrun_trim), std::move(settle_freeze_trim)})};
            if (!midrun_trim.is_none())
                return {std::move(m), std::move(midrun_trim)};
            if (!settle_freeze_trim.is_none())
                return {std::move(m), std::move(settle_freeze_trim)};
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
            //   • Resizing the terminal window — maya treats a WIDTH
            //     change as a coherence-collapse, routing the next
            //     render through HardReset which emits
            //     \x1b[2J\x1b[3J\x1b[H (every row's wrap points
            //     shift, prev_cells is byte-invalid, no other
            //     recovery is coherent). That sequence wipes host
            //     scrollback too — destructive but structurally
            //     unavoidable on a width change. A HEIGHT-only
            //     resize stays soft (case-(B), no scrollback wipe).
            //     Either way it's a passive consequence of the
            //     resize event, not bound to a keystroke.
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
