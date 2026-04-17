#include "moha/view/thread.hpp"

#include <string>
#include <vector>

#include <maya/widget/bash_tool.hpp>
#include <maya/widget/edit_tool.hpp>
#include <maya/widget/markdown.hpp>
#include <maya/widget/message.hpp>
#include <maya/widget/read_tool.hpp>
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
    if (!tc.args.empty()) cfg.description = tc.args.dump();
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

} // namespace

Element render_tool_call(const ToolUse& tc) {
    auto path = tc.args.value("path", "");
    auto cmd  = tc.args.value("command", "");

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
    if (tc.name == "edit") {
        EditTool et(path.empty() ? tc.name.value : path);
        et.set_expanded(tc.expanded);
        et.set_old_text(tc.args.value("old_string", ""));
        et.set_new_text(tc.args.value("new_string", ""));
        et.set_status(map_status<EditTool>(tc.status,
            EditStatus::Applying, EditStatus::Failed, EditStatus::Applied));
        return et.build();
    }
    if (tc.name == "write") {
        WriteTool wt(path.empty() ? tc.name.value : path);
        wt.set_expanded(tc.expanded);
        wt.set_content(tc.args.value("content", ""));
        wt.set_max_preview_lines(8);
        wt.set_status(map_status<WriteTool>(tc.status,
            WriteStatus::Writing, WriteStatus::Failed, WriteStatus::Written));
        return wt.build();
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
        spin.set_style(fg_bold(warn));
        rows.push_back(h(
            spin.build(),
            text(" Thinking\u2026", fg_italic(muted))
        ).build());
    }
    if (rows.empty()) {
        rows.push_back(text("Start a conversation \u2014 ask me to read, edit, or run anything.",
                            fg_italic(muted)));
    }
    return (v(std::move(rows)) | padding(0, 1)).build();
}

} // namespace moha::ui
