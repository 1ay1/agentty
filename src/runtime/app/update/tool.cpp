// Tool-execution-result helpers: apply_tool_output translates a
// ToolExecOutput into a Done/Failed status on the matching ToolUse;
// mark_tool_rejected is the symmetric one-liner for permission denial.
// Both walk m.d.current.messages because a ToolCallId is only locally
// unique within a turn — we don't index them.

#include "agentty/runtime/app/update/internal.hpp"
#include "agentty/runtime/app/update.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>
#include <maya/app/app.hpp>   // maya::request_animation_frame
#include <nlohmann/json.hpp>

#include "agentty/runtime/app/cmd_factory.hpp"
#include "agentty/runtime/app/deps.hpp"
#include "agentty/diff/diff.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/spec.hpp"

namespace agentty::app::detail {

using json = nlohmann::json;

namespace {

// Per-tool-output ceiling carried in the conversation. Tool runners
// already cap their own captures (read = 1 MiB, grep = 8 MiB, bash =
// 30 KB, etc.), but those are tuned for fidelity inside one call.
// For long-lived process memory, a uniform conversation-side cap is
// the right knob: a session with 50 grep results at near-cap was
// 400+ MB of *terminal* tool output we never trim. Head + tail
// keeps the model-relevant context (top of file, last error, etc.)
// and lets a multi-MiB capture compact to ~256 KiB.
constexpr std::size_t kStoredOutputCap = 256u * 1024u; // 256 KiB

std::string clamp_output(std::string s) {
    if (s.size() <= kStoredOutputCap) return s;
    constexpr std::size_t half = kStoredOutputCap / 2;
    std::string out;
    out.reserve(kStoredOutputCap + 64);
    out.append(s, 0, half);
    out.append("\n\n… (");
    out.append(std::to_string((s.size() - kStoredOutputCap) / 1024));
    out.append(" KiB elided to keep memory bounded) …\n\n");
    out.append(s, s.size() - half, half);
    return out;
}

// Once a tool reaches a terminal state, the per-call streaming
// scratch buffers (raw delta accumulator, lazy args.dump cache) are
// dead weight — the wire is closed, the args are parsed, and any
// re-render comes from `args` directly. Free them so a long session
// doesn't pin one copy per finished tool call.
void release_streaming_buffers(ToolUse& tc) {
    std::string{}.swap(tc.args_streaming);
    tc.args_dump_cache.clear();
    tc.args_dump_cache.shrink_to_fit();
    tc.args_dump_valid = false;
}

// Keep the render clock ticking for a bounded window so maya gets the
// follow-up frames its live-tail shrink/overflow reconciliation needs
// after a card's rendered height changes mid-turn. This is the same
// lever the deferred settle-freeze uses (subscribe.cpp gates the tick
// on settle_cooldown_ticks > 0); arming it here at every card-height
// mutation reproduces agent_session's always-on clock at the exact
// seams where the height changes. Grow-only so a longer in-flight
// window is never truncated by a shorter one.
void arm_reconcile_cooldown(Model& m) {
    constexpr int kReconcileTicks = 6;
    if (m.ui.settle_cooldown_ticks < kReconcileTicks)
        m.ui.settle_cooldown_ticks = kReconcileTicks;
    ::maya::request_animation_frame();
}

// Fold a completed tool's FileChange into m.d.pending_changes so the
// changes strip + diff-review pane track the agent's cumulative session
// edits. Coalesced PER PATH: a second edit to the same file replaces the
// entry but keeps the ORIGINAL original_contents (the review baseline is
// "since the user last looked", not "since the last tool call") and
// recomputes hunks across the whole before→after span. Hunk-status
// choices the user already made on the stale entry are dropped by design
// — they were made against hunks that no longer exist.
void fold_pending_change(Model& m, FileChange&& ch) {
    if (ch.path.empty()) return;
    auto it = std::find_if(m.d.pending_changes.begin(),
                           m.d.pending_changes.end(),
                           [&](const FileChange& fc) {
                               return fc.path == ch.path;
                           });
    if (it == m.d.pending_changes.end()) {
        m.d.pending_changes.push_back(std::move(ch));
        return;
    }
    // Chain: keep the existing baseline, adopt the new after-state, and
    // recompute the structured hunks over the full span. If the file has
    // returned to its baseline content exactly (agent undid itself), the
    // change is no longer pending — drop the entry.
    if (it->original_contents == ch.new_contents) {
        m.d.pending_changes.erase(it);
        return;
    }
    FileChange merged = diff::compute(ch.path, it->original_contents,
                                      ch.new_contents);
    merged.original_contents = std::move(it->original_contents);
    merged.new_contents      = std::move(ch.new_contents);
    *it = std::move(merged);
}

} // namespace

void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result) {
    with_live_tool(m, id, [&](ToolUse& tc) {
        // Idempotent: a tool already in a terminal state
        // (Done / Failed / Rejected) keeps that state. Realistic
        // ways a late ToolExecOutput can land here:
        //   (a) Wall-clock watchdog force-failed the tool at
        //       60 s; the worker thread eventually unwound
        //       seconds/minutes later. The original failure
        //       reason ("hung") is more useful to the user
        //       than the late output would be — and overwriting
        //       could re-arm a turn that's already advanced
        //       past this tool.
        //   (b) A duplicate dispatch on the same id (shouldn't
        //       happen but cheap to defend against).
        // Either way, dropping the late result keeps history
        // stable.
        //
        // Frozen prefix: with_live_tool already skips messages with
        // index < frozen_through, so a ToolExecOutput that races a
        // freeze (turn settled, user submitted again, tool worker
        // finally returned) silently no-ops here. Without the gate
        // the mutation would land on a Message whose rendered
        // Element in m.ui.frozen is immutable — visible as a
        // permanently-Running spinner in scrollback.
        if (tc.is_terminal()) return;
        auto now = std::chrono::steady_clock::now();
        auto started = tc.started_at();
        if (result) {
            tc.status = ToolUse::Done{started, now,
                clamp_output(std::move(*result))};
        } else {
            // Render typed error as "[kind] detail" so the category
            // is visible in tool-card / history without losing the
            // human-readable detail. The model needs only the
            // string back; the kind is preserved structurally for
            // the future, when the view branches on category.
            tc.status = ToolUse::Failed{started, now,
                clamp_output(result.error().render())};
        }
        release_streaming_buffers(tc);
    });
    // A tool reaching a terminal state SWAPS its card body from the
    // running spinner into the full output/error body — the card's
    // rendered height changes at this instant (a Failed card in
    // particular grows by its error rows). If that height change shifts
    // the overflowed prefix while the clock is about to lapse (last tool
    // of the turn dropping toward Idle, or a coalesced fps=0 frame),
    // maya composes the shifted frame ONCE and never gets the follow-up
    // frames its shrink/overflow reconciliation needs — the old card's
    // top rows strand in scrollback (the "card cut off one screen up"
    // corruption). agent_session never sees this because its clock ticks
    // UNCONDITIONALLY. Mirror that: arm the reconciliation cooldown so
    // the clock keeps running for a few frames after any card-height
    // change, guaranteeing maya reconciles the seam exactly like the
    // reference's always-on tick.
    arm_reconcile_cooldown(m);
}

