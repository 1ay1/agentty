// Stream-side helpers for the update reducer: live-preview salvage during
// input_json_delta, partial-JSON closing + truncation guards, and the
// finalize_turn state-machine handoff from Streaming → Idle / Permission /
// ExecutingTool. Kept out of update.cpp so the reducer orchestrator stays
// easy to read.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update/param_tag_repair.hpp"
#include "agentty/runtime/app/update/stream_args.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>

#include <maya/core/overload.hpp>

#include "agentty/auth/auth.hpp"
#include "agentty/provider/error_class.hpp"
#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/runtime/code_block_picker.hpp"
#include "agentty/runtime/mem.hpp"
#include "agentty/runtime/view/cache.hpp"
#include "agentty/tool/spec.hpp"
#include <maya/widget/markdown.hpp>
#include "agentty/tool/util/partial_json.hpp"

namespace agentty::app::detail {

using json = nlohmann::json;
using maya::Cmd;

namespace {

// Cap on transparent retries per user turn before we give up and surface the
// truncation as a real Error to the model. Two attempts rides out intermittent
// edge idle-timeouts; more would loop on a genuinely broken upstream.
constexpr int kMaxTruncationRetries = 2;

// Pre-settle an assistant message's StreamingMarkdown to the EXACT state
// cached_markdown_for's settled fast-path expects, so the freeze build
// (freeze_range → cached_markdown_for) returns a byte-identical element
// tree and maya blits instead of re-emitting the whole turn.
//
// The redraw-after-stream-end symptom: the old pre-settle did a raw
// set_content + finish() that (a) abruptly flipped live_ off while the
// reveal cursor was mid-glide — collapsing the pre-finish overlay tree
// (prefix ComponentElement + tail in vstack.gap(1)) to the flat
// post-finish tree, a ±N-row height step — and (b) DIDN'T fold long code
// blocks or stamp last_settled_size/revealed_size, so the subsequent
// cached_markdown_for call missed its fast-path, re-folded, and produced
// a DIFFERENT height than the last live frame. Either delta makes maya
// diff a shorter settled frame against the taller live one and re-emit
// the turn from the top.
//
// Doing the full settle sequence here — set_content(final) → finish() →
// auto_fold_long_blocks(same threshold/kinds) → stamp the cache sizes —
// means: the live frame already folded the block (turn.cpp's per-frame
// fold), this locks the SAME folded+finished tree, and cached_markdown_for
// hits its settled fast-path returning that identical tree. No height
// step, no re-emit. Mirrors agent_session's single coherent
// finish()+push-to-frozen at MessageStop.
//
// Hoisted OUT of the anonymous namespace (declared in internal.hpp) so
// meta.cpp's deferred settle-freeze can call it once the reveal has
// drained.
}  // namespace (close anon for the cross-TU helpers below)

void settle_message_md(Model& m, const Message& msg) {
    auto& cache = m.ui.view_cache.message_md(m.d.current.id, msg.id);
    if (!cache.streaming)
        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
    cache.streaming->set_content(msg.text);
    cache.streaming->finish();
    // Same fold preset cached_markdown_for applies (40-line code blocks).
    constexpr std::uint16_t kFoldLineThreshold = 40;
    constexpr std::uint32_t kFoldKinds =
        (1u << static_cast<unsigned>(maya::StreamingMarkdown::BlockKind::CodeBlock));
    cache.streaming->auto_fold_long_blocks(kFoldLineThreshold, kFoldKinds);
    // Stamp so cached_markdown_for's settled fast-path engages and returns
    // the cached build() unchanged on the freeze pass and every frame after.
    cache.last_settled_size = msg.text.size();
    cache.revealed_size     = msg.text.size();
}

// True iff every Assistant message in the live tail has fully drained its
// reveal animation. See internal.hpp for the full rationale. The check is
// purely a READ of each message's StreamingMarkdown widget state; it never
// mutates. A message whose widget doesn't exist yet (never rendered) or
// whose text is empty is treated as settled (nothing to animate). The
// gate is what guarantees the freeze handoff is byte-identical to the
// on-screen frame: we only freeze AFTER the widget itself flipped live_
// off, so the last LIVE frame maya cached already IS the settled tree.
bool live_tail_reveal_settled(const Model& m) {
    for (std::size_t i = m.ui.frozen_through;
         i < m.d.current.messages.size(); ++i) {
        const auto& mm = m.d.current.messages[i];
        if (mm.role != Role::Assistant) continue;
        // Still has uncommitted wire bytes — not done arriving, let it ride.
        if (!mm.streaming_text.empty() || !mm.pending_stream.empty())
            return false;
        if (mm.text.empty()) continue;   // no prose body to reveal
        const auto& cache = m.ui.view_cache.message_md(m.d.current.id, mm.id);
        if (!cache.streaming) continue;  // never rendered — nothing animating
        // The widget still live_, the reveal gliding to the edge, a finalize
        // ramp running, or a background parse pending — any of these means
        // the live frame in maya's prev_cells is NOT yet the settled shape.
        // This predicate set MUST mirror build_live_tail's `reveal_settled`
        // exactly: that one decides whether the live tail STAMPS the
        // cacheable assistant_run_hash_id, this one decides whether the
        // freeze fires. If they disagree the freeze can stamp a key the
        // live tail never painted (cache MISS) → freeze_range rebuilds the
        // run under FrozenBuildScope (show_all) at a possibly different
        // height → the seam shifts and strands a duplicate. is_live() is a
        // DISTINCT term from the other three: a widget can be live_ with the
        // reveal cursor already at the edge (reveal_in_progress false, no
        // ramp, no parse) during a mid-stream pause, so dropping it would
        // re-open the asymmetry. finish() drops all four together, so once
        // finalize_turn has settled the tail this returns true immediately.
        if (cache.streaming->is_live()
         || cache.streaming->reveal_in_progress()
         || cache.streaming->is_finalizing()
         || cache.streaming->is_parsing())
            return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────
// The streaming-args preview decoder (update_stream_preview) and the todo
// sync (sync_todo_state_from_args) moved to stream_preview.cpp. The shared
// leaf helpers they and the salvage path below use (sniff_any,
// try_parse_partial, get_string_any, missing_required_field + the alias
// constants) live in update/stream_args.hpp.

bool guard_truncated_tool_args(ToolUse& tc) {
    auto missing = missing_required_field(tc.name.value, tc.args);
    if (missing.empty()) return false;
    // A required field is absent — before failing, try to recover a mixed
    // XML/JSON tool input that buried the field inside a stray string
    // value. Gating on the already-failing path is the safety guarantee:
    // a well-formed edit/write whose content legitimately contains the
    // literal `<parameter name="…">` marker (e.g. editing this very file)
    // has its required fields present, so we never reach here and never
    // rewrite it.
    if (repair_param_tag_leak(tc.name.value, tc.args)) {
        tc.mark_args_dirty();
        missing = missing_required_field(tc.name.value, tc.args);
        if (missing.empty()) return false;
    }
    auto now = std::chrono::steady_clock::now();
    tc.status = ToolUse::Failed{
        tc.started_at(),
        now,
        std::string{"Tool call arguments look incomplete — `"}
            + std::string{missing}
            + "` is missing. This usually means the stream was truncated "
              "before the full tool input arrived. Please emit a fresh "
              "tool call with every required field populated (including `"
            + std::string{missing} + "`).",
    };
    return true;
}

json salvage_args(const ToolUse& tc) {
    // Refuse salvage when the wire stopped mid-string. close_partial_json
    // would synthesise a closing `"` and produce a parseable object whose
    // last string value is truncated at an arbitrary byte boundary;
    // dispatching `write`/`edit`/`bash` on that silently corrupts the
    // target file or runs a half-typed command. The caller's parse-failed
    // path (`StreamToolUseEnd`) already has the right shape — we just
    // need to keep it from accepting a synthesised-quote salvage. See
    // Finding 3 in docs/corruption-analysis.md.
    //
    // Path-only tools (read, list_dir, glob) would also be affected but
    // their values are tiny and rarely truncated mid-string in practice;
    // applying the rule uniformly keeps the policy reviewable in one
    // place at the cost of a few false refusals on degenerate streams.
    if (agentty::tools::util::ended_inside_string(tc.args_streaming)) {
        return json::object();
    }
    if (auto parsed = try_parse_partial(tc.args_streaming)) {
        if (!parsed->empty()) return *parsed;
    }
    json out = json::object();
    auto pick = [&](std::string_view canon,
                    std::span<const std::string_view> keys = {}) {
        auto ks = keys.empty() ? std::span{&canon, 1} : keys;
        if (auto v = sniff_any(tc.args_streaming, ks, /*partial=*/true))
            out[std::string{canon}] = *v;
    };
    const auto& n = tc.name.value;
    if      (n == "read" || n == "list_dir") { pick("path", kPathAliases); }
    else if (n == "write") { pick("path", kPathAliases); pick("content", kContentAliases); }
    else if (n == "edit")  { pick("path", kPathAliases);
                             pick("old_string", kOldStrAliases);
                             pick("new_string", kNewStrAliases); }
    else if (n == "bash")  { pick("command"); }
    else if (n == "grep")  { pick("pattern"); pick("path", kPathAliases); }
    else if (n == "glob")  { pick("pattern"); }
    else if (n == "find_definition") { pick("symbol"); }
    else if (n == "web_fetch")       { pick("url"); }
    else if (n == "web_search")      { pick("query"); }
    else if (n == "diagnostics")     { pick("command"); }
    else if (n == "git_commit")      { pick("message"); }
    pick("display_description", std::span{&kDisplayDescription, 1});
    return out;
}

maya::Cmd<Msg> finalize_turn(Model& m, StopReason stop_reason) {
    using maya::Cmd;
    // Stream is over — drop the cancel handle so a stale Esc can't trip
    // the next turn's stream the moment it launches. Phase transitions
    // below (or in kick_pending_tools) drive whether active() flips off.
    if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

    // Compaction completion. The compaction stream wrote its summary
    // text into `m.s.compaction_buffer` (NOT into messages — the
    // transcript is immutable across compaction events). Now:
    //   1. Lift the summary text out of the buffer.
    //   2. Strip <summary>…</summary> tags / trim whitespace.
    //   3. Push a Thread::CompactionRecord describing
    //      [0, compaction_target_index) so future wire payloads
    //      built by `cmd_factory::wire_messages_for` substitute the
    //      summary for the raw prefix.
    //
    // The user-visible transcript stays exactly as it was before
    // CompactContext fired. No swap, no "reset to a shorter conversation",
    // no thread_view_start fiddling. The only UI signal is the status
    // banner flipping to "context compacted" and a divider the view
    // draws between messages[up_to_index-1] and messages[up_to_index].
    if (m.s.compacting) {
        std::string summary = std::move(m.s.compaction_buffer);
        m.s.compaction_buffer.clear();

        constexpr std::string_view kOpen = "<summary>";
        constexpr std::string_view kClose = "</summary>";
        if (auto a_pos = summary.find(kOpen); a_pos != std::string::npos) {
            auto body = a_pos + kOpen.size();
            auto b_pos = summary.find(kClose, body);
            if (b_pos != std::string::npos) {
                summary = summary.substr(body, b_pos - body);
            }
        }
        auto is_space = [](char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; };
        while (!summary.empty() && is_space(summary.front())) summary.erase(summary.begin());
        while (!summary.empty() && is_space(summary.back()))  summary.pop_back();
        if (summary.empty()) summary = "[compaction produced no text]";

        // Clamp the target index to the current transcript size in
        // case messages were queued/submitted during compaction
        // (queue-on-compact path drains AFTER this branch runs, but a
        // future code path could in principle land messages earlier).
        // The invariant we care about: up_to_index never exceeds
        // messages.size() so the wire-substitution helper stays
        // well-defined.
        const std::size_t up_to = std::min(m.s.compaction_target_index,
                                           m.d.current.messages.size());
        Thread::CompactionRecord rec;
        rec.up_to_index = up_to;
        rec.summary     = std::move(summary);
        rec.created_at  = std::chrono::system_clock::now();
        m.d.current.compactions.push_back(std::move(rec));
        m.d.current.updated_at = std::chrono::system_clock::now();

        // Do NOT rehydrate frozen. The currently-frozen prefix is
        // unchanged by compaction (the transcript is immutable — only
        // the wire payload swaps the prefix for a summary). Rebuilding
        // it from scratch trashes every Element identity in the
        // m.ui.frozen vector, which torpedoes maya's pointer-keyed
        // component cache for the inner ComponentElements (markdown
        // body, tool cards) that don't carry their own hash_id. The
        // visible symptom on the user side is a one-frame ghost / full
        // re-layout of the entire scrollback the moment compaction
        // completes.
        //
        // The compaction divider doesn't need to be inserted here:
        // `needs_compaction_divider(i)` in freeze_range consults
        // m.d.current.compactions on every freeze pass. The next
        // freeze_through call (next user turn → freeze_through fires
        // in submit_message via the freeze-prior-prefix path) will
        // hit `i == up_to_index` and push the divider naturally,
        // immediately before the user's next message. The frozen
        // prefix that's already painted stays byte-identical across
        // the transition.

        // Rapid-refill breaker bookkeeping. If this compact landed
        // within `kRapidRefillTurns` assistant turns of the previous
        // one, it counts toward the breaker; otherwise the streak
        // resets. Crossing `kRapidRefillCount` flips the disable
        // flag so the auto-trigger in modal.cpp stops firing.
        constexpr int kRapidRefillTurns = 3;
        constexpr int kRapidRefillCount = 3;
        if (m.s.turns_since_last_compact <= kRapidRefillTurns) {
            ++m.s.recent_compacts;
        } else {
            m.s.recent_compacts = 1;
        }
        m.s.turns_since_last_compact = 0;
        if (m.s.recent_compacts >= kRapidRefillCount) {
            m.s.autocompact_disabled = true;
        }

        m.s.compaction_target_index = 0;
        m.s.compacting    = false;
        m.s.phase         = phase::Idle{};
        // tokens_in / tokens_out are intentionally NOT touched: the
        // StreamUsage handler skipped writes during compaction, so
        // these still hold the values from the last real (non-
        // compaction) turn. Resetting them to 0 would produce a
        // visible flicker on the context gauge between this point
        // and the next StreamUsage event.
        // Smooth UX: when a user message is queued behind compaction,
        // it's about to fire below — the user will immediately see
        // their request go out, so a "context compacted" toast is
        // noise on top of their answer streaming in. Skip it entirely.
        // Only surface the rapid-refill disable warning (it's actionable
        // info: tells the user to run /compact manually next time)
        // and only show "context compacted" when there's no queued
        // work and the user might still be looking at the spinner.
        // Even then, keep it brief (1.5s) instead of the prior 4s
        // dwell that competed for attention with the next thing the
        // user actually wants to read.
        const bool queued_about_to_fire = !m.ui.composer.queued.empty();
        auto now_ts = std::chrono::steady_clock::now();
        if (m.s.autocompact_disabled) {
            m.s.status        = "auto-compact disabled (rapid refill); use /compact manually";
            m.s.status_until  = now_ts + std::chrono::seconds{6};
        } else if (queued_about_to_fire) {
            // Silent path — the queued message becomes the next visible
            // event in the UI, no toast needed.
            m.s.status.clear();
            m.s.status_until = {};
        } else {
            m.s.status        = "context compacted";
            m.s.status_until  = now_ts + std::chrono::milliseconds{1500};
        }
        deps().save_thread(m.d.current);
        // Hand the freshly-freed arenas back to the OS. Less to free
        // now that we don't drop tool outputs — the only freed bytes
        // are the compaction stream's input/output buffers — but
        // still cheap and keeps the historical "compact → RSS dips"
        // user signal alive on glibc. No-op on musl/macOS/Windows.
        release_to_kernel();

        // Drain any messages the user queued during compaction. Same
        // shape as the post-StreamFinished drain at the end of this
        // function, but tailored: we're already Idle, no pending tools
        // to kick.
        if (queued_about_to_fire) {
            auto& head = m.ui.composer.queued.front();
            m.ui.composer.text        = std::move(head.text);
            m.ui.composer.attachments = std::move(head.attachments);
            m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
            m.ui.composer.queued.erase(m.ui.composer.queued.begin());
            auto [mm, sub_cmd] = submit_message(std::move(m));
            m = std::move(mm);
            return sub_cmd;
        }
        if (m.s.status.empty()) return Cmd<Msg>::none();
        auto stamp = m.s.status_until;
        auto ttl_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            m.s.status_until - now_ts);
        return Cmd<Msg>::after(ttl_ms + std::chrono::milliseconds{50},
                               Msg{ClearStatus{stamp}});
    }
    bool any_truncated = false;
    const bool max_tokens_hit = (stop_reason == StopReason::MaxTokens);
    if (!m.d.current.messages.empty()) {
        auto& last = m.d.current.messages.back();
        // Drain any text still in the smoothing buffer before committing
        // — message_stop should leave no in-flight bytes invisible.
        if (last.role == Role::Assistant && !last.pending_stream.empty()) {
            last.streaming_text += last.pending_stream;
            last.pending_stream.clear();
        }
        if (last.role == Role::Assistant && !last.streaming_text.empty()) {
            if (last.text.empty()) last.text = std::move(last.streaming_text);
            else                   last.text += std::move(last.streaming_text);
            std::string{}.swap(last.streaming_text);
        }

        // Pre-settle StreamingMarkdown so the message's rendered height
        // is locked in BEFORE the next view() runs. Lazy finish() during
        // view shifts the assistant message height by 1+ rows when the
        // closing ``` of a code block (or a trailing block boundary)
        // commits the tail into a prefix block — maya's diff sees that
        // shrink and emits the shrink path, but rows already overflowed
        // into native scrollback retain the pre-shrink layout, leaving
        // border fragments at the scrollback↔viewport seam.
        // Settling here matches agent_session's `m.md.finish() + push to
        // frozen` discipline: one coherent transition per update step.
        //
        // Settle NOW, unconditionally — agent_session's MessageStop does
        // exactly this (m.md.finish()). The previous code armed a 200 ms
        // request_finalize() GLIDE when the turn was about to go idle and
        // deferred the finish() to meta.cpp's Tick once the glide drained.
        // That animated handoff needs dense ~16 ms frames to hold the
        // live-tail height steady while the reveal cursor rolls to the
        // edge; at fps=0 over SSH/tmux the frames land ~80 ms apart, so the
        // height drifts mid-glide and the eventual freeze diffs a moved
        // tree against a stale prev_cells — stranding a duplicate turn in
        // scrollback (the reported bug). finish() resolves the reveal
        // overlay immediately, so the live height equals the settled/frozen
        // height in the SAME frame and the freeze handoff is clean. The
        // only thing lost is the ~200 ms post-stream typewriter glide —
        // exactly the trade agent_session makes.
        if (last.role == Role::Assistant && !last.text.empty()) {
            settle_message_md(m, last);
        }
        // Flush any tool_calls whose StreamToolUseEnd never fired — Anthropic
        // normally sends content_block_stop per tool block, but proxies /
        // message_stop cut-offs can skip it.
        for (auto& tc : last.tool_calls) {
            if (!tc.args_streaming.empty() && tc.is_pending()) {
                try {
                    tc.args = json::parse(tc.args_streaming);
                    tc.mark_args_dirty();
                } catch (const std::exception& ex) {
                    auto salvaged = salvage_args(tc);
                    if (!salvaged.empty()) {
                        tc.args = std::move(salvaged);
                        tc.mark_args_dirty();
                    } else {
                        // Distinguish "never closed" (parse failed AND no
                        // mid-string truncation) from "truncated mid-string"
                        // (salvage_args refused because close_partial_json
                        // would have synthesised a closing quote on a
                        // half-written value). The mid-string case is
                        // retry-eligible — mark the flag, leave the tool
                        // Pending, and let the retry block below pop the
                        // assistant placeholder and re-launch on the same
                        // ctx. Only on retry-budget exhaustion does the
                        // tool surface as Failed (handled in the retry
                        // block's fall-through).
                        const bool mid_string =
                            agentty::tools::util::ended_inside_string(
                                tc.args_streaming);
                        if (mid_string) {
                            tc.stream_mid_string_truncated = true;
                            // Leave status Pending; the retry block
                            // below will see any_truncated and re-launch.
                        } else {
                            auto now = std::chrono::steady_clock::now();
                            tc.status = ToolUse::Failed{
                                tc.started_at(), now,
                                std::string{"tool args never closed: "}
                                    + ex.what()};
                        }
                    }
                }
            }
            std::string{}.swap(tc.args_streaming);
            if (tc.is_pending()) {
                if (guard_truncated_tool_args(tc)) any_truncated = true;
                // Mid-string cutoffs also participate in the
                // transparent-retry loop — the wire died inside a
                // string value, no salvage is safe, so the only path
                // to a usable tool call is re-launching the stream.
                // Tool stays Pending here; the retry block pops the
                // placeholder. On retry-budget exhaustion the block
                // below sets Failed with the mid-string message.
                if (tc.stream_mid_string_truncated) any_truncated = true;
            }
        }
    }

    // ── max_tokens cutoff handling ──
    //
    // When stop_reason == max_tokens, generation halted at the model's
    // output cap. If a tool block was being streamed at that moment, its
    // input_json is truncated — which has two failure modes:
    //
    //   (a) The truncation is "syntactically obvious" (the parser can't
    //       close the JSON, or required schema fields are missing).
    //       guard_truncated_tool_args above sets `any_truncated` and
    //       fails the tool with a generic incomplete-args message.
    //
    //   (b) The truncation lands at a syntactically valid point — common
    //       for `write`/`edit` whose `content` field happens to close
    //       cleanly mid-body. The args parse, schema validation passes,
    //       guard returns false. The tool would dispatch and write a
    //       half-truncated file, surfacing as either silent corruption
    //       or a confusing tool-runner error that misleads the model
    //       into thinking its argument shape was wrong.
    //
    // Both need the same treatment: fail the tool with a message that
    // names the real cause (max_tokens cap) and points the model at
    // the actionable workaround (smaller payload / `edit` over `write` /
    // multiple calls). Don't retry — the budget is the same next time
    // and the model would burn it the same way. The retry guard below
    // already enforces this via `!max_tokens_hit`.
    //
    // We force-fail *every* still-pending tool here, not just ones
    // guard caught, to cover case (b) cleanly. Tools already in a
    // terminal state (Done from an earlier sub-turn, etc.) are left
    // alone.
    if (max_tokens_hit
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        constexpr std::string_view kMaxTokensExplanation =
            "Output token cap (max_tokens) was reached before the tool "
            "input finished streaming, so the call was cut off. Even if "
            "the args parsed, the body is likely truncated. Retry with a "
            "smaller payload: prefer `edit` over `write` for long files, "
            "or split the change across multiple calls.";
        const auto now = std::chrono::steady_clock::now();
        for (auto& tc : m.d.current.messages.back().tool_calls) {
            if (tc.is_pending()) {
                tc.status = ToolUse::Failed{
                    tc.started_at(), now, std::string{kMaxTokensExplanation}};
            } else if (auto* f = std::get_if<ToolUse::Failed>(&tc.status);
                       f && f->output.starts_with("Tool call arguments look incomplete")) {
                // guard_truncated_tool_args already failed this one with
                // a generic message. Upgrade it to the max_tokens-aware
                // version so the model gets a single coherent story.
                f->output.assign(kMaxTokensExplanation);
            }
        }
        (void)any_truncated;  // both (a) and (b) covered above
    }

    // Transparent retry on upstream truncation — libcurl's TCP keepalive
    // can't prevent an edge LB from closing an idle connection. When that
    // happens mid-tool-input we silently re-launch on the same context,
    // capped at kMaxTruncationRetries.
    if (auto* a = active_ctx(m.s.phase);
        a && any_truncated
        && !max_tokens_hit
        && a->truncation_retries < kMaxTruncationRetries
        && !m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        auto& last = m.d.current.messages.back();
        const bool has_committed_work =
            !last.text.empty() ||
            std::ranges::any_of(last.tool_calls, [](const auto& tc) {
                return tc.is_done() || tc.is_running();
            });
        if (!has_committed_work) {
            ++a->truncation_retries;
            m.d.current.messages.pop_back();
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.d.current.messages.push_back(std::move(placeholder));
            // Streaming → Streaming (truncation retry). Reuse the
            // same ctx — the just-incremented truncation_retries
            // counter persists so the kMaxTruncationRetries cap
            // works across retries within the turn.
            auto ctx = take_active_ctx(std::move(m.s.phase)).value();
            m.s.phase = phase::Streaming{std::move(ctx)};
            m.s.status = "retrying (upstream cut off)…";
            return cmd::launch_stream(m);
        }
    }

    // Retry budget exhausted (or max_tokens hit, or committed work
    // blocked the retry) and a tool is still flagged mid-string
    // truncated. We left it Pending earlier so the retry loop could
    // see it; now surface the final Failed message so the model gets
    // a coherent terminal story instead of a still-Pending tool that
    // kick_pending_tools would dispatch with empty args.
    if (!m.d.current.messages.empty()
        && m.d.current.messages.back().role == Role::Assistant) {
        const auto now_ts = std::chrono::steady_clock::now();
        for (auto& tc : m.d.current.messages.back().tool_calls) {
            if (tc.stream_mid_string_truncated && tc.is_pending()) {
                tc.status = ToolUse::Failed{
                    tc.started_at(), now_ts,
                    "tool args truncated mid-string \u2014 the wire cut "
                    "off inside a string value (likely `content` / "
                    "`command` / `new_text`), so the body is incomplete "
                    "and the call was refused. Re-emit the tool with the "
                    "full payload — prefer `edit` over `write` for long "
                    "files, or split the change across multiple calls."};
            }
            tc.stream_mid_string_truncated = false;
        }
    }

    // Bump the rapid-refill breaker's turn counter on every settled
    // assistant turn (whether or not a compact happened this round).
    // The compact-finalize branch above resets it to 0; non-compact
    // turns increment it, so a quiet stretch eventually re-arms
    // auto-compaction by letting `recent_compacts` reset on the next
    // trigger. Cap at INT_MAX/2 to avoid overflow on very long sessions.
    if (m.s.turns_since_last_compact < 1000000) ++m.s.turns_since_last_compact;
    if (m.s.autocompact_disabled
        && m.s.turns_since_last_compact > 10) {
        // Long quiet stretch — re-enable auto-compact. The user has
        // either stopped triggering huge tool outputs or the
        // conversation naturally drifted away from the thrash trigger.
        m.s.autocompact_disabled = false;
        m.s.recent_compacts      = 0;
    }

    deps().save_thread(m.d.current);
    auto kp = cmd::kick_pending_tools(m);
    // Set by the idle-settle block below when the reply carries runnable
    // shell blocks; batched into whichever return path fires.
    Cmd<Msg> block_toast = Cmd<Msg>::none();

    if (m.s.is_idle() && !m.ui.composer.queued.empty()) {
        auto& head = m.ui.composer.queued.front();
        m.ui.composer.text        = std::move(head.text);
        m.ui.composer.attachments = std::move(head.attachments);
        m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
        m.ui.composer.queued.erase(m.ui.composer.queued.begin());
        auto [mm, sub_cmd] = submit_message(std::move(m));
        m = std::move(mm);
        return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
    }

    // Settle freeze. agent_session pushes the assistant Turn into
    // m.frozen at MessageStop; we do the same — once the stream is
    // truly idle (no pending tools, no queued message), commit every
    // message that's settled into m.ui.frozen and let the live tail
    // shrink to empty. From here the next view() reads the turn out
    // of frozen via list_ref (zero-copy) and the live tail draws
    // nothing until the next submit pushes a fresh placeholder.
    if (m.s.is_idle()) {
        // Settle every Assistant message in the live tail NOW — finish()
        // resolves the reveal overlay and locks the rendered height,
        // exactly like agent_session at MessageStop (m.md.finish()). We
        // then set pending_settle_freeze so meta.cpp's Tick performs the
        // actual freeze on the very next frame — by which point one view()
        // has already painted the settled (post-finish) tree into maya's
        // prev_cells, so the live-tail→frozen handoff is a byte-identical
        // cache HIT (zero re-emit, the cheapest possible transition over a
        // slow SSH wire).
        //
        // What changed: the old code armed a 200 ms request_finalize()
        // GLIDE here and left the widget live_, deferring finish() to the
        // Tick once the glide drained. That animation needs dense frames to
        // hold the live height steady; at fps=0 over SSH the sparse ticks
        // let the height drift mid-glide, so the freeze diffed a moved tree
        // against a stale prev_cells and stranded a duplicate turn in
        // scrollback. Finishing immediately removes the animation window.
        for (std::size_t i = m.ui.frozen_through;
             i < m.d.current.messages.size(); ++i) {
            auto& mm = m.d.current.messages[i];
            if (mm.role != Role::Assistant || mm.text.empty()) continue;
            settle_message_md(m, mm);
        }
        // Freeze on the next Tick (meta.cpp), gated on
        // live_tail_reveal_settled() — already true now that we finished,
        // so the freeze fires immediately on the next frame.
        m.ui.pending_settle_freeze = true;

        // Runnable-code nudge. The reply just settled and the user is
        // reading it — if it carries shell blocks, surface the Ctrl+G
        // affordance NOW, while the commands are on screen. Count only
        // RUNNABLE blocks (is_shell_language): a reply that's all
        // python/json snippets shouldn't advertise a run key that would
        // only toast "isn't shell" back. Quiet if the reply has none —
        // the nudge must stay rare enough to keep meaning something.
        if (!m.d.current.messages.empty()
            && m.d.current.messages.back().role == Role::Assistant
            && !m.d.current.messages.back().text.empty()) {
            const auto blocks = code_block_picker::extract_code_blocks(
                m.d.current.messages.back().text);
            int runnable = 0;
            for (const auto& b : blocks)
                if (code_block_picker::is_shell_language(b.language)) ++runnable;
            if (runnable > 0) {
                block_toast = set_status_toast(m,
                    "\xe2\x96\xb6 " + std::to_string(runnable)
                        + (runnable == 1 ? " runnable code block"
                                         : " runnable code blocks")
                        + " \xe2\x80\x94 Ctrl+G to run",
                    std::chrono::seconds{6});
            }
        }
    }

    // Post-turn idle auto-compaction.
    //
    // The turn finished, no pending tools, no queued messages. This
    // is the natural pause moment — the user is reading the model's
    // output, not typing. If the wire estimate is over the threshold
    // (~80% of context_max with output-reserve headroom), kick off
    // compaction in the background. By the time the user types their
    // next message, the wire view has been replaced with a summary
    // and the next request goes out at a fraction of the size.
    //
    // Threshold mirrors the old submit-time formula
    // (context_max - kOutputReserve - kCompactSlack) since the
    // intent is the same: "is the NEXT turn at risk of hitting the
    // ceiling?" The difference is WHEN we check — we check at the
    // post-turn idle boundary, not at submit — so the compaction
    // round happens while the user is reading, not blocking them.
    //
    // Soft-trim on the normal-turn path already handles "the next
    // request would overflow" by silently dropping oldest raw turns,
    // so this trigger is purely a quality optimisation: summary >
    // truncation. If compaction fails (rate-limited, network drop)
    // the user still gets through their next turn via soft-trim.
    if (m.s.is_idle()
        && !m.s.compacting
        && !m.s.autocompact_disabled
        && m.s.context_max > 0
        && m.ui.composer.queued.empty()
        && !m.d.current.messages.empty()) {
        constexpr int kOutputReserve = 13000;
        constexpr int kCompactSlack  = 4000;
        const int threshold = std::max(0,
            m.s.context_max - kOutputReserve - kCompactSlack);
        const int est = cmd::estimate_wire_tokens(m.d.current);
        if (std::max(m.s.tokens_in, est) > threshold) {
            // Dispatch CompactContext as an async Msg so it goes
            // through the same reducer arm /compact uses; that arm
            // is the single source of truth for compaction kickoff
            // (target_index capture, buffer clear, phase transition).
            // The user sees the banner flip to "compacting context…"
            // while they're reading; their next submit (when they
            // get to it) either lands after compaction finishes
            // (queueing for max ~1-2 turn-times) OR Esc-cancels
            // compaction and fires immediately.
            auto compact_cmd = Cmd<Msg>::task(
                [](std::function<void(Msg)> dispatch) {
                    dispatch(CompactContext{});
                });
            return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
                std::move(kp), std::move(block_toast), std::move(compact_cmd)});
        }
    }

