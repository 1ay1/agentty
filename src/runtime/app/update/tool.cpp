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
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_args.hpp"
#include "agentty/runtime/view/thread/turn/agent_timeline/tool_helpers.hpp"
#include "agentty/store/store.hpp"
#include "agentty/tool/spec.hpp"
#include "agentty/tool/util/utf8.hpp"

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
    // Both cut points MUST land on UTF-8 code-point boundaries: the
    // clamped string is what goes onto the wire as the tool_result, and
    // nlohmann::json::dump() throws type_error.316 on a split multi-byte
    // sequence — which would kill serialization of the whole request.
    // Head: largest boundary ≤ half. Tail: walk the start FORWARD past
    // any continuation bytes so the kept suffix begins on a lead byte.
    const std::size_t head_end = tools::util::safe_utf8_cut(s, half);
    std::size_t tail_start = s.size() - half;
    while (tail_start < s.size()
           && (static_cast<unsigned char>(s[tail_start]) & 0xC0) == 0x80)
        ++tail_start;
    std::string out;
    out.reserve(kStoredOutputCap + 64);
    out.append(s, 0, head_end);
    out.append("\n\n… (");
    out.append(std::to_string((s.size() - head_end - (s.size() - tail_start)) / 1024));
    out.append(" KiB elided to keep memory bounded) …\n\n");
    out.append(s, tail_start, s.size() - tail_start);
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

// Build the tool-output-viewer entry list: every settled tool call in the
// current thread with a non-empty stored output, NEWEST FIRST (the one the
// user just watched scroll past is entry 0). Bounded by kMaxEntries /
// kSnapshotBudget so a marathon session can't balloon the overlay open.
// Snapshotting (copying the output bytes) makes the overlay immune to the
// transcript mutating underneath it — each stored output is already
// clamped to 256 KiB upstream, so the copies are cheap and bounded.
[[nodiscard]] std::vector<tool_viewer::Entry> collect_viewer_entries(const Model& m) {
    std::vector<tool_viewer::Entry> out;
    std::size_t budget = tool_viewer::kSnapshotBudget;
    for (auto mit = m.d.current.messages.rbegin();
         mit != m.d.current.messages.rend(); ++mit) {
        for (auto tit = mit->tool_calls.rbegin();
             tit != mit->tool_calls.rend(); ++tit) {
            const auto& tc = *tit;
            if (!tc.is_terminal()) continue;
            const auto& body = tc.output();
            if (body.empty()) continue;
            if (out.size() >= tool_viewer::kMaxEntries) return out;
            if (body.size() > budget) continue;   // skip, keep older smaller ones
            budget -= body.size();

            tool_viewer::Entry e;
            e.failed = tc.is_failed();
            e.output = body;
            // Raw name drives the category colour badge; display name +
            // detail reuse the timeline's helpers so the list reads
            // exactly like the transcript cards being re-inspected.
            e.name   = tc.name.value;
            e.title  = ui::tool_display_name(tc.name.value);
            e.detail = ui::tool_timeline_detail(tc);
            e.call   = tc;   // snapshot for the rich body render (diff/gutter)
            // Trailing: ok/failed · duration · size.
            e.trailing = e.failed ? "failed" : "ok";
            if (float secs = ui::tool_elapsed(tc); secs >= 0.05f) {
                char buf[32];
                std::snprintf(buf, sizeof buf, " \xc2\xb7 %.1fs", secs);
                e.trailing += buf;
            }
            e.trailing += (body.size() >= 1024)
                ? " \xc2\xb7 " + std::to_string(body.size() / 1024) + " KB"
                : " \xc2\xb7 " + std::to_string(body.size()) + " B";
            out.push_back(std::move(e));
        }
    }
    return out;
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
                        // useful confirmation of progress. Advance the
                        // cut past any UTF-8 continuation bytes so the
                        // kept suffix starts on a code-point boundary
                        // (a split sequence renders as mojibake in the
                        // card body).
                        std::size_t cut = e.snapshot.size() - kProgressKeep;
                        while (cut < e.snapshot.size()
                               && (static_cast<unsigned char>(
                                       e.snapshot[cut]) & 0xC0) == 0x80)
                            ++cut;
                        e.snapshot.erase(0, cut);
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

        // ── Tool-output viewer ────────────────────────────────────
        [&](OpenToolOutputViewer) -> Step {
            auto entries = collect_viewer_entries(m);
            if (entries.empty()) {
                auto cmd = set_status_toast(m, "no tool outputs to inspect yet");
                return {std::move(m), std::move(cmd)};
            }
            m.ui.tool_viewer = tool_viewer::Open{std::move(entries), 0, false};
            m.ui.tool_viewer_scroll.y = 0;
            return done(std::move(m));
        },
        [&](CloseToolOutputViewer) -> Step {
            // Esc semantics: body stage → back to the list; list → closed.
            if (auto* o = tool_viewer_opened(m.ui.tool_viewer); o && o->viewing) {
                o->viewing = false;
                m.ui.tool_viewer_scroll.y = 0;
                return done(std::move(m));
            }
            m.ui.tool_viewer = tool_viewer::Closed{};
            return done(std::move(m));
        },
        [&](ToolViewerMove& e) -> Step {
            auto* o = tool_viewer_opened(m.ui.tool_viewer);
            if (!o) return done(std::move(m));
            if (o->viewing) {
                // Body stage: deltas scroll the output viewport directly.
                // max_y is paint-written-back by the Picker widget.
                auto& sc = m.ui.tool_viewer_scroll;
                sc.y = std::clamp(sc.y + e.delta, 0, std::max(0, sc.max_y));
            } else {
                int sz = static_cast<int>(o->entries.size());
                if (sz > 0)
                    o->index = std::clamp(o->index + e.delta, 0, sz - 1);
            }
            return done(std::move(m));
        },
        [&](ToolViewerSelect) -> Step {
            auto* o = tool_viewer_opened(m.ui.tool_viewer);
            if (!o || o->viewing) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            o->viewing = true;
            m.ui.tool_viewer_scroll.y = 0;
            return done(std::move(m));
        },
        [&](ToolViewerStep& e) -> Step {
            // ←/→ while reading an output: hop to the neighbouring
            // entry's body directly. Clamped at the ends (no wrap — the
            // list is short and wrap-around disorients more than it
            // helps). List stage: no-op.
            auto* o = tool_viewer_opened(m.ui.tool_viewer);
            if (!o || !o->viewing) return done(std::move(m));
            int sz = static_cast<int>(o->entries.size());
            if (sz <= 0) return done(std::move(m));
            int next = std::clamp(o->index + e.delta, 0, sz - 1);
            if (next != o->index) {
                o->index = next;
                m.ui.tool_viewer_scroll.y = 0;
            }
            return done(std::move(m));
        },
        [&](ToolViewerCopy) -> Step {
            auto* o = tool_viewer_opened(m.ui.tool_viewer);
            if (!o) return done(std::move(m));
            if (o->index < 0
                || o->index >= static_cast<int>(o->entries.size()))
                return done(std::move(m));
            std::string body =
                o->entries[static_cast<std::size_t>(o->index)].output;
            auto toast = set_status_toast(m, "tool output copied to clipboard");
            return {std::move(m), maya::Cmd<Msg>::batch(
                maya::Cmd<Msg>::write_clipboard(std::move(body)),
                std::move(toast))};
        },
    }, tm);
}

} // namespace agentty::app::detail