void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    with_live_tool(m, id, [&](ToolUse& tc) {
        auto now = std::chrono::steady_clock::now();
        if (reason.empty()) {
            tc.status = ToolUse::Rejected{now};
        } else {
            tc.status = ToolUse::Failed{tc.started_at(), now,
                clamp_output(std::string{reason})};
        }
        release_streaming_buffers(tc);
    });
    // Same rationale as apply_tool_output: a rejected tool's card body
    // changes height (spinner → rejection reason), so keep the clock
    // ticking a few frames to let maya reconcile the seam.
    arm_reconcile_cooldown(m);
}

// ============================================================================
// tool_update — reducer for `msg::ToolMsg`
// ============================================================================
// Tool-execution results from the local runner + permission-prompt
// resolutions from the user. Permission lives here because a permission
// prompt is always *about* a specific pending tool call and the resolution
// feeds back into the tool state machine — no clean split.

Step tool_update(Model m, msg::ToolMsg tm) {
    using maya::overload;
    using maya::Cmd;

    return std::visit(overload{
        // ── Live tool progress (streaming subprocess output) ────────────
        // Arrives from the subprocess runner every ~80 ms with the full
        // accumulated output so far. We just set it — no Cmd to return —
        // and rely on the existing Tick subscription (active during
        // ExecutingTool) to re-render. Ignore if the tool has already
        // finalised (a late snapshot racing the terminal ToolExecOutput).
        [&](ToolExecProgress& e) -> Step {
            // Frozen prefix is immutable — a late progress snapshot
            // for a turn that's already settled into m.ui.frozen
            // silently no-ops here.
            with_live_tool(m, e.id, [&](ToolUse& tc) {
                if (auto* r = std::get_if<ToolUse::Running>(&tc.status)) {
                    // Cap the stored snapshot: the body preview shows
                    // the trailing window only, but `tc.progress_text`
                    // gets COPIED into a ToolBodyPreview Config every
                    // frame the live timeline is rebuilt. Unbounded
                    // bash output (e.g. `find /`) would otherwise push
                    // 100s of KB through that copy each frame and
                    // visibly stall the UI on long commands. Mirrors
                    // the write fast path's content cap.
                    constexpr std::size_t kProgressKeep = 16 * 1024;
                    if (e.snapshot.size() > kProgressKeep) {
                        // Keep the tail — newest bytes are the most
                        // useful confirmation of progress.
                        e.snapshot.erase(
                            0, e.snapshot.size() - kProgressKeep);
                    }
                    r->progress_text = std::move(e.snapshot);
                }
            });
            return done(std::move(m));
        },

        // ── Per-tool wall-clock watchdog ──────────────────────────────────
        [&](ToolTimeoutCheck& e) -> Step {
            bool flipped = false;
            with_live_tool(m, e.id, [&](ToolUse& tc) {
                if (tc.is_terminal()) return;
                auto now = std::chrono::steady_clock::now();
                const auto* sp = tools::spec::lookup(tc.name.value);
                auto secs = sp ? sp->max_seconds : std::chrono::seconds{0};
                std::string reason;
                if (tc.is_pending() || tc.is_approved()) {
                    reason = "tool stayed " + std::string{tc.status_name()}
                        + " for " + std::to_string(secs.count())
                        + " s \xe2\x80\x94 args probably never finished streaming "
                        "(transient API error mid-tool_use, or the "
                        "stream silently exited without a terminal event).";
                } else {
                    reason = "tool execution exceeded "
                        + std::to_string(secs.count())
                        + " s wall-clock \xe2\x80\x94 likely hung on a blocking "
                        "syscall (slow/dead filesystem mount, network "
                        "freeze, or worker deadlock). The tool's worker "
                        "thread may continue in the background; its "
                        "result will be discarded if it ever returns.";
                }
                tc.status = ToolUse::Failed{
                    tc.started_at(), now, std::move(reason)};
                flipped = true;
            });
            if (!flipped) return done(std::move(m));
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            // todo's side effect on the UI's plan state — runs only
            // when the call actually succeeded; failures don't synthesise
            // a plan. The final exact state lands here even if the live
            // streaming sync (stream.cpp) raced a partial array.
            if (e.result) {
                for (const auto& msg_ : m.d.current.messages)
                    for (const auto& tc : msg_.tool_calls)
                        if (tc.id == e.id && tc.name == "todo")
                            sync_todo_state_from_args(m, tc.args);
            }
            // Structured file edit → session change set (feeds the
            // changes strip + diff-review). Only successful runs carry
            // a change; per-path coalescing keeps one entry per file.
            // With review disabled the edit is auto-accepted: the tool
            // already wrote the new bytes, so simply not tracking it IS
            // acceptance — no strip, no review pane, no revert baseline.
            if (e.result && e.change && m.d.review_enabled) {
                fold_pending_change(m, std::move(*e.change));
            }
            apply_tool_output(m, e.id, std::move(e.result));
            // No mid-run freeze or trim here. The single freeze site is
            // finalize_turn (the agent_session MessageStop analog) — the
            // whole agent turn is wrapped into one Turn Element and
            // pushed to m.ui.frozen atomically there. Carving mid-run
            // (the prior freeze_settled_subturns + trim_frozen_above_
            // viewport calls that lived here) was the documented source
            // of "redraws from top + scrollback corruption" at every
            // tool→continuation seam: the freeze pushed an entry whose
            // hash_id maya's component cache had not seen on the live
            // tail's previous frame, so the cache missed and re-emitted
            // those rows — sometimes over already-committed scrollback.
            // agent_session never carves mid-stream and shows zero
            // corruption / zero slowdown on long runs (proven by the
            // long_session bench); we now do the same.
            auto kick = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(kick)};
        },

        // ── Permission ──────────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            // Permission only ever fires against a tool in the live
            // tail — a frozen turn is by definition past every pending
            // permission. with_live_tool's frozen-prefix gate is the
            // structural guarantee of that invariant.
            with_live_tool(m, id, [&](ToolUse& tc) {
                // Mark approval as type state: Pending → Approved.
                // kick_pending_tools then treats Approved as
                // "permission already granted" and routes through
                // the same effect-parallel gate as a non-permissioned
                // tool — so if a sibling Read is still running, a
                // freshly approved Write/Bash waits for it instead
                // of racing.
                tc.status = ToolUse::Approved{tc.started_at()};
            });
            m.d.pending_permission.reset();
            return {std::move(m), cmd::kick_pending_tools(m)};
        },
        [&](PermissionReject) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            mark_tool_rejected(m, id, "User rejected this tool call.");
            m.d.pending_permission.reset();
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },
        [&](PermissionApproveAlways) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id   = m.d.pending_permission->id;
            auto name = m.d.pending_permission->tool_name;
            // Record a session-scoped grant for this tool NAME so every
            // future call to it this session auto-approves (consulted in
            // kick_pending_tools). This also propagates to sibling
            // pending tools of the same name in the current batch: the
            // re-kick below re-evaluates each pending tool against the
            // now-populated grant set, so a queued sibling `bash` won't
            // re-prompt. Mirrors Zed's per-session allow-list with live
            // sibling propagation.
            m.d.session_grants.insert(name.value);
            // Persist the grant (Zed's always_allow rules): reload-proof.
            // Load-modify-save so we never clobber provider keys etc.
            {
                auto s = deps().load_settings();
                if (std::find(s.always_allow_tools.begin(),
                              s.always_allow_tools.end(), name.value)
                        == s.always_allow_tools.end()) {
                    s.always_allow_tools.push_back(name.value);
                    deps().save_settings(s);
                }
            }
            m.s.status = name.value + ": always allowed";
            m.s.status_until = std::chrono::steady_clock::now()
                             + std::chrono::seconds{3};
            with_live_tool(m, id, [&](ToolUse& tc) {
                tc.status = ToolUse::Approved{tc.started_at()};
            });
            m.d.pending_permission.reset();
            return {std::move(m), cmd::kick_pending_tools(m)};
        },
    }, tm);
}

} // namespace agentty::app::detail
