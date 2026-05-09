// Tool-execution-result helpers: apply_tool_output translates a
// ToolExecOutput into a Done/Failed status on the matching ToolUse;
// mark_tool_rejected is the symmetric one-liner for permission denial.
// Both walk m.d.current.messages because a ToolCallId is only locally
// unique within a turn — we don't index them.

#include "moha/runtime/app/update/internal.hpp"
#include "moha/runtime/app/update.hpp"

#include <chrono>
#include <utility>

#include <maya/core/overload.hpp>
#include <nlohmann/json.hpp>

#include "moha/runtime/app/cmd_factory.hpp"
#include "moha/tool/spec.hpp"

namespace moha::app::detail {

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

} // namespace

void apply_tool_output(Model& m, const ToolCallId& id,
                       std::expected<std::string, tools::ToolError>&& result) {
    for (auto& msg : m.d.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
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
            }
}

void mark_tool_rejected(Model& m, const ToolCallId& id,
                        std::string_view reason) {
    for (auto& msg : m.d.current.messages)
        for (auto& tc : msg.tool_calls)
            if (tc.id == id) {
                auto now = std::chrono::steady_clock::now();
                if (reason.empty()) {
                    tc.status = ToolUse::Rejected{now};
                } else {
                    tc.status = ToolUse::Failed{tc.started_at(), now,
                        clamp_output(std::string{reason})};
                }
                release_streaming_buffers(tc);
            }
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
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == e.id) {
                        if (auto* r = std::get_if<ToolUse::Running>(&tc.status))
                            r->progress_text = std::move(e.snapshot);
                    }
            return done(std::move(m));
        },

        // ── Per-tool wall-clock watchdog ────────────────────────────────
        [&](ToolTimeoutCheck& e) -> Step {
            bool flipped = false;
            for (auto& msg_ : m.d.current.messages) {
                for (auto& tc : msg_.tool_calls) {
                    if (tc.id != e.id) continue;
                    if (tc.is_terminal())                  continue;
                    auto now = std::chrono::steady_clock::now();
                    const auto* sp = tools::spec::lookup(tc.name.value);
                    auto secs = sp ? sp->max_seconds : std::chrono::seconds{0};
                    std::string reason;
                    if (tc.is_pending() || tc.is_approved()) {
                        reason = "tool stayed " + std::string{tc.status_name()}
                            + " for " + std::to_string(secs.count())
                            + " s — args probably never finished streaming "
                            "(transient API error mid-tool_use, or the "
                            "stream silently exited without a terminal event).";
                    } else {
                        reason = "tool execution exceeded "
                            + std::to_string(secs.count())
                            + " s wall-clock — likely hung on a blocking "
                            "syscall (slow/dead filesystem mount, network "
                            "freeze, or worker deadlock). The tool's worker "
                            "thread may continue in the background; its "
                            "result will be discarded if it ever returns.";
                    }
                    tc.status = ToolUse::Failed{
                        tc.started_at(), now, std::move(reason)};
                    flipped = true;
                }
            }
            if (!flipped) return done(std::move(m));
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Tool execution result ───────────────────────────────────────
        [&](ToolExecOutput& e) -> Step {
            // todo's side effect on the UI's todo modal — runs only when
            // the call actually succeeded; failures don't synthesise a
            // todo list.
            if (e.result) {
                for (const auto& msg_ : m.d.current.messages)
                    for (const auto& tc : msg_.tool_calls)
                        if (tc.id == e.id && tc.name == "todo") {
                            auto todos = tc.args.value("todos", json::array());
                            m.ui.todo.items.clear();
                            for (const auto& td : todos) {
                                TodoItem item;
                                item.content = td.value("content", "");
                                auto st = td.value("status", "pending");
                                item.status = st == "completed"   ? TodoStatus::Completed
                                            : st == "in_progress" ? TodoStatus::InProgress
                                                                  : TodoStatus::Pending;
                                m.ui.todo.items.push_back(std::move(item));
                            }
                        }
            }
            apply_tool_output(m, e.id, std::move(e.result));
            auto cmd = cmd::kick_pending_tools(m);
            return {std::move(m), std::move(cmd)};
        },

        // ── Permission ──────────────────────────────────────────────────
        [&](PermissionApprove) -> Step {
            if (!m.d.pending_permission) return done(std::move(m));
            auto id = m.d.pending_permission->id;
            for (auto& msg_ : m.d.current.messages)
                for (auto& tc : msg_.tool_calls)
                    if (tc.id == id) {
                        // Mark approval as type state: Pending → Approved.
                        // kick_pending_tools then treats Approved as
                        // "permission already granted" and routes through
                        // the same effect-parallel gate as a non-permissioned
                        // tool — so if a sibling Read is still running, a
                        // freshly approved Write/Bash waits for it instead
                        // of racing.
                        tc.status = ToolUse::Approved{tc.started_at()};
                    }
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
            // MVP: same as Approve. Sticky grants TBD.
            return moha::app::update(std::move(m), Msg{PermissionApprove{}});
        },
    }, tm);
}

} // namespace moha::app::detail