    return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{
        std::move(kp), std::move(block_toast)});
}

// ============================================================================
// stream_update — reducer for `msg::StreamMsg`
// ============================================================================
// Every event handler bumps `last_event_at` so the Tick-based stall watchdog
// can tell "stream is alive but quiet" from "stream is stalled."

Step stream_update(Model m, msg::StreamMsg sm) {
    using maya::overload;

    return std::visit(overload{
        [&](StreamStarted) -> Step {
            auto now = std::chrono::steady_clock::now();
            // The phase variant guarantees a non-null ctx when active;
            // StreamStarted only fires after submit_message / retry has
            // already moved us into Streaming.
            if (auto* a = active_ctx(m.s.phase)) {
                a->started        = now;
                a->last_event_at  = now;
                a->retry          = retry::Fresh{};   // fresh stream → re-arm watchdog
                // Reset the live-rate accumulator so each sub-turn
                // (post-tool) measures its own generation speed instead
                // of polluting the CURRENT-rate display with the previous
                // turn's bytes. The sparkline ring (rate_history) lives
                // on StreamState — it carries across sub-turns / tool
                // gaps so the user sees a continuous trace of generation
                // rate over the whole session, not a fresh empty bar
                // after every tool call.
                a->live_delta_bytes       = 0;
                a->first_delta_at         = {};
                a->rate_last_sample_at    = {};
                a->rate_last_sample_bytes = 0;
            }
            // Fresh stream is alive — wipe any leftover toast (retry
            // countdown, "error: …", "cancelled") from the previous
            // attempt so the status row doesn't show a stale message
            // on top of a healthy connection.
            m.s.status.clear();
            m.s.status_until = {};
            return done(std::move(m));
        },
        [&](StreamTextDelta& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) {
                a->last_event_at = now;
                if (!e.text.empty()) {
                    if (a->first_delta_at.time_since_epoch().count() == 0) {
                        a->first_delta_at = now;
                        // First real byte of this stream attempt —
                        // the connection demonstrably works. Reset
                        // the retry budget so a later mid-stream
                        // failure gets a fresh ladder instead of
                        // inheriting whatever attempts the prior
                        // (now-succeeded) connect-time retries
                        // consumed. Without this, an opus turn that
                        // survived 2 transient connect failures and
                        // then hit a mid-stream stall 90 s in could
                        // only retry once before being marked
                        // terminal — even though the wire-level
                        // health was perfect for 90 s of streaming.
                        a->transient_retries = 0;
                    }
                    a->live_delta_bytes += e.text.size();
                }
            }
            // Compaction routing: during a compaction stream the
            // transcript has no synthetic Assistant placeholder to
            // stream into — the summary lives off-transcript in
            // `m.s.compaction_buffer` and only ever surfaces as a
            // CompactionRecord at finalize_turn. Route every byte
            // there instead of into messages.back(), with the same
            // per-stream byte cap so a runaway summary can't OOM.
            if (m.s.compacting) {
                if (m.s.compaction_buffer.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - m.s.compaction_buffer.size();
                    if (e.text.size() <= room) m.s.compaction_buffer += e.text;
                    else m.s.compaction_buffer.append(e.text, 0, room);
                }
                return done(std::move(m));
            }
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                // Append to the smoothing buffer rather than directly to
                // streaming_text — the Tick handler drips it out at a
                // controlled rate so server bursts don't visually jump.
                // Cap is on the COMBINED visible + buffered size so
                // smoothing can't push past the per-message budget.
                auto& msg = m.d.current.messages.back();
                const std::size_t in_flight =
                    msg.streaming_text.size() + msg.pending_stream.size();
                if (in_flight < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - in_flight;
                    // Head-of-stream fast path: while the message has no
                    // visible content yet, append the first bytes DIRECTLY
                    // to streaming_text so they paint on this delta's own
                    // render (fps=0 → every Msg renders). Without this the
                    // first word sits in pending_stream until the next
                    // Tick fires — up to one full tick period (33 ms on
                    // sync terminals, 100 ms elsewhere, more over SSH) —
                    // which reads as "the first word sticks" before the
                    // rest flows. Once there's visible text to smooth
                    // against, subsequent bytes go through the pacer.
                    constexpr std::size_t kHeadReveal = 512;
                    if (msg.streaming_text.empty()
                        && msg.pending_stream.empty()) {
                        const auto head = std::min({e.text.size(), room, kHeadReveal});
                        msg.streaming_text.append(e.text, 0, head);
                        if (head < e.text.size() && head < room) {
                            const auto rest = std::min(e.text.size() - head,
                                                       room - head);
                            msg.pending_stream.append(e.text, head, rest);
                        }
                    } else if (e.text.size() <= room) {
                        msg.pending_stream += e.text;
                    } else {
                        msg.pending_stream.append(e.text, 0, room);
                    }
                }
                // After the head, all bytes flow through the Tick pacer
                // in meta.cpp so server bursts reveal smoothly instead of
                // jumping in.
            }
            return done(std::move(m));
        },
        [&](StreamToolUseStart& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) a->last_event_at = now;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                ToolUse tc;
                tc.id   = e.id;
                tc.name = e.name;
                tc.args = json::object();
                // Stamp start now so the card shows a live timer during the
                // args-streaming phase too — lets the user tell "model hasn't
                // started emitting" from "execution is slow" at a glance.
                tc.status = ToolUse::Pending{now};
                m.d.current.messages.back().tool_calls.push_back(std::move(tc));
                // Args-stream watchdog removed at user request — no
                // automatic Pending → Failed timeout. A tool whose
                // tool_use_start streams in but never gets its End
                // event will stay Pending until the user cancels via
                // Esc.  Stream-level errors still surface via the
                // StreamError handler, which clears the in-flight tools.
            }
            return done(std::move(m));
        },
        [&](StreamToolUseDelta& e) -> Step {
            auto now = std::chrono::steady_clock::now();
            if (auto* a = active_ctx(m.s.phase)) {
                a->last_event_at = now;
                if (!e.partial_json.empty()) {
                    if (a->first_delta_at.time_since_epoch().count() == 0) {
                        a->first_delta_at = now;
                        // See StreamTextDelta — same rationale.
                        a->transient_retries = 0;
                    }
                    a->live_delta_bytes += e.partial_json.size();
                }
            }
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Bounded append — beyond the cap we drop further bytes so
                // the salvage path at StreamToolUseEnd runs on whatever
                // scalars sniffed cleanly.
                if (tc.args_streaming.size() < kMaxStreamingBytes) {
                    const auto room = kMaxStreamingBytes - tc.args_streaming.size();
                    if (e.partial_json.size() <= room) tc.args_streaming += e.partial_json;
                    else tc.args_streaming.append(e.partial_json, 0, room);
                }
                // Throttle the live preview. First delta runs unconditionally
                // so the header paints immediately, then subsequent re-parses
                // are spaced ~120 ms. StreamToolUseEnd always runs the full
                // parse, so the final state is exact.
                using clock = std::chrono::steady_clock;
                constexpr auto kPreviewInterval = std::chrono::milliseconds{120};
                auto now2 = clock::now();
                if (tc.last_preview_at.time_since_epoch().count() == 0
                    || now2 - tc.last_preview_at >= kPreviewInterval) {
                    update_stream_preview(tc);
                    tc.last_preview_at = now2;
                    // Live plan sync: a `todo` call mirrors its partial
                    // todos[] into tc.args during the preview above; push
                    // that into the persistent plan state so the modal /
                    // global indicator track the in-progress item the
                    // instant the model writes it, not only at exec output.
                    if (tc.name == "todo")
                        sync_todo_state_from_args(m, tc.args);
                }
            }
            return done(std::move(m));
        },
        [&](StreamToolUseEnd) -> Step {
            if (auto* a = active_ctx(m.s.phase))
                a->last_event_at = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant
                && !m.d.current.messages.back().tool_calls.empty()) {
                auto& tc = m.d.current.messages.back().tool_calls.back();
                // Empty args_streaming is legitimate for argumentless tools;
                // args was seeded to {} at StreamToolUseStart.
                if (!tc.args_streaming.empty()) {
                    try {
                        tc.args = json::parse(tc.args_streaming);
                        tc.mark_args_dirty();
                        std::string{}.swap(tc.args_streaming);
                    } catch (const std::exception& ex) {
                        // Parse failed — typically an SSE cutoff mid-content.
                        // Salvage whatever scalar fields we can so the tool
                        // still has a shot at running instead of nuking the
                        // whole turn.  salvage_args refuses to salvage when
                        // the wire stopped mid-string (Finding 3) since
                        // close_partial_json would synthesise a closing
                        // quote on a half-written body — in that case we
                        // surface a clear truncated-mid-string failure
                        // instead of "invalid tool arguments".
                        const bool mid_string =
                            agentty::tools::util::ended_inside_string(
                                tc.args_streaming);
                        auto salvaged = salvage_args(tc);
                        if (!salvaged.empty()) {
                            tc.args = std::move(salvaged);
                            tc.mark_args_dirty();
                            std::string{}.swap(tc.args_streaming);
                        } else if (mid_string) {
                            // Don't eagerly fail. Mark the truncation
                            // and leave the tool Pending so
                            // finalize_turn's retry loop can pop the
                            // placeholder and silently re-launch on
                            // the same ctx — a fresh tool-use stream
                            // is the only thing that can salvage a
                            // mid-string cutoff. Args_streaming is
                            // kept until the retry path pops it.
                            tc.stream_mid_string_truncated = true;
                            std::string{}.swap(tc.args_streaming);
                        } else {
                            auto now = std::chrono::steady_clock::now();
                            tc.status = ToolUse::Failed{
                                tc.started_at(), now,
                                std::string{"invalid tool arguments: "} + ex.what()
                                    + "\nraw: " + tc.args_streaming};
                            std::string{}.swap(tc.args_streaming);
                        }
                    }
                }
                // Required-field check is deferred to finalize_turn so the
                // turn-level retry logic owns the single decision point.
            }
            return done(std::move(m));
        },
        [&](StreamThinkingDelta& e) -> Step {
            // Adaptive-thinking block delta. Two shapes arrive: a
            // `thinking_delta` carries reasoning text (usually empty under
            // display:omitted), a `signature_delta` carries the opaque
            // per-block signature near the block's end. Accumulate both onto
            // the in-flight assistant message so the block can be replayed
            // verbatim next turn — Anthropic 400s a tool_use turn whose
            // thinking block was dropped.
            if (auto* a = active_ctx(m.s.phase)) {
                // Liveness: a thinking delta proves the wire is alive during
                // long silent reasoning passes. Bump last_event_at and reset
                // the retry budget exactly like the heartbeat path so a
                // connect→think→stall sequence gets a fresh ladder.
                a->last_event_at = std::chrono::steady_clock::now();
                a->transient_retries = 0;
            }
            // Captured for replay only — never rendered, and there's no
            // assistant placeholder to attach to during compaction.
            if (m.s.compacting) return done(std::move(m));
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& msg = m.d.current.messages.back();
                if (!e.text.empty())      msg.thinking          += e.text;
                if (!e.signature.empty()) msg.thinking_signature = e.signature;
            }
            return done(std::move(m));
        },
        [&](StreamUsage& e) -> Step {
            if (auto* a = active_ctx(m.s.phase))
                a->last_event_at = std::chrono::steady_clock::now();
            // Suppress token writes during compaction. The compaction
            // stream's `input_tokens` reflects the SUMMARISATION request
            // (full prefix + the verbose summarisation prompt) and its
            // `output_tokens` reflects the model's summary length —
            // neither belongs in the user-facing context gauge or the
            // "tokens generated" counter. Without this guard the gauge
            // briefly jumps to the wire-payload size mid-compaction and
            // resets to 0 in finalize_turn, producing a visible flicker
            // on the status bar. The PRIOR turn's values are still on
            // m.s.tokens_in/out from before compaction started; leaving
            // them in place gives a more honest "what's the model
            // actually carrying right now" reading until the next real
            // turn settles.
            if (m.s.compacting) return done(std::move(m));
            // `input_tokens` from Anthropic is the FULL prefix for this
            // request, NOT the delta. Accumulating across turns triple-counted
            // by turn 5. Replace, don't add. Cache fields are excluded from
            // `input_tokens` per the API but still occupy the context window,
            // so the true "tokens in context" is the sum.
            if (e.input_tokens || e.cache_read_input_tokens
                || e.cache_creation_input_tokens) {
                m.s.tokens_in = e.input_tokens
                                   + e.cache_read_input_tokens
                                   + e.cache_creation_input_tokens;
            }
            if (e.output_tokens) m.s.tokens_out = e.output_tokens;
            return done(std::move(m));
        },
        [&](StreamHeartbeat) -> Step {
            // Wire-alive signal from the transport (SSE `ping` or
            // `thinking_delta`). No payload, no UI effect — we just
            // bump last_event_at so the stall watchdog knows the
            // connection is healthy. Critical during extended-thinking
            // passes where the model reasons silently for 60-120 s
            // between visible deltas; without this the watchdog would
            // fire on every non-trivial opus turn.
            if (auto* a = active_ctx(m.s.phase)) {
                a->last_event_at = std::chrono::steady_clock::now();
                // A heartbeat proves the wire is alive even before any
                // content delta. Reset the retry budget so a stream
                // that connects, pings, then stalls before its first
                // byte gets a fresh ladder instead of inheriting the
                // connect-time attempts. This is the pre-delta analogue
                // of the first-delta reset in StreamTextDelta and the
                // primary fix for sessions that latched terminal after
                // a run of healthy-but-stalled attempts.
                a->transient_retries = 0;
            }
            return done(std::move(m));
        },
        [&](StreamFinished e) -> Step {
            auto cmd = finalize_turn(m, e.stop_reason);
            // No force_redraw arming. The previous version armed
            // needs_force_redraw here so the next user input would
            // trigger maya's case-(B) soft redraw — meant to flush
            // any prev_cells/wire desync left by streaming. That
            // desync's only real source was the shrink path's
            // \r\n\x1b[2K loop scrolling at viewport bottom, which is
            // now a single \x1b[J in maya/src/render/serialize.cpp;
            // prev_cells stays in sync with the wire on its own.
            //
            // Re-arming case (B) on every first keypress was actively
            // harmful: when the shrink left the cursor mid-viewport
            // (cursor at content_rows - 1 < term_h - 1), case (B)
            // pulled the cursor back to viewport bottom, shifting the
            // canvas-to-buffer mapping by the same delta. The first
            // re-emitted row landed one buffer row below its original
            // streaming position, so the original row stayed stranded
            // in scrollback as a duplicate of the row right at viewport
            // top. With the arming gone, the keypress takes the normal
            // diff path and the composer stays where the shrink left
            // it (no "pull down," no duplicate).
            return {std::move(m), std::move(cmd)};
        },
        [&](StreamError& e) -> Step {
            // Dedupe: when the stall watchdog fired, it tripped the
            // cancel token, which causes the worker thread to unwind
            // and emit its own StreamError("cancelled") shortly after.
            // The first error already scheduled a retry; ignore any
            // subsequent ones that arrive before that retry runs,
            // otherwise we'd race two worker threads into the same
            // session.
            if (m.s.in_scheduled()) return done(std::move(m));

            // Compaction failed mid-flight (rate limit, network drop,
            // etc.). Compaction is wire-only — nothing was ever pushed
            // into `messages` — so we don't have to rewind any transcript
            // state. Either retry on a backoff (transient / rate-limit
            // with budget remaining) or surface a clear actionable toast
            // and return to Idle. Either way the user-visible transcript
            // is identical to its pre-compaction shape because it was
            // never mutated.
            //
            // Workflow protection: retry transparently on transient and
            // rate-limit failures so a flaky network doesn't force the
            // user to babysit /compact. The summarisation request is
            // stateless from the server's POV — every retry rebuilds
            // the same wire payload from the immutable transcript — so
            // there's nothing to corrupt across attempts. On terminal
            // failure (auth, content filter, retry budget exhausted)
            // we drop to Idle, clear the buffer, and rely on the
            // launch_stream soft-trim to keep subsequent NORMAL turns
            // working even though compaction couldn't finish. The
            // user can re-invoke /compact later when conditions improve.
            if (m.s.compacting) {
                if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

                auto klass = provider::classify(e.message);
                if (e.from_stall
                    && klass == provider::ErrorClass::Cancelled) {
                    klass = provider::ErrorClass::Transient;
                }

                const phase::Active* cctx = active_ctx(m.s.phase);
                int prior = cctx ? cctx->transient_retries : 0;
                // Same budget-decay as the normal path: a compaction
                // that failed long after the last failure starts fresh.
                {
                    auto cnow = std::chrono::steady_clock::now();
                    if (cctx
                        && cctx->last_failure_at.time_since_epoch().count() != 0
                        && cnow - cctx->last_failure_at >= provider::kRetryDecayWindow) {
                        prior = 0;
                    }
                }
                const bool can_retry_compact =
                    cctx
                    && (klass == provider::ErrorClass::Transient
                        || klass == provider::ErrorClass::RateLimit)
                    && prior < provider::kMaxRetries;

                if (can_retry_compact) {
                    std::chrono::milliseconds delay;
                    if (e.retry_after.has_value()) {
                        auto s = e.retry_after->count();
                        if (s < 1) s = 1;
                        if (s > 600) s = 600;   // honor server backoff; bound garbage
                        delay = std::chrono::seconds(s);
                    } else {
                        delay = provider::backoff_with_jitter(klass, prior);
                    }
                    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                        delay + std::chrono::milliseconds{999}).count();
                    // Keep the user-facing wording calm: "context
                    // compacting" is already on the banner; we just
                    // append the retry hint so the user knows the
                    // operation is still alive, not silently wedged.
                    m.s.status = "compacting — retrying in " + std::to_string(secs) + "s";
                    m.s.status_until = std::chrono::steady_clock::now()
                                     + delay + std::chrono::milliseconds{1500};
                    // Reset only the streamed bytes — keep `compacting`
                    // and `compaction_target_index` so launch_stream
                    // rebuilds the same compaction request shape on
                    // RetryStream.
                    m.s.compaction_buffer.clear();
                    auto ctx = take_active_ctx(std::move(m.s.phase)).value();
                    ctx.transient_retries = prior + 1;
                    ctx.last_failure_at   = std::chrono::steady_clock::now();
                    ctx.retry             = retry::Scheduled{};
                    m.s.phase = phase::Streaming{std::move(ctx)};
                    return {std::move(m),
                            Cmd<Msg>::after(delay, Msg{RetryStream{}})};
                }

                // Terminal compaction failure. Drop everything compaction-
                // related and surface ONE clear toast. Soft-trim on the
                // normal-turn path keeps the agent working at reduced
                // wire size; the user can /compact again later.
                m.s.compacting              = false;
                m.s.compaction_target_index = 0;
                m.s.compaction_buffer.clear();
                m.s.phase = phase::Idle{};
                m.s.status = (klass == provider::ErrorClass::Cancelled)
                    ? "compaction cancelled"
                    : ("compaction failed: " + e.message
                       + " — retry with /compact");
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{
                    klass == provider::ErrorClass::Cancelled ? 3 : 8};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                // Drain queued messages even though compaction failed —
                // launch_stream's soft-trim will fit them onto the
                // wire. Better to give the user their reply than to
                // leave their typing stuck behind a dead compaction.
                if (!m.ui.composer.queued.empty()) {
                    auto& head = m.ui.composer.queued.front();
                    m.ui.composer.text        = std::move(head.text);
                    m.ui.composer.attachments = std::move(head.attachments);
                    m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
                    m.ui.composer.queued.erase(m.ui.composer.queued.begin());
                    auto [mm, sub_cmd] = submit_message(std::move(m));
                    m = std::move(mm);
                    return {std::move(m), Cmd<Msg>::batch(
                        std::move(status_cmd), std::move(sub_cmd))};
                }
                return {std::move(m), std::move(status_cmd)};
            }

            // Worker thread is unwinding; drop the token so the next turn
            // (or scheduled retry) mints a fresh one. Reaches into the
            // ctx (rather than dropping the whole ctx) because we may
            // still need the retry counters for the can_retry check.
            if (auto* a = active_ctx(m.s.phase)) a->cancel.reset();

            // Drain pacer buffer + streaming_text into the committed
            // body so a retry or terminal path doesn't lose received
            // bytes. Pre-settle StreamingMarkdown so the message's
            // rendered height is locked before the next view() — lazy
            // finish() during view can shift assistant height by 1+
            // rows when a closing ``` commits the tail into a prefix
            // block, which disturbs the scrollback↔viewport seam.
            Message* last = nullptr;
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                last = &m.d.current.messages.back();
                if (!last->pending_stream.empty()) {
                    last->streaming_text += last->pending_stream;
                    last->pending_stream.clear();
                }
                if (!last->streaming_text.empty()) {
                    if (last->text.empty()) last->text = std::move(last->streaming_text);
                    else                    last->text += std::move(last->streaming_text);
                    std::string{}.swap(last->streaming_text);
                }
                if (!last->text.empty()) {
                    auto& cache = m.ui.view_cache.message_md(
                        m.d.current.id, last->id);
                    if (!cache.streaming)
                        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
                    cache.streaming->set_content(last->text);
                    cache.streaming->finish();
                }
            }

            // Classify and decide retry vs terminal.
            auto klass = provider::classify(e.message);
            // A "cancelled" error is reclassified to Transient only when WE
            // cancelled the stream via the stall watchdog (not the user).
            // Two shapes carry that intent:
            //   • the synthetic stall StreamError itself (e.from_stall), or
            //   • the worker thread unwinding from the cancel the watchdog
            //     tripped THIS turn — recognised by the ctx still sitting in
            //     StallFired (the synthetic error hasn't scheduled a retry
            //     yet, so we haven't moved to Scheduled/Fresh). Using the
            //     flag for the synthetic error means a later turn re-entering
            //     StallFired can't retroactively reclassify an unrelated
            //     user-cancel from a different turn.
            if ((e.from_stall || m.s.in_stall_fired())
                && klass == provider::ErrorClass::Cancelled) {
                klass = provider::ErrorClass::Transient;
            }

            // "Committed work" gating for retry: only Done/Running tool
            // calls + finalized text body block a retry. A Pending tool
            // (StreamToolUseStart fired, args may have been mid-streaming
            // when the stall hit) is NOT committed — re-running gives
            // the model a chance to re-emit it cleanly. Same definition
            // the truncation-retry path uses (see above).
            bool has_committed = false;
            if (last) {
                has_committed = !last->text.empty() ||
                    std::ranges::any_of(last->tool_calls, [](const auto& tc) {
                        return tc.is_done() || tc.is_running();
                    });
            }
            const phase::Active* err_ctx = active_ctx(m.s.phase);
            int prior_transient = err_ctx ? err_ctx->transient_retries : 0;
            // Budget decay: if the previous failure on this ctx was
            // longer ago than kRetryDecayWindow, the wire was healthy in
            // between (no failure, no stall for that whole window), so
            // this failure starts a fresh ladder instead of inheriting
            // an unrelated brown-out from earlier in a long turn. Keeps
            // a multi-hour session from accumulating sporadic blips into
            // a permanent terminal latch.
            {
                auto now = std::chrono::steady_clock::now();
                if (err_ctx
                    && err_ctx->last_failure_at.time_since_epoch().count() != 0
                    && now - err_ctx->last_failure_at >= provider::kRetryDecayWindow) {
                    prior_transient = 0;
                }
            }

            // ── Auth (401/403): try a one-shot OAuth refresh ─────────
            // The bearer token expired (or was rotated) mid-session.
            // Deps still holds the stale header; load creds from disk
            // to recover the refresh_token, kick off the same
            // background refresh `init()` uses on startup, and park the
            // stream ctx in retry::Scheduled. The TokenRefreshed handler
            // dispatches RetryStream on success (which re-launches with
            // the freshly-installed bearer) or drops to terminal on
            // failure, surfacing the existing "run 'agentty login'"
            // hint exactly as before.
            //
            // Gated on err_ctx (can't retry from Idle), !has_committed
            // (the model already produced visible output — a refresh-
            // and-retry would duplicate it), and oauth_refresh_in_flight
            // not already set (avoid stacking refreshes). One refresh
            // per turn: counted against transient_retries so a server
            // that keeps returning 401 after a successful refresh
            // surfaces as terminal rather than looping forever.
            if (klass == provider::ErrorClass::Auth
                && err_ctx
                && !has_committed
                && !m.s.oauth_refresh_in_flight
                && prior_transient < provider::kMaxRetries) {
                std::string refresh_token;
                if (auto loaded = auth::load_credentials()) {
                    if (auto* o = std::get_if<auth::cred::OAuth>(&*loaded))
                        refresh_token = o->refresh_token;
                }
                if (!refresh_token.empty()) {
                    // cancel token was already reset at the top of this
                    // handler; we just rebuild the ctx with bumped
                    // counters and the Scheduled retry sentinel.
                    auto ctx = take_active_ctx(std::move(m.s.phase)).value();
                    ctx.transient_retries = prior_transient + 1;
                    ctx.last_failure_at   = std::chrono::steady_clock::now();
                    ctx.retry             = retry::Scheduled{};
                    m.s.phase = phase::Streaming{std::move(ctx)};
                    if (last) m.d.current.messages.pop_back();
                    Message placeholder;
                    placeholder.role = Role::Assistant;
                    m.d.current.messages.push_back(std::move(placeholder));
                    m.s.oauth_refresh_in_flight = true;
                    m.s.status = "auth expired \xE2\x80\x94 refreshing token\xE2\x80\xA6";
                    m.s.status_until = {};
                    auto refresh_cmd = cmd::refresh_oauth(std::move(refresh_token));
                    // No force_redraw needed: the StreamingMarkdown
                    // pre-settle above already locks the message's
                    // height; the next render takes the normal diff
                    // path. (Inline force_redraw is safe here — just
                    // a soft case-(B) redraw, not destructive — but
                    // also unnecessary.)
                    return {std::move(m), std::move(refresh_cmd)};
                }
                // No refresh_token on disk (env-var OAuth, api-key with
                // a stale Bearer, etc.) — fall through to the terminal
                // path so the user gets the actionable login hint.
            }

            // Mid-stream signal for the per-class retry cap. A failure is
            // "mid-stream" if the wire proved itself alive this turn before
            // dying: the stall watchdog fired (it only fires after a healthy
            // connect), OR this ctx had already seen a content delta
            // (first_delta_at set). Those get a SHORT retry budget (see
            // provider::max_retries_for) because a wire that keeps cutting
            // out after reaching us is a real outage, not a reconnect blip
            // — re-hammering it 6× just spams the banner. A clean connect
            // failure (no delta, no stall) keeps the full budget since a
            // fresh connection almost always succeeds.
            const bool mid_stream =
                m.s.in_stall_fired()
                || (err_ctx
                    && err_ctx->first_delta_at.time_since_epoch().count() != 0);
            const int retry_cap = provider::max_retries_for(klass, mid_stream);

            // For mid-stream failures, gate on `mid_stream_failures` rather
            // than `transient_retries`. The latter is reset to 0 by any
            // heartbeat or first content delta, so a wire that delivers one
            // byte/ping before each drop would reset its budget every attempt
            // and retry forever. `mid_stream_failures` is monotonic per turn
            // (never reset by wire-health signals), so the short mid-stream
            // cap actually latches terminal on a flapping connection.
            const int mid_prior = err_ctx ? err_ctx->mid_stream_failures : 0;
            const bool budget_left =
                mid_stream ? (mid_prior      < retry_cap)
                           : (prior_transient < retry_cap);

            bool can_retry = (klass == provider::ErrorClass::Transient
                           || klass == provider::ErrorClass::RateLimit)
                          && budget_left
                          && !has_committed
                          && err_ctx;   // can't retry from Idle (no ctx)

            if (can_retry) {
                int attempt = prior_transient;
                std::chrono::milliseconds delay;
                if (e.retry_after.has_value()) {
                    auto s = e.retry_after->count();
                    if (s < 1)   s = 1;
                    // Honor the server's backoff. Cap at 10 min only to bound
                    // a pathological/garbage value — a 300 s Retry-After must
                    // be respected, not silently shortened to 120 s (which
                    // would re-hit the same 429 and burn the budget faster
                    // than the server permits).
                    if (s > 600) s = 600;
                    delay = std::chrono::seconds(s);
                } else {
                    delay = provider::backoff_with_jitter(klass, attempt);
                }
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    delay + std::chrono::milliseconds{999}).count();
                // The attempt counter shown to the user reflects whichever
                // budget actually gates this failure.
                const int shown_attempt = mid_stream ? mid_prior : prior_transient;
                m.s.status = std::string{provider::to_string(klass)}
                           + " — retrying in " + std::to_string(secs) + "s"
                           + " (attempt " + std::to_string(shown_attempt + 1)
                           + "/" + std::to_string(retry_cap) + ")…";
                m.s.status_until = std::chrono::steady_clock::now()
                                 + delay + std::chrono::milliseconds{1500};
                auto ctx = take_active_ctx(std::move(m.s.phase)).value();
                ctx.transient_retries = attempt + 1;
                if (mid_stream) ctx.mid_stream_failures = mid_prior + 1;
                ctx.last_failure_at   = std::chrono::steady_clock::now();
                ctx.retry             = retry::Scheduled{};
                m.s.phase = phase::Streaming{std::move(ctx)};
                if (last) m.d.current.messages.pop_back();
                Message placeholder;
                placeholder.role = Role::Assistant;
                m.d.current.messages.push_back(std::move(placeholder));
                auto retry_cmd = Cmd<Msg>::after(delay, Msg{RetryStream{}});
                // No force_redraw needed: the pre-settle above already
                // finalised StreamingMarkdown so its height is locked
                // before the retry stream feeds new deltas; the normal
                // diff path handles the rest. (Inline force_redraw is
                // safe — soft case-(B) — but unnecessary here.)
                return {std::move(m), std::move(retry_cmd)};
            }

            // Terminal path — discard the source ctx and drop to Idle.
            m.s.phase = phase::Idle{};
            if (klass == provider::ErrorClass::Cancelled) {
                m.s.status = "cancelled";
            } else {
                m.s.status = "error: " + e.message;
            }
            if (last) {
                if (klass != provider::ErrorClass::Cancelled)
                    last->error = e.message;
                std::string fail_msg = "stream ended before tool args "
                                       "closed: " + e.message;
                auto contains_ci = [](std::string_view hay,
                                      std::string_view needle) noexcept {
                    if (needle.size() > hay.size()) return false;
                    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                        bool ok = true;
                        for (std::size_t j = 0; j < needle.size(); ++j) {
                            char a = hay[i + j], b = needle[j];
                            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
                            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
                            if (a != b) { ok = false; break; }
                        }
                        if (ok) return true;
                    }
                    return false;
                };
                if (contains_ci(e.message, "filtering policy")
                    || contains_ci(e.message, "content filter")
                    || contains_ci(e.message, "blocked by content")) {
                    fail_msg =
                        "Anthropic's safety classifier blocked the tool "
                        "input mid-stream (\"" + e.message + "\"). This "
                        "is the upstream policy on the OAuth path "
                        "tripping on generated content; it is "
                        "probabilistic. Try once more if the content is "
                        "innocuous (lorem ipsum, JSON, code) — most "
                        "false positives clear on retry. If it blocks "
                        "again, write a short stub file via `write` and "
                        "build the rest with successive `edit` calls. "
                        "(Direct API-key callers usually bypass this "
                        "filter entirely; a long chain of safety "
                        "blocks on OAuth is a hint to switch auth.)";
                }
                for (auto& tc : last->tool_calls) {
                    if (tc.is_pending()) {
                        auto now = std::chrono::steady_clock::now();
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now, fail_msg};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
            }
            {
                auto now = std::chrono::steady_clock::now();
                auto ttl = std::chrono::seconds{
                    klass == provider::ErrorClass::Cancelled ? 3 : 6};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                return {std::move(m), std::move(status_cmd)};
            }
        },
        [&](RetryStream) -> Step {
            // Scheduled retry fired. If the user cancelled during the
            // wait (Esc → CancelStream dropped phase to Idle), do
            // nothing. Otherwise transition retry back to Fresh on
            // the in-flight ctx so the freshly-launched stream's own
            // errors flow through the normal classifier path.
            if (auto* a = active_ctx(m.s.phase)) a->retry = retry::Fresh{};
            if (m.s.is_idle()) return done(std::move(m));
            return {std::move(m), cmd::launch_stream(m)};
        },
        [&](CancelStream) -> Step {
            // Esc — full synchronous teardown.
            if (auto* a = active_ctx(m.s.phase); a && a->cancel) a->cancel->cancel();

            // Compaction-cancel: nothing was ever appended to the
            // transcript, so we just discard the off-transcript buffer
            // and clear the in-flight flag. The user sees their full
            // pre-compaction transcript stay intact.
            //
            // Workflow protection: a message queued behind the
            // cancelled compaction would otherwise sit dormant until
            // the user manually re-submits something. Drain it here
            // so cancellation feels like "keep going without compacting"
            // rather than "freeze my whole session." launch_stream's
            // soft-trim will fit the wire payload even though we
            // skipped the summarisation.
            const bool was_compacting = m.s.compacting;
            if (m.s.compacting) {
                m.s.compacting              = false;
                m.s.compaction_target_index = 0;
                m.s.compaction_buffer.clear();
            }

            // Salvage partial assistant work and finalise in-flight tool calls.
            auto now = std::chrono::steady_clock::now();
            if (!m.d.current.messages.empty()
                && m.d.current.messages.back().role == Role::Assistant) {
                auto& last = m.d.current.messages.back();
                if (!last.pending_stream.empty()) {
                    last.streaming_text += last.pending_stream;
                    last.pending_stream.clear();
                }
                if (!last.streaming_text.empty()) {
                    if (last.text.empty()) last.text = std::move(last.streaming_text);
                    else                   last.text += std::move(last.streaming_text);
                    std::string{}.swap(last.streaming_text);
                }
                // Pre-settle StreamingMarkdown to lock the message's
                // height before the next view() runs. See finalize_turn
                // and StreamError for the full rationale.
                if (!last.text.empty()) {
                    auto& cache = m.ui.view_cache.message_md(
                        m.d.current.id, last.id);
                    if (!cache.streaming)
                        cache.streaming = std::make_shared<maya::StreamingMarkdown>();
                    cache.streaming->set_content(last.text);
                    cache.streaming->finish();
                }
                for (auto& tc : last.tool_calls) {
                    if (!tc.is_terminal()) {
                        tc.status = ToolUse::Failed{
                            tc.started_at(), now, "cancelled"};
                    }
                    std::string{}.swap(tc.args_streaming);
                }
                if (last.text.empty() && last.tool_calls.empty()) {
                    m.d.current.messages.pop_back();
                }
            }

            m.d.pending_permission.reset();
            m.s.phase = phase::Idle{};
            m.s.status = "cancelled";
            {
                auto ttl = std::chrono::seconds{3};
                m.s.status_until = now + ttl;
                auto stamp = m.s.status_until;
                auto status_cmd = Cmd<Msg>::after(
                    std::chrono::duration_cast<std::chrono::milliseconds>(ttl)
                        + std::chrono::milliseconds{50},
                    Msg{ClearStatus{stamp}});
                // If the user cancelled a COMPACTION (not a normal
                // turn) and had queued work behind it, fire that
                // queued message immediately. Cancelling a normal
                // turn leaves the queue alone because the user just
                // killed their own in-flight request — they probably
                // want to type something different next, not have the
                // queue automatically drain. Compaction was a side
                // operation the user didn't ask for in the first
                // place; killing it shouldn't penalise their actual
                // request.
                if (was_compacting && !m.ui.composer.queued.empty()) {
                    auto& head = m.ui.composer.queued.front();
                    m.ui.composer.text        = std::move(head.text);
                    m.ui.composer.attachments = std::move(head.attachments);
                    m.ui.composer.cursor      = static_cast<int>(m.ui.composer.text.size());
                    m.ui.composer.queued.erase(m.ui.composer.queued.begin());
                    auto [mm, sub_cmd] = submit_message(std::move(m));
                    m = std::move(mm);
                    return {std::move(m), Cmd<Msg>::batch(
                        std::move(status_cmd), std::move(sub_cmd))};
                }
                return {std::move(m), std::move(status_cmd)};
            }
        },
    }, sm);
}

} // namespace agentty::app::detail
