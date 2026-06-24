// task — subagent dispatch tool.
//
// Spawns an isolated agent loop: a fresh single-message Thread seeded
// with the caller's prompt, streamed against the provider, with the
// model's tool calls executed locally, looping until the subagent
// finishes (end_turn) or hits the turn budget. Returns ONE condensed
// text result to the parent agent — the subagent's full transcript is
// NOT surfaced, keeping the parent's context clean. Mirrors Claude
// Code's `Task` / Zed's `spawn_agent`.
//
// Isolation properties:
//   • Own Thread / context — parent transcript is never sent.
//   • Own tool budget (kMaxTurns completions).
//   • Depth-capped (kMaxDepth) so subagents-spawning-subagents can't
//     fork-bomb.
//   • Synchronous: runs entirely inside execute() on the parent tool's
//     worker thread (run_tool is task_isolated), so no Elm async
//     machinery and no per-event reducer cost.

#include "agentty/tool/spec.hpp"
#include "agentty/tool/subagent.hpp"
#include "agentty/tool/tool.hpp"
#include "agentty/tool/tools.hpp"
#include "agentty/tool/registry.hpp"  // tools::progress::emit
#include "agentty/tool/util/arg_reader.hpp"
#include "agentty/tool/util/tool_args.hpp"

#include "agentty/provider/anthropic/transport.hpp"
#include "agentty/provider/provider.hpp"

#include <chrono>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentty::tools {

using json = nlohmann::json;

