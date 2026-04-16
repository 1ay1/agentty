// moha — terminal Claude Code clone built on maya, Zed-agent-inspired UX.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <maya/maya.hpp>
#include <maya/widget/activity_bar.hpp>
#include <maya/widget/api_usage.hpp>
#include <maya/widget/bash_tool.hpp>
#include <maya/widget/callout.hpp>
#include <maya/widget/context_window.hpp>
#include <maya/widget/cost_tracker.hpp>
#include <maya/widget/diff_view.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/error_block.hpp>
#include <maya/widget/file_changes.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/modal.hpp>
#include <maya/widget/model_badge.hpp>
#include <maya/widget/permission.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/spinner.hpp>
#include <maya/widget/streaming_cursor.hpp>
#include <maya/widget/system_banner.hpp>
#include <maya/widget/thinking.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include <nlohmann/json.hpp>

#include "moha/anthropic.hpp"
#include "moha/auth.hpp"
#include "moha/diff.hpp"
#include "moha/model.hpp"
#include "moha/msg.hpp"
#include "moha/persistence.hpp"
#include "moha/tools.hpp"

using namespace maya;
using namespace maya::dsl;
using namespace moha;
using json = nlohmann::json;

// ============================================================================
// Cross-thread event queue — streaming + tool-exec workers push here; the
// maya runtime drains it via Tick + a Task that dispatches accumulated Msgs.
// ============================================================================

// Process-wide credentials resolved at startup.
namespace { auth::Credentials g_creds; }

// ============================================================================
// Colors (Zed-ish palette)
// ============================================================================

namespace color {
constexpr Color purple  = Color::rgb(198, 120, 221);
constexpr Color blue    = Color::rgb(97, 175, 239);
constexpr Color green   = Color::rgb(152, 195, 121);
constexpr Color amber   = Color::rgb(229, 192, 123);
constexpr Color red     = Color::rgb(224, 108, 117);
constexpr Color cyan    = Color::rgb(86, 182, 194);
constexpr Color fg      = Color::rgb(200, 204, 212);
constexpr Color muted   = Color::rgb(127, 132, 142);
constexpr Color dim_col = Color::rgb(62, 68, 81);
constexpr Color border_col = Color::rgb(50, 54, 62);
}

// ============================================================================
// Helpers
// ============================================================================

namespace {
const char* profile_label(Profile p) {
    switch (p) {
        case Profile::Write:   return "Write";
        case Profile::Ask:     return "Ask";
        case Profile::Minimal: return "Minimal";
    }
    return "?";
}

Color profile_color(Profile p) {
    switch (p) {
        case Profile::Write:   return color::purple;
        case Profile::Ask:     return color::blue;
        case Profile::Minimal: return color::muted;
    }
    return color::fg;
}

std::vector<ModelInfo> seed_models() {
    return {
        {"claude-opus-4-5",    "Claude Opus 4.5",    "anthropic", 200000, true},
        {"claude-sonnet-4-5",  "Claude Sonnet 4.5",  "anthropic", 200000, true},
        {"claude-haiku-4-5",   "Claude Haiku 4.5",   "anthropic", 200000, false},
    };
}

std::string timestamp_hh_mm(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}
} // namespace

// ============================================================================
// Streaming — each request runs synchronously on a maya worker thread spawned
// by Cmd::task. Events reach the UI via maya's BackgroundQueue → wake fd, so
// the UI redraws the instant an SSE delta arrives (no polling, no g_queue).
// ============================================================================

namespace {
Cmd<Msg> launch_stream_cmd(const Model& m) {
    anthropic::Request req;
    req.model = m.model_id;
    req.system_prompt = anthropic::default_system_prompt();
    req.messages = m.current.messages;

    if (m.profile != Profile::Minimal) {
        for (const auto& t : tools::registry()) {
            if (m.profile == Profile::Ask &&
                (t.name == "write" || t.name == "edit" || t.name == "bash")) continue;
            req.tools.push_back({t.name, t.description, t.input_schema});
        }
    }
    req.auth_header = g_creds.header_value();
    req.auth_style  = g_creds.style();

    return Cmd<Msg>::task([req = std::move(req)](std::function<void(Msg)> dispatch) mutable {
        anthropic::run_stream_sync(std::move(req),
            [dispatch](Msg msg) { dispatch(std::move(msg)); });
    });
}

Cmd<Msg> run_tool_cmd(std::string tool_call_id, std::string tool_name, json args) {
    return Cmd<Msg>::task(
        [id = std::move(tool_call_id),
         name = std::move(tool_name),
         args = std::move(args)]
        (std::function<void(Msg)> dispatch) {
            const auto* td = tools::find(name);
            if (!td) {
                dispatch(ToolExecOutput{id, "error: unknown tool", true});
                return;
            }
            try {
                auto r = td->execute(args);
                dispatch(ToolExecOutput{id, r.output, r.error});
            } catch (const std::exception& e) {
                dispatch(ToolExecOutput{id, std::string("exception: ") + e.what(), true});
            }
        });
}

// After a turn ends (message_stop), check if any tool_use blocks still need
// execution. Returns the Cmd that drives the next async work (tool exec and/or
// the follow-up stream once tool results are in).
Cmd<Msg> kick_pending_tools(Model& m) {
    if (m.current.messages.empty()) return Cmd<Msg>::none();
    auto& last = m.current.messages.back();
    if (last.role != Role::Assistant) return Cmd<Msg>::none();

    std::vector<Cmd<Msg>> cmds;
    bool any_pending = false;
    for (auto& tc : last.tool_calls) {
        if (tc.status == ToolUse::Status::Pending) {
            const auto* td = tools::find(tc.name);
            bool needs_perm = td ? td->needs_permission(m.profile) : true;
            if (needs_perm && !m.pending_permission) {
                m.pending_permission = PendingPermission{tc.id, tc.name,
                    "Tool " + tc.name + " needs permission under "
                    + profile_label(m.profile) + " profile"};
                m.phase = Phase::AwaitingPermission;
                return Cmd<Msg>::none();
            } else if (!needs_perm) {
                tc.status = ToolUse::Status::Running;
                cmds.push_back(run_tool_cmd(tc.id, tc.name, tc.args));
                m.phase = Phase::ExecutingTool;
                any_pending = true;
            }
        } else if (tc.status == ToolUse::Status::Running) {
            any_pending = true;
        }
    }
    if (!any_pending) {
        bool has_results = false;
        for (const auto& tc : last.tool_calls) {
            if (tc.status == ToolUse::Status::Done
                || tc.status == ToolUse::Status::Error
                || tc.status == ToolUse::Status::Rejected) {
                has_results = true; break;
            }
        }
        if (has_results) {
            m.phase = Phase::Streaming;
            m.stream_active = true;
            Message placeholder;
            placeholder.role = Role::Assistant;
            m.current.messages.push_back(std::move(placeholder));
            cmds.push_back(launch_stream_cmd(m));
        } else {
            m.phase = Phase::Idle;
        }
    }
    return Cmd<Msg>::batch(std::move(cmds));
}
} // namespace

// ============================================================================
// View helpers
// ============================================================================

