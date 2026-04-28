#include "moha/runtime/view/tool_body_preview.hpp"

#include <string>
#include <utility>

#include "moha/runtime/view/palette.hpp"
#include "moha/runtime/view/tool_args.hpp"

namespace moha::ui {

maya::ToolBodyPreview::Config tool_body_preview_config(const ToolUse& tc) {
    using Kind = maya::ToolBodyPreview::Kind;
    const auto& n = tc.name.value;
    maya::ToolBodyPreview::Config out;

    // ── Failure: surface stderr inline so it isn't hidden.
    if (tc.is_failed() && !tc.output().empty()) {
        out.kind = Kind::Failure;
        out.text = tc.output();
        return out;
    }

    // ── Edit: parse hunks from args.
    if (n == "edit" && tc.args.is_object()) {
        if (auto it = tc.args.find("edits");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            out.kind = Kind::EditDiff;
            out.hunks.reserve(it->size());
            for (const auto& e : *it) {
                if (!e.is_object()) continue;
                auto ot = e.value("old_text", e.value("old_string", std::string{}));
                auto nt = e.value("new_text", e.value("new_string", std::string{}));
                out.hunks.push_back({std::move(ot), std::move(nt)});
            }
            return out;
        }
        // Top-level legacy single-edit shape.
        auto ot = safe_arg(tc.args, "old_text");
        if (ot.empty()) ot = safe_arg(tc.args, "old_string");
        auto nt = safe_arg(tc.args, "new_text");
        if (nt.empty()) nt = safe_arg(tc.args, "new_string");
        if (!ot.empty() || !nt.empty()) {
            out.kind = Kind::EditDiff;
            out.hunks.push_back({std::move(ot), std::move(nt)});
        }
        return out;
    }

    // ── Write: head+tail of the streaming/written content.
    if (n == "write") {
        auto content = safe_arg(tc.args, "content");
        if (!content.empty()) {
            out.kind = Kind::CodeBlock;
            out.text = std::move(content);
            out.text_color = fg;
        }
        return out;
    }

    // ── Bash / diagnostics: head+tail of output.
    if ((n == "bash" || n == "diagnostics") && tc.is_terminal()) {
        auto stripped = strip_bash_output_fence(tc.output());
        if (!stripped.empty()) {
            out.kind = Kind::CodeBlock;
            out.text = std::move(stripped);
            out.text_color = fg;
        }
        return out;
    }
    if (n == "bash" && tc.is_running() && !tc.progress_text().empty()) {
        out.kind = Kind::CodeBlock;
        out.text = tc.progress_text();
        out.text_color = fg;
        return out;
    }

    // ── git_diff: per-line diff coloring.
    if (n == "git_diff" && tc.is_done()) {
        const auto& body = tc.output();
        if (!body.empty() && body != "no changes") {
            out.kind = Kind::GitDiff;
            out.text = body;
            out.text_color = fg;
        }
        return out;
    }

    // ── Generic line-oriented tools: head+tail preview.
    if ((n == "read" || n == "list_dir" || n == "grep" || n == "glob"
         || n == "find_definition"
         || n == "web_fetch" || n == "web_search"
         || n == "git_status" || n == "git_log" || n == "git_commit")
        && tc.is_done())
    {
        if (!tc.output().empty()) {
            out.kind = Kind::CodeBlock;
            out.text = tc.output();
            out.text_color = fg;
        }
        return out;
    }

    // ── Todo: parse items + statuses.
    if (n == "todo" && tc.args.is_object()) {
        if (auto it = tc.args.find("todos");
            it != tc.args.end() && it->is_array() && !it->empty())
        {
            using Status = maya::ToolBodyPreview::TodoItem::Status;
            out.kind = Kind::TodoList;
            out.todos.reserve(it->size());
            for (const auto& td : *it) {
                if (!td.is_object()) continue;
                Status s = Status::Pending;
                auto st = td.value("status", std::string{"pending"});
                if      (st == "completed")   s = Status::Completed;
                else if (st == "in_progress") s = Status::InProgress;
                out.todos.push_back({td.value("content", ""), s});
            }
            return out;
        }
    }

    return out;     // kind = None
}

} // namespace moha::ui