namespace {

struct TaskArgs {
    std::string prompt;
    std::string display_description;
    std::string agent_type;   // explorer | reviewer | tester | coder | general
};

std::expected<TaskArgs, ToolError> parse_task_args(const json& j) {
    util::ArgReader ar(j);
    TaskArgs out;
    out.prompt = ar.str("prompt", "");
    out.display_description = ar.str("display_description", "");
    out.agent_type = ar.str("agent_type", "general");
    if (out.prompt.empty())
        return std::unexpected(ToolError::invalid_args(
            "task requires a non-empty `prompt` describing the subagent's goal"));
    return out;
}

// ── Agent types ──────────────────────────────────────────────────────────
// A subagent is far more useful when it's SPECIALISED: a tuned role prompt
// + a restricted tool set keeps it focused, prevents it from wandering
// (or fork-bombing via nested `task`), and makes its report predictable.
// Each type names the tools it may use; an empty allow-list means "every
// tool except `task`". `read_only` types never get write/exec tools even
// if listed, a hard guard for investigation roles.
struct AgentType {
    std::string_view name;
    bool             read_only;
    std::string_view role;     // appended to the subagent system prompt
    // Tools this type is allowed to call. Empty => all (minus `task`).
    std::vector<std::string_view> tools;
};

const AgentType& resolve_agent_type(std::string_view t) {
    static const std::vector<AgentType> kTypes = {
        {"explorer", /*read_only=*/true,
         "Your role: EXPLORER. Map and explain the codebase region the task "
         "names. Read widely, trace call sites and definitions, and return a "
         "precise map: the key files, the functions/types involved, how they "
         "connect, and any gotchas. Cite exact file paths and line numbers. "
         "You are READ-ONLY \xe2\x80\x94 never modify anything.",
         {"read", "grep", "glob", "list_dir", "find_definition", "web_search",
          "web_fetch"}},

        {"reviewer", /*read_only=*/true,
         "Your role: REVIEWER. Critically review the code or change the task "
         "names. Look for bugs, edge cases, race conditions, security issues, "
         "and deviations from the surrounding conventions. Return findings as "
         "a prioritised list (blocker / major / minor / nit), each with the "
         "exact file:line and a concrete fix suggestion. You are READ-ONLY.",
         {"read", "grep", "glob", "list_dir", "find_definition", "git_diff",
          "git_log", "git_status"}},

        {"tester", /*read_only=*/false,
         "Your role: TESTER. Reproduce, run, and diagnose. Build/run the "
         "relevant tests or commands the task names, read the failures, and "
         "report the root cause with the exact failing assertion and the "
         "file:line that produced it. Prefer running over guessing. Do NOT "
         "rewrite production code \xe2\x80\x94 only run, read, and diagnose.",
         {"read", "grep", "glob", "list_dir", "find_definition", "bash",
          "diagnostics", "git_diff", "git_status"}},

        {"coder", /*read_only=*/false,
         "Your role: CODER. Implement the change the task names end-to-end: "
         "read the relevant code first, make the edits, and verify they build/"
         "compile if a build command is obvious. Follow the surrounding "
         "conventions exactly. Report what you changed (files + a one-line "
         "summary each) and whether it built.",
         {}},  // full tool set (minus task)

        {"general", /*read_only=*/false,
         "Your role: GENERAL. Complete the delegated task end-to-end using "
         "whatever tools fit, then report the outcome.",
         {}},
    };
    for (const auto& a : kTypes)
        if (a.name == t) return a;
    return kTypes.back();  // "general"
}

// System prompt for the subagent. Deliberately terse + outcome-focused:
// the subagent exists to do ONE delegated job and report back, not to
// chat. It reuses the same tool catalog as the parent, narrowed to the
// agent type's allow-list. The role line specialises behaviour.
std::string subagent_system_prompt(const AgentType& type) {
    std::string base = provider::anthropic::default_system_prompt();
    base += "\n\n<subagent>\n";
    base += std::string{type.role};
    base +=
        "\n\nYou are a SUBAGENT spawned to complete ONE delegated task in "
        "isolation. You do NOT see the parent conversation and cannot ask it "
        "questions \xe2\x80\x94 work fully autonomously from the task prompt alone. "
        "Use your tools to investigate and act, then STOP calling tools and "
        "write your final report as plain text.\n\n"
        "Your final message is the ONLY thing the parent receives \xe2\x80\x94 not your "
        "transcript, not your tool output. So the report must stand alone. "
        "Structure it as:\n"
        "  \xe2\x80\xa2 A one-line OUTCOME (what you found / did).\n"
        "  \xe2\x80\xa2 The key details the parent needs to act, with exact file:line "
        "references where relevant.\n"
        "  \xe2\x80\xa2 Anything you could NOT determine, stated plainly.\n"
        "Be concrete and cite evidence (paths, line numbers, command output). "
        "Do not pad. If the task is impossible or underspecified, say so and "
        "explain what's missing rather than guessing.";
    if (type.read_only)
        base += "\n\nYou are READ-ONLY: you have no tools that modify files, "
                "run commands, or reach the network. Investigate and report only.";
    base += "\n</subagent>";
    return base;
}

// A short one-line summary of a tool call for the live progress log:
// the tool name plus its most identifying argument (path / pattern /
// command), so the parent card reads like a running activity feed.
std::string summarize_call(const ToolUse& tc) {
    std::string s = tc.name.value;
    if (tc.args.is_object()) {
        for (const char* key : {"path", "file_path", "pattern", "command",
                                "url", "query", "symbol", "prompt"}) {
            auto it = tc.args.find(key);
            if (it != tc.args.end() && it->is_string()) {
                std::string v = it->get<std::string>();
                if (v.size() > 80) { v.resize(77); v += "..."; }
                // collapse newlines so the feed stays one-line-per-call
                for (auto& ch : v) if (ch == '\n' || ch == '\r') ch = ' ';
                s += "  ";
                s += v;
                break;
            }
        }
    }
    return s;
}

// Drive ONE subagent completion synchronously. Appends the assistant
// Message (text + tool_calls) to `thread`. Returns the stop reason.
// Reassembles streamed Msg events into the assistant Message. `log`
// accumulates a human-readable activity feed that is pushed to the
// parent's tool card via progress::emit as the subagent works — so the
// running card shows what the subagent is doing instead of sitting
// blank until the final report lands.
StopReason run_one_completion(Thread& thread, const subagent::Config& cfg,
                              const AgentType& type,
                              std::string& log, std::string& err_out) {
    namespace ap = provider::anthropic;
    ap::Request req;
    req.model         = cfg.model;
    req.system_prompt = subagent_system_prompt(type);
    // Re-resolve the wire credential on every completion rather than reusing
    // the header captured at startup. For OAuth (Pro/Max), the access token
    // is short-lived; the frozen cfg.auth goes stale and 401s with no
    // reducer here to refresh it. fresh_auth_header() refreshes + persists
    // an expired token in place (blocking is fine — we're on a worker
    // thread), and is a no-op pass-through for static API keys.
    req.auth          = auth::fresh_auth_header(cfg.auth);
    // Subagents do real, multi-step investigation/implementation; a tight
    // token cap truncates long reports. Give them headroom — the turn
    // budget (kMaxTurns) bounds total spend, not per-completion size.
    req.max_tokens    = 32000;
    req.messages      = thread.messages;

    // Build the allowed-tool name set for this agent type. An empty
    // allow-list means "everything". `task` is ALWAYS excluded so a
    // subagent can't recurse (the depth cap is a backstop, but not
    // exposing the tool is cleaner — it never wastes a turn trying).
    // read_only types additionally drop any WriteFs/Exec/Net tool even
    // if the allow-list named it, a hard guard against an investigation
    // agent mutating state.
    auto allowed = [&](const tools::ToolDef& t) -> bool {
        if (t.name.value == "task") return false;
        if (!type.tools.empty()) {
            bool listed = false;
            for (auto n : type.tools)
                if (n == t.name.value) { listed = true; break; }
            if (!listed) return false;
        }
        if (type.read_only) {
            // Prefer the compile-time spec; fall back to the ToolDef's own
            // declared effects for tools the spec table doesn't know (MCP
            // tools synthesised at runtime). A read-only subagent must never
            // get a WriteFs/Exec/Net tool, MCP or not.
            tools::EffectSet eff = t.effects;
            if (const auto* sp = tools::spec::lookup(t.name.value)) eff = sp->effects;
            using tools::Effect;
            if (eff.has(Effect::WriteFs) || eff.has(Effect::Exec) || eff.has(Effect::Net))
                return false;
        }
        return true;
    };
    for (const auto& t : tools::wire_tools()) {
        if (!allowed(t)) continue;
        req.tools.push_back({t.name.value, t.description, t.input_schema,
                             t.eager_input_streaming});
    }

    Message asst;
    asst.role = Role::Assistant;

    StopReason stop = StopReason::Unspecified;
    std::string cur_tool_json;   // accumulates input_json_delta for the open tool

    // Emit a snapshot of the activity log plus whatever the subagent has
    // streamed so far this turn. Cheap (no-op if no parent sink), so we
    // can call it on every text delta for a live typing effect.
    auto pump = [&] {
        std::string snap = log;
        if (!asst.text.empty()) {
            snap += "\n  \xe2\x96\xb8 ";  // ▸
            snap += asst.text;
        }
        progress::emit(snap);
    };

    ap::run_stream_sync(std::move(req),
        [&](Msg m) {
            // Msg is a TWO-LEVEL variant: the top level is the domain
            // (ComposerMsg / StreamMsg / ToolMsg / ...) and each domain is
            // itself a variant of leaf events. The transport only ever
            // emits StreamMsg leaves, so unwrap the domain first, then
            // visit the inner StreamMsg to reach StreamTextDelta etc. The
            // previous single std::visit matched T against StreamTextDelta
            // directly — but T was msg::StreamMsg (the nested variant), so
            // NO arm ever fired and every delta was silently dropped
            // (subagent always "finished with no text report").
            auto* sm = std::get_if<msg::StreamMsg>(&m);
            if (!sm) return;  // non-stream domain can't arrive on this path
            std::visit([&](auto&& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::same_as<T, StreamTextDelta>) {
                    asst.text += e.text;
                    pump();
                } else if constexpr (std::same_as<T, StreamToolUseStart>) {
                    ToolUse tc;
                    tc.id     = e.id;
                    tc.name   = e.name;
                    tc.status = ToolUse::Pending{std::chrono::steady_clock::now()};
                    asst.tool_calls.push_back(std::move(tc));
                    cur_tool_json.clear();
                } else if constexpr (std::same_as<T, StreamToolUseDelta>) {
                    cur_tool_json += e.partial_json;
                } else if constexpr (std::same_as<T, StreamToolUseEnd>) {
                    if (!asst.tool_calls.empty() && !cur_tool_json.empty()) {
                        try {
                            asst.tool_calls.back().args = json::parse(cur_tool_json);
                        } catch (...) {
                            // Leave args null; run_task marks it failed.
                        }
                    }
                    cur_tool_json.clear();
                    if (!asst.tool_calls.empty()) {
                        log += "\n  \xe2\x9a\x99 ";  // ⚙
                        log += summarize_call(asst.tool_calls.back());
                        pump();
                    }
                } else if constexpr (std::same_as<T, StreamFinished>) {
                    stop = e.stop_reason;
                } else if constexpr (std::same_as<T, StreamError>) {
                    // Surface the failure instead of swallowing it — an
                    // auth/400/rate-limit error inside the subagent thread
                    // otherwise looks like "finished with no text report".
                    err_out = e.message;
                    log += "\n  \xe2\x9a\xa0 error: " + e.message;  // ⚠
                    pump();
                }
                // StreamStarted / StreamUsage / heartbeat: ignored.
            }, *sm);
        });