namespace views {

Element header(const Model& m) {
    ModelBadge badge(m.model_id);
    auto prof_text = text(std::format(" {} ", profile_label(m.profile)),
        Style{}.with_fg(Color::rgb(30, 30, 30)).with_bg(profile_color(m.profile)).with_bold());
    const char* phase_label = "idle";
    switch (m.phase) {
        case Phase::Streaming:          phase_label = "streaming"; break;
        case Phase::AwaitingPermission: phase_label = "permission"; break;
        case Phase::ExecutingTool:      phase_label = "working"; break;
        default: break;
    }
    return h(
        text("  moha  ", Style{}.with_fg(color::fg).with_bg(color::border_col).with_bold()),
        text(" "),
        badge.build(),
        text(" "),
        prof_text,
        text("  "),
        text(phase_label, Style{}.with_fg(color::muted)),
        spacer(),
        text(std::format("{}/{} tok", m.tokens_in + m.tokens_out, m.context_max),
            Style{}.with_fg(color::muted))
    ).build();
}

Element render_tool_call(const ToolUse& tc);
Element render_inline_permission(const PendingPermission& pp, const ToolUse& tc);
Element render_checkpoint_divider();

// Derive a human-readable "always" pattern from a tool's args.
// bash → "bash <first word>*"; file tools → directory or "<tool> *"; else → tool name.
static std::string derive_always_pattern(const ToolUse& tc) {
    if (tc.name == "bash") {
        std::string cmd = tc.args.value("command", "");
        // First whitespace-delimited token.
        auto end = cmd.find_first_of(" \t\n");
        std::string first = (end == std::string::npos) ? cmd : cmd.substr(0, end);
        if (first.empty()) return "bash *";
        return "bash " + first + " *";
    }
    if (tc.name == "read" || tc.name == "edit" || tc.name == "write") {
        std::string path = tc.args.value("path", "");
        if (path.empty()) return tc.name + " *";
        // Use parent directory if it has one.
        auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            return tc.name + " " + path.substr(0, slash) + "/*";
        return tc.name + " " + path;
    }
    return tc.name + " *";
}

Element render_tool_call(const ToolUse& tc) {
    // Dispatch to specialized maya widgets where applicable.
    auto path = tc.args.value("path", "");
    auto cmd  = tc.args.value("command", "");
    auto status = tc.status;

    if (tc.name == "read") {
        ReadTool rt(path.empty() ? tc.name : path);
        rt.set_expanded(tc.expanded);
        if (status == ToolUse::Status::Running || status == ToolUse::Status::Pending) {
            rt.set_status(ReadStatus::Reading);
        } else if (status == ToolUse::Status::Error || status == ToolUse::Status::Rejected) {
            rt.set_status(ReadStatus::Failed);
            rt.set_content(tc.output);
        } else {
            rt.set_status(ReadStatus::Success);
            rt.set_content(tc.output);
            rt.set_max_lines(12);
        }
        return rt.build();
    }
    if (tc.name == "bash") {
        BashTool bt(cmd.empty() ? tc.name : cmd);
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(10);
        if (status == ToolUse::Status::Running || status == ToolUse::Status::Pending) {
            bt.set_status(BashStatus::Running);
        } else if (status == ToolUse::Status::Error || status == ToolUse::Status::Rejected) {
            bt.set_status(BashStatus::Failed);
            bt.set_output(tc.output);
        } else {
            bt.set_status(BashStatus::Success);
            bt.set_exit_code(0);
            bt.set_output(tc.output);
        }
        return bt.build();
    }
    if (tc.name == "edit") {
        EditTool et(path.empty() ? tc.name : path);
        et.set_expanded(tc.expanded);
        et.set_old_text(tc.args.value("old_string", ""));
        et.set_new_text(tc.args.value("new_string", ""));
        if (status == ToolUse::Status::Running || status == ToolUse::Status::Pending) {
            et.set_status(EditStatus::Applying);
        } else if (status == ToolUse::Status::Error || status == ToolUse::Status::Rejected) {
            et.set_status(EditStatus::Failed);
        } else {
            et.set_status(EditStatus::Applied);
        }
        return et.build();
    }
    if (tc.name == "write") {
        WriteTool wt(path.empty() ? tc.name : path);
        wt.set_expanded(tc.expanded);
        wt.set_content(tc.args.value("content", ""));
        wt.set_max_preview_lines(8);
        if (status == ToolUse::Status::Running || status == ToolUse::Status::Pending) {
            wt.set_status(WriteStatus::Writing);
        } else if (status == ToolUse::Status::Error || status == ToolUse::Status::Rejected) {
            wt.set_status(WriteStatus::Failed);
        } else {
            wt.set_status(WriteStatus::Written);
        }
        return wt.build();
    }

    // Fallback: generic tool call card.
    maya::ToolCall::Config cfg;
    cfg.tool_name = tc.name;
    cfg.kind = maya::ToolCallKind::Other;
    if (!tc.args.empty()) cfg.description = tc.args.dump();
    maya::ToolCall card(cfg);
    card.set_expanded(tc.expanded);
    if (status == ToolUse::Status::Running || status == ToolUse::Status::Pending)
        card.set_status(maya::ToolCallStatus::Running);
    else if (status == ToolUse::Status::Error || status == ToolUse::Status::Rejected)
        card.set_status(maya::ToolCallStatus::Failed);
    else
        card.set_status(maya::ToolCallStatus::Completed);
    if (!tc.output.empty()) {
        card.set_content(text(tc.output, Style{}.with_fg(color::muted)));
    }
    return card.build();
}

Element render_inline_permission(const PendingPermission& pp, const ToolUse& tc) {
    auto pattern = derive_always_pattern(tc);
    // ── divider · footer · divider, matching Zed's tool-card footer aesthetic.
    auto allow_key = Style{}.with_bold().with_fg(color::green);
    auto deny_key  = Style{}.with_bold().with_fg(color::red);
    auto label     = Style{}.with_fg(color::muted);
    auto pattern_s = Style{}.with_fg(color::amber).with_bold();

    auto footer_row = h(
        text("[", label),
        text("Y", allow_key),
        text("] Allow  ", label),
        text("[", label),
        text("N", deny_key),
        text("] Deny  ", label),
        spacer(),
        text("[", label),
        text("A", allow_key),
        text("] Always for ", label),
        text(pattern, pattern_s),
        text(" ▾", label.with_dim())
    );

    std::vector<Element> rows;
    if (!pp.reason.empty()) {
        rows.push_back(text(pp.reason,
            Style{}.with_fg(color::muted).with_italic()).build());
    }
    rows.push_back(footer_row.build());

    return (v(std::move(rows))
        | border(BorderStyle::Round)
        | bcolor(color::amber)
        | padding(0, 1, 0, 1)).build();
}

Element render_checkpoint_divider() {
    // ──── [↺ Restore checkpoint · press Ctrl+Z] ────
    auto dim = Style{}.with_fg(color::muted).with_dim();
    auto label = Style{}.with_fg(color::amber);
    return h(
        text("─── ", dim),
        text("[", dim),
        text("\u21BA Restore checkpoint", label),
        text("] ", dim),
        text("───", dim),
        spacer()
    ).build();
}

Element render_message(const Message& msg, int turn_num, const Model& m) {
    std::vector<Element> rows;
    if (msg.role == Role::User) {
        if (msg.checkpoint_id) {
            rows.push_back(render_checkpoint_divider());
        }
        rows.push_back(TurnDivider(TurnRole::User, turn_num).build());
        rows.push_back(text(""));
        rows.push_back(UserMessage::build(msg.text));
        rows.push_back(text(""));
    } else if (msg.role == Role::Assistant) {
        rows.push_back(TurnDivider(TurnRole::Assistant, turn_num).build());
        rows.push_back(text(""));
        std::string body = msg.text.empty() ? msg.streaming_text : msg.text;
        if (!body.empty()) {
            rows.push_back((v(markdown(body)) | padding(0, 0, 0, 2)).build());
            rows.push_back(text(""));
        }
        for (const auto& tc : msg.tool_calls) {
            rows.push_back(render_tool_call(tc));
            // Inline permission footer attached to the tool call awaiting it.
            if (m.pending_permission && m.pending_permission->tool_call_id == tc.id) {
                rows.push_back(render_inline_permission(*m.pending_permission, tc));
            }
            rows.push_back(text(""));
        }
    }
    return v(std::move(rows)).build();
}

Element thread_panel(const Model& m) {
    std::vector<Element> rows;
    int turn = 1;
    for (const auto& msg : m.current.messages) {
        rows.push_back(render_message(msg, turn, m));
        if (msg.role == Role::Assistant) turn++;
    }
    if (m.stream_active && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        auto spin = m.spinner;
        spin.set_style(Style{}.with_fg(color::amber).with_bold());
        rows.push_back(h(
            spin.build(),
            text(" Thinking…", Style{}.with_fg(color::muted).with_italic())
        ).build());
    }
    if (rows.empty()) {
        rows.push_back(text("Start a conversation — ask me to read, edit, or run anything.",
            Style{}.with_fg(color::muted).with_italic()));
    }
    return (v(std::move(rows)) | padding(0, 1)).build();
}

Element changes_strip(const Model& m) {
    if (m.pending_changes.empty()) return text("");
    FileChanges fc;
    for (const auto& c : m.pending_changes) {
        auto kind = c.original_contents.empty() ? FileChangeKind::Created
                                                 : FileChangeKind::Modified;
        fc.add(c.path, kind, c.added, c.removed);
    }
    auto summary = (v(
        h(text("Changes ", Style{}.with_fg(color::amber).with_bold()),
          text(std::format("({} files)", m.pending_changes.size()),
               Style{}.with_fg(color::muted)),
          spacer(),
          text("R review  A accept-all  X reject-all", Style{}.with_fg(color::muted).with_dim())
        ).build(),
        fc.build()
    ) | border(BorderStyle::Round) | bcolor(color::amber) | padding(0, 1));
    return summary.build();
}

Element composer(const Model& m) {
    std::string display = m.composer_text;
    std::string placeholder;
    if (display.empty()) {
        placeholder = (m.phase == Phase::Streaming)
            ? "(streaming — type to queue)"
            : "ask a question, or give an instruction...";
    }

    // Insert cursor glyph at composer_cursor (byte index).
    std::string with_cursor = display;
    int cur = std::min<int>(m.composer_cursor, (int)display.size());
    with_cursor.insert(cur, "\u2588"); // block cursor

    int rows = m.composer_expanded ? 8 : 3;

    auto inner = v(
        !display.empty()
          ? text(with_cursor, Style{}.with_fg(color::fg))
          : text(placeholder, Style{}.with_fg(color::muted).with_italic())
    ) | padding(0, 1) | height(rows);

    auto hint = h(
        text("@", Style{}.with_fg(color::cyan)),
        text(" mention  ", Style{}.with_fg(color::muted).with_dim()),
        text("Enter", Style{}.with_fg(color::fg)),
        text(" send  ", Style{}.with_fg(color::muted).with_dim()),
        text("Alt+Enter", Style{}.with_fg(color::fg)),
        text(" newline  ", Style{}.with_fg(color::muted).with_dim()),
        text("Ctrl+E", Style{}.with_fg(color::fg)),
        text(" expand", Style{}.with_fg(color::muted).with_dim())
    );

    return (v(
        inner.build(),
        hint.build()
    ) | border(BorderStyle::Round) | bcolor(color::border_col)
      | btext(" compose ", BorderTextPos::Top, BorderTextAlign::Start)).build();
}

Element status_bar(const Model& m) {
    int pct = m.context_max > 0 ? (m.tokens_in + m.tokens_out) * 100 / m.context_max : 0;
    std::string status_label;
    switch (m.phase) {
        case Phase::Idle: status_label = "ready"; break;
        case Phase::Streaming: status_label = "streaming"; break;
        case Phase::AwaitingPermission: status_label = "awaiting permission"; break;
        case Phase::ExecutingTool: status_label = "executing tool"; break;
    }
    return h(
        text(" ", Style{}),
        text(status_label, Style{}.with_fg(color::green)),
        text("   "),
        text(std::format("{}%", pct), Style{}.with_fg(pct > 80 ? color::red : color::muted)),
        spacer(),
        text("Ctrl+/ model  ", Style{}.with_fg(color::muted).with_dim()),
        text("Shift+Tab profile  ", Style{}.with_fg(color::muted).with_dim()),
        text("Ctrl+J threads  ", Style{}.with_fg(color::muted).with_dim()),
        text("Ctrl+K palette  ", Style{}.with_fg(color::muted).with_dim()),
        text("Ctrl+R review  ", Style{}.with_fg(color::muted).with_dim()),
        text("Ctrl+C quit", Style{}.with_fg(color::muted).with_dim())
    ).build();
}

Element permission_modal(const Model& m) {
    if (!m.pending_permission) return text("");
    Permission perm(m.pending_permission->tool_name,
                    m.pending_permission->reason);
    perm.set_result(PermissionResult::Pending);
    auto body = v(
        perm.build(),
        text(""),
        text("Y allow   N reject   A allow-always",
            Style{}.with_fg(color::amber).with_bold())
    ) | padding(1, 2);
    return (v(body.build())
        | border(BorderStyle::Round)
        | bcolor(color::amber)
        | btext(" Permission Required ")).build();
}

Element model_picker(const Model& m) {
    if (!m.show_model_picker) return text("");
    std::vector<Element> rows;
    rows.push_back(text("Select model", Style{}.with_fg(color::fg).with_bold()));
    rows.push_back(text(""));
    int i = 0;
    for (const auto& mi : m.available_models) {
        bool sel = i == m.model_picker_index;
        bool active = mi.id == m.model_id;
        auto prefix = sel ? text("› ", Style{}.with_fg(color::purple).with_bold())
                          : text("  ");
        auto star = mi.favorite ? text("★ ", Style{}.with_fg(color::amber))
                                : text("  ");
        auto active_mark = active ? text(" (active)", Style{}.with_fg(color::green))
                                   : text("");
        rows.push_back(h(prefix, star,
            text(mi.display_name, Style{}.with_fg(sel ? color::fg : color::muted)
                                       .with_bold(sel)),
            active_mark).build());
        i++;
    }
    rows.push_back(text(""));
    rows.push_back(text("↑↓ move  Enter select  F favorite  Esc close",
        Style{}.with_fg(color::muted).with_dim()));
    auto content = (v(std::move(rows)) | padding(1, 2) | width(50));
    return (v(content.build())
        | border(BorderStyle::Round) | bcolor(color::purple)
        | btext(" Models ")).build();
}

Element thread_list(const Model& m) {
    if (!m.show_thread_list) return text("");
    std::vector<Element> rows;
    rows.push_back(text("Recent threads", Style{}.with_fg(color::fg).with_bold()));
    rows.push_back(text(""));
    if (m.threads.empty()) {
        rows.push_back(text("No threads yet.",
            Style{}.with_fg(color::muted).with_italic()));
    }
    int i = 0;
    for (const auto& t : m.threads) {
        bool sel = i == m.thread_list_index;
        auto prefix = sel ? text("› ", Style{}.with_fg(color::blue).with_bold())
                          : text("  ");
        rows.push_back(h(prefix,
            text(t.title.empty() ? "(untitled)" : t.title,
                Style{}.with_fg(sel ? color::fg : color::muted)),
            spacer(),
            text(timestamp_hh_mm(t.updated_at), Style{}.with_fg(color::muted).with_dim())
        ).build());
        if (++i > 15) break;
    }
    rows.push_back(text(""));
    rows.push_back(text("↑↓ move  Enter open  N new  Esc close",
        Style{}.with_fg(color::muted).with_dim()));
    auto content = (v(std::move(rows)) | padding(1, 2) | width(60));
    return (v(content.build())
        | border(BorderStyle::Round) | bcolor(color::blue)
        | btext(" Threads ")).build();
}

Element command_palette(const Model& m) {
    if (!m.show_command_palette) return text("");
    std::vector<std::pair<std::string, std::string>> cmds = {
        {"New thread",         "Start a fresh conversation"},
        {"Review changes",     "Open diff review pane"},
        {"Accept all changes", "Apply every pending hunk"},
        {"Reject all changes", "Discard every pending hunk"},
        {"Cycle profile",      "Write → Ask → Minimal"},
        {"Open model picker",  ""},
        {"Open threads",       ""},
        {"Quit",               "Exit moha"},
    };
    std::vector<Element> rows;
    rows.push_back(h(text("› ", Style{}.with_fg(color::cyan).with_bold()),
        text(m.command_palette_query.empty() ? "(type to filter)"
                                              : m.command_palette_query,
            Style{}.with_fg(m.command_palette_query.empty() ? color::muted
                                                              : color::fg))
    ).build());
    rows.push_back(sep);
    int i = 0;
    for (const auto& [name, desc] : cmds) {
        if (!m.command_palette_query.empty()
            && name.find(m.command_palette_query) == std::string::npos)
            continue;
        bool sel = i == m.command_palette_index;
        auto prefix = sel ? text("› ", Style{}.with_fg(color::cyan).with_bold())
                          : text("  ");
        rows.push_back(h(prefix,
            text(name, Style{}.with_fg(sel ? color::fg : color::muted).with_bold(sel)),
            spacer(),
            text(desc, Style{}.with_fg(color::muted).with_dim())).build());
        i++;
    }
    auto content = (v(std::move(rows)) | padding(1, 2) | width(70));
    return (v(content.build())
        | border(BorderStyle::Round) | bcolor(color::cyan)
        | btext(" Command ")).build();
}

Element diff_review(const Model& m) {
    if (!m.show_diff_review || m.pending_changes.empty()) return text("");
    const auto& fc = m.pending_changes[std::min<int>(m.diff_review_file_index,
                                                     (int)m.pending_changes.size()-1)];
    std::vector<Element> rows;
    rows.push_back(h(
        text("File ", Style{}.with_fg(color::muted)),
        text(std::format("{}/{}", m.diff_review_file_index+1, m.pending_changes.size()),
            Style{}.with_fg(color::fg)),
        text("  "),
        text(fc.path, Style{}.with_fg(color::fg).with_bold()),
        spacer(),
        text(std::format("+{} -{}", fc.added, fc.removed),
            Style{}.with_fg(color::green))
    ).build());
    rows.push_back(sep);
    int hi = 0;
    for (const auto& h_ : fc.hunks) {
        bool sel = hi == m.diff_review_hunk_index;
        const char* stag = h_.status == Hunk::Status::Accepted ? "[✓ accepted]"
                         : h_.status == Hunk::Status::Rejected ? "[✗ rejected]"
                         : "[ pending ]";
        Color stag_color = h_.status == Hunk::Status::Accepted ? color::green
                         : h_.status == Hunk::Status::Rejected ? color::red
                         : color::amber;
        rows.push_back((h(
            sel ? text("› ", Style{}.with_fg(color::purple).with_bold()) : text("  "),
            text(std::format("hunk @@ -{} +{}", h_.old_start, h_.new_start),
                Style{}.with_fg(color::muted)),
            text("  "),
            text(stag, Style{}.with_fg(stag_color))
        )).build());
        DiffView dv(fc.path, h_.patch);
        rows.push_back((v(dv.build()) | padding(0, 0, 0, 2)).build());
        hi++;
    }
    rows.push_back(sep);
    rows.push_back(text("↑↓ hunk  ← → file  Y accept  N reject  A all  X none  Esc close",
        Style{}.with_fg(color::muted).with_dim()));
    auto content = (v(std::move(rows)) | padding(1, 2));
    return (v(content.build())
        | border(BorderStyle::Round) | bcolor(color::border_col)
        | btext(" Review Changes ")).build();
}
} // namespace views

