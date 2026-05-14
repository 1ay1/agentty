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
            // Refuse if last message is a streaming assistant
            // placeholder (would corrupt mid-turn state).
            if (!m.s.is_idle() || m.s.compacting) return done(std::move(m));
            if (m.d.current.messages.empty()) return done(std::move(m));

            // Pre-trim guard: when CompactContext fires because the
            // mid-turn ceiling check tripped, the conversation is
            // already near (or over) context-max. The summarization
            // request itself includes the full message history, so
            // without trimming it would hit the same context-length
            // wall the trigger was trying to avoid. Drop the oldest
            // messages until the estimate fits a hard ceiling (~65%
            // of context-max, leaving ~35% headroom for the synth
            // prompt + the summary response itself). The dropped
            // turns are exactly the ones whose content is least
            // load-bearing for "resume the task" — earliest
            // exploratory tool calls — and the summary will be told
            // (by virtue of seeing only the recent state) to focus
            // on what's left.
            if (m.s.context_max > 0) {
                int compact_ceiling = static_cast<int>(
                    static_cast<double>(m.s.context_max) * 0.65);
                // Drop one message at a time from the front. Cheap because
                // the messages vector is small (low hundreds of entries on
                // any realistic session) and we only fall into this branch
                // when we've genuinely overrun.
                while (estimate_prefix_tokens(m.d.current) > compact_ceiling
                       && m.d.current.messages.size() > 1) {
                    m.d.current.messages.erase(m.d.current.messages.begin());
                }
                // The first surviving message may now be an Assistant
                // (we erased a User from in front of it), which makes
                // an invalid wire — Anthropic requires the message
                // sequence to start with a User. Drop leading
                // Assistants until we hit a User or run out.
                while (!m.d.current.messages.empty()
                       && m.d.current.messages.front().role == Role::Assistant) {
                    m.d.current.messages.erase(m.d.current.messages.begin());
                }
                // Empty after trim is fine — the synth user prompt below
                // becomes the only message and the model summarises from
                // whatever context fits in the system prompt (effectively
                // a "fresh start" notice). Better than wedging.
            }

            // Snapshot the post-trim message count so the
            // compaction-finalize path in stream.cpp can recover the
            // original slice [0, compact_pre_synth_count). The synthetic
            // summarisation prompt + assistant placeholder appended
            // below are bookkeeping that doesn't belong in the
            // post-compact conversation; preserving the slice lets us
            // keep a recent-tail of real turns verbatim so the UI
            // doesn't reset to a single message.
            m.s.compact_pre_synth_count = m.d.current.messages.size();

            // Synthetic User message asking the model to summarise.
            // Mirrors Claude Code's `mm8` summary prompt (binary near
            // offset 134600). The schema (Task / State / Discoveries /
            // Next Steps / Context-to-Preserve) is deliberately verbose
            // — it nudges the model to write a recoverable summary
            // rather than a one-paragraph précis that loses
            // operationally-load-bearing details.
            Message synth;
            synth.role = Role::User;
            synth.text =
                "You have been working on the task described above but have "
                "not yet completed it. Write a continuation summary that "
                "will allow you (or another instance of yourself) to resume "
                "work efficiently in a future context window where the "
                "conversation history will be replaced with this summary. "
                "Your summary should be structured, concise, and actionable. "
                "Include:\n"
                "1. Task Overview\n"
                "  The user's core request and success criteria\n"
                "  Any clarifications or constraints they specified\n"
                "2. Current State\n"
                "  What has been completed so far\n"
                "  Files created, modified, or analyzed (with paths if relevant)\n"
                "  Key outputs or artifacts produced\n"
                "3. Important Discoveries\n"
                "  Technical constraints or requirements uncovered\n"
                "  Decisions made and their rationale\n"
                "  Errors encountered and how they were resolved\n"
                "  What approaches were tried that didn't work (and why)\n"
                "4. Next Steps\n"
                "  Specific actions needed to complete the task\n"
                "  Any blockers or open questions to resolve\n"
                "  Priority order if multiple steps remain\n"
                "5. Context to Preserve\n"
                "  User preferences or style requirements\n"
                "  Domain-specific details that aren't obvious\n"
                "  Any promises made to the user\n"
                "Be concise but complete \xe2\x80\x94 err on the side of "
                "including information that would prevent duplicate work or "
                "repeated mistakes. Write in a way that enables immediate "
                "resumption of the task. Do not call any tools; just write "
                "the summary text. Wrap the summary in <summary></summary> "
                "tags.";
            m.d.current.messages.push_back(std::move(synth));

            // Assistant placeholder + fresh phase::Active matching the
            // standard submit path so the existing stream-event pipeline
            // (deltas, finished, error) doesn't need a second code path.
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));

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
            // Drop the renderer's cell cache and repaint the visible
            // viewport in place. Useful as a recovery hatch when
            // something external corrupts the terminal (a stray
            // subprocess writing to fd 1, a tmux pane swap, etc.) and
            // as a debug knob during development. Cheaper than a
            // resize-style \x1b[2J wipe — see maya's
            // `inline-redraw-paths.md` for the case (B) soft redraw
            // this rides on.
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