    thread.messages.push_back(std::move(asst));
    return stop;
}

ExecResult run_task(const TaskArgs& a) {
    auto cfg = subagent::current();
    if (!cfg.installed || cfg.model.empty() || auth::is_empty(cfg.auth)) {
        return std::unexpected(ToolError::invalid_args(
            "subagents are unavailable in this context (no model/auth wired)"));
    }
    if (subagent::current_depth() >= subagent::kMaxDepth) {
        return std::unexpected(ToolError::invalid_args(
            "subagent depth limit reached — a subagent cannot spawn further "
            "subagents at this nesting level"));
    }

    subagent::push_depth();
    struct DepthGuard {
        ~DepthGuard() { subagent::pop_depth(); }
    } depth_guard;

    const AgentType& type = resolve_agent_type(a.agent_type);

    // Seed the subagent thread with the delegated prompt.
    Thread thread;
    {
        Message user;
        user.role = Role::User;
        user.text = a.prompt;
        thread.messages.push_back(std::move(user));
    }

    int turns = 0;
    // Human-readable activity feed streamed to the parent's tool card and
    // appended to the final report so even a no-text-report subagent shows
    // what it actually did.
    std::string log = "\xe2\x97\x86 " + std::string{type.name} + " subagent\n";  // ◆
    std::string last_error;  // last StreamError, if any
    while (turns < subagent::kMaxTurns) {
        ++turns;
        log += (turns == 1 ? "" : "\n");
        log += "\xe2\x80\xa2 turn " + std::to_string(turns);  // •
        progress::emit(log);
        std::string err;
        StopReason stop = run_one_completion(thread, cfg, type, log, err);
        if (!err.empty()) last_error = err;

        Message& asst = thread.messages.back();

        // Execute any tool calls the subagent emitted; append a User turn
        // carrying the results so the next completion sees them.
        bool ran_a_tool = false;
        if (!asst.tool_calls.empty()) {
            const auto now = std::chrono::steady_clock::now();
            for (auto& tc : asst.tool_calls) {
                if (tc.args.is_null()) {
                    tc.status = ToolUse::Failed{now, now,
                        "subagent tool args failed to parse"};
                    log += "\n    \xe2\x9c\x97 " + tc.name.value + ": bad args";
                    progress::emit(log);
                    continue;
                }
                ran_a_tool = true;
                auto res = tool::DynamicDispatch::execute(tc.name.value, tc.args);
                if (res) {
                    tc.status = ToolUse::Done{now, now, std::move(res->text)};
                    log += "\n    \xe2\x9c\x93 " + tc.name.value;  // ✓
                } else {
                    tc.status = ToolUse::Failed{now, now, res.error().render()};
                    log += "\n    \xe2\x9c\x97 " + tc.name.value + ": "  // ✗
                         + res.error().render();
                }
                progress::emit(log);
            }
            // The transport synthesises the tool_result User turn from the
            // assistant's terminal tool_calls, so we don't push one here —
            // appending the next placeholder is enough to continue the loop.
        }

        // Stop when the model ended its turn without (further) tool use.
        if (stop != StopReason::ToolUse && !ran_a_tool) break;
        // Defensive: if the model claimed tool_use but emitted nothing we
        // could run, stop rather than spin.
        if (stop == StopReason::ToolUse && !ran_a_tool) break;
        // A hard stream error (auth/400/etc.) won't fix itself by looping.
        if (!err.empty() && !ran_a_tool) break;
    }

    // Harvest the final report: the last assistant message's text.
    std::string report;
    for (auto it = thread.messages.rbegin(); it != thread.messages.rend(); ++it) {
        if (it->role == Role::Assistant && !it->text.empty()) {
            report = it->text;
            break;
        }
    }
    if (report.empty()) {
        // No final text report. Surface the real reason: a stream error
        // (auth/400/rate-limit) is the usual culprit and was previously
        // swallowed, leaving a bare "finished without a text report".
        // Fall back to the activity feed so the parent sees what happened.
        std::string why;
        if (!last_error.empty())
            why = "[subagent failed: " + last_error + "]";
        else if (turns >= subagent::kMaxTurns)
            why = "[subagent hit its turn budget without producing a final report]";
        else
            why = "[subagent finished without a text report]";
        report = log.empty() ? why : (why + "\n\nActivity:\n" + log);
    }

    std::ostringstream out;
    out << "Subagent report (" << type.name << ", " << turns << " turn"
        << (turns == 1 ? "" : "s") << "):\n\n" << report;
    return ToolOutput{out.str(), std::nullopt};
}

} // namespace