// ============================================================================
// Program
// ============================================================================

struct MohaApp {
    using Model = moha::Model;
    using Msg = moha::Msg;

    static Model init() {
        Model m;
        m.threads = persistence::load_all_threads();
        m.available_models = seed_models();
        auto settings = persistence::load_settings();
        if (!settings.model_id.empty()) m.model_id = settings.model_id;
        m.profile = settings.profile;
        for (auto& mi : m.available_models) {
            for (const auto& fav : settings.favorite_models)
                if (mi.id == fav) mi.favorite = true;
        }
        m.current.id = persistence::new_id();
        m.status_text = "ready";
        return m;
    }

    // --- Msg handlers --------------------------------------------------------

    static auto handle_composer_submit(Model m) -> std::pair<Model, Cmd<Msg>> {
        if (m.composer_text.empty()) return {m, Cmd<Msg>::none()};
        if (m.phase == Phase::Streaming || m.phase == Phase::ExecutingTool) {
            // Queue
            m.queued_messages.push_back(std::exchange(m.composer_text, {}));
            m.composer_cursor = 0;
            return {m, Cmd<Msg>::none()};
        }
        Message user;
        user.role = Role::User;
        user.text = std::exchange(m.composer_text, {});
        m.composer_cursor = 0;
        if (m.current.title.empty())
            m.current.title = persistence::title_from_first_message(user.text);
        m.current.messages.push_back(std::move(user));

        Message assistant_placeholder;
        assistant_placeholder.role = Role::Assistant;
        m.current.messages.push_back(std::move(assistant_placeholder));

        m.current.updated_at = std::chrono::system_clock::now();
        m.phase = Phase::Streaming;
        m.stream_active = true;
        auto cmd = launch_stream_cmd(m);
        return {m, std::move(cmd)};
    }

