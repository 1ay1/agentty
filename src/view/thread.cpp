#include "moha/view/thread.hpp"

#include <string>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/fetch_tool.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/read_tool.hpp>
#include <maya/widget/search_result.hpp>
#include <maya/widget/tool_call.hpp>
#include <maya/widget/turn_divider.hpp>
#include <maya/widget/write_tool.hpp>

#include "moha/view/palette.hpp"
#include "moha/view/permission.hpp"

namespace moha::ui {

using namespace maya;
using namespace maya::dsl;

namespace {

template <class W, class StatusEnum>
StatusEnum map_status(ToolUse::Status s, StatusEnum running, StatusEnum failed,
                      StatusEnum done) {
    switch (s) {
        case ToolUse::Status::Pending:
        case ToolUse::Status::Running:  return running;
        case ToolUse::Status::Error:
        case ToolUse::Status::Rejected: return failed;
        case ToolUse::Status::Done:
        case ToolUse::Status::Approved: return done;
    }
    return done;
}

Element fallback_card(const ToolUse& tc) {
    maya::ToolCall::Config cfg;
    cfg.tool_name = tc.name.value;
    cfg.kind = maya::ToolCallKind::Other;
    if (tc.args.is_object() && !tc.args.empty()) cfg.description = tc.args.dump();
    maya::ToolCall card(cfg);
    card.set_expanded(tc.expanded);
    using S = ToolUse::Status;
    if (tc.status == S::Running || tc.status == S::Pending)
        card.set_status(maya::ToolCallStatus::Running);
    else if (tc.status == S::Error || tc.status == S::Rejected)
        card.set_status(maya::ToolCallStatus::Failed);
    else
        card.set_status(maya::ToolCallStatus::Completed);
    if (!tc.output.empty())
        card.set_content(text(tc.output, fg_of(muted)));
    return card.build();
}

std::string safe_arg(const nlohmann::json& args, const char* key) {
    if (!args.is_object()) return {};
    return args.value(key, "");
}

} // namespace

Element render_tool_call(const ToolUse& tc) {
    auto path = safe_arg(tc.args, "path");
    auto cmd  = safe_arg(tc.args, "command");

    // ── File: read ──────────────────────────────────────────────────
    if (tc.name == "read") {
        ReadTool rt(path.empty() ? tc.name.value : path);
        rt.set_expanded(tc.expanded);
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        if (tc.status != ToolUse::Status::Pending && tc.status != ToolUse::Status::Running) {
            rt.set_content(tc.output);
            rt.set_max_lines(12);
        }
        return rt.build();
    }

    // ── Shell: bash ─────────────────────────────────────────────────
    if (tc.name == "bash") {
        BashTool bt(cmd.empty() ? tc.name.value : cmd);
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(10);
        bt.set_status(map_status<BashTool>(tc.status,
            BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        if (tc.status == ToolUse::Status::Done) bt.set_exit_code(0);
        if (tc.status != ToolUse::Status::Pending && tc.status != ToolUse::Status::Running)
            bt.set_output(tc.output);
        return bt.build();
    }

    // ── File: edit ──────────────────────────────────────────────────
    if (tc.name == "edit") {
        EditTool et(path.empty() ? tc.name.value : path);
        et.set_expanded(tc.expanded);
        et.set_old_text(safe_arg(tc.args, "old_string"));
        et.set_new_text(safe_arg(tc.args, "new_string"));
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        return et.build();
    }

    // ── File: write ─────────────────────────────────────────────────
    if (tc.name == "write") {
        WriteTool wt(path.empty() ? tc.name.value : path);
        wt.set_expanded(tc.expanded);
        wt.set_content(safe_arg(tc.args, "content"));
        wt.set_max_preview_lines(8);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        return wt.build();
    }

    // ── Search: grep ────────────────────────────────────────────────
    if (tc.name == "grep") {
        auto pattern = safe_arg(tc.args, "pattern");
        SearchResult sr(SearchKind::Grep, pattern);
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            SearchFileGroup current_group;
            std::istringstream iss(tc.output);
            std::string line;
            while (std::getline(iss, line)) {
                auto c1 = line.find(':');
                if (c1 == std::string::npos) continue;
                auto c2 = line.find(':', c1 + 1);
                if (c2 == std::string::npos) continue;
                std::string file = line.substr(0, c1);
                int lineno = 0;
                try { lineno = std::stoi(line.substr(c1+1, c2-c1-1)); } catch(...) {}
                std::string content = line.substr(c2 + 1);
                if (current_group.file_path != file) {
                    if (!current_group.file_path.empty())
                        sr.add_group(std::move(current_group));
                    current_group = SearchFileGroup{file, {}};
                }
                current_group.matches.push_back({lineno, content});
            }
            if (!current_group.file_path.empty())
                sr.add_group(std::move(current_group));
        }
        return sr.build();
    }

    // ── Search: glob ────────────────────────────────────────────────
    if (tc.name == "glob") {
        auto pattern = safe_arg(tc.args, "pattern");
        SearchResult sr(SearchKind::Glob, pattern);
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            SearchFileGroup group{"", {}};
            std::istringstream iss(tc.output);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) group.matches.push_back({0, line});
            }
            if (!group.matches.empty()) sr.add_group(std::move(group));
        }
        return sr.build();
    }

    // ── Search: find_definition ─────────────────────────────────────
    if (tc.name == "find_definition") {
        auto sym = safe_arg(tc.args, "symbol");
        SearchResult sr(SearchKind::Grep, sym);
        sr.set_expanded(tc.expanded);
        sr.set_status(map_status<SearchResult>(tc.status,
            SearchStatus::Searching, SearchStatus::Failed, SearchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            SearchFileGroup current_group;
            std::istringstream iss(tc.output);
            std::string line;
            while (std::getline(iss, line)) {
                auto c1 = line.find(':');
                if (c1 == std::string::npos) continue;
                auto c2 = line.find(':', c1 + 1);
                if (c2 == std::string::npos) continue;
                std::string file = line.substr(0, c1);
                int lineno = 0;
                try { lineno = std::stoi(line.substr(c1+1, c2-c1-1)); } catch(...) {}
                std::string content = line.substr(c2 + 1);
                if (current_group.file_path != file) {
                    if (!current_group.file_path.empty())
                        sr.add_group(std::move(current_group));
                    current_group = SearchFileGroup{file, {}};
                }
                current_group.matches.push_back({lineno, content});
            }
            if (!current_group.file_path.empty())
                sr.add_group(std::move(current_group));
        }
        return sr.build();
    }

    // ── Web: web_fetch ──────────────────────────────────────────────
    if (tc.name == "web_fetch") {
        auto url = safe_arg(tc.args, "url");
        FetchTool ft(url);
        ft.set_expanded(tc.expanded);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        if (!tc.output.empty() && tc.status == ToolUse::Status::Done) {
            // Parse "HTTP CODE (content-type)\n\nbody" format
            auto first_nl = tc.output.find('\n');
            if (first_nl != std::string::npos) {
                auto header = tc.output.substr(0, first_nl);
                // Extract status code
                auto sp = header.find(' ');
                if (sp != std::string::npos) {
                    try { ft.set_status_code(std::stoi(header.substr(sp+1))); } catch(...) {}
                }
                auto paren = header.find('(');
                if (paren != std::string::npos) {
                    auto close = header.find(')', paren);
                    if (close != std::string::npos)
                        ft.set_content_type(header.substr(paren+1, close-paren-1));
                }
                auto body_start = tc.output.find("\n\n");
                if (body_start != std::string::npos)
                    ft.set_body(tc.output.substr(body_start + 2));
            }
        } else if (tc.status == ToolUse::Status::Error) {
            ft.set_body(tc.output);
        }
        return ft.build();
    }

    // ── Web: web_search ─────────────────────────────────────────────
    if (tc.name == "web_search") {
        auto query = safe_arg(tc.args, "query");
        FetchTool ft("search: " + query);
        ft.set_expanded(tc.expanded);
        ft.set_status(map_status<FetchTool>(tc.status,
            FetchStatus::Fetching, FetchStatus::Failed, FetchStatus::Done));
        if (!tc.output.empty()) ft.set_body(tc.output);
        return ft.build();
    }

    // ── File: list_dir ──────────────────────────────────────────────
    if (tc.name == "list_dir") {
        auto dir_path = safe_arg(tc.args, "path");
        if (dir_path.empty()) dir_path = ".";
        ReadTool rt(dir_path);
        rt.set_expanded(tc.expanded);
        rt.set_status(map_status<ReadTool>(tc.status,
            ReadStatus::Reading, ReadStatus::Failed, ReadStatus::Success));
        if (!tc.output.empty()) {
            rt.set_content(tc.output);
            rt.set_max_lines(20);
        }
        return rt.build();
    }

    // ── Diagnostics ─────────────────────────────────────────────────
    if (tc.name == "diagnostics") {
        BashTool bt(safe_arg(tc.args, "command").empty() ? "diagnostics" : safe_arg(tc.args, "command"));
        bt.set_expanded(tc.expanded);
        bt.set_max_output_lines(20);
        bt.set_status(map_status<BashTool>(tc.status,
            BashStatus::Running, BashStatus::Failed, BashStatus::Success));
        if (!tc.output.empty()) bt.set_output(tc.output);
        return bt.build();
    }

    // ── Git tools ───────────────────────────────────────────────────
    if (tc.name == "git_status" || tc.name == "git_diff" ||
        tc.name == "git_log" || tc.name == "git_commit") {
        maya::ToolCall::Config cfg;
        cfg.tool_name = tc.name.value;
        cfg.kind = maya::ToolCallKind::Other;
        if (tc.name == "git_commit")
            cfg.description = safe_arg(tc.args, "message");
        else if (tc.name == "git_log")
            cfg.description = safe_arg(tc.args, "ref");
        else if (tc.name == "git_diff")
            cfg.description = safe_arg(tc.args, "ref");
        maya::ToolCall card(cfg);
        card.set_expanded(tc.expanded);
        using S = ToolUse::Status;
        if (tc.status == S::Running || tc.status == S::Pending)
            card.set_status(maya::ToolCallStatus::Running);
        else if (tc.status == S::Error || tc.status == S::Rejected)
            card.set_status(maya::ToolCallStatus::Failed);
        else
            card.set_status(maya::ToolCallStatus::Completed);
        if (!tc.output.empty())
            card.set_content(text(tc.output, fg_of(muted)));
        return card.build();
    }

    return fallback_card(tc);
}

Element render_message(const Message& msg, int turn_num, const Model& m) {
    std::vector<Element> rows;
    if (msg.role == Role::User) {
        if (msg.checkpoint_id) rows.push_back(render_checkpoint_divider());
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
            if (m.pending_permission && m.pending_permission->id == tc.id)
                rows.push_back(render_inline_permission(*m.pending_permission, tc));
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
        if (msg.role == Role::Assistant) ++turn;
    }
    if (m.stream.active && !m.current.messages.empty()
        && m.current.messages.back().role == Role::Assistant) {
        auto spin = m.stream.spinner;
        spin.set_style(fg_bold(accent));
        rows.push_back((h(
            spin.build(),
            text(" Thinking\u2026", fg_italic(muted))
        ) | padding(0, 0, 0, 2)).build());
    }
    if (rows.empty()) {
        rows.push_back(
            (v(text("Ask me to read, edit, or run anything.",
                    fg_italic(muted))) | padding(2, 0)).build());
    }
    return (v(std::move(rows)) | padding(0, 1)).build();
}

} // namespace moha::ui