ToolDef tool_task() {
    ToolDef t;
    constexpr const auto& kSpec = spec::require<"task">();
    t.name = ToolName{std::string{kSpec.name}};
    t.description =
        "Spawn an autonomous subagent to complete a self-contained task in "
        "isolation (own context window, own tool budget). The subagent runs "
        "to completion and returns ONE condensed report; you do NOT see its "
        "intermediate steps, and it cannot ask you questions \xe2\x80\x94 so give a "
        "complete, unambiguous prompt with all the context it needs.\n\n"
        "Pick an `agent_type` to specialise it (each has a tuned role + a "
        "restricted tool set):\n"
        "  \xe2\x80\xa2 explorer  \xe2\x80\x94 read-only: map/understand a codebase region, trace "
        "call sites, return a file:line map.\n"
        "  \xe2\x80\xa2 reviewer  \xe2\x80\x94 read-only: critique a change/file for bugs, return "
        "prioritised findings with fixes.\n"
        "  \xe2\x80\xa2 tester    \xe2\x80\x94 run/diagnose: build+run tests, report the root cause "
        "of failures (no production edits).\n"
        "  \xe2\x80\xa2 coder     \xe2\x80\x94 full tools: implement a change end-to-end and verify "
        "it builds.\n"
        "  \xe2\x80\xa2 general   \xe2\x80\x94 (default) full tools, no specialisation.\n\n"
        "Best for focused, parallelisable jobs you want handled without "
        "cluttering this conversation \xe2\x80\x94 e.g. 'explore how auth flows through "
        "the request pipeline', 'review my last commit for races', 'reproduce "
        "and diagnose the failing midrun_freeze_test'.";
    t.input_schema = json{
        {"type", "object"},
        {"required", {"prompt"}},
        {"properties", {
            {"prompt", {{"type", "string"},
                {"description", "Complete, self-contained description of the "
                                "task for the subagent to accomplish. Include "
                                "all context it needs \xe2\x80\x94 it cannot see this "
                                "conversation and cannot ask follow-ups."}}},
            {"agent_type", {{"type", "string"},
                {"enum", {"explorer", "reviewer", "tester", "coder", "general"}},
                {"description", "Subagent specialisation. explorer/reviewer are "
                                "read-only; tester runs+diagnoses; coder edits; "
                                "general (default) is unrestricted."}}},
            {"display_description", {{"type", "string"},
                {"description", "One-line summary shown in the UI. Optional."}}},
        }},
    };
    t.effects = kSpec.effects;
    t.eager_input_streaming = kSpec.eager_input_streaming;
    t.execute = util::adapt<TaskArgs>(parse_task_args, run_task);
    return t;
}

} // namespace agentty::tools