    static auto finalize_turn(Model& m) -> Cmd<Msg> {
        m.stream_active = false;
        // Flush partial streaming_text into final text
        if (!m.current.messages.empty()) {
            auto& last = m.current.messages.back();
            if (last.role == Role::Assistant && !last.streaming_text.empty()) {
                if (last.text.empty()) last.text = std::move(last.streaming_text);
                else last.text += std::move(last.streaming_text);
                last.streaming_text.clear();
            }
        }
        persistence::save_thread(m.current);
        auto kp = kick_pending_tools(m);

        // Flush queued messages (only if idle)
        if (m.phase == Phase::Idle && !m.queued_messages.empty()) {
            m.composer_text = m.queued_messages.front();
            m.queued_messages.erase(m.queued_messages.begin());
            auto [mm, sub_cmd] = handle_composer_submit(std::move(m));
            m = std::move(mm);
            return Cmd<Msg>::batch(std::vector<Cmd<Msg>>{std::move(kp), std::move(sub_cmd)});
        }
        return kp;
    }

    static auto update(Model m, Msg msg) -> std::pair<Model, Cmd<Msg>> {
        return std::visit(overload{
            // --- Composer ---
            [&](ComposerCharInput e) -> std::pair<Model, Cmd<Msg>> {
                std::string utf8;
                auto cp = (uint32_t)e.ch;
                if (cp < 0x80) utf8.push_back((char)cp);
                else if (cp < 0x800) {
                    utf8.push_back((char)(0xC0 | (cp >> 6)));
                    utf8.push_back((char)(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    utf8.push_back((char)(0xE0 | (cp >> 12)));
                    utf8.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    utf8.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    utf8.push_back((char)(0xF0 | (cp >> 18)));
                    utf8.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                    utf8.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    utf8.push_back((char)(0x80 | (cp & 0x3F)));
                }
                m.composer_text.insert(m.composer_cursor, utf8);
                m.composer_cursor += (int)utf8.size();
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerBackspace) -> std::pair<Model, Cmd<Msg>> {
                if (m.composer_cursor > 0 && !m.composer_text.empty()) {
                    // step back one UTF-8 byte boundary
                    int p = m.composer_cursor - 1;
                    while (p > 0 && ((uint8_t)m.composer_text[p] & 0xC0) == 0x80) p--;
                    m.composer_text.erase(p, m.composer_cursor - p);
                    m.composer_cursor = p;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerEnter) { return handle_composer_submit(std::move(m)); },
            [&](ComposerSubmit) { return handle_composer_submit(std::move(m)); },
            [&](ComposerNewline) -> std::pair<Model, Cmd<Msg>> {
                m.composer_text.insert(m.composer_cursor, "\n");
                m.composer_cursor += 1;
                m.composer_expanded = true;
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerToggleExpand) -> std::pair<Model, Cmd<Msg>> {
                m.composer_expanded = !m.composer_expanded;
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerCursorLeft) -> std::pair<Model, Cmd<Msg>> {
                if (m.composer_cursor > 0) {
                    int p = m.composer_cursor - 1;
                    while (p > 0 && ((uint8_t)m.composer_text[p] & 0xC0) == 0x80) p--;
                    m.composer_cursor = p;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerCursorRight) -> std::pair<Model, Cmd<Msg>> {
                if (m.composer_cursor < (int)m.composer_text.size()) {
                    int p = m.composer_cursor + 1;
                    while (p < (int)m.composer_text.size()
                           && ((uint8_t)m.composer_text[p] & 0xC0) == 0x80) p++;
                    m.composer_cursor = p;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](ComposerCursorHome) -> std::pair<Model, Cmd<Msg>> {
                m.composer_cursor = 0; return {m, Cmd<Msg>::none()};
            },
            [&](ComposerCursorEnd) -> std::pair<Model, Cmd<Msg>> {
                m.composer_cursor = (int)m.composer_text.size(); return {m, Cmd<Msg>::none()};
            },
            [&](ComposerPaste& e) -> std::pair<Model, Cmd<Msg>> {
                m.composer_text.insert(m.composer_cursor, e.text);
                m.composer_cursor += (int)e.text.size();
                if (e.text.find('\n') != std::string::npos) m.composer_expanded = true;
                return {m, Cmd<Msg>::none()};
            },

            // --- Streaming ---
            [&](StreamStarted) -> std::pair<Model, Cmd<Msg>> {
                m.stream_active = true;
                m.stream_started = std::chrono::steady_clock::now();
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamTextDelta& e) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()
                    && m.current.messages.back().role == Role::Assistant) {
                    m.current.messages.back().streaming_text += e.text;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamToolUseStart& e) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()
                    && m.current.messages.back().role == Role::Assistant) {
                    ToolUse tc;
                    tc.id = e.id;
                    tc.name = e.name;
                    tc.status = ToolUse::Status::Pending;
                    m.current.messages.back().tool_calls.push_back(std::move(tc));
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamToolUseDelta& e) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()
                    && m.current.messages.back().role == Role::Assistant
                    && !m.current.messages.back().tool_calls.empty()) {
                    auto& tc = m.current.messages.back().tool_calls.back();
                    tc.args_streaming += e.partial_json;
                    // Try to parse on each delta (no-op if invalid).
                    try { tc.args = json::parse(tc.args_streaming); } catch (...) {}
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamToolUseEnd) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()
                    && m.current.messages.back().role == Role::Assistant
                    && !m.current.messages.back().tool_calls.empty()) {
                    auto& tc = m.current.messages.back().tool_calls.back();
                    try { tc.args = json::parse(tc.args_streaming); } catch (...) {}
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamUsage& e) -> std::pair<Model, Cmd<Msg>> {
                if (e.input_tokens) m.tokens_in += e.input_tokens;
                if (e.output_tokens) m.tokens_out = e.output_tokens;
                return {m, Cmd<Msg>::none()};
            },
            [&](StreamFinished) -> std::pair<Model, Cmd<Msg>> {
                auto cmd = finalize_turn(m);
                return {m, std::move(cmd)};
            },
            [&](StreamError& e) -> std::pair<Model, Cmd<Msg>> {
                m.stream_active = false;
                m.phase = Phase::Idle;
                m.status_text = "error: " + e.message;
                // Surface the error inside the assistant turn so it's visible.
                if (!m.current.messages.empty()
                    && m.current.messages.back().role == Role::Assistant
                    && m.current.messages.back().text.empty()
                    && m.current.messages.back().streaming_text.empty()
                    && m.current.messages.back().tool_calls.empty()) {
                    m.current.messages.back().text = "⚠ " + e.message;
                }
                return {m, Cmd<Msg>::none()};
            },

            // --- Tool execution output ---
            [&](ToolExecOutput& e) -> std::pair<Model, Cmd<Msg>> {
                // Find the tool_call matching id
                for (auto& msg_ : m.current.messages) {
                    for (auto& tc : msg_.tool_calls) {
                        if (tc.id == e.tool_call_id) {
                            tc.output = e.output;
                            tc.status = e.error ? ToolUse::Status::Error
                                                  : ToolUse::Status::Done;
                        }
                    }
                }
                // If edit/write, attach pending change — we already saved it; tool output
                // carries no change handle yet. (Simplified for MVP.)
                auto cmd = kick_pending_tools(m);
                return {m, std::move(cmd)};
            },

            // --- Permission ---
            [&](PermissionApprove) -> std::pair<Model, Cmd<Msg>> {
                if (!m.pending_permission) return {m, Cmd<Msg>::none()};
                auto id = m.pending_permission->tool_call_id;
                std::vector<Cmd<Msg>> cmds;
                for (auto& msg_ : m.current.messages) {
                    for (auto& tc : msg_.tool_calls) {
                        if (tc.id == id) {
                            tc.status = ToolUse::Status::Running;
                            cmds.push_back(run_tool_cmd(tc.id, tc.name, tc.args));
                        }
                    }
                }
                m.pending_permission.reset();
                m.phase = Phase::ExecutingTool;
                return {m, Cmd<Msg>::batch(std::move(cmds))};
            },
            [&](PermissionReject) -> std::pair<Model, Cmd<Msg>> {
                if (!m.pending_permission) return {m, Cmd<Msg>::none()};
                auto id = m.pending_permission->tool_call_id;
                for (auto& msg_ : m.current.messages) {
                    for (auto& tc : msg_.tool_calls) {
                        if (tc.id == id) {
                            tc.status = ToolUse::Status::Rejected;
                            tc.output = "User rejected this tool call.";
                        }
                    }
                }
                m.pending_permission.reset();
                auto cmd = kick_pending_tools(m);
                return {m, std::move(cmd)};
            },
            [&](PermissionApproveAlways) -> std::pair<Model, Cmd<Msg>> {
                // Simplified: same as Approve for MVP. Later: sticky grants.
                return update(std::move(m), PermissionApprove{});
            },

            // --- Navigation ---
            [&](OpenModelPicker) -> std::pair<Model, Cmd<Msg>> {
                m.show_model_picker = true;
                for (int i = 0; i < (int)m.available_models.size(); ++i)
                    if (m.available_models[i].id == m.model_id) m.model_picker_index = i;
                return {m, Cmd<Msg>::none()};
            },
            [&](CloseModelPicker) -> std::pair<Model, Cmd<Msg>> {
                m.show_model_picker = false; return {m, Cmd<Msg>::none()};
            },
            [&](ModelPickerMove& e) -> std::pair<Model, Cmd<Msg>> {
                if (m.available_models.empty()) return {m, Cmd<Msg>::none()};
                int sz = (int)m.available_models.size();
                m.model_picker_index = (m.model_picker_index + e.delta + sz) % sz;
                return {m, Cmd<Msg>::none()};
            },
            [&](ModelPickerSelect) -> std::pair<Model, Cmd<Msg>> {
                if (!m.available_models.empty()) {
                    m.model_id = m.available_models[m.model_picker_index].id;
                    persistence::Settings s;
                    s.model_id = m.model_id;
                    s.profile = m.profile;
                    for (const auto& mi : m.available_models)
                        if (mi.favorite) s.favorite_models.push_back(mi.id);
                    persistence::save_settings(s);
                }
                m.show_model_picker = false;
                return {m, Cmd<Msg>::none()};
            },
            [&](ModelPickerToggleFavorite) -> std::pair<Model, Cmd<Msg>> {
                if (!m.available_models.empty()) {
                    auto& mi = m.available_models[m.model_picker_index];
                    mi.favorite = !mi.favorite;
                }
                return {m, Cmd<Msg>::none()};
            },

            [&](OpenThreadList) -> std::pair<Model, Cmd<Msg>> {
                m.show_thread_list = true;
                m.threads = persistence::load_all_threads();
                m.thread_list_index = 0;
                return {m, Cmd<Msg>::none()};
            },
            [&](CloseThreadList) -> std::pair<Model, Cmd<Msg>> {
                m.show_thread_list = false; return {m, Cmd<Msg>::none()};
            },
            [&](ThreadListMove& e) -> std::pair<Model, Cmd<Msg>> {
                if (m.threads.empty()) return {m, Cmd<Msg>::none()};
                int sz = (int)m.threads.size();
                m.thread_list_index = (m.thread_list_index + e.delta + sz) % sz;
                return {m, Cmd<Msg>::none()};
            },
            [&](ThreadListSelect) -> std::pair<Model, Cmd<Msg>> {
                if (!m.threads.empty()) {
                    m.current = m.threads[m.thread_list_index];
                }
                m.show_thread_list = false;
                return {m, Cmd<Msg>::none()};
            },
            [&](NewThread) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()) persistence::save_thread(m.current);
                m.current = Thread{};
                m.current.id = persistence::new_id();
                m.current.created_at = m.current.updated_at = std::chrono::system_clock::now();
                m.show_thread_list = false;
                m.show_command_palette = false;
                m.composer_text.clear();
                m.composer_cursor = 0;
                m.phase = Phase::Idle;
                return {m, Cmd<Msg>::none()};
            },

            [&](OpenCommandPalette) -> std::pair<Model, Cmd<Msg>> {
                m.show_command_palette = true;
                m.command_palette_query.clear();
                m.command_palette_index = 0;
                return {m, Cmd<Msg>::none()};
            },
            [&](CloseCommandPalette) -> std::pair<Model, Cmd<Msg>> {
                m.show_command_palette = false; return {m, Cmd<Msg>::none()};
            },
            [&](CommandPaletteInput& e) -> std::pair<Model, Cmd<Msg>> {
                if ((uint32_t)e.ch < 0x80) m.command_palette_query.push_back((char)e.ch);
                return {m, Cmd<Msg>::none()};
            },
            [&](CommandPaletteBackspace) -> std::pair<Model, Cmd<Msg>> {
                if (!m.command_palette_query.empty()) m.command_palette_query.pop_back();
                return {m, Cmd<Msg>::none()};
            },
            [&](CommandPaletteMove& e) -> std::pair<Model, Cmd<Msg>> {
                m.command_palette_index = std::max(0, m.command_palette_index + e.delta);
                return {m, Cmd<Msg>::none()};
            },
            [&](CommandPaletteSelect) -> std::pair<Model, Cmd<Msg>> {
                m.show_command_palette = false;
                // Map index → action (very simple).
                // 0 new thread, 1 review, 2 accept all, 3 reject all, 4 cycle profile,
                // 5 model picker, 6 threads, 7 quit
                switch (m.command_palette_index) {
                    case 0: return update(std::move(m), NewThread{});
                    case 1: return update(std::move(m), OpenDiffReview{});
                    case 2: return update(std::move(m), AcceptAllChanges{});
                    case 3: return update(std::move(m), RejectAllChanges{});
                    case 4: return update(std::move(m), CycleProfile{});
                    case 5: return update(std::move(m), OpenModelPicker{});
                    case 6: return update(std::move(m), OpenThreadList{});
                    case 7: return update(std::move(m), Quit{});
                }
                return {m, Cmd<Msg>::none()};
            },

            [&](CycleProfile) -> std::pair<Model, Cmd<Msg>> {
                m.profile = m.profile == Profile::Write ? Profile::Ask
                          : m.profile == Profile::Ask ? Profile::Minimal
                          : Profile::Write;
                persistence::Settings s;
                s.model_id = m.model_id;
                s.profile = m.profile;
                for (const auto& mi : m.available_models)
                    if (mi.favorite) s.favorite_models.push_back(mi.id);
                persistence::save_settings(s);
                return {m, Cmd<Msg>::none()};
            },

            // --- Diff review ---
            [&](OpenDiffReview) -> std::pair<Model, Cmd<Msg>> {
                m.show_diff_review = !m.pending_changes.empty();
                m.diff_review_file_index = 0;
                m.diff_review_hunk_index = 0;
                return {m, Cmd<Msg>::none()};
            },
            [&](CloseDiffReview) -> std::pair<Model, Cmd<Msg>> {
                m.show_diff_review = false; return {m, Cmd<Msg>::none()};
            },
            [&](DiffReviewMove& e) -> std::pair<Model, Cmd<Msg>> {
                if (m.pending_changes.empty()) return {m, Cmd<Msg>::none()};
                auto& fc = m.pending_changes[m.diff_review_file_index];
                int sz = (int)fc.hunks.size();
                if (sz == 0) return {m, Cmd<Msg>::none()};
                m.diff_review_hunk_index = (m.diff_review_hunk_index + e.delta + sz) % sz;
                return {m, Cmd<Msg>::none()};
            },
            [&](DiffReviewNextFile) -> std::pair<Model, Cmd<Msg>> {
                if (m.pending_changes.empty()) return {m, Cmd<Msg>::none()};
                int sz = (int)m.pending_changes.size();
                m.diff_review_file_index = (m.diff_review_file_index + 1) % sz;
                m.diff_review_hunk_index = 0;
                return {m, Cmd<Msg>::none()};
            },
            [&](DiffReviewPrevFile) -> std::pair<Model, Cmd<Msg>> {
                if (m.pending_changes.empty()) return {m, Cmd<Msg>::none()};
                int sz = (int)m.pending_changes.size();
                m.diff_review_file_index = (m.diff_review_file_index - 1 + sz) % sz;
                m.diff_review_hunk_index = 0;
                return {m, Cmd<Msg>::none()};
            },
            [&](AcceptHunk) -> std::pair<Model, Cmd<Msg>> {
                if (!m.pending_changes.empty()) {
                    auto& fc = m.pending_changes[m.diff_review_file_index];
                    if (!fc.hunks.empty())
                        fc.hunks[m.diff_review_hunk_index].status = Hunk::Status::Accepted;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](RejectHunk) -> std::pair<Model, Cmd<Msg>> {
                if (!m.pending_changes.empty()) {
                    auto& fc = m.pending_changes[m.diff_review_file_index];
                    if (!fc.hunks.empty())
                        fc.hunks[m.diff_review_hunk_index].status = Hunk::Status::Rejected;
                }
                return {m, Cmd<Msg>::none()};
            },
            [&](AcceptAllChanges) -> std::pair<Model, Cmd<Msg>> {
                for (auto& fc : m.pending_changes)
                    for (auto& h_ : fc.hunks) h_.status = Hunk::Status::Accepted;
                return {m, Cmd<Msg>::none()};
            },
            [&](RejectAllChanges) -> std::pair<Model, Cmd<Msg>> {
                for (auto& fc : m.pending_changes)
                    for (auto& h_ : fc.hunks) h_.status = Hunk::Status::Rejected;
                m.pending_changes.clear();
                m.show_diff_review = false;
                return {m, Cmd<Msg>::none()};
            },

            [&](RestoreCheckpoint&) -> std::pair<Model, Cmd<Msg>> {
                // MVP: not yet implemented. Would revert to a saved snapshot.
                m.status_text = "checkpoint restore not implemented yet";
                return {m, Cmd<Msg>::none()};
            },

            [&](ScrollThread& e) -> std::pair<Model, Cmd<Msg>> {
                m.thread_scroll = std::max(0, m.thread_scroll + e.delta);
                return {m, Cmd<Msg>::none()};
            },
            [&](ToggleToolExpanded& e) -> std::pair<Model, Cmd<Msg>> {
                for (auto& msg_ : m.current.messages)
                    for (auto& tc : msg_.tool_calls)
                        if (tc.id == e.tool_call_id) tc.expanded = !tc.expanded;
                return {m, Cmd<Msg>::none()};
            },

            [&](Tick) -> std::pair<Model, Cmd<Msg>> {
                // Tick drives the spinner only — stream/tool events arrive
                // directly via maya's BackgroundQueue (see Cmd::task above).
                auto now = std::chrono::steady_clock::now();
                if (m.last_tick.time_since_epoch().count() == 0) m.last_tick = now;
                float dt = std::chrono::duration<float>(now - m.last_tick).count();
                m.last_tick = now;
                if (m.stream_active) m.spinner.advance(dt);
                return {m, Cmd<Msg>::none()};
            },
            [&](Quit) -> std::pair<Model, Cmd<Msg>> {
                if (!m.current.messages.empty()) persistence::save_thread(m.current);
                return {m, Cmd<Msg>::quit()};
            },
            [&](NoOp) -> std::pair<Model, Cmd<Msg>> {
                return {m, Cmd<Msg>::none()};
            },
        }, msg);
    }

    // --- view ----------------------------------------------------------------

    static Element view(const Model& m) {
        std::vector<Element> rows;
        rows.push_back(views::header(m));
        rows.push_back(sep);
        rows.push_back((v(views::thread_panel(m)) | grow_<1>).build());
        rows.push_back(views::changes_strip(m));
        rows.push_back(views::composer(m));
        rows.push_back(views::status_bar(m));

        auto base = (v(std::move(rows)) | pad<1>).build();

        // Overlays (modal-like). We simulate stacking by rendering over the base.
        if (m.show_model_picker) {
            return (v(base, views::model_picker(m))).build();
        }
        if (m.show_thread_list) {
            return (v(base, views::thread_list(m))).build();
        }
        if (m.show_command_palette) {
            return (v(base, views::command_palette(m))).build();
        }
        if (m.show_diff_review) {
            return (v(base, views::diff_review(m))).build();
        }
        // Permission no longer overlays — it renders inline as a footer on the
        // tool call awaiting it (see render_inline_permission).
        return base;
    }

    // --- subscribe -----------------------------------------------------------

    static auto subscribe(const Model& m) -> Sub<Msg> {
        // Snapshot relevant mode flags for the key filter lambda.
        bool in_model_picker   = m.show_model_picker;
        bool in_thread_list    = m.show_thread_list;
        bool in_command        = m.show_command_palette;
        bool in_diff_review    = m.show_diff_review;
        bool in_permission     = m.pending_permission.has_value();

        auto key_sub = Sub<Msg>::on_key(
            [=](const KeyEvent& ev) -> std::optional<Msg> {
                // --- Modal-specific handlers take priority ---
                if (in_permission) {
                    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                        if (ck->codepoint == 'y' || ck->codepoint == 'Y') return PermissionApprove{};
                        if (ck->codepoint == 'n' || ck->codepoint == 'N') return PermissionReject{};
                        if (ck->codepoint == 'a' || ck->codepoint == 'A') return PermissionApproveAlways{};
                    }
                    if (std::holds_alternative<SpecialKey>(ev.key)) {
                        auto sk = std::get<SpecialKey>(ev.key);
                        if (sk == SpecialKey::Escape) return PermissionReject{};
                    }
                    return std::nullopt;
                }
                if (in_command) {
                    if (std::holds_alternative<SpecialKey>(ev.key)) {
                        auto sk = std::get<SpecialKey>(ev.key);
                        if (sk == SpecialKey::Escape)    return CloseCommandPalette{};
                        if (sk == SpecialKey::Enter)     return CommandPaletteSelect{};
                        if (sk == SpecialKey::Up)        return CommandPaletteMove{-1};
                        if (sk == SpecialKey::Down)      return CommandPaletteMove{+1};
                        if (sk == SpecialKey::Backspace) return CommandPaletteBackspace{};
                    }
                    if (auto* ck = std::get_if<CharKey>(&ev.key))
                        return CommandPaletteInput{ck->codepoint};
                    return std::nullopt;
                }
                if (in_model_picker) {
                    if (std::holds_alternative<SpecialKey>(ev.key)) {
                        auto sk = std::get<SpecialKey>(ev.key);
                        if (sk == SpecialKey::Escape) return CloseModelPicker{};
                        if (sk == SpecialKey::Enter)  return ModelPickerSelect{};
                        if (sk == SpecialKey::Up)     return ModelPickerMove{-1};
                        if (sk == SpecialKey::Down)   return ModelPickerMove{+1};
                    }
                    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                        if (ck->codepoint == 'f' || ck->codepoint == 'F')
                            return ModelPickerToggleFavorite{};
                    }
                    return std::nullopt;
                }
                if (in_thread_list) {
                    if (std::holds_alternative<SpecialKey>(ev.key)) {
                        auto sk = std::get<SpecialKey>(ev.key);
                        if (sk == SpecialKey::Escape) return CloseThreadList{};
                        if (sk == SpecialKey::Enter)  return ThreadListSelect{};
                        if (sk == SpecialKey::Up)     return ThreadListMove{-1};
                        if (sk == SpecialKey::Down)   return ThreadListMove{+1};
                    }
                    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                        if (ck->codepoint == 'n' || ck->codepoint == 'N') return NewThread{};
                    }
                    return std::nullopt;
                }
                if (in_diff_review) {
                    if (std::holds_alternative<SpecialKey>(ev.key)) {
                        auto sk = std::get<SpecialKey>(ev.key);
                        if (sk == SpecialKey::Escape) return CloseDiffReview{};
                        if (sk == SpecialKey::Up)     return DiffReviewMove{-1};
                        if (sk == SpecialKey::Down)   return DiffReviewMove{+1};
                        if (sk == SpecialKey::Left)   return DiffReviewPrevFile{};
                        if (sk == SpecialKey::Right)  return DiffReviewNextFile{};
                    }
                    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                        if (ck->codepoint == 'y' || ck->codepoint == 'Y') return AcceptHunk{};
                        if (ck->codepoint == 'n' || ck->codepoint == 'N') return RejectHunk{};
                        if (ck->codepoint == 'a' || ck->codepoint == 'A') return AcceptAllChanges{};
                        if (ck->codepoint == 'x' || ck->codepoint == 'X') return RejectAllChanges{};
                    }
                    return std::nullopt;
                }

                // --- Global shortcuts ---
                if (ev.mods.ctrl) {
                    if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                        char c = (char)ck->codepoint;
                        if (c == 'c' || c == 'C') return Quit{};
                        if (c == '/')             return OpenModelPicker{};
                        if (c == 'j' || c == 'J') return OpenThreadList{};
                        if (c == 'k' || c == 'K') return OpenCommandPalette{};
                        if (c == 'r' || c == 'R') return OpenDiffReview{};
                        if (c == 'n' || c == 'N') return NewThread{};
                        if (c == 'e' || c == 'E') return ComposerToggleExpand{};
                    }
                }
                if (ev.mods.shift && std::holds_alternative<SpecialKey>(ev.key)) {
                    auto sk = std::get<SpecialKey>(ev.key);
                    if (sk == SpecialKey::Tab || sk == SpecialKey::BackTab)
                        return CycleProfile{};
                }

                // --- Composer (default) ---
                if (std::holds_alternative<SpecialKey>(ev.key)) {
                    auto sk = std::get<SpecialKey>(ev.key);
                    switch (sk) {
                        case SpecialKey::Enter:
                            if (ev.mods.alt) return ComposerNewline{};
                            return ComposerEnter{};
                        case SpecialKey::Backspace: return ComposerBackspace{};
                        case SpecialKey::Left:      return ComposerCursorLeft{};
                        case SpecialKey::Right:     return ComposerCursorRight{};
                        case SpecialKey::Home:      return ComposerCursorHome{};
                        case SpecialKey::End:       return ComposerCursorEnd{};
                        case SpecialKey::Escape:    return Quit{};
                        default: return std::nullopt;
                    }
                }
                if (auto* ck = std::get_if<CharKey>(&ev.key)) {
                    if (ck->codepoint >= 0x20) return ComposerCharInput{ck->codepoint};
                }
                return std::nullopt;
            });

        auto paste_sub = Sub<Msg>::on_paste([](std::string s) -> Msg {
            return ComposerPaste{std::move(s)};
        });

        auto tick = Sub<Msg>::every(std::chrono::milliseconds(16), Tick{});
        return Sub<Msg>::batch(std::move(key_sub), std::move(paste_sub), std::move(tick));
    }
};

static_assert(Program<MohaApp>);

static void print_usage() {
    std::fprintf(stderr,
        "usage: moha [subcommand] [options]\n"
        "\n"
        "subcommands:\n"
        "  login             Authenticate (OAuth via claude.ai or API key)\n"
        "  logout            Remove saved credentials\n"
        "  status            Show current auth status\n"
        "  help              Show this message\n"
        "\n"
        "options:\n"
        "  -k, --key KEY     API-key override for this session\n"
        "  -m, --model ID    Model id (e.g. claude-opus-4-5)\n"
        "\n");
}

int main(int argc, char** argv) {
    std::string cli_key;
    std::string cli_model;
    std::string subcommand;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "login" || a == "logout" || a == "status" || a == "help") {
            subcommand = a;
        } else if ((a == "-k" || a == "--key") && i + 1 < argc) {
            cli_key = argv[++i];
        } else if ((a == "-m" || a == "--model") && i + 1 < argc) {
            cli_model = argv[++i];
        } else if (a == "-h" || a == "--help") {
            subcommand = "help";
        } else {
            std::fprintf(stderr, "unknown arg: %s\n\n", a.c_str());
            print_usage();
            return 2;
        }
    }

    if (subcommand == "help") { print_usage(); return 0; }
    if (subcommand == "login")  return auth::cmd_login();
    if (subcommand == "logout") return auth::cmd_logout();
    if (subcommand == "status") return auth::cmd_status();

    g_creds = auth::resolve(cli_key);
    if (!g_creds.is_valid()) {
        std::fprintf(stderr,
            "moha: not authenticated.\n"
            "  run:  moha login\n"
            "  or:   export ANTHROPIC_API_KEY=sk-ant-...\n"
            "  or:   export CLAUDE_CODE_OAUTH_TOKEN=...\n");
        return 1;
    }

    if (!cli_model.empty()) {
        persistence::Settings s = persistence::load_settings();
        s.model_id = cli_model;
        persistence::save_settings(s);
    }

    run<MohaApp>({.title = "moha", .fps = 30, .mode = Mode::Inline});
    return 0;
}
